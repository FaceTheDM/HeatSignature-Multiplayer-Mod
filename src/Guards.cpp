// Guard replication on ships both players share.
// Authority: the HOST when its player is aboard, otherwise whichever player is aboard. The authority simulates guards
// normally and streams their state; the other side suspends its copies' AI and makes them follow. Hits made on a
// mirrored guard are forwarded to the authority; hits that land on the remote player's puppet are forwarded to that
// player's real character. Authority guards perceive the remote player by seeing its puppet as `Player` during their
// own update when it is the nearer of the two players.
#include "Guards.h"
#include "TimePolicy.h"
#include "Console.h"
#include "Place.h"
#include "GmArgs.h"
#include "Net.h"
#include "Objects.h"
#include "Seed.h"
#include "Frame.h"
#include "Snapshot.h"
#include <cstring>
#include "Wire.h"
#ifdef HS_COOP_DEVTOOLS   // developer builds only (build.py --dev): the private developer extension, not in this repository
#include "DevPort.h"
#endif
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

static const HS_ModApi* api;
static int* player;
static int* puppet;
static double enemyObject=-1;
static const double OBJ_SHIP=164;
static const DWORD SNAPSHOT_MS=16, SNAPSHOT_STALE_MS=1500, SNAPSHOT_REFRESH_MS=500;
static char status[160]="no shared guards";
int CoopRemoteParent();
bool CoopRemoteDown();
double CoopRemoteSeenAsThreat();
const char* CoopRemoteStateName();

static const char* const HIT_VARS[]={"DamageSignal","DamageMask","DamageType","DamageSpeedX","DamageSpeedY","ImpactSignal",
    "ImpactFore","ImpactStarboard","ImpactMomentumSignal","KnockoutMomentum","StaggerSignal","PanicSignal","KnockOutTime"};
static const size_t HIT_COUNT=sizeof(HIT_VARS)/sizeof(HIT_VARS[0]);
// Noises and alerts (a punch, a thrown item bouncing, gunfire nearby) land on guards as these signals.
static bool replayingEvent;   // a mission counter replayed from the partner's game (not sent back)
static const char* const ALERT_VARS[]={"DistractionSignal","RespondingSignal","InvestigatingSignal","DistractionFore","DistractionStarboard","RespondingFore","RespondingStarboard"};
// Mission counters run with a guard (or the ship, for alarms) as self and no arguments; each checks itself whether the
// guard belongs to this game's mission, so running it in the other game is safe.
static const char* const MISSION_EVENTS[]={"gml_Script_IncreaseMissionKills","gml_Script_IncreaseMissionNonTargetKills","gml_Script_IncreaseMissionKnockouts",
    "gml_Script_IncreaseMissionNonTargetKnockouts","gml_Script_DecreaseMissionKnockouts","gml_Script_DecreaseMissionLivingWitnesses",
    "gml_Script_IncreaseMissionLivingWitnesses","gml_Script_ShipIncreasesMissionAlarms"};
// Identity of a guard the follower creates because it joined the ship after generation.
static const char* const SPAWN_TEXT[]={"Name","Skin"};
static const char* const SPAWN_REAL[]={"MercType","Female","Security"};
// oEnemy's step event runs its state script directly; mirrored guards must not run any of them.
static const char* const ENEMY_STATES[]={"gml_Script_EnemyStateCarried","gml_Script_EnemyStateCarryingBody","gml_Script_EnemyStateChasing","gml_Script_EnemyStateConfronting","gml_Script_EnemyStateControlling","gml_Script_EnemyStateDistracted","gml_Script_EnemyStateDown","gml_Script_EnemyStateErrand","gml_Script_EnemyStateEscaping","gml_Script_EnemyStateFetchingBody","gml_Script_EnemyStateFloating","gml_Script_EnemyStateIdle","gml_Script_EnemyStateInvestigating","gml_Script_EnemyStateLeavingBody","gml_Script_EnemyStatePanicking","gml_Script_EnemyStatePatrolling","gml_Script_EnemyStatePatrolPath","gml_Script_EnemyStatePlacingJammers","gml_Script_EnemyStateResponding","gml_Script_EnemyStateSearching","gml_Script_EnemyStateSeeking","gml_Script_EnemyStateSentry","gml_Script_EnemyStateStagger","gml_Script_EnemyStateStatic","gml_Script_EnemyStateSucking"};


static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static void setReal(int id,const char* n,double d){ RValue v{}; v.type=0; v.real=d; api->SetVarByName(id,n,0x80000000,&v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static void setString(int id,const char* n,const std::string& s){ RValue v{}; v.type=1; api->SetString(&v,s.c_str()); api->SetVarByName(id,n,0x80000000,&v); }
static RValue call(const char* script,CInstance* self,GmArgs& a){ RValue r{}; r.type=5; api->CallScript(script,self,self,&r,a.Count(),a.Build()); return r; }
static double nz(double d){ return std::isnan(d)?0:d; }

// ---- Guards aboard a ship, keyed across processes by generation order ----
// Both copies of a ship are generated identically, so the n-th guard in its contents is the same guard on both sides.
// Names are not used: a mission target is named after the mission only in the owner's world. The ordinal is recorded the
// first time a guard is seen so later reordering of the contents list (bodies carried, guards leaving) cannot reshuffle it.
struct Guard { std::string key; int id; };
static std::map<int,std::map<int,int>> ordinals;   // ship -> (guard id -> ordinal), fallback for unkeyed guards
// Guards are keyed by where generation placed them (rounded Fore/Starboard right after AddGuardsToShip /
// AddObjectiveToHighestSecurityModule). Generation is identical in both games, so the keys match no matter how the
// contents list is reordered later or when each game first looks at the ship.
static std::map<int,std::string> generationKeys;   // guard id -> key
// Guns given to guards in the partner's game, waiting for this game's copy of the guard (see applyPartnerKits).
struct PendingKit { std::string ship,key; std::vector<uint8_t> data; DWORD at; };
static std::vector<PendingKit> partnerKits;
static bool isEnemy(CInstance* inst) {
    if(!inst) return false;
    if(enemyObject<0 && inst->GetObjectName(api)=="oEnemy") enemyObject=inst->type;
    return inst->type==(uint32_t)enemyObject;
}
static void recordGenerationKeys(int ship) {
    std::map<std::string,int> used;
    int count=(int)getReal(ship,"ContentsCount");
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
        if(!isEnemy(inst) || generationKeys.count(id)) continue;
        char key[64]; snprintf(key,sizeof(key),"p%.0f,%.0f",std::floor(getReal(id,"Fore")+0.5),std::floor(getReal(id,"Starboard")+0.5));
        std::string k=key; int n=used[k]++; if(n) k+="#"+std::to_string(n);
        generationKeys[id]=k;
    }
}
static void postGenerationStep(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) { if(self) recordGenerationKeys((int)self->id); }
static bool authority;        // this side simulates its guards
static std::vector<Guard> guardsAboard(int ship) {
    std::vector<Guard> out;
    auto& known=ordinals[ship];
    int count=(int)getReal(ship,"ContentsCount");
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
        if(!isEnemy(inst)) continue;
        auto g=generationKeys.find(id);
        if(g!=generationKeys.end()) { out.push_back({g->second,id}); continue; }
        // Guards that arrive later (defenders, reinforcements) are created by each game itself, with setup only the
        // game's own arrival code does; they are matched by arrival order.
        auto it=known.find(id);
        int ordinal=it!=known.end()?it->second:(known[id]=(int)known.size());
        char key[24]; snprintf(key,sizeof(key),"g%d",ordinal);
        out.push_back({key,id});
    }
    return out;
}

// ---- Shared-ship bookkeeping ----
static int sharedShip;        // the ship both players are aboard (0 if none)
static std::set<int> mirrored;// local guards following the authority
// Each mirrored guard's own state name from before it was mirrored. The authority's state name is shown on the copy
// while its state scripts are off; when mirroring ends (partner gone, authority moves here) the guard gets its own back,
// since that state's script would otherwise run with data only the authority's copy has (it crashed EnemyStateResponding).
static std::map<int,std::string> ownState;
static std::string getString(int id,const char* n);
static void setString(int id,const char* n,const std::string& s);
static void releaseMirrors() {
    for(auto& kv:ownState) {
        int id=kv.first; if(!api->ResolveCInstance(id)) continue;
        std::string st=getString(id,"State");
        if(st=="Down" || st=="Carried" || st==kv.second) continue;
        setString(id,"State",kv.second); setString(id,"NewState",kv.second);
    }
    if(!ownState.empty()) CoopLog("Guards: %d guards run on their own again",(int)ownState.size());
    ownState.clear();
}
struct GuardState { double fore,starboard,angle,legAngle,torsoAngle,foreSpeed,starboardSpeed,measuredSpeed; uint8_t dead,conscious,bleeding,carried; std::string state, anim;
    uint8_t hacked,crashed,laser; float laserX,laserY;
    float alertSprite, secondsSinceAlarmed; uint8_t showAlarm, seeing, witness, waiting;
    float motion[10];
    uint8_t carriedByPlayer; float carrierFore, carrierStarboard;
    float shieldOff, shieldOn;   // emergency shield: up while shieldOff > 0 (each game counted its own)
    DWORD at; };   // when this guard's data arrived (unchanged guards are not re-sent every frame)   // carried by the sender's own player (and where they stood)
// What the guard's own movement step steers by: without them it brakes towards standing every frame (walk a step, stop).
static std::set<int> partnerCarried;   // bodies the partner carries (follower side)
static int carriedByMe;                     // the body this player carried last frame (follower side)
static std::map<int,DWORD> thrownBodies;    // bodies this player just put down or threw -> until when they are followed
static std::map<int,std::pair<double,double>> carriedOffset;   // their smoothed offset from the partner's stand-in
// The last two are the frame counts of the leg and torso animations: the copy plays the same animation names, but a
// seated pilot's legs ran a 10-frame animation on a 60-frame counter there (legs twitching instead of sitting still).
static const char* const MOTION_VARS[10]={"ForeSpeedDesired","StarboardSpeedDesired","CurrentSpeed","Speed","ForeSpeedUnit","StarboardSpeedUnit","LegFrameCount","TorsoFrameCount","LegFrame","InventorySelection"};
static std::vector<std::vector<uint8_t>> pendingPlayerHits; static DWORD pendingPlayerHitsAt;   // hits from guards in the partner's world
static std::map<std::string,GuardState> latest;
int GuardsGuardByKey(const char* key);
struct Body { double fore,starboard,angle; DWORD at; };
static std::map<std::string,Body> bodies;   // guards the partner is carrying (authority side)
static double latestAlarm, latestEscape; static DWORD latestAt; static std::string latestShip;

// Sent as several small unreliable datagrams (each self-contained, filled up to a size that always fits one datagram) so
// it never needs the reliable channel at 30 Hz; the follower merges them by guard key.
static void writeGuard(Writer& w,const Guard& g);
// Only guards whose data changed since they were last sent go out every frame; every guard is re-sent at least every
// SNAPSHOT_REFRESH_MS (lost datagrams, the follower's "still aboard" check).
static std::map<int,std::pair<std::vector<uint8_t>,DWORD>> lastSentGuard;
static void sendSnapshot(int ship) {
    std::vector<Guard> all=guardsAboard(ship), gs;
    DWORD now=GetTickCount();
    for(const Guard& g:all) {
        Writer probe; writeGuard(probe,g);
        auto& last=lastSentGuard[g.id];
        if(last.first==probe.bytes && now-last.second<SNAPSHOT_REFRESH_MS) continue;
        last.first=std::move(probe.bytes); last.second=now;
        gs.push_back(g);
    }
    // Nothing changed: only the ship-wide alert state still goes out now and then (and at once when it changes).
    static double sentAlarm=-1, sentEscape=-1; static DWORD sentHeaderAt;
    double alarm=nz(getReal(ship,"Alarm")), escape=nz(getReal(ship,"EscapeProtocol"));
    if(gs.empty() && !all.empty() && alarm==sentAlarm && escape==sentEscape && now-sentHeaderAt<SNAPSHOT_REFRESH_MS) return;
    sentAlarm=alarm; sentEscape=escape; sentHeaderAt=now;
    const size_t PACKET_BUDGET=900;   // bytes of guards per datagram (one guard is ~120 bytes; the datagram limit is ~1200)
    size_t start=0;
    do {
        std::vector<Writer> one; size_t used=0, end=start;
        for(;end<gs.size() && (end==start || used<PACKET_BUDGET);end++) { one.emplace_back(); writeGuard(one.back(),gs[end]); used+=one.back().bytes.size(); }
        Writer w; w.str(getString(ship,"Name"));
        // Ship-wide alert state shown in the HUD (alarm, target escaping).
        w.put<float>((float)nz(getReal(ship,"Alarm"))); w.put<float>((float)nz(getReal(ship,"EscapeProtocol")));
        w.put<uint16_t>((uint16_t)(end-start));
        for(Writer& g:one) w.bytes.insert(w.bytes.end(),g.bytes.begin(),g.bytes.end());
        NetSendUnreliable(MSG_GUARDS,w.bytes.data(),w.bytes.size());
        start=end;
    } while(start<gs.size());
}
static void writeGuard(Writer& w,const Guard& g) {
        {
            int id=g.id;
            w.str(g.key);
            for(const char* n:{"Fore","Starboard","MyImageAngle","LegAngle","TorsoAngle","ForeSpeed","StarboardSpeed","MeasuredSpeed"}) w.put<float>((float)nz(getReal(id,n)));
            w.put<uint8_t>((uint8_t)nz(getReal(id,"Dead"))); w.put<uint8_t>((uint8_t)nz(getReal(id,"Conscious")));
            w.put<uint8_t>((uint8_t)nz(getReal(id,"Bleeding"))); w.put<uint8_t>((uint8_t)nz(getReal(id,"Carried")));
            w.str(getString(id,"State")); w.str(getString(id,"Animation"));
            // Hacked/crashed (shield and gear state the follower's draw and melee checks read) and the aim laser.
            double fx=0,fy=0; FrameRef(fx,fy);
            w.put<uint8_t>((uint8_t)nz(getReal(id,"Hacked"))); w.put<uint8_t>((uint8_t)nz(getReal(id,"Crashed"))); w.put<uint8_t>((uint8_t)nz(getReal(id,"Laser")));
            w.put<float>((float)(nz(getReal(id,"LaserX"))-fx)); w.put<float>((float)(nz(getReal(id,"LaserY"))-fy));
            // What the guard's own draw shows over its head (alert icon, alarm marker, "seen you").
            w.put<float>((float)nz(getReal(id,"AlertSpriteOverride"))); w.put<float>((float)nz(getReal(id,"SecondsSinceAlarmed")));
            w.put<uint8_t>((uint8_t)nz(getReal(id,"ShowAlarm"))); w.put<uint8_t>((uint8_t)nz(getReal(id,"SeeingPlayerThisStep")));
            w.put<uint8_t>((uint8_t)nz(getReal(id,"Witness"))); w.put<uint8_t>((uint8_t)nz(getReal(id,"Waiting")));
            for(const char* n:MOTION_VARS) w.put<float>((float)nz(getReal(id,n)));
            // A body this game's player carries: where the carrier stood, so the partner's game keeps the body at the
            // same offset from its stand-in for them every frame (smooth, instead of following 30 Hz snapshots).
            // The emergency shield: its countdowns (the guard's own UpdateRecognizingPlayer raises and drops it).
            w.put<float>((float)nz(getReal(id,"SecondsUntilEmergencyShieldTurnsOff"))); w.put<float>((float)nz(getReal(id,"SecondsUntilEmergencyShieldTurnsOn")));
            bool mine=*player>0 && (int)nz(getReal(*player,"CarriedObject"))==id;
            w.put<uint8_t>(mine?1:0);
            if(mine) { w.put<float>((float)getReal(*player,"Fore")); w.put<float>((float)getReal(*player,"Starboard")); }
        }
}
static double partnerThreat;   // how much the guards see the partner as a threat (kept per shared ship)
static void readHit(int id,Writer& w){ for(const char* n:HIT_VARS) w.put<double>(nz(getReal(id,n))); }
static void clearHit(int id){ setReal(id,"DamageSignal",0); setReal(id,"ImpactSignal",0); setReal(id,"StaggerSignal",0); setReal(id,"PanicSignal",0); }
static bool hitPending(int id){ return nz(getReal(id,"DamageSignal"))!=0 || nz(getReal(id,"ImpactSignal"))!=0 || nz(getReal(id,"StaggerSignal"))!=0; }
static void applyHit(int id,Reader& r){ for(const char* n:HIT_VARS){ double v=r.get<double>(); if(r.ok) setReal(id,n,v); } }

// Followers show guards slightly ahead of the last snapshot (by its speeds and age) and ease toward that, so they move
// every frame instead of jumping at snapshot rate.
static std::map<int,std::pair<double,double>> shown;   // guard id -> displayed Fore/Starboard
// Mirrored guards that are down on the authority run the game's own Down state here, so they fall, become bodies,
// drop their weapon/loot/keycard and can be searched or carried exactly as in the host's game.
static std::set<int> downMirrors;
// Where each mirrored guard is shown this frame. Its own step still moves it by its speed (kept for its walk animation);
// it is put back after that movement (when its animation advances, later in the same step), so it never sits a frame of
// movement off (aim and prompts locking onto it jittered).
static std::map<int,std::pair<double,double>> pinned;
// The authority's leg/torso animation frame counts per mirrored guard: the copy's own animation step sets its own (a
// seated pilot's 10-frame legs ran on a 60-frame counter), so they are put back right before its frames advance.
static std::map<int,std::pair<float,float>> frameCounts;   // guard -> (leg frame count, leg frame to hold or -1)
static void prePersonAnimates(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self) return;
    int id=(int)self->id; if(!mirrored.count(id) || downMirrors.count(id)) return;
    auto it=frameCounts.find(id); if(it==frameCounts.end()) return;
    // Legs only: forcing the torso count ended the copy's one-shot torso animations early, and their (empty) return
    // animation raised "Torso animation not found".
    setReal(id,"LegFrameCount",it->second.first);
    // A guard standing or sitting still keeps the authority's leg frame: the copy otherwise animated its legs from the
    // small position corrections it gets on a moving ship (a seated pilot's legs cycling instead of resting).
    if(it->second.second>=0) setReal(id,"LegFrame",it->second.second);
}
static void postPersonMoved(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self) return;
    int id=(int)self->id;
    if(!mirrored.count(id) || downMirrors.count(id)) return;
    // The copy advances its legs on its own frame count (a seated pilot's 10-frame legs ran to frame 54): its leg frame
    // is kept inside the authority's animation, or held where the authority's still guard holds it.
    { auto fc=frameCounts.find(id);
      if(fc!=frameCounts.end() && fc->second.first>0) {
          double lf=nz(getReal(id,"LegFrame"));
          if(fc->second.second>=0) setReal(id,"LegFrame",fc->second.second);
          else if(lf>=fc->second.first) setReal(id,"LegFrame",std::fmod(lf,(double)fc->second.first));
      } }
    auto it=pinned.find(id); if(it==pinned.end()) return;
    if(getReal(id,"Fore")==it->second.first && getReal(id,"Starboard")==it->second.second) return;
    setReal(id,"Fore",it->second.first); setReal(id,"Starboard",it->second.second);
    PlaceAboard(api,self);
}
// Guards can join a ship after generation (errands, reinforcements) or leave it. The follower asks the host for guards
// it does not have and removes copies the host no longer reports.
static std::map<std::string,DWORD> lastSeen, requestedAt;
static bool animateMirrors;   // false while this player is not aboard: that ship's guards are not set up for drawing
// Guards put down here because the guards' game said so. Their drops come from that game (as gdrop items); one that
// dropped its own as well left two of the item (a copy released from mirroring for a moment, during a practice restart).
static std::set<int> downByAuthority;
bool GuardsDownByAuthority(int id) { return downByAuthority.count(id)!=0; }
static void mirrorGuard(int id,const GuardState& s,double ageSeconds,double dt) {
    CInstance* inst=api->ResolveCInstance(id); if(!inst) return;
    GmArgs none;
    bool wasDown=nz(getReal(id,"Dead"))!=0 || nz(getReal(id,"Conscious"))==0;
    bool down=s.dead || !s.conscious;
    setReal(id,"Dead",s.dead); setReal(id,"Conscious",s.conscious); setReal(id,"Bleeding",s.bleeding);
    setReal(id,"Witness",s.witness);   // down guards too: the game's KillEnemy changes it, and a body counts for the clause
    setReal(id,"Hacked",s.hacked); setReal(id,"Crashed",s.crashed);
    setReal(id,"SecondsUntilEmergencyShieldTurnsOff",s.shieldOff); setReal(id,"SecondsUntilEmergencyShieldTurnsOn",s.shieldOn);
    { double fx=0,fy=0; FrameRef(fx,fy); setReal(id,"Laser",s.laser); setReal(id,"LaserX",s.laserX+fx); setReal(id,"LaserY",s.laserY+fy); }
    if(down) {
        // A body lies still: it is only moved when the host's copy really moved (carried, dragged, sucked out), so its
        // pick-up prompt and search work like any body here. Carrying here is decided by this game's player.
        downMirrors.insert(id); shown.erase(id);
        if(s.carriedByPlayer && *puppet && api->ResolveCInstance(*puppet)) {
            // Carried by the partner: kept at the carrier's offset from the partner's stand-in, turning with them.
            // The offset is smoothed: sampled where the carrier and the body were in the same frame, it jitters by a
            // frame of movement.
            auto& off=carriedOffset[id];
            double of=s.fore-s.carrierFore, os=s.starboard-s.carrierStarboard;
            if(!partnerCarried.count(id)) off={of,os};
            double ko=dt*10; if(ko>1) ko=1; off.first+=(of-off.first)*ko; off.second+=(os-off.second)*ko;
            double f=getReal(*puppet,"Fore")+off.first, st=getReal(*puppet,"Starboard")+off.second;
            double k=dt*15; if(k>1) k=1;
            double a=nz(getReal(id,"MyImageAngle")), d=std::fmod(s.angle-a+540.0,360.0)-180.0;
            setReal(id,"Fore",f); setReal(id,"Starboard",st); setReal(id,"MyImageAngle",a+d*k);
            PlaceAboard(api,inst); call("gml_Script_SetImageAngle",inst,none);
            partnerCarried.insert(id);
        } else if(partnerCarried.erase(id) || std::hypot(getReal(id,"Fore")-s.fore,getReal(id,"Starboard")-s.starboard)>48) {
            // Put down (or moved for real on the authority): exactly where and how it lies there.
            setReal(id,"Fore",s.fore); setReal(id,"Starboard",s.starboard); setReal(id,"MyImageAngle",s.angle);
            PlaceAboard(api,inst); call("gml_Script_SetImageAngle",inst,none);
        } else if(std::fabs(std::fmod(s.angle-nz(getReal(id,"MyImageAngle"))+540.0,360.0)-180.0)>2) {
            setReal(id,"MyImageAngle",s.angle); call("gml_Script_SetImageAngle",inst,none);
        }
        if(animateMirrors && getString(id,"State")!="Down" && getString(id,"NewState")!="Down") { GmArgs st; st.AddStr(api,"Down"); call("gml_Script_ChangeStateTo",inst,st); }
        if(!wasDown) { CoopLog("Guards: %d is %s on the authority",id,s.dead?"dead":"unconscious"); downByAuthority.insert(id); }
        return;
    }
    if(downMirrors.count(id)) {
        // Up again on the authority: the game's own Down state stands it up (re-arming it as the game does); it is
        // mirrored again once it has left that state.
        if(getString(id,"State")=="Down") return;
        downMirrors.erase(id);
        CoopLog("Guards: %d got up on the authority",id);
    }
    // Predicted to now from the guard's own speed (the network delay is made up), then eased only lightly.
    double age=(GetTickCount()-s.at)/1000.0+0.03;
    double lead=age>0.25?0.25:age;
    // The lead is game time: none while time stands still (a pause, or the partner planning), less in slow motion. In
    // real seconds it kept growing during a pause, and guards rubber-banded between the lead and each new state.
    if(TimePolicyLocalPaused() || TimePolicyPausedByPartner()) lead=0;
    else if(api->GetTimeScale) { double ts=api->GetTimeScale(); if(ts>=0 && ts<1) lead*=ts; }
    double tf=s.fore+s.foreSpeed*lead, ts=s.starboard+s.starboardSpeed*lead;
    auto it=shown.find(id);
    if(it==shown.end() || std::hypot(it->second.first-tf,it->second.second-ts)>200) it=shown.insert_or_assign(id,std::make_pair(tf,ts)).first;
    double k=dt*25; if(k>1) k=1;
    it->second.first+=(tf-it->second.first)*k; it->second.second+=(ts-it->second.second)*k;
    setReal(id,"Fore",it->second.first); setReal(id,"Starboard",it->second.second); setReal(id,"MyImageAngle",s.angle);
    pinned[id]={it->second.first,it->second.second};
    setReal(id,"LegAngle",s.legAngle); setReal(id,"TorsoAngle",s.torsoAngle);
    setReal(id,"ForeSpeed",s.foreSpeed); setReal(id,"StarboardSpeed",s.starboardSpeed); setReal(id,"MeasuredSpeed",s.measuredSpeed);
    for(int i=0;i<6;i++) setReal(id,MOTION_VARS[i],s.motion[i]);
    // The weapon in hand: the authority's guards switch between their items (a shotgun at range, a blade up close).
    { int sel=(int)s.motion[9], n=(int)nz(getReal(id,"InventoryCount")); if(sel>=0 && sel<n && (int)nz(getReal(id,"InventorySelection"))!=sel) setReal(id,"InventorySelection",sel); }
    if(s.motion[6]>0) frameCounts[id]={s.motion[6],(s.measuredSpeed==0 && s.foreSpeed==0 && s.starboardSpeed==0)?s.motion[8]:-1.0f};
    setReal(id,"Carried",s.carried);
    // The guard's own step picks its body animation (walk, run, aim, punch) from Animation, and its draw picks the icons
    // over its head from its state and alert variables. Its state scripts never run here, so the state name is safe.
    if(!s.anim.empty() && getString(id,"Animation")!=s.anim) setString(id,"Animation",s.anim);
    if(!s.state.empty() && s.state!="Down" && s.state!="Carried" && getString(id,"State")!=s.state) {
        if(!ownState.count(id)) ownState[id]=getString(id,"State");
        setString(id,"State",s.state); setString(id,"NewState",s.state);
    }
    setReal(id,"AlertSpriteOverride",s.alertSprite); setReal(id,"SecondsSinceAlarmed",s.secondsSinceAlarmed);
    setReal(id,"ShowAlarm",s.showAlarm); setReal(id,"SeeingPlayerThisStep",s.seeing); setReal(id,"Witness",s.witness); setReal(id,"Waiting",s.waiting);

    PlaceAboard(api,inst);
    if(animateMirrors) {
        call("gml_Script_SetImageAngle",inst,none);
        call("gml_Script_AnimateLegsOnGround",inst,none); call("gml_Script_ProgressSpineLegAndTorsoFrames",inst,none);
    }
}

static void receiveDelivered(Reader& r);
static void receiveSlowMo(Reader& r);
bool MissionsShipIsCopy(int ship);
static void receiveAlarmRequest(Reader& r);
static void receiveReinforcement(Reader& r);
void GuardsOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    Reader r(data,size);
    if(type==MSG_ALARM_REQUEST) { receiveAlarmRequest(r); return; }
    if(type==MSG_REINFORCEMENT) { receiveReinforcement(r); return; }
    if(type==MSG_GUARD_KIT) {
        PendingKit k; k.ship=r.str(); k.key=r.str(); k.at=GetTickCount();
        if(r.ok) { size_t used=size-r.left; k.data.assign(data+used,data+size); partnerKits.push_back(k); }
        return;
    }
    if(type==MSG_GUARDS) {
        std::string ship=r.str(); latestAlarm=r.get<float>(); latestEscape=r.get<float>(); uint16_t n=r.get<uint16_t>();
        std::map<std::string,GuardState> next;
        for(uint16_t i=0;i<n && r.ok;i++) {
            std::string key=r.str(); GuardState s;
            s.fore=r.get<float>(); s.starboard=r.get<float>(); s.angle=r.get<float>(); s.legAngle=r.get<float>(); s.torsoAngle=r.get<float>();
            s.foreSpeed=r.get<float>(); s.starboardSpeed=r.get<float>(); s.measuredSpeed=r.get<float>();
            s.dead=r.get<uint8_t>(); s.conscious=r.get<uint8_t>(); s.bleeding=r.get<uint8_t>(); s.carried=r.get<uint8_t>();
            s.state=r.str(); s.anim=r.str();
            s.hacked=r.get<uint8_t>(); s.crashed=r.get<uint8_t>(); s.laser=r.get<uint8_t>(); s.laserX=r.get<float>(); s.laserY=r.get<float>();
            s.alertSprite=r.get<float>(); s.secondsSinceAlarmed=r.get<float>();
            s.showAlarm=r.get<uint8_t>(); s.seeing=r.get<uint8_t>(); s.witness=r.get<uint8_t>(); s.waiting=r.get<uint8_t>();
            for(float& m:s.motion) m=r.get<float>();
            s.shieldOff=r.get<float>(); s.shieldOn=r.get<float>();
            s.carriedByPlayer=r.get<uint8_t>(); s.carrierFore=s.carrierStarboard=0;
            if(s.carriedByPlayer) { s.carrierFore=r.get<float>(); s.carrierStarboard=r.get<float>(); }
            if(r.ok) next[key]=s;
        }
        if(r.ok){ if(ship!=latestShip) { latest.clear(); lastSeen.clear(); } for(auto& kv:next) { latest[kv.first]=kv.second; latest[kv.first].at=GetTickCount(); lastSeen[kv.first]=GetTickCount(); } latestAt=GetTickCount(); latestShip=ship; }
        return;
    }
    if(type==MSG_GUARD_HIT) {
        std::string ship=r.str(), key=r.str();
        if(!r.ok || !sharedShip || !authority) return;
        for(const Guard& g:guardsAboard(sharedShip)) if(g.key==key){
            applyHit(g.id,r); CoopLog("Guards: partner hit %s (signal %g mask %g)",key.c_str(),getReal(g.id,"DamageSignal"),getReal(g.id,"DamageMask"));
            // Attacking a guard makes the partner a threat to the guards, as it does a player in the game itself.
            if(partnerThreat<1) { partnerThreat=1; CoopLog("Guards: the partner attacked a guard; the guards now see them as a threat"); }
            return;
        }
        CoopLog("Guards: partner hit %s, but no such guard here",key.c_str());
        return;
    }
    if(type==MSG_GUARD_REQUEST) {
        std::string ship=r.str(), key=r.str();
        if(!r.ok || !sharedShip || !authority || getString(sharedShip,"Name")!=ship) return;
        int id=GuardsGuardByKey(key.c_str()); if(id<=0) return;
        Writer w; w.str(ship); w.str(key); w.put<double>(getReal(id,"Fore")); w.put<double>(getReal(id,"Starboard"));
        for(const char* n:SPAWN_TEXT) w.str(getString(id,n));
        for(const char* n:SPAWN_REAL) w.put<double>(getReal(id,n));
        NetSendReliable(MSG_GUARD_SPAWN,w.bytes.data(),w.bytes.size());
        CoopLog("Guards: sent %s (%s) to the partner, who does not have this guard",key.c_str(),getString(id,"Name").c_str());
        return;
    }
    if(type==MSG_GUARD_SPAWN) {
        std::string ship=r.str(), key=r.str(); double fore=r.get<double>(), starboard=r.get<double>();
        if(!r.ok || !sharedShip || authority || getString(sharedShip,"Name")!=ship || GuardsGuardByKey(key.c_str())) return;
        CInstance* s=api->ResolveCInstance(sharedShip); if(!s) return;
        int before=(int)getReal(sharedShip,"ContentsCount");
        GmArgs a; a.AddReal(sharedShip); a.AddReal(fore); a.AddReal(starboard);
        RValue made=call("gml_Script_CreateEnemyOnShip",s,a);
        int id=(int)Number(made);
        if(!(id>0 && api->ResolveCInstance(id) && isEnemy(api->ResolveCInstance(id)))) {
            id=0; int count=(int)getReal(sharedShip,"ContentsCount");
            for(int i=count-1;i>=before && i>=0;i--) { RValue item{}; item.type=5; api->GetVarByName(sharedShip,"Contents",i,&item); int c=(int)Number(item); if(c>0 && isEnemy(api->ResolveCInstance(c))) { id=c; break; } }
        }
        if(!id) { CoopLog("Guards: could not create %s",key.c_str()); return; }
        // Only who the guard is: the game equips and initialises it itself (copying its whole state would leave it with
        // flags for equipment it does not have here).
        for(const char* n:SPAWN_TEXT) { std::string s=r.str(); if(!s.empty()) setString(id,n,s); }
        for(const char* n:SPAWN_REAL) { double d=r.get<double>(); if(d==d) setReal(id,n,d); }
        { std::string skin=getString(id,"Skin"); if(!skin.empty()) { GmArgs sk; sk.AddStr(api,skin.c_str()); call("gml_Script_SetSkin",api->ResolveCInstance(id),sk); } }
        setReal(id,"Fore",fore); setReal(id,"Starboard",starboard);
        PlaceAboard(api,api->ResolveCInstance(id));
        generationKeys[id]=key;
        CoopLog("Guards: created %s (%s), who joined the ship in the host's game",key.c_str(),getString(id,"Name").c_str());
        return;
    }
    if(type==MSG_TARGET_DELIVERED) { receiveDelivered(r); return; }
    if(type==MSG_SLOWMO) { receiveSlowMo(r); return; }
    if(type==MSG_GUARD_ALERT) {
        std::string ship=r.str(), key=r.str(); double v[7]; for(double& d:v) d=r.get<double>();
        std::string itemKey=r.str(); double thenSearch=r.get<double>();
        if(!r.ok || !sharedShip || !authority) return;
        int id=GuardsGuardByKey(key.c_str()); if(id<=0) return;
        // The states these signals start read the distracting item (Exists) and RespondingThenSearch: they must hold a
        // value here (an unset one stopped the game in EnemyStateDistracted / EnemyStateResponding).
        int item=itemKey=="@"?*puppet:(itemKey.empty()?0:ObjectsIdOfKey(sharedShip,itemKey.c_str()));
        if(v[0]!=0) setReal(id,"DistractionItem",item>0?item:-4);
        if(!(getReal(id,"RespondingThenSearch")==getReal(id,"RespondingThenSearch"))) setReal(id,"RespondingThenSearch",thenSearch);
        for(int i=0;i<7;i++) if(v[i]!=0) setReal(id,ALERT_VARS[i],v[i]);
        return;
    }
    if(type==MSG_MISSION_EVENT) {
        uint8_t which=r.get<uint8_t>(); std::string key=r.str();
        if(!r.ok || which>=sizeof(MISSION_EVENTS)/sizeof(MISSION_EVENTS[0]) || !sharedShip || authority) return;
        int self=key=="$"?sharedShip:GuardsGuardByKey(key.c_str()); CInstance* inst=self>0?api->ResolveCInstance(self):nullptr;
        if(!inst) return;
        replayingEvent=true; GmArgs none; call(MISSION_EVENTS[which],inst,none); replayingEvent=false;
        return;
    }
    if(type==MSG_GUARD_ANIM) {
        std::string key=r.str(), anim=r.str(), back=r.str();
        if(!r.ok || authority || !sharedShip) return;
        int id=GuardsGuardByKey(key.c_str()); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
        if(!inst || downMirrors.count(id) || !animateMirrors) return;
        GmArgs a; a.AddStr(api,anim.c_str()); a.AddStr(api,back.c_str()); call("gml_Script_PlayTorsoAnimOnce",inst,a);
        return;
    }
    if(type==MSG_BODY) {
        std::string key=r.str(); float fore=r.get<float>(), starboard=r.get<float>(), angle=r.get<float>();
        if(r.ok) bodies[key]={fore,starboard,angle,GetTickCount()};
        return;
    }
    if(type==MSG_PLAYER_HIT) {
        // Applied just before the player's own damage handler runs (signals are reset at the start of each step).
        if(GetTickCount()-pendingPlayerHitsAt>1000) pendingPlayerHits.clear();   // stale hits never land later
        pendingPlayerHits.emplace_back(data,data+size); pendingPlayerHitsAt=GetTickCount();
    }
}

// ---- Authority guards see the remote player's puppet as `Player` when it is the nearer target ----
static double savedPlayer; static bool swapped;
static std::map<int,bool> guardTarget;   // per guard: after the partner (true) or this game's player (false)
static std::string savedState; static double savedThreat;
// This game follows the guards: a hit on a guard copy (this player's shot or blow) goes to the guards' game, which
// decides it; the copy itself does not take it. Sent from the copy's own damage handling (EnemyHandleDamageSignal),
// where the game consumes the signal: checked only once a frame from the mod's tick, a hit the game had already used
// up in the same frame was lost (playtest: blood on the guard, nothing happened, 2026-10-05).
static bool forwardHit(int id,const std::string& key) {
    if(!hitPending(id)) return false;
    if(ObjectsExplosionNear(id) || (nz(getReal(id,"DamageSignal"))!=0 && nz(getReal(id,"DamageMask"))==0)) { clearHit(id); return false; }   // explosion also on the host / a shown-only bullet
    CoopLog("Guards: your hit on %s sent to the host (signal %g mask %g)",key.c_str(),getReal(id,"DamageSignal"),getReal(id,"DamageMask"));
    Writer w; w.str(latestShip); w.str(key); readHit(id,w); clearHit(id);
    NetSendReliable(MSG_GUARD_HIT,w.bytes.data(),w.bytes.size());
    return true;
}
const char* GuardsKeyOfGuard(int id);
static void preEnemyDamage(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || authority || !sharedShip || !mirrored.count((int)self->id)) return;
    const char* key=GuardsKeyOfGuard((int)self->id); if(!key || !*key) return;
    if(forwardHit((int)self->id,key)) api->RequestBypass();
}
static DWORD bothSince; static int bothShip;   // since when both players are aboard this ship
static int* decRunning; static int* decFollowed;   // diagnostics: what this game did for which ship   // what this game's guards concluded about the partner on the shared ship
static int swapCount; static int swappedGuard; static std::string swappedGuardState;
// The swap wraps every place a guard looks or acts: its vision check (EnemyCanSeePlayer, called straight from oEnemy's
// step), its state script and UpdateState. Calls nest, so the original player is restored when the outermost one ends.
static int swapDepth;
static void swapIn(CInstance* self) {
    if(swapDepth++>0) return;
    if(!self || enemyObject<0 || self->type!=(uint32_t)enemyObject) return;
    int id=(int)self->id;
    // A downed partner is not presented to guards: their body logic (carrying, finishing) must not act on the puppet.
    if(!authority || !sharedShip || !*puppet || CoopRemoteParent()!=sharedShip || CoopRemoteDown()) {
#ifdef HS_COOP_DEVTOOLS
        static DWORD lastWhy; if(authority && sharedShip && *puppet && GetTickCount()-lastWhy>2000) { lastWhy=GetTickCount(); CoopLog("Guards: partner not presented to guards (partner ship %d, shared %d, down %d)",CoopRemoteParent(),sharedShip,(int)CoopRemoteDown()); }
#endif
        return;
    }
    if((int)getReal(id,"Parent")!=sharedShip) return;
    // Only perception and combat: body handling, capture and other player-specific logic must never act on the puppet.
    static const char* const ENGAGE[]={"Patrolling","PatrolPath","Sentry","Idle","Errand","Investigating","Searching","Seeking","Responding","Confronting","Chasing","Distracted","Static"};
    std::string state=getString(id,"State"); bool engage=false;
    for(const char* s:ENGAGE) if(state==s){ engage=true; break; }
    if(!engage) return;
    double gf=getReal(id,"Fore"), gs=getReal(id,"Starboard");
    double dl=std::hypot(getReal(*player,"Fore")-gf,getReal(*player,"Starboard")-gs);
    double dr=std::hypot(getReal(*puppet,"Fore")-gf,getReal(*puppet,"Starboard")-gs);
    if((int)getReal(*player,"Parent")!=sharedShip) dl=1e18;   // the local player is elsewhere: only the partner is here
    // A guard keeps after the player it is already after; it turns to the other one only when that one is clearly
    // closer (well under two thirds of the distance, or right next to it), not each time the two are about level.
    auto cur=guardTarget.find(id);
    bool onPartner=cur!=guardTarget.end()?cur->second:(dr<dl);
    if(onPartner && (dl<dr*0.6 || (dl<120 && dr>200))) onPartner=false;
    else if(!onPartner && (dr<dl*0.6 || (dr<120 && dl>200))) onPartner=true;
    if(dl>=1e17) onPartner=true;
#ifdef HS_COOP_DEVTOOLS
    if(cur==guardTarget.end() || cur->second!=onPartner)
        CoopLog("Guards: %s is now after the %s (player at %.0f, partner at %.0f, state %s)",getString(id,"Name").c_str(),onPartner?"partner":"local player",dl>=1e17?-1.0:dl,dr,state.c_str());
#endif
    guardTarget[id]=onPartner;
    if(!onPartner) return;
    RValue* p=const_cast<RValue*>(GlobalVar(GLOBAL_PLAYER)); if(!p || GetRValueKind(*p)!=RValueKind::Real) return;
    savedPlayer=p->real; p->real=*puppet; swapped=true; swapCount++;
    swappedGuard=id; swappedGuardState=state;
    // The guard measures and paths to its target in room x/y (its hit test, its line to the player). The stand-in's x/y
    // otherwise come from another point of the frame, hundreds of units off on a moving ship: the guard chased, found
    // its path "blocked", panicked, and never hit the partner. Bring them up to date for this guard's step.
    { CInstance* pi=api->ResolveCInstance(*puppet); if(pi){ PlaceAboard(api,pi); } }
    // The puppet runs the Resting state (not a threat); for this guard it is the partner as they really are.
    savedState=getString(*puppet,"State"); savedThreat=getReal(*puppet,"SeenAsThreat");
    // Whether the guards see the partner as a threat is decided here, by these guards (it is what they set on the player
    // they watch); the partner's own game never decides it, its guards only follow. Kept for this ship.
    double threat=CoopRemoteSeenAsThreat(); if(!(threat==threat)) threat=0;
    if(partnerThreat>threat) threat=partnerThreat;
    setReal(*puppet,"SeenAsThreat",threat);
    if(CoopRemoteStateName()[0]) setString(*puppet,"State",CoopRemoteStateName());
}
static void swapOut() {
    if(swapDepth>0 && --swapDepth>0) return;
    if(!swapped) return;
    RValue* p=const_cast<RValue*>(GlobalVar(GLOBAL_PLAYER)); if(p) p->real=savedPlayer;
    std::string after=getString(swappedGuard,"State");
    if(after!=swappedGuardState && CoopVerbose()) CoopLog("Guards: %s went %s -> %s while watching the partner",getString(swappedGuard,"Name").c_str(),swappedGuardState.c_str(),after.c_str());
    if(*puppet && api->ResolveCInstance(*puppet)) {
        double seen=getReal(*puppet,"SeenAsThreat");
        if(seen==seen && seen>partnerThreat) { partnerThreat=seen; CoopLog("Guards: the guards now see the partner as a threat"); }
        setString(*puppet,"State",savedState); setReal(*puppet,"SeenAsThreat",savedThreat==savedThreat?savedThreat:0);
    }
    swapped=false;
}
// A body this player carries runs the game's own carried state here (its pose and position follow the carrier).
static bool carriedHere(int id){ return *player>0 && (int)nz(getReal(*player,"CarriedObject"))==id; }
static void preGuardUpdate(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void* user) {
    if(self && mirrored.count((int)self->id)) {
        if(user==(void*)1 && carriedHere((int)self->id)) return;
        if(user==(void*)1 && downMirrors.count((int)self->id)) return;   // UpdateState of a body: enters/keeps the Down state
        api->RequestBypass(); return;   // the authority decides
    }
    swapIn(self);
}
static void postGuardUpdate(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) { if(self && mirrored.count((int)self->id)) return; swapOut(); }
// Slow-mo a guard requests while targeting the puppet is meant for the partner, not this player.
// It happens in the partner's game instead: their own slow-mo, with its screen effect (the shared time policy then slows
// this game too).
static void preSetTimeScale(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!swapped) return;
    api->RequestBypass();
    if(argc<1 || argc>4 || !argv) return;
    for(int i=0;i<argc;i++) if(!argv[i] || GetRValueKind(*argv[i])!=RValueKind::Real) return;
    static DWORD last; if(GetTickCount()-last<100) return; last=GetTickCount();
    Writer w; w.put<uint8_t>((uint8_t)argc); for(int i=0;i<argc;i++) w.put<double>(argv[i]->real);
    NetSendUnreliable(MSG_SLOWMO,w.bytes.data(),w.bytes.size());
}
static void receiveSlowMo(Reader& r) {
    uint8_t n=r.get<uint8_t>(); if(!r.ok || n<1 || n>4) return;
    GmArgs a; for(int i=0;i<n;i++) a.AddReal(r.get<double>()); if(!r.ok) return;
    GmArgs f; f.AddReal(42); f.AddReal(0);   // oTime runs the game's time scale
    CInstance* me=*player>0?api->ResolveCInstance(*player):nullptr; if(!me) return;
    int timeObj=(int)Number(Engine(api,"instance_find",me,f.Count(),f.Build()));
    CInstance* t=api->ResolveCInstance(timeObj); if(!t) return;
    call("gml_Script_SetTimeScale",t,a);
    static DWORD lastLog; if(GetTickCount()-lastLog>3000) { lastLog=GetTickCount(); CoopLog("Guards: a guard in the partner's game spotted you: slow-mo here"); }
}
// Damage that lands on the puppet belongs to the remote player's real character.
static void prePuppetDamage(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && *player>0 && (int)self->id==*player && !pendingPlayerHits.empty()) {
        std::vector<uint8_t> hit=std::move(pendingPlayerHits.front()); pendingPlayerHits.erase(pendingPlayerHits.begin());
        Reader r(hit.data(),hit.size()); applyHit(*player,r);
        CoopLog("Guards: a guard in the partner's world hit you (mask %g)",getReal(*player,"DamageMask"));
        return;
    }
    if(!self || !*puppet || (int)self->id!=*puppet || !hitPending(*puppet)) return;
    // Explosions are replayed in the partner's game, where they hit the partner's real character.
    if(ObjectsExplosionNear(*puppet) || (nz(getReal(*puppet,"DamageSignal"))!=0 && nz(getReal(*puppet,"DamageMask"))==0)) { clearHit(*puppet); api->RequestBypass(); return; }
    // One swing is one hit. A melee swing connects on several frames, and the damage it leaves on the stand-in is
    // handled over the next few hundred ms; a real player takes only the first (it knocks them out, and the game's hit
    // test skips the unconscious). The stand-in stays up until the partner's own game reports them down, so until then
    // (and for 0.7 s after a forwarded hit, the round trip) further hits are dropped here.
    static DWORD lastForward;
    { DWORD now=GetTickCount(); if(CoopRemoteDown() || now-lastForward<700) { clearHit(*puppet); api->RequestBypass(); return; } lastForward=now; }
    Writer w; readHit(*puppet,w); clearHit(*puppet);
    NetSendReliable(MSG_PLAYER_HIT,w.bytes.data(),w.bytes.size());
    CoopLog("Guards: forwarded a hit on the partner's puppet");
    api->RequestBypass();
}

static DWORD sharedSince;
static void removeGuard(int id,const std::string& key) {
    CInstance* inst=api->ResolveCInstance(id); if(!inst) return; if(*player>0 && (int)nz(getReal(*player,"CarriedObject"))==id) return;
    CoopLog("Guards: %s (%s) is no longer aboard in the host's game; removed here",key.c_str(),getString(id,"Name").c_str());
    int legs=(int)getReal(id,"MyLegs");
    GmArgs r; r.AddReal(id); call("gml_Script_RemoveFromParentsContents",inst,r);
    if(legs>0 && api->ResolveCInstance(legs)) Engine(api,"instance_destroy",api->ResolveCInstance(legs),0,nullptr);
    Engine(api,"instance_destroy",inst,0,nullptr);
    mirrored.erase(id); downMirrors.erase(id); generationKeys.erase(id); ownState.erase(id);
}
static void watchDeliveries(); static void sendOwnKits(); static void applyPartnerKits(); static void sendKitChanges();
static bool followRebuilt; static std::string takeOverName; static DWORD takeOverAt; static int takeOverShip;
void GuardsFollowRebuiltShip() {
    followRebuilt=true;
    // The partner's game was following this one: it takes the guards over (its copy of the ship is the one played on).
    if(authority && sharedShip) { Writer w; w.str(getString(sharedShip,"Name")); NetSendReliable(MSG_GUARDS_TAKE_OVER,w.bytes.data(),w.bytes.size()); }
}
void GuardsTakeOver(const char* ship) { takeOverName=ship; takeOverAt=GetTickCount(); CoopLog("Guards: the partner rebuilt '%s'; this game takes its guards over",ship); }
void GuardsTick(CInstance*,bool inGameplay) {
    { static DWORD lastWatch; if(GetTickCount()-lastWatch>250) { lastWatch=GetTickCount(); watchDeliveries(); sendOwnKits(); sendKitChanges(); if(inGameplay) applyPartnerKits(); } }
    static DWORD lastSnapshot,lastFrame;
    double frameDt=lastFrame?(GetTickCount()-lastFrame)/1000.0:0.016; lastFrame=GetTickCount();
    if(frameDt<0.008) frameDt=0.016;
    // Which ship's guards are shared, and which game runs them: both aboard -> the host; only one player aboard -> that
    // player's game, and the other game's copy follows it, so the guards are where they should be when the second
    // player arrives (each game running its own copy while only one player was there made them drift apart).
    int ship=0; bool runHere=false;
    if(inGameplay && *player>0) {
        int parent=(int)getReal(*player,"Parent");
        CInstance* p=api->ResolveCInstance(parent);
        int myShip=p && p->type==(uint32_t)OBJ_SHIP?parent:0;
        // A knocked-out player being carried off by a guard has no ship as parent, but is still aboard: this game keeps
        // running the guards (handing them over mid-carry would leave the player held by a guard that no longer acts).
        static int lastShip;
        std::string myState=getString(*player,"State");
        if(!myShip && lastShip && api->ResolveCInstance(lastShip) && (myState=="Unconscious" || myState=="Carried" || getReal(*player,"Dead")==1)) myShip=lastShip;
        lastShip=myShip;
        int partnerShip=*puppet?CoopRemoteParent():0;
        // Both aboard: whichever game already runs this ship's guards keeps running them (handing them over mid-mission
        // reset them: their alerts, targets and what they made of each player). Only when neither does, the host does.
        static int runningShip, followedShip; decRunning=&runningShip; decFollowed=&followedShip;
        if(myShip && myShip==partnerShip) {
            ship=myShip;
            if(!takeOverName.empty() && GetTickCount()-takeOverAt<15000 && getString(myShip,"Name")==takeOverName) { runningShip=myShip; followedShip=0; takeOverName.clear(); takeOverShip=myShip; takeOverAt=GetTickCount(); }
            bool takingOver=takeOverShip==myShip && GetTickCount()-takeOverAt<6000;
            if(runningShip==myShip) runHere=true;
            else if(followedShip==myShip) runHere=false;
            // This player rebuilt the ship the partner plays on (practice restarted): the partner's game keeps its guards.
            else if(followRebuilt) { runHere=false; followRebuilt=false; CoopLog("Guards: '%s' was rebuilt here (practice restarted); following the partner's game",getString(myShip,"Name").c_str()); }
            else runHere=NetIsHost();
            DWORD now=GetTickCount();
            bool partnerRuns=latestShip==getString(myShip,"Name") && now-latestAt<1000;
            // Both claim it (the partner's snapshots keep arriving): the client gives way. Neither does (no snapshots for
            // a while): the host takes over.
            if(runHere && partnerRuns && !NetIsHost() && !takingOver) runHere=false;
            if(bothShip!=myShip) { bothShip=myShip; bothSince=now; }
            if(!runHere && NetIsHost() && now-latestAt>3000 && now-bothSince>3000) runHere=true;
        }
        else if(partnerShip && (!myShip || guardsAboard(myShip).empty())) { ship=partnerShip; runHere=false; }
        else if(myShip && *puppet) { ship=myShip; runHere=true; }
        if(!(myShip && myShip==partnerShip)) bothShip=0;   // the partner gets the same grace the next time both are aboard
        // What this game did for a ship (ran its guards, alone or shared; or followed the partner's).
        // (Running it alone counts only after a few seconds aboard: on arrival the partner's stand-in may not be there yet.)
        static int aboardShip; static DWORD aboardSince;
        if(myShip!=aboardShip) { aboardShip=myShip; aboardSince=GetTickCount(); }
        bool settled=GetTickCount()-aboardSince>3000 || (ship==myShip && runHere);
        if(myShip && settled && !(ship==myShip && !runHere)) { runningShip=myShip; if(followedShip==myShip) followedShip=0; }
        if(ship && !runHere) { followedShip=ship; if(runningShip==ship) runningShip=0; }
    }
    if(ship!=sharedShip || (ship && runHere!=authority)) {
        releaseMirrors(); mirrored.clear(); downMirrors.clear(); guardTarget.clear(); pinned.clear(); if(ship!=sharedShip) partnerThreat=0; sharedShip=ship; sharedSince=GetTickCount();
        authority=ship && runHere;
        snprintf(status,sizeof(status),ship?"guards on '%s': %s":"no shared guards",getString(ship,"Name").c_str(),authority?"this game simulates them":"following the partner");
        CoopLog("Guards: %s",status);
        if(*player>0) CoopLog("Guards: (local parent %d state '%s'; partner on %d; ran %d followed %d; partner snapshot %ums ago)",(int)getReal(*player,"Parent"),getString(*player,"State").c_str(),*puppet?CoopRemoteParent():0,decRunning?*decRunning:0,decFollowed?*decFollowed:0,(unsigned)(GetTickCount()-latestAt));
    }
    if(!ship) return;
    DWORD now=GetTickCount();
    // The game draws its alarm HUD only for the player's own mission; show the shared ship's alarm in the status line.
    {
        bool alarm=nz(getReal(ship,"Alarm"))!=0 || (!authority && latestAlarm!=0);
        snprintf(status,sizeof(status),"guards on '%s': %s%s",getString(ship,"Name").c_str(),authority?"this game simulates them":"following the partner",alarm?" | ALARM":"");
    }
    // The puppet might not have had its damage handler run this frame; forward anything pending.
    if(*puppet && hitPending(*puppet)) prePuppetDamage(nullptr,api->ResolveCInstance(*puppet),nullptr,nullptr,0,nullptr,nullptr);
    if(authority) {
        static DWORD lastSwapLog; if(CoopVerbose() && now-lastSwapLog>=10000){ lastSwapLog=now; CoopLog("Guards: %d guard updates treated the partner as the player in the last 10 s",swapCount); swapCount=0; }
        // Bodies the partner carries are carried by the partner's stand-in here, in the game's own carried state.
        for(auto it=bodies.begin();it!=bodies.end();) {
            int id=GuardsGuardByKey(it->first.c_str()); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
            if(now-it->second.at>400) {
                if(inst && *puppet && (int)nz(getReal(*puppet,"CarriedObject"))==id) { setReal(*puppet,"CarriedObject",49); CoopLog("Guards: partner put down %s",it->first.c_str()); }
                it=bodies.erase(it); continue;
            }
            if(inst && *puppet && api->ResolveCInstance(*puppet)) {
                if((int)nz(getReal(*puppet,"CarriedObject"))!=id) {
                    setReal(*puppet,"CarriedObject",id); setReal(id,"Carrier",*puppet);
                    GmArgs st; st.AddStr(api,"Carried"); call("gml_Script_ChangeStateTo",inst,st);
                    CoopLog("Guards: partner picked up %s",it->first.c_str());
                }
                setReal(id,"Fore",it->second.fore); setReal(id,"Starboard",it->second.starboard); setReal(id,"MyImageAngle",it->second.angle);
                PlaceAboard(api,inst); GmArgs none; call("gml_Script_SetImageAngle",inst,none);
            }
            ++it;
        }
        if(now-lastSnapshot>=SNAPSHOT_MS){ lastSnapshot=now; sendSnapshot(ship); }
        return;
    }
    animateMirrors=*player>0 && (int)nz(getReal(*player,"Parent"))==ship;
    // Follower: its guards never act on their own while the ship is shared, even before the first snapshot arrives.
    for(const Guard& g:guardsAboard(ship)) mirrored.insert(g.id);
    // Follower: apply the authority's latest snapshot; guards without fresh data keep their last state (AI stays off).
    if(latestShip!=getString(ship,"Name") || now-latestAt>SNAPSHOT_STALE_MS) return;
    if(animateMirrors && nz(getReal(ship,"Alarm"))!=latestAlarm) {
        setReal(ship,"Alarm",latestAlarm); CoopLog("Guards: ship alarm %s (from the host)",latestAlarm?"raised":"cleared");
        // The game that flies the ship makes it flee when its alarm goes off (the alarm script sets FleeSignal). When
        // that is this game and the alarm was raised in the other, the signal is set here, the way the script does.
        if(latestAlarm!=0 && !MissionsShipIsCopy(ship)) {
            GmArgs a; a.AddReal(ship);
            if(Number(call("gml_Script_ShipHasAlarm",api->ResolveCInstance(ship),a))>0.5) { setReal(ship,"FleeSignal",1); CoopLog("Guards: this game flies the ship: it flees"); }
        }
    }
    if(animateMirrors && nz(getReal(ship,"EscapeProtocol"))!=latestEscape) setReal(ship,"EscapeProtocol",latestEscape);
    {
        static DWORD lastCheck;
        if(animateMirrors && now-lastCheck>=1000) {   // only aboard: guards are created and removed on a ship the game has set up
            lastCheck=now;
            std::set<std::string> here;
            for(const Guard& g:guardsAboard(ship)) {
                here.insert(g.key);
                auto seen=lastSeen.find(g.key);
                // Gone on the host (left, destroyed) while snapshots keep arriving: remove this copy.
                // Only generated guards (keyed by position) are added or removed; arrivals are each game's own.
                if(g.key[0]!='p') continue;
                if(seen!=lastSeen.end() && now-seen->second>5000) removeGuard(g.id,g.key);   // reported before, gone now
            }
            for(auto& kv:lastSeen) if(kv.first[0]=='p' && !here.count(kv.first) && now-kv.second<1000 && now-requestedAt[kv.first]>5000) {
                requestedAt[kv.first]=now;
                Writer w; w.str(latestShip); w.str(kv.first); NetSendReliable(MSG_GUARD_REQUEST,w.bytes.data(),w.bytes.size());
            }
        }
    }
    if(carriedByMe && !(*player>0 && (int)nz(getReal(*player,"CarriedObject"))==carriedByMe)) { thrownBodies[carriedByMe]=now+2500; carriedByMe=0; }
    const bool frozen=TimePolicyLocalPaused() || TimePolicyPausedByPartner();
    for(const Guard& g:guardsAboard(ship)) {
        mirrored.insert(g.id);
        if(nz(getReal(g.id,"DistractionSignal"))!=0 || nz(getReal(g.id,"RespondingSignal"))!=0 || nz(getReal(g.id,"InvestigatingSignal"))!=0) {
            Writer w; w.str(latestShip); w.str(g.key);
            for(const char* n:ALERT_VARS) w.put<double>(nz(getReal(g.id,n)));
            // What the guard's distracted/responding states read along with the signals: the distracting item (by its
            // shared key; "@" = this player, the partner's stand-in there) and whether to search afterwards.
            int item=(int)nz(getReal(g.id,"DistractionItem"));
            std::string itemKey=item>0&&item==*player?"@":(item>0?ObjectsKeyOf(item):"");
            w.str(itemKey); w.put<double>(nz(getReal(g.id,"RespondingThenSearch")));
            NetSendReliable(MSG_GUARD_ALERT,w.bytes.data(),w.bytes.size());
            setReal(g.id,"DistractionSignal",0); setReal(g.id,"RespondingSignal",0); setReal(g.id,"InvestigatingSignal",0);
        }
        forwardHit(g.id,g.key);
        if(*player>0 && (int)nz(getReal(*player,"CarriedObject"))==g.id) {
            // This player carries the body: it follows them here, and the host's copy follows this one.
            static DWORD lastBody; if(now-lastBody>=50){ lastBody=now; Writer w; w.str(g.key); w.put<float>((float)getReal(g.id,"Fore")); w.put<float>((float)getReal(g.id,"Starboard")); w.put<float>((float)getReal(g.id,"MyImageAngle")); NetSendUnreliable(MSG_BODY,w.bytes.data(),w.bytes.size()); }
            carriedByMe=g.id;
            continue;
        }
        // Just put down or thrown by this player: it flies and lands here, and the host's copy follows for a moment
        // longer. (Snapped back to the host's copy as soon as the carrying ended, a throw went nowhere: in a
        // playtest the partner could not throw a body while the host's game ran the guards, 2026-10-05.)
        { auto th=thrownBodies.find(g.id);
          if(th!=thrownBodies.end()) {
              if(now<th->second) {
                  static DWORD lastFlight; if(now-lastFlight>=50){ lastFlight=now; Writer w; w.str(g.key); w.put<float>((float)getReal(g.id,"Fore")); w.put<float>((float)getReal(g.id,"Starboard")); w.put<float>((float)getReal(g.id,"MyImageAngle")); NetSendUnreliable(MSG_BODY,w.bytes.data(),w.bytes.size()); }
                  continue;
              }
              thrownBodies.erase(th);
          } }
        auto it=latest.find(g.key);
        // Time stands still (this player or the partner is paused or planning): the copies are left exactly where they
        // are, with no speed for their own step to move them by. Updating them then made them rubber-band (playtest,
        // 2026-10-05): each new state pulled them one way, something in the paused game pushed them back.
        if(frozen) {
            if(!downMirrors.count(g.id)) for(const char* n:{"ForeSpeed","StarboardSpeed","MeasuredSpeed","ForeSpeedDesired","StarboardSpeedDesired","CurrentSpeed"}) setReal(g.id,n,0);
            auto p=pinned.find(g.id); if(p!=pinned.end()) { p->second={getReal(g.id,"Fore"),getReal(g.id,"Starboard")}; }
            shown.erase(g.id);
            continue;
        }
        if(it!=latest.end()) mirrorGuard(g.id,it->second,(now-latestAt)/1000.0,frameDt);
    }
}
// Values the distracted/responding states read that co-op signals can leave unset in this game (the game itself sets
// them where it raises those signals); an unset one stops the game ("REAL argument is unset").
static void ensureStateValues(int id) {
    double item=getReal(id,"DistractionItem"); if(!(item==item)) setReal(id,"DistractionItem",-4);
    double then=getReal(id,"RespondingThenSearch"); if(!(then==then)) setReal(id,"RespondingThenSearch",0);
}
static void preEnemyState(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void* user) {
    if(self && !mirrored.count((int)self->id) && user==(void*)4) ensureStateValues((int)self->id);
    if(self && mirrored.count((int)self->id)) {
        if(user==(void*)3 && carriedHere((int)self->id)) return;
        if(user==(void*)2 && downMirrors.count((int)self->id)) return;   // EnemyStateDown of a body
        api->RequestBypass(); return;
    }
    swapIn(self);
}
// One-shot torso animations of the host's guards (a melee swing, a punch, a reload) are played on the partner's copies.
static void prePlayTorsoAnimOnce(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!self || !authority || !sharedShip || argc<2 || !argv || !argv[0] || !argv[1] || !isEnemy(self)) return;
    if((int)nz(getReal((int)self->id,"Parent"))!=sharedShip) return;
    if(GetRValueKind(*argv[0])!=RValueKind::String || GetRValueKind(*argv[1])!=RValueKind::String || !argv[0]->str || !argv[1]->str) return;
    auto k=generationKeys.find((int)self->id); std::string key;
    if(k!=generationKeys.end()) key=k->second;
    else for(const Guard& g:guardsAboard(sharedShip)) if(g.id==(int)self->id) { key=g.key; break; }
    if(key.empty()) return;
    Writer w; w.str(key); w.str(argv[0]->str->text?argv[0]->str->text:""); w.str(argv[1]->str->text?argv[1]->str->text:"");
    NetSendUnreliable(MSG_GUARD_ANIM,w.bytes.data(),w.bytes.size());
}
static void postMissionEvent(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void* which) {
    if(replayingEvent || !self || !authority || !sharedShip) return;
    int id=(int)self->id; std::string key;
    if(id==sharedShip) key="$";
    else { if((int)nz(getReal(id,"Parent"))!=sharedShip) return; key=GuardsKeyOfGuard(id); if(key.empty()) return; }
    Writer w; w.put<uint8_t>((uint8_t)(intptr_t)which); w.str(key);
    NetSendReliable(MSG_MISSION_EVENT,w.bytes.data(),w.bytes.size());
}
// ---- The ship's alarm is raised in one game ----
// TriggerShipsAlarm(ship) is the game's only way to raise it (a security camera seeing the player, a guard, a destroyed
// hull section). Raised in the game that mirrors the guards, it used to be lost: that game set its own alarm, which the
// next state from the guards' game cleared again, and the guards' game never heard of it (a camera that saw the
// partner did nothing). Now that game does not raise it itself: it asks, and the guards' game runs the script, which
// decides (the bridge must be manned, it may be raised already).
bool MissionsShipIsCopy(int ship);
static bool raisingAlarm;
// ---- Reinforcements ----
// After an alarm the ship's telepad (oTeleporterModule's step, TeleportInReinforcementsAtIntervals) counts down
// SecondsUntilReinforcements and brings in the next contractor with CreateMercenaryOnShip. Each game used to run that
// timer itself, so the two games got different contractors at different times. On a shared ship only the guards' game
// runs it: each arrival is sent with the arguments it was made with, and the other game makes the same one; the
// countdown is only drawn for the player's own mission, so it is not sent.
static bool inReinforcement;
static int telepadShip(CInstance* self) { return self?(int)nz(getReal((int)self->id,"Parent")):0; }
static void preReinforcements(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    int ship=telepadShip(self);
    if(!sharedShip || ship!=sharedShip) return;
    if(!authority) { api->RequestBypass(); return; }   // the guards' game decides
    inReinforcement=true;
}
static void postReinforcements(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { inReinforcement=false; }
static void postCreateMercenary(const char*,CInstance*,CInstance*,RValue* made,int argc,RValue** argv,void*) {
    if(!inReinforcement || !authority || !sharedShip || argc>8 || !argv) return;
    Writer w; w.str(getString(sharedShip,"Name")); w.put<uint8_t>((uint8_t)argc);
    std::string shown;
    for(int i=0;i<argc;i++) {
        const RValue* a=argv[i];
        if(a && GetRValueKind(*a)==RValueKind::String) { std::string t=a->str&&a->str->text?a->str->text:""; w.put<uint8_t>(1); w.str(t); shown+=" '"+t+"'"; }
        else { double d=a?Number(*a):0; bool isShip=(int)d==sharedShip && d>=100000; w.put<uint8_t>(isShip?2:0); w.put<double>(d); char b[32]; snprintf(b,sizeof(b)," %s",isShip?"$ship":std::to_string(d).c_str()); shown+=b; }
    }
    // Who arrived (each game's ConfigureMercenary rolls its own name and look): as for a guard sent on request.
    int id=made?(int)Number(*made):0;
    for(const char* n:SPAWN_TEXT) w.str(id>0?getString(id,n):"");
    for(const char* n:SPAWN_REAL) w.put<double>(id>0?getReal(id,n):NAN);
    NetSendReliable(MSG_REINFORCEMENT,w.bytes.data(),w.bytes.size());
    CoopLog("Guards: reinforcement arrived (%s; args%s); sent to the partner",id>0?getString(id,"Name").c_str():"?",shown.c_str());
}
static void receiveReinforcement(Reader& r) {
    std::string ship=r.str(); uint8_t argc=r.get<uint8_t>();
    if(!r.ok || argc>8) return;
    GmArgs a;
    for(int i=0;i<argc && r.ok;i++) {
        uint8_t kind=r.get<uint8_t>();
        if(kind==1) { std::string t=r.str(); a.AddStr(api,t.c_str()); }
        else { double d=r.get<double>(); a.AddReal(kind==2?sharedShip:d); }
    }
    std::vector<std::string> texts; for(size_t i=0;i<sizeof(SPAWN_TEXT)/sizeof(SPAWN_TEXT[0]);i++) texts.push_back(r.str());
    std::vector<double> reals; for(size_t i=0;i<sizeof(SPAWN_REAL)/sizeof(SPAWN_REAL[0]);i++) reals.push_back(r.get<double>());
    if(!r.ok || !sharedShip || authority || getString(sharedShip,"Name")!=ship) { CoopLog("Guards: a reinforcement for '%s' was not made here (not following that ship)",ship.c_str()); return; }
    // Made by this ship's own telepad, as the game does (the script reads its caller); the ship itself if none is found.
    CInstance* self=nullptr;
    { GmArgs n; n.AddStr(api,"oTeleporterModule"); CInstance* me=api->ResolveCInstance(sharedShip);
      double obj=Number(Engine(api,"asset_get_index",me,n.Count(),n.Build()));
      GmArgs c; c.AddReal(obj); int count=(int)Number(Engine(api,"instance_number",me,c.Count(),c.Build()));
      for(int i=0;i<count && !self;i++) { GmArgs f; f.AddReal(obj); f.AddReal(i); int id=(int)Number(Engine(api,"instance_find",me,f.Count(),f.Build()));
        if(id>0 && (int)nz(getReal(id,"Parent"))==sharedShip) self=api->ResolveCInstance(id); } }
    if(!self) self=api->ResolveCInstance(sharedShip);
    RValue made=call("gml_Script_CreateMercenaryOnShip",self,a);
    int id=(int)Number(made);
    if(id>0 && api->ResolveCInstance(id)) {
        for(size_t i=0;i<texts.size();i++) if(!texts[i].empty()) setString(id,SPAWN_TEXT[i],texts[i]);
        for(size_t i=0;i<reals.size();i++) if(reals[i]==reals[i]) setReal(id,SPAWN_REAL[i],reals[i]);
        std::string skin=getString(id,"Skin"); if(!skin.empty()) { GmArgs sk; sk.AddStr(api,skin.c_str()); call("gml_Script_SetSkin",api->ResolveCInstance(id),sk); }
    }
    CoopLog("Guards: reinforcement made as in the guards' game: %s",id>0?getString(id,"Name").c_str():"(no instance returned)");
}

static void preTriggerShipsAlarm(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(raisingAlarm || authority || !sharedShip || argc<1 || !argv || !argv[0]) return;
    if((int)Number(*argv[0])!=sharedShip) return;
    api->RequestBypass();
    static DWORD lastAsk; DWORD now=GetTickCount(); if(now-lastAsk<500) return; lastAsk=now;
    Writer w; w.str(getString(sharedShip,"Name"));
    NetSendReliable(MSG_ALARM_REQUEST,w.bytes.data(),w.bytes.size());
    CoopLog("Guards: alarm on '%s' asked of the partner's game (it runs the guards)",getString(sharedShip,"Name").c_str());
}
static void receiveAlarmRequest(Reader& r) {
    std::string name=r.str();
    if(!r.ok || !authority || !sharedShip || getString(sharedShip,"Name")!=name) return;
    CInstance* ship=api->ResolveCInstance(sharedShip); if(!ship) return;
    double before=nz(getReal(sharedShip,"Alarm"));
    GmArgs a; a.AddReal(sharedShip);
    raisingAlarm=true; call("gml_Script_TriggerShipsAlarm",ship,a); raisingAlarm=false;
    CoopLog("Guards: alarm on '%s' raised at the partner's request (%s)",name.c_str(),before!=0?"it was on already":nz(getReal(sharedShip,"Alarm"))!=0?"now on":"the ship did not raise it");
}
// A guard going down drops random loot. The game that runs the guards decides it and sends the dropped items to the
// partner (as any dropped item); the other game's copy of the guard drops nothing itself. (Seeding both games to roll
// the same drop did not hold: the copy often never rolled, or rolled something else.)
static int lootCountBefore=-1;
static void preEnemyDropLoot(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    lootCountBefore=-1;
    if(!self || !sharedShip || (int)nz(getReal((int)self->id,"Parent"))!=sharedShip) return;
    if(!authority) { setReal((int)self->id,"HasLoot",0); api->RequestBypass(); return; }
    lootCountBefore=(int)nz(getReal(sharedShip,"ContentsCount"));
}
static void postEnemyDropLoot(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(lootCountBefore<0 || !self || !sharedShip || !authority) return;
    int count=(int)nz(getReal(sharedShip,"ContentsCount"));
    for(int i=lootCountBefore;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(sharedShip,"Contents",i,&item);
        int id=(int)Number(item); if(id>=100000 && api->ResolveCInstance(id)) ObjectsShareNewItem(id);
    }
    if(count>lootCountBefore) CoopLog("Guards: %s dropped %d item(s); sent to the partner",getString((int)self->id,"Name").c_str(),count-lootCountBefore);
    lootCountBefore=-1;
}
// A guard's melee hit test (CanHitPeople, a with-loop over people) measures the distance in room x/y, which the game
// derives from Fore/Starboard and the ship's position in UpdatePosition. The stand-in's x/y were refreshed at another
// point of the frame than the guard's, while the ship moves hundreds of units per frame, so a guard next to the partner
// saw them far away and never hit them. Refresh the stand-in right before each hit test of this game's guards.
static void preCanHitPeople(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || !authority || !sharedShip || !*puppet || !isEnemy(self)) return;
    CInstance* p=api->ResolveCInstance(*puppet); if(!p) return;
    PlaceAboard(api,p);
}
#ifdef HS_COOP_DEVTOOLS   // developer builds only (build.py --dev): the private developer extension, not in this repository
// Diagnostics: why a guard on the shared ship panics (it gave up on a player instead of fighting).
static void preEnemyChangeState(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!self || !authority || !sharedShip || !isEnemy(self) || argc<1 || !argv || !argv[0] || GetRValueKind(*argv[0])!=RValueKind::String || !argv[0]->str || !argv[0]->str->text) return;
    if(strcmp(argv[0]->str->text,"Panicking")) return;
    int id=(int)self->id; const RValue* pl=GlobalVar(GLOBAL_PLAYER); int target=pl?(int)Number(*pl):0;
    CoopLog("Guards: %s panics (from %s) target=%s teleported=%g pathBlocked=%g targetTeleported=%g targetGhost=%g targetParent=%g",
        getString(id,"Name").c_str(),getString(id,"State").c_str(),target==*puppet?"stand-in":target==*player?"player":"other",
        getReal(id,"Teleported"),getReal(id,"ChasePathBlocked"),getReal(target,"Teleported"),getReal(target,"IsGhost"),getReal(target,"Parent"));
}
#endif
// ---- Guard kits from the game that generated the ship ----
// Each game rolls a guard's kit itself (GiveGuardEquipment), and a mission's own rules arm its ship's guards only in
// the owner's game (the mission is not in the partner's game): the partner's copies carried other things (a wrench
// where the owner's guard had a Quiet Gun, no gadget where it had one). The game that generated the ship as its own
// sends each guard's whole inventory (every item's object and variables, and the selected slot) once the guard has its
// cross-game key; the partner's copy of that guard drops what it rolled and gets the same items.
bool MissionsShipIsCopy(int ship);
static std::vector<std::pair<int,DWORD>> ownKits;   // guards whose kit this game rolled for its own ship
static std::set<std::string> kitsApplied;            // ship/key already re-kitted here
static void postGiveGuardEquipment(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || !isEnemy(self)) return;
    int ship=(int)nz(getReal((int)self->id,"Parent"));
    if(ship>0 && !MissionsShipIsCopy(ship)) ownKits.push_back({(int)self->id,GetTickCount()});
}
static std::vector<int> inventoryOf(int guard) {
    std::vector<int> items; int n=(int)nz(getReal(guard,"InventoryCount"));
    for(int i=0;i<n;i++) { RValue v{}; v.type=5; api->GetVarByName(guard,"Inventory",i,&v); int item=(int)Number(v); if(item>0 && api->ResolveCInstance(item)) items.push_back(item); }
    return items;
}
// The guards' game's guard dropped an item (its drop arrives as an item on the floor): the copy here, which never drops
// anything itself, gives up its own copy of that item, through the game's RemoveFromOwnersInventory.
static std::map<int,std::string> kitBaseline; static std::string kitSignature(int guard);
void GuardsCopyGivesUp(const char* guardKey,const char* objectName) {
    if(authority || !sharedShip) return;
    int guard=GuardsGuardByKey(guardKey); if(guard<=0 || !mirrored.count(guard)) return;
    // Only a standing guard's kit is seen (and used); taking an item from a dead or downed copy stopped the game (a
    // practice ship rebuilt with that guard already dead).
    if(getReal(guard,"Dead")==1 || getReal(guard,"Conscious")==0) return;
    for(int item:inventoryOf(guard)) {
        CInstance* inst=api->ResolveCInstance(item);
        if(!inst || inst->GetObjectName(api)!=objectName) continue;
        // Taken out of the guard's Inventory list directly: the game's RemoveFromOwnersInventory stopped the game on a
        // guard just rebuilt with the ship (practice restart).
        std::vector<int> keep; for(int other:inventoryOf(guard)) if(other!=item) keep.push_back(other);
        for(size_t i=0;i<keep.size();i++) { RValue v{}; v.type=0; v.real=keep[i]; api->SetVarByName(guard,"Inventory",(int)i,&v); }
        setReal(guard,"InventoryCount",(double)keep.size());
        if((int)getReal(guard,"CurrentlySelectedItem")==item) { setReal(guard,"CurrentlySelectedItem",keep.empty()?-4:keep[0]); setReal(guard,"InventorySelection",0); }
        else { double sel=getReal(guard,"InventorySelection"); if(sel==sel && sel>=(double)keep.size()) setReal(guard,"InventorySelection",keep.empty()?0:(double)keep.size()-1); }
        setReal(item,"Owner",-4);
        Engine(api,"instance_destroy",inst,0,nullptr);
        CoopLog("Guards: %s dropped its %s in the guards' game; the copy here gave it up",guardKey,objectName);
        break;
    }
    kitBaseline[guard]=kitSignature(guard);
}
static std::string kitSignature(int guard) { std::string sig; for(int item:inventoryOf(guard)) sig+=getString(item,"Name")+"|"; return sig; }
// kitBaseline: guard -> inventory last sent or applied here (runner changes are sent)
static void sendKit(int guard,int ship,const std::string& key,uint8_t kind) {
    Writer w; w.str(getString(ship,"Name")); w.str(key); w.put<uint8_t>(kind);
    if(kind==2) w.str(kitSignature(guard));   // a check: the following game re-kits only if it shows something else
    w.put<double>(nz(getReal(guard,"InventorySelection")));
    std::vector<int> items=inventoryOf(guard);
    w.put<uint8_t>((uint8_t)items.size());
    for(int item:items) { w.str(api->ResolveCInstance(item)->GetObjectName(api)); SnapshotCapture(item,w); }
    NetSendReliable(MSG_GUARD_KIT,w.bytes.data(),w.bytes.size());
    kitBaseline[guard]=kitSignature(guard);
}
// The game that runs a shared ship's guards sends any later change of a guard's inventory (its AI picks up or drops
// items, e.g. a Sovereign guard fetching a Longblade): the following game shows the same items in hand.
static void sendKitChanges() {
    if(!authority || !sharedShip) return;
    for(const Guard& g:guardsAboard(sharedShip)) {
        if(nz(getReal(g.id,"Dead"))!=0 || nz(getReal(g.id,"Conscious"))==0) continue;   // bodies drop their loot (synced as drops)
        std::string sig=kitSignature(g.id); auto b=kitBaseline.find(g.id);
        // First seen while sharing: what the guard carries now goes once (it may have changed before the ship was
        // shared, e.g. a guard that fetched a weapon while only the owner was aboard; such changes were never sent).
        // Only the game that generated the ship sends it: on a copy the kits came from that game (kind 0) and may not be
        // applied yet, so sending the copy's own would race them.
        if(b==kitBaseline.end()) { if(!MissionsShipIsCopy(sharedShip)) sendKit(g.id,sharedShip,g.key,2); else kitBaseline[g.id]=sig; continue; }
        if(b->second==sig) continue;
        sendKit(g.id,sharedShip,g.key,1);
        CoopLog("Guards: %s now carries %s; sent to the partner",g.key.c_str(),sig.c_str());
    }
}
static void sendOwnKits() {
    for(auto it=ownKits.begin();it!=ownKits.end();) {
        int guard=it->first; auto k=generationKeys.find(guard);
        if(GetTickCount()-it->second>30000 || !api->ResolveCInstance(guard)) { it=ownKits.erase(it); continue; }
        if(k==generationKeys.end()) { ++it; continue; }
        int ship=(int)nz(getReal(guard,"Parent"));
        if(MissionsShipIsCopy(ship)) { it=ownKits.erase(it); continue; }
        sendKit(guard,ship,k->second,0);
        it=ownKits.erase(it);
    }
}
static void applyPartnerKits() {
    if(partnerKits.empty()) return;
    CInstance* me=*player>0?api->ResolveCInstance(*player):nullptr; if(!me) return;
    std::map<std::string,int> copies;
    GmArgs n; n.AddReal(OBJ_SHIP); int count=(int)Number(Engine(api,"instance_number",me,n.Count(),n.Build()));
    for(int i=0;i<count;i++) { GmArgs f; f.AddReal(OBJ_SHIP); f.AddReal(i); int s=(int)Number(Engine(api,"instance_find",me,f.Count(),f.Build())); if(s>0) copies[getString(s,"Name")]=s; }
    for(auto it=partnerKits.begin();it!=partnerKits.end();) {
        std::string done=it->ship+"/"+it->key;
        uint8_t kind=it->data.empty()?0:it->data[0];
        // Initial kits (from the game that generated the ship) go to this game's copy once; later changes come from the
        // game running the guards and apply while this game follows that ship.
        if((kind==0 && kitsApplied.count(done)) || GetTickCount()-it->at>120000) { it=partnerKits.erase(it); continue; }
        auto sh=copies.find(it->ship); int guard=0;
        if(sh!=copies.end() && kind==0 && !MissionsShipIsCopy(sh->second)) { it=partnerKits.erase(it); continue; }
        if(sh!=copies.end() && kind>=1 && (sh->second!=sharedShip || authority)) { it=partnerKits.erase(it); continue; }
        // Items are only made on the ship this player stands on: a copy that is still being set up (or far away) has no
        // parent yet, and making contents on it raised "Add to parents contents, but I don't have a parent. I'm a oShip".
        if(sh!=copies.end() && (int)nz(getReal(*player,"Parent"))!=sh->second) { ++it; continue; }
        if(sh!=copies.end()) for(const Guard& g:guardsAboard(sh->second)) if(g.key==it->key) { guard=g.id; break; }
        CInstance* inst=guard?api->ResolveCInstance(guard):nullptr;
        if(!inst) { ++it; continue; }
        Reader r(it->data.data(),it->data.size()); r.get<uint8_t>();
        if(kind==2) {
            std::string theirs=r.str(), mine=kitSignature(guard);
            if(theirs==mine) { kitBaseline[guard]=mine; it=partnerKits.erase(it); continue; }
            CoopLog("Guards: %s carries %s here but %s in the guards' game",it->key.c_str(),mine.c_str(),theirs.c_str());
        }
        CoopLog("Guards: re-kitting %s on '%s' (ship %d) as in the partner's game",it->key.c_str(),it->ship.c_str(),sh->second);
        // What this copy rolled goes (destroyed off the ship), then the generating game's items are made and given.
        int had=(int)nz(getReal(guard,"InventoryCount"));
        for(int i=0;i<had;i++) { RValue v{}; v.type=5; api->GetVarByName(guard,"Inventory",i,&v); int item=(int)Number(v); CInstance* ii=item>0?api->ResolveCInstance(item):nullptr; if(ii) Engine(api,"instance_destroy",ii,0,nullptr); }
        setReal(guard,"InventoryCount",0);
        double selection=r.get<double>(); uint8_t items=r.get<uint8_t>();
        int ship=(int)nz(getReal(guard,"Parent")); std::string names;
        for(uint8_t i=0;i<items && r.ok;i++) {
            std::string obj=r.str();
            GmArgs idx; idx.AddStr(api,obj.c_str()); double asset=Number(Engine(api,"asset_get_index",me,idx.Count(),idx.Build()));
            GmArgs mk; mk.AddReal(asset); mk.AddReal(guard);
            int item=asset>=0?(int)Number(call("gml_Script_CreateContentsOnTopOf",api->ResolveCInstance(ship),mk)):0;
            if(!item || !api->ResolveCInstance(item)) { CoopLog("Guards: could not make %s for %s",obj.c_str(),it->key.c_str()); break; }
            double counts[3]; static const char* const LOCAL_COUNTS[]={"InventoryCount","ContentsCount","ItemCount"};
            for(int c=0;c<3;c++) counts[c]=getReal(item,LOCAL_COUNTS[c]);
            SnapshotApply(item,r,true);
            for(int c=0;c<3;c++) if(counts[c]==counts[c]) setReal(item,LOCAL_COUNTS[c],counts[c]);
            GmArgs give; give.AddReal(item); give.AddReal(guard); call("gml_Script_AddToInventory",inst,give);
            names+=(names.empty()?"":", ")+getString(item,"Name");
        }
        setReal(guard,"InventorySelection",selection);
        if(kind==0) kitsApplied.insert(done);
        kitBaseline[guard]=kitSignature(guard);
        CoopLog("Guards: %s on '%s' carries what it carries in the partner's game: %s",it->key.c_str(),it->ship.c_str(),names.c_str());
        it=partnerKits.erase(it);
    }
}
// Becoming a witness (seen a player: the "no living witnesses" clause) is the guards' game's decision; a copy that marks
// itself here differed from it until the next full state (guard_variables_match: Witness 1 here, 0 there).
bool GuardsIsMirroredGuard(int id) { return !authority && sharedShip && mirrored.count(id)!=0; }
static void preBecomeWitness(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && !authority && mirrored.count((int)self->id)) api->RequestBypass();
}
void GuardsInit(const HS_ModApi* a,int* localPlayer,int* puppetId) {
    api=a; player=localPlayer; puppet=puppetId;
    api->SubscribeHook("gml_Script_UpdateState",preGuardUpdate,(void*)1);
    api->SubscribeHookPost("gml_Script_UpdateState",postGuardUpdate,nullptr);
    api->SubscribeHook("gml_Script_PlayerHandleDamageSignal",prePuppetDamage,nullptr);
    api->SubscribeHook("gml_Script_EnemyHandleDamageSignal",preEnemyDamage,nullptr);
    api->SubscribeHook("gml_Script_CanHitPeople",preCanHitPeople,nullptr);
    api->SubscribeHookPost("gml_Script_GiveGuardEquipment",postGiveGuardEquipment,nullptr);
#ifdef HS_COOP_DEVTOOLS
    api->SubscribeHook("gml_Script_ChangeStateTo",preEnemyChangeState,nullptr);
#endif
    api->SubscribeHook("gml_Script_SetTimeScale",preSetTimeScale,nullptr);
    api->SubscribeHookPost("gml_Script_AddGuardsToShip",postGenerationStep,nullptr);
    api->SubscribeHookPost("gml_Script_AddObjectiveToHighestSecurityModule",postGenerationStep,nullptr);
    api->SubscribeHookPost("gml_Script_FinishGeneratingShip",postGenerationStep,nullptr);
    for(const char* n:ENEMY_STATES) {
        void* user=!strcmp(n,"gml_Script_EnemyStateDown")?(void*)2:!strcmp(n,"gml_Script_EnemyStateCarried")?(void*)3:
            (!strcmp(n,"gml_Script_EnemyStateDistracted") || !strcmp(n,"gml_Script_EnemyStateResponding") || !strcmp(n,"gml_Script_EnemyStateInvestigating"))?(void*)4:nullptr;
        api->SubscribeHook(n,preEnemyState,user); api->SubscribeHookPost(n,postGuardUpdate,nullptr);
    }
    api->SubscribeHook("gml_Script_TriggerShipsAlarm",preTriggerShipsAlarm,nullptr);
    api->SubscribeHook("gml_Script_EnemyBecomeWitness",preBecomeWitness,nullptr);
    api->SubscribeHook("gml_Script_TeleportInReinforcementsAtIntervals",preReinforcements,nullptr);
    api->SubscribeHookPost("gml_Script_TeleportInReinforcementsAtIntervals",postReinforcements,nullptr);
    api->SubscribeHookPost("gml_Script_CreateMercenaryOnShip",postCreateMercenary,nullptr);
    api->SubscribeHook("gml_Script_EnemyDropLoot",preEnemyDropLoot,nullptr);
    api->SubscribeHookPost("gml_Script_EnemyDropLoot",postEnemyDropLoot,nullptr);
    api->SubscribeHook("gml_Script_PlayTorsoAnimOnce",prePlayTorsoAnimOnce,nullptr);
    for(size_t i=0;i<sizeof(MISSION_EVENTS)/sizeof(MISSION_EVENTS[0]);i++) api->SubscribeHookPost(MISSION_EVENTS[i],postMissionEvent,(void*)(intptr_t)i);
    api->SubscribeHook("gml_Script_EnemyCanSeePlayer",preGuardUpdate,nullptr);
    // (Hooking the movement scripts themselves stopped the game on arrival; the animation step comes after the movement.)
    api->SubscribeHook("gml_Script_ProgressSpineLegAndTorsoFrames",prePersonAnimates,nullptr);
    api->SubscribeHookPost("gml_Script_ProgressSpineLegAndTorsoFrames",postPersonMoved,nullptr);
    api->SubscribeHookPost("gml_Script_EnemyCanSeePlayer",postGuardUpdate,nullptr);
}
const char* GuardsStatus() { return status; }
#ifdef HS_COOP_DEVTOOLS   // developer builds only (build.py --dev): the private developer extension, not in this repository
std::string GuardsSnapshotJson(int ship) {
    std::string o="[";
    for(const Guard& g:guardsAboard(ship)) {
        int id=g.id; auto t=guardTarget.find(id);
        // The guard's weapon as the game's own AI picks it (EnemysWeapon); none means it chases without attacking.
        std::string weapon;
        { CInstance* inst=api->ResolveCInstance(id); GmArgs a; a.AddReal(id); RValue w=inst?call("gml_Script_EnemysWeapon",inst,a):RValue{};
          int wid=(int)nz(Number(w)); CInstance* wi=wid>0?api->ResolveCInstance(wid):nullptr;
          if(wi) { weapon=getString(wid,"Name"); if(weapon.empty()) weapon=wi->GetObjectName(api); } }
        // The torso (spine) animation actually playing, as the game's draw uses it.
        std::string torso;
        { CInstance* inst=api->ResolveCInstance(id); if(inst) { RValue t=Engine(api,"skeleton_animation_get",inst,0,nullptr); if(GetRValueKind(t)==RValueKind::String && t.str && t.str->text) torso=t.str->text; } }
        char b[900];
        snprintf(b,sizeof(b),"%s{\"key\":\"%s\",\"id\":%d,\"name\":\"%s\",\"state\":\"%s\",\"fore\":%.1f,\"stb\":%.1f,\"angle\":%.1f,\"conscious\":%g,\"dead\":%g,"
            "\"teleported\":%g,\"pathBlocked\":%g,\"meleeReady\":%.2f,\"animation\":\"%s\",\"target\":\"%s\",\"mirrored\":%s,\"weapon\":\"%s\",\"torso\":\"%s\",\"legFrames\":%g}",
            o.size()>1?",":"",JsonEscape(g.key).c_str(),id,JsonEscape(getString(id,"Name")).c_str(),JsonEscape(getString(id,"State")).c_str(),
            nz(getReal(id,"Fore")),nz(getReal(id,"Starboard")),nz(getReal(id,"MyImageAngle")),nz(getReal(id,"Conscious")),nz(getReal(id,"Dead")),
            nz(getReal(id,"Teleported")),nz(getReal(id,"ChasePathBlocked")),nz(getReal(id,"SecondsUntilMeleeAttackReady")),JsonEscape(getString(id,"Animation")).c_str(),
            t==guardTarget.end()?"":t->second?"partner":"player",mirrored.count(id)?"true":"false",JsonEscape(weapon).c_str(),JsonEscape(torso).c_str(),nz(getReal(id,"LegFrameCount")));
        o+=b;
    }
    return o+"]";
}
#endif
void GuardsDebugDump(int ship) {
    for(const Guard& g:guardsAboard(ship))
        CoopLog("guard %s name=%s id=%d fore=%.1f stb=%.1f angle=%.1f state=%s dead=%g conscious=%g",g.key.c_str(),getString(g.id,"Name").c_str(),g.id,getReal(g.id,"Fore"),getReal(g.id,"Starboard"),getReal(g.id,"MyImageAngle"),getString(g.id,"State").c_str(),getReal(g.id,"Dead"),getReal(g.id,"Conscious"));
}

bool GuardsTargetingPuppet() { return swapped; }
bool GuardsIsAuthority() { return sharedShip && authority; }
int GuardsSharedShip() { return sharedShip; }
const char* GuardsKeyOfGuard(int id) {
    if(!sharedShip || (int)nz(getReal(id,"Parent"))!=sharedShip) return "";   // a guard on another ship may share a position key
    auto it=generationKeys.find(id); return it==generationKeys.end()?"":it->second.c_str();
}
// ---- Mission targets delivered by the partner ----
// Capture and rescue missions complete when the target is in the mission owner's pod (EnemyCheckObjectiveDelivered on
// the target checks its parent). A person this game's player brings into their own pod is reported; if it is the
// owner's mission target, the owner's game completes the mission the way that check does.
static std::set<int> reportedDelivered;
static void watchDeliveries() {
    const RValue* g=GlobalVar(7); int pod=g?(int)Number(*g):0;
    if(pod<=0 || !api->ResolveCInstance(pod)) return;
    for(auto& kv:generationKeys) {
        int id=kv.first; if(!api->ResolveCInstance(id) || (int)nz(getReal(id,"Parent"))!=pod) continue;
        if(!reportedDelivered.insert(id).second) continue;
        int ship=(int)nz(getReal(id,"OriginalShip"));
        Writer w; w.str(api->ResolveCInstance(ship)?getString(ship,"Name"):""); w.str(kv.second);
        NetSendReliable(MSG_TARGET_DELIVERED,w.bytes.data(),w.bytes.size());
        CoopLog("Guards: %s (%s) is in your pod; the partner's game is told",kv.second.c_str(),getString(id,"Name").c_str());
    }
}
// Test: report the guard with this generation key as delivered to this player's pod.
const char* GuardsTestDeliver(const char* key) {
    for(auto& kv:generationKeys) if(kv.second==key && api->ResolveCInstance(kv.first)) {
        int ship=(int)nz(getReal(kv.first,"OriginalShip"));
        Writer w; w.str(api->ResolveCInstance(ship)?getString(ship,"Name"):""); w.str(kv.second);
        NetSendReliable(MSG_TARGET_DELIVERED,w.bytes.data(),w.bytes.size());
        return "delivery sent";
    }
    return "no guard with that key";
}
static void receiveDelivered(Reader& r) {
    std::string shipName=r.str(), key=r.str(); if(!r.ok) return;
    // Only this game's own mission objective is moved (other people stay where they are here).
    const RValue* o=GlobalVar(6); int objective=o?(int)Number(*o):0;
    int id=0; for(auto& kv:generationKeys) if(kv.second==key && api->ResolveCInstance(kv.first)) { id=kv.first; break; }
    if(!id || id!=objective) { CoopLog("Guards: partner brought %s into their pod (not this game's mission target)",key.c_str()); return; }
    // Completed the way the game does when a target reaches the owner's pod: CompleteMission(mission, message).
    const RValue* m=GlobalVar(2); int mission=m?(int)Number(*m):0;
    if(!api->ResolveCInstance(mission)) return;
    std::string type=getString(mission,"Type");
    std::string text=getString(id,"Name")+(type=="Rescue"?" rescued by your partner.":" captured by your partner.");
    GmArgs c; c.AddReal(mission); c.AddStr(api,text.c_str());
    call("gml_Script_CompleteMission",api->ResolveCInstance(*player),c);
    CoopLog("Guards: your partner brought your target %s to their pod; mission completed",getString(id,"Name").c_str());
}
int GuardsGuardByKey(const char* key) {
    if(!sharedShip || !key) return 0;
    for(const Guard& g:guardsAboard(sharedShip)) if(g.key==key) return g.id;
    return 0;
}
