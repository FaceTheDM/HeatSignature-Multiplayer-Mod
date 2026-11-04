// Ship objects both players share: doors, loot boxes, money, items, fuel barrels, windows, airlocks, terminals, sentries
// and the mission objective. Objects are keyed by where generation placed them (identical in both games); items dropped
// later get keys too (by the guard that dropped them, or chosen by the player who dropped them).
//  - State changes (box opened, window shot, door hacked, ...) and pickups are sent both ways and applied the way the
//    game itself would.
//  - Doors: each game reports the doors it holds open for its own reasons (its player, its guards); the other game keeps
//    those doors signalled open. Airlock doors follow each game's own docked pod and are left alone.
//  - Items a player drops are recreated in the other game.
//  - Sentries: the host's sentries decide and also target the partner; the follower replays their shots harmlessly.
//  - Explosions from unkeyed sources are replayed at the same spot; guard sounds are forwarded from the host.
#include "Objects.h"
#include "Console.h"
#include "Place.h"
#include "GmArgs.h"
#include "Guards.h"
#include "Net.h"
#include "Snapshot.h"
#include "Visuals.h"
#include "Wire.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static const HS_ModApi* api;
static int* player;
static int* puppet;
static const DWORD SCAN_MS=100, DOOR_REPORT_MS=150, DOOR_FRESH_MS=600, HOLD_MS=1500, EXPLOSION_WINDOW_MS=700;
static const int GLOBAL_OBJECTIVE=6;
static const double OBJ_PLAYER=137;
int CoopRemoteParent();
bool CoopRemoteDown();

static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static void setReal(int id,const char* n,double d){ RValue v{}; v.type=0; v.real=d; api->SetVarByName(id,n,0x80000000,&v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static RValue call(const char* script,CInstance* self,GmArgs& a){ RValue r{}; r.type=5; api->CallScript(script,self,self,&r,a.Count(),a.Build()); return r; }
static bool live(int id){ return id>0 && api->ResolveCInstance(id); }
static std::string objectName(int id){ CInstance* i=id>0?api->ResolveCInstance(id):nullptr; return i?i->GetObjectName(api):""; }
static bool isPlayerObject(int id){ CInstance* i=id>0?api->ResolveCInstance(id):nullptr; return i && i->type==(uint32_t)OBJ_PLAYER; }

// ---- Keys ----
static std::map<int,std::string> keyOf;                   // instance -> key
static std::map<int,std::map<std::string,int>> byShip;    // ship instance -> key -> instance
static const std::set<std::string> SKIP={"oPlayer","oEnemy","oErrandBuddyPosition","oUseHandle","oBloodSplatter","oParticleBurst","oLegs"};
static void addKey(int ship,int id,const std::string& key){ keyOf[id]=key; byShip[ship][key]=id; }
static void recordKeys(int ship) {
    if(getReal(ship,"Station")==1) return;   // station shops and stock are per save
    std::map<std::string,int> used;
    int count=(int)getReal(ship,"ContentsCount");
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); if(!live(id) || keyOf.count(id)) continue;
        std::string name=objectName(id); if(name.empty() || SKIP.count(name)) continue;
        double fore=getReal(id,"Fore"), starboard=getReal(id,"Starboard");
        if(fore==0 && starboard==0) continue;   // held in an inventory, not placed on the ship
        char key[96]; snprintf(key,sizeof(key),"%s@%.0f,%.0f",name.c_str(),std::floor(fore+0.5),std::floor(starboard+0.5));
        std::string k=key; int n=used[k]++; if(n) k+="#"+std::to_string(n);
        addKey(ship,id,k);
    }
}
static void postFinishGeneratingShip(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) { if(self) recordKeys((int)self->id); }
const char* ObjectsKeyOf(int id){ auto it=keyOf.find(id); return it==keyOf.end()?"":it->second.c_str(); }
int ObjectsIdOfKey(int ship,const char* key) {
    auto sit=byShip.find(ship); if(sit==byShip.end()) return 0;
    auto it=sit->second.find(key); return it!=sit->second.end() && live(it->second)?it->second:0;
}

// ---- State exchanged per object (variables an object does not have read as NaN and are left alone) ----
static bool isArmable(const std::string& kind){ return kind=="oGrenade" || kind=="oAcidTrap" || kind=="oGlitchTrap" || kind=="oCrashTrap"; }
static const char* const VARS[]={"Open","Dead","Collected","Breach","Security","Hacked","HackedSecondsLeft","CrashedSecondsLeft","Disabled","On","HasIntel","DestroyMe","Armed"};
static const int VAR_COUNT=sizeof(VARS)/sizeof(VARS[0]);
static const double NONE=-99999;
struct Known { double v[VAR_COUNT]; bool present; DWORD heldUntil; };
static std::map<int,Known> known;   // instance -> last state agreed with the partner
static DWORD explosionUntil;
static int sharedShipNow;
static bool isDoor(int id){ return objectName(id)=="oDoor"; }
static bool isAirlockDoor(int id){ int a=(int)getReal(id,"MyAirlock"); return a>0 && live(a); }
// An airlock's doors are cycled by the game itself, and opening one from outside while the far side is open vents the ship.
// They are shared only where this game also has a pod docked at that airlock (the pod seals it), e.g. when both players
// docked at the same airlock; elsewhere each game shows its own airlock.
static double airlockDocked(int airlock) {
    static int var=api->GetVarId("DockedState");
    RValue v{}; v.type=5; if(var>=0) api->GetVar(airlock,var,(int)0x80000000,&v); double d=Number(v); return d==d?d:0;
}
static bool doorShareable(int door) {
    if(!isAirlockDoor(door)) return true;
    return airlockDocked((int)getReal(door,"MyAirlock"))!=0;
}
static void readState(int id,double* out) {
    for(int i=0;i<VAR_COUNT;i++){ double d=getReal(id,VARS[i]); out[i]=d==d?d:NONE; }
    if(isDoor(id)) out[0]=NONE;   // door opening is exchanged as "held open" sets, below
    // Only where the variable is that kind of state: a security camera switched off at its bypass panel, a terminal's
    // intel taken, a jammer being removed. (Items have an "On" of their own, which is each player's business.)
    std::string kind=objectName(id);
    for(int i=0;i<VAR_COUNT;i++) {
        if(!strcmp(VARS[i],"On") && kind!="oSecurityCamera") out[i]=NONE;
        if(!strcmp(VARS[i],"HasIntel") && kind!="oTerminal") out[i]=NONE;
        if(!strcmp(VARS[i],"DestroyMe") && kind!="oJammerDevice") out[i]=NONE;
        // A grenade or trap's Armed: the game draws its radius / set look only while it is (a trap arms itself just
        // after it is dropped, so the drop alone carried it unarmed).
        if(!strcmp(VARS[i],"Armed") && !isArmable(kind)) out[i]=NONE;
    }
}
// Timers count down on their own in both games; only their (re)start is exchanged.
static bool isTimer(int var){ return strstr(VARS[var],"SecondsLeft")!=nullptr; }
static bool differs(int var,double from,double to){ return isTimer(var)?((from>0)!=(to>0) || (to>from+2.0)):from!=to; }
static bool onShip(int ship,int id){ return live(id) && (int)getReal(id,"Parent")==ship; }
// The inventory menu keeps the item under the cursor (oMenu.CursorItem) and draws its details every frame; removing that
// item crashes the menu, so such removals wait until the cursor has moved off it.
static std::vector<int> pendingRemovals;
static bool underCursor(int id) {
    static double menuObject=-2;
    CInstance* me=*player>0?api->ResolveCInstance(*player):nullptr; if(!me) return false;
    if(menuObject==-2) { GmArgs a; a.AddStr(api,"oMenu"); menuObject=Number(Engine(api,"asset_get_index",me,a.Count(),a.Build())); }
    if(!(menuObject>=0)) return false;
    GmArgs n; n.AddReal(menuObject); int count=(int)Number(Engine(api,"instance_number",me,n.Count(),n.Build()));
    for(int i=0;i<count;i++) {
        GmArgs f; f.AddReal(menuObject); f.AddReal(i); int m=(int)Number(Engine(api,"instance_find",me,f.Count(),f.Build()));
        if(m>0 && (int)getReal(m,"CursorItem")==id) return true;
    }
    return false;
}
static void removeLocal(int id) {
    CInstance* inst=api->ResolveCInstance(id); if(!inst) return;
    if(underCursor(id)) { pendingRemovals.push_back(id); return; }
    // Stashed, not destroyed: the game may still hold the id (the menu's cursor item, a guard's target), and reading a
    // destroyed instance crashes it. Off the ship and parented to itself like a held item, it is neither drawn nor
    // reachable; a later drop of the same item reuses it.
    if(CoopVerbose()) CoopLog("Objects: %s %d removed from the ship here",objectName(id).c_str(),id);
    GmArgs r; r.AddReal(id); call("gml_Script_RemoveFromParentsContents",inst,r);
    setReal(id,"Parent",id); setReal(id,"Fore",0); setReal(id,"Starboard",0);
    { double tc=getReal(id,"TextColour"); if(!(tc==tc)) setReal(id,"TextColour",16777215); }
    setReal(id,"xReal",1e8); setReal(id,"yReal",1e8); setReal(id,"xSpeed",0); setReal(id,"ySpeed",0);
}
static double explosionFore, explosionStarboard;
static void markExplosion(int id){ explosionUntil=GetTickCount()+EXPLOSION_WINDOW_MS; explosionFore=getReal(id,"Fore"); explosionStarboard=getReal(id,"Starboard"); }
// A replayed blast goes off at once: the partner's game already burned the barrel's one-second fuse. The fuse starts when
// the game handles the hit, so it is cut short on the next frames.
static std::map<int,DWORD> shortenFuse;
static void detonate(int id){ markExplosion(id); setReal(id,"DamageMask",1); setReal(id,"DamageSignal",1); shortenFuse[id]=GetTickCount()+1000; }
// Armed copies of the partner's thrown grenades -> when their own explosion was first blocked (0: not yet).
static std::map<int,DWORD> copyGrenades;
// Copies that tried to go off. The game's grenade then destroys itself; that must not be reported to the thrower as
// taken (their real grenade was removed unexploded).
static std::set<int> triggeredCopies;
// A copy whose own explosion was blocked stays until the thrower's grenade is gone (it leaves with it, through the
// object sync). Removing it here was reported to the thrower as taken, and their real grenade vanished unexploded.
// Grenades that appear armed aboard without a throw or drop (fired from a grenade launcher: GunFiresGrenadeInDirection
// makes them) were never sent, and the other player saw neither them nor their radius (2026-10-05). Any armed grenade
// on the shared ship that the other game does not know yet is sent, the way a throw is.
static std::set<int> replayedExplosions;   // grenades made here to replay the partner's explosion (not sent back)
static void sendDrop(int item);
static double grenadeObject=-2;
static void sendNewGrenades(int ship) {
    CInstance* me=*player>0?api->ResolveCInstance(*player):nullptr; if(!me) return;
    if(grenadeObject==-2) { GmArgs n; n.AddStr(api,"oGrenade"); grenadeObject=Number(Engine(api,"asset_get_index",me,n.Count(),n.Build())); }
    if(!(grenadeObject>=0)) return;
    GmArgs c; c.AddReal(grenadeObject); int count=(int)Number(Engine(api,"instance_number",me,c.Count(),c.Build()));
    for(int i=0;i<count && i<64;i++) {
        GmArgs f; f.AddReal(grenadeObject); f.AddReal(i);
        int id=(int)Number(Engine(api,"instance_find",me,f.Count(),f.Build()));
        if(!live(id) || keyOf.count(id) || copyGrenades.count(id) || replayedExplosions.count(id) || !onShip(ship,id)) continue;
        double armed=getReal(id,"Armed"); if(!(armed==armed) || armed==0) continue;
        CoopLog("Objects: an armed %s appeared aboard (fired, not thrown); sent to the partner",getString(id,"Name").c_str());
        sendDrop(id);
    }
}
// A trap copy's own effect is blocked like a grenade copy's explosion: acid and crash traps hit through
// SetDamageSignal (a hit on a guard copy here is forwarded to the guards' game, so it would land twice), the glitch trap
// teleports through TeleportTo. The placer's trap does it in their game, and the result arrives from there.
static void preCopyItemEffect(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && copyGrenades.count((int)self->id)) { api->RequestBypass(); triggeredCopies.insert((int)self->id); }
}
static void spentCopyGrenades() {
    for(auto it=copyGrenades.begin();it!=copyGrenades.end();) { if(!live(it->first)) it=copyGrenades.erase(it); else ++it; }
}
static void shortenFuses() {
    for(auto it=shortenFuse.begin();it!=shortenFuse.end();) {
        if(!live(it->first) || GetTickCount()>it->second) { it=shortenFuse.erase(it); continue; }
        if(getReal(it->first,"SecondsUntilExplosion")>0.02) { setReal(it->first,"SecondsUntilExplosion",0.01); it=shortenFuse.erase(it); continue; }
        ++it;
    }
}
static void openBox(int box);
static void applyVar(int id,int var,double value) {
    const char* n=VARS[var];
    if(!strcmp(n,"Open")) {
        if(objectName(id)=="oLootBox") { if(value>0) openBox(id); return; }   // its own logic opens the lid and releases the loot
        setReal(id,"OpenSignal",value>0?1:0); setReal(id,"Open",value); return;
    }
    if(!strcmp(n,"Dead") && value==1) {
        double ds=getReal(id,"DamageSignal");
        if(ds==ds) { detonate(id); return; }   // breakables (windows, barrels, sentries) break through their damage handler
    }
    setReal(id,n,value);
}
// The partner picked up this game's mission objective: it belongs to the mission owner, so it goes to their inventory.
static bool giveObjectiveToOwner(int id) {
    const RValue* g=GlobalVar(GLOBAL_OBJECTIVE); if(!g || (int)Number(*g)!=id) return false;
    CInstance* me=*player>0?api->ResolveCInstance(*player):nullptr; CInstance* item=api->ResolveCInstance(id);
    if(!me || !item) return false;
    GmArgs r; r.AddReal(id); call("gml_Script_RemoveFromParentsContents",item,r);
    GmArgs a; a.AddReal(id); a.AddReal(*player); call("gml_Script_AddToInventory",me,a);
    CoopLog("Objects: your partner picked up your mission objective; it is now in your inventory");
    return true;
}
static void goneRemotely(int id,const std::string& key) {
    if(!onShip(sharedShipNow,id)) return;
    std::string name=objectName(id);
    if(name=="oFuelBarrel") { detonate(id); CoopLog("Objects: partner's fuel barrel %s went off",key.c_str()); return; }
    if(giveObjectiveToOwner(id)) return;
    // Parts of the ship itself (doors, airlocks, forcefields, terminals, windows, sentry guns, crates) are never taken
    // off the ship one by one: they go when their hull section goes, through the owner's destroyed section replayed
    // here (Hull.cpp), which removes everything in it the game's own way. Taken off alone, a door is left without a
    // ship, and its Draw (which reads its ship) stops the game with "REAL argument is unset".
    static const char* const FIXTURES[]={"oDoor","oAirlock","oForcefield","oTerminal","oWindowGlass","oLootBox","oLootBoxLid"};
    for(const char* f:FIXTURES) if(name==f) { if(CoopVerbose()) CoopLog("Objects: %s is gone in the partner's game; it goes here with its hull section",key.c_str()); return; }
    if(name.rfind("oSentry",0)==0) return;
    CoopLog("Objects: %s taken or destroyed in the partner's game",key.c_str());
    removeLocal(id);
}

// ---- Doors held open ----
// A door is held open by the walkers next to it: this game's player, and (in the game that runs them) the guards. Each
// game reports the doors its own walkers hold open; the other game keeps them open too and lets them close once nobody
// on either side is at the door. (Using "the door is open" as the reason deadlocked: each game saw the door open because
// of the other, neither reported it, and it shut on the player walking through.)
static std::set<std::string> partnerDoors; static DWORD partnerDoorsAt;
static const double DOOR_REACH=120;
static std::vector<std::pair<double,double>> walkers;
static void collectWalkers(int ship) {
    walkers.clear();
    if(*player>0 && live(*player) && (int)getReal(*player,"Parent")==ship) walkers.push_back({getReal(*player,"Fore"),getReal(*player,"Starboard")});
    if(!GuardsIsAuthority()) return;   // the follower's guards are copies; the host reports what its guards hold open
    int count=(int)getReal(ship,"ContentsCount");
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); if(id<=0 || id==*puppet) continue;
        if(!*GuardsKeyOfGuard(id)) continue;
        if(getReal(id,"Conscious")!=1 || getReal(id,"Dead")==1) continue;
        walkers.push_back({getReal(id,"Fore"),getReal(id,"Starboard")});
    }
}
static bool walkerAt(int door) {
    double f=getReal(door,"Fore"), s=getReal(door,"Starboard");
    for(auto& w:walkers) if(std::hypot(w.first-f,w.second-s)<DOOR_REACH) return true;
    return false;
}
static void reportDoors(int ship) {
    auto sit=byShip.find(ship); if(sit==byShip.end()) return;
    Writer w; w.str(getString(ship,"Name")); size_t countAt=w.bytes.size(); w.put<uint16_t>(0); uint16_t n=0;
    for(auto& kv:sit->second) {
        int id=kv.second;
        if(!onShip(ship,id) || !isDoor(id) || !doorShareable(id)) continue;
        if(getReal(id,"Open")!=1 || !walkerAt(id)) continue;
        w.str(kv.first); n++;
    }
    memcpy(w.bytes.data()+countAt,&n,sizeof(n));
    NetSendUnreliable(MSG_DOORS,w.bytes.data(),w.bytes.size());
}
static std::set<int> openedForPartner;   // doors this game opened only because the partner holds them open
static void holdPartnerDoors(int ship) {
    auto sit=byShip.find(ship); if(sit==byShip.end()) return;
    bool fresh=GetTickCount()-partnerDoorsAt<DOOR_FRESH_MS;
    std::set<int> held;
    if(fresh) for(const std::string& k:partnerDoors) {
        auto it=sit->second.find(k); if(it==sit->second.end() || !onShip(ship,it->second) || !doorShareable(it->second)) continue;
        int door=it->second; held.insert(door);
        setReal(door,"OpenSignal",1);
        // OpenDoor toggles it the game's way (collision grid, depth, sound).
        if(getReal(door,"Open")!=1) { GmArgs a; a.AddReal(door); call("gml_Script_OpenDoor",api->ResolveCInstance(door),a); if(openedForPartner.insert(door).second) if(CoopVerbose()) CoopLog("Objects: opened %s for the partner",k.c_str()); }
    }
    for(auto it=openedForPartner.begin();it!=openedForPartner.end();) {
        int door=*it;
        if(held.count(door)) { ++it; continue; }
        if(live(door) && getReal(door,"Open")==1 && !walkerAt(door)) { GmArgs a; a.AddReal(door); call("gml_Script_CloseDoor",api->ResolveCInstance(door),a); }
        it=openedForPartner.erase(it);
    }
}

// ---- Keycards: whatever keycard either player gets, the other gets too ----
static bool givingKeycard;
static std::map<int,double> knownCards;   // keycards this player already holds, and their level (watchKeycards)
// Keycards are passive-inventory items named "Level N keycard for this ship", with Security and OriginalShip.
static bool isKeycard(int item){ std::string n=getString(item,"Name"); for(char& c:n) c=(char)tolower(c); return n.find("keycard")!=std::string::npos && getReal(item,"Security")==getReal(item,"Security"); }
int MissionsFindShip(const char* name,double x,double y);
static void receiveKeycard(Reader& r) {
    std::string shipName=r.str(); double level=r.get<double>();
    if(!r.ok || *player<=0 || shipName.empty()) return;
    int ship=MissionsFindShip(shipName.c_str(),0,0); if(!ship) { CoopLog("Objects: partner's keycard is for '%s', which is not here",shipName.c_str()); return; }
    int count=(int)getReal(*player,"PassiveInventoryCount");
    for(int i=0;i<count;i++) {
        RValue v{}; v.type=5; api->GetVarByName(*player,"PassiveInventory",i,&v); int it=(int)Number(v);
        if(live(it) && isKeycard(it) && (int)getReal(it,"OriginalShip")==ship && getReal(it,"Security")>=level) { CoopLog("Objects: partner's level %g keycard: you already have it",level); return; }
    }
    // The partner took it from a guard: the same card leaves that guard's copy here (its key icon goes away).
    if(sharedShipNow==ship) {
        int n=(int)getReal(ship,"ContentsCount"); bool removed=false;
        for(int i=0;i<n && !removed;i++) {
            RValue it{}; it.type=5; api->GetVarByName(ship,"Contents",i,&it); int person=(int)Number(it);
            if(!live(person) || person==*player || person==*puppet) continue;
            // Only the body the partner took it from: knocked out or dead, right next to the partner (a card picked up
            // from the floor leaves this game through the floor item's own sync).
            if(getReal(person,"Conscious")==1 && getReal(person,"Dead")!=1) continue;
            if(!*puppet || std::hypot(getReal(person,"Fore")-getReal(*puppet,"Fore"),getReal(person,"Starboard")-getReal(*puppet,"Starboard"))>250) continue;
            int pc=(int)getReal(person,"PassiveInventoryCount");
            for(int j=0;j<pc;j++) {
                RValue v{}; v.type=5; api->GetVarByName(person,"PassiveInventory",j,&v); int card=(int)Number(v);
                if(!live(card) || !isKeycard(card) || (int)getReal(card,"OriginalShip")!=ship || getReal(card,"Security")!=level) continue;
                // Out of the guard's passive inventory (entries after it move down), and owned by nobody.
                for(int m=j;m<pc-1;m++) { RValue nx{}; nx.type=5; api->GetVarByName(person,"PassiveInventory",m+1,&nx); api->SetVarByName(person,"PassiveInventory",m,&nx); }
                setReal(person,"PassiveInventoryCount",pc-1); setReal(card,"Owner",49);
                CoopLog("Objects: took the level %g keycard from %s here too",level,getString(person,"Name").c_str());
                removed=true; break;
            }
        }
    }
    givingKeycard=true;
    GmArgs a; a.AddReal(*player); a.AddReal(level); call("gml_Script_GiveThisPersonAKeycard",api->ResolveCInstance(ship),a);
    givingKeycard=false;
    int after=(int)getReal(*player,"PassiveInventoryCount");
    if(after>count) { RValue v{}; v.type=5; api->GetVarByName(*player,"PassiveInventory",after-1,&v); int it=(int)Number(v); if(live(it)) setReal(it,"OriginalShip",ship); }
    // The new card (or the existing card, upgraded in place) is remembered at its level so it is not sent back.
    for(int i=0;i<after;i++) { RValue v{}; v.type=5; api->GetVarByName(*player,"PassiveInventory",i,&v); int it=(int)Number(v); if(live(it) && isKeycard(it)) knownCards[it]=getReal(it,"Security"); }
    CoopLog("Objects: your partner got a keycard (security %g) for '%s'; you have it too",level,shipName.c_str());
}

// ---- Loot boxes: opened with the game's own signal; the loot they release is keyed by box ----
static std::map<int,DWORD> pendingLoot;
static std::map<int,DWORD> pendingDiscard;    // crate loot to send to the partner, and when
static std::map<int,DWORD> openingBoxes;   // box -> until when new items next to it count as its loot
static std::set<int> remoteBoxes;           // boxes opened because the partner opened them: their own loot is replaced
static void sendDrop(int item);
static DWORD partnerOpenedAt;
static void openBox(int box){ remoteBoxes.insert(box); partnerOpenedAt=GetTickCount(); setReal(box,"Activate",1); openingBoxes[box]=GetTickCount()+3000; }
bool ObjectsBoxOpenedByPartner(int box){ return remoteBoxes.count(box)>0; }
bool ObjectsPartnerOpenedABoxJustNow(){ return GetTickCount()-partnerOpenedAt<1500; }
static void keyBoxLoot(int ship) {
    DWORD now=GetTickCount();
    for(auto it=pendingDiscard.begin();it!=pendingDiscard.end();) {
        if(now<it->second) { ++it; continue; }
        int item=it->first;
        if(live(item)) { removeLocal(item); if(CoopVerbose()) CoopLog("Objects: %s from the crate your partner opened is theirs; removed here",objectName(item).c_str()); }
        it=pendingDiscard.erase(it);
    }
    for(auto it=pendingLoot.begin();it!=pendingLoot.end();) {
        if(now<it->second) { ++it; continue; }
        if(live(it->first) && onShip(ship,it->first)) sendDrop(it->first);
        it=pendingLoot.erase(it);
    }
    for(auto it=openingBoxes.begin();it!=openingBoxes.end();) {
        if(now>it->second || !live(it->first)) { remoteBoxes.erase(it->first); it=openingBoxes.erase(it); continue; }
        int box=it->first; std::string boxKey=ObjectsKeyOf(box);
        double bf=getReal(box,"Fore"), bs=getReal(box,"Starboard");
        int count=(int)getReal(ship,"ContentsCount");
        for(int i=0;i<count;i++) {
            RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
            int id=(int)Number(item); if(!live(id) || keyOf.count(id)) continue;
            std::string name=objectName(id); if(SKIP.count(name) || name=="oLootBoxLid") continue;
            if(std::hypot(getReal(id,"Fore")-bf,getReal(id,"Starboard")-bs)>96) continue;
            // The partner opened this crate: what it released is theirs (it goes to their character here, so it is neither
            // on the floor for this player nor destroyed under the game's menu).
            // A mission objective in this game's crate stays: it is this game's mission item (the partner's copy of the
            // crate held their own roll), and this player can still take it.
            if(remoteBoxes.count(box)) {
                if(!getString(box,"ObjectiveText").empty() || [&]{ const RValue* g=GlobalVar(GLOBAL_OBJECTIVE); return g && (int)Number(*g)==id; }()) { keyOf[id]="objective-in-crate"; continue; }
                keyOf[id]="opener"; pendingDiscard[id]=now+500; continue;
            }
            std::string base="loot:"+boxKey+":"+name, key=base;
            auto& keys=byShip[ship];
            for(int n=1;keys.count(key) && live(keys[key]) && keys[key]!=id;n++) key=base+"#"+std::to_string(n);
            // Each game's crate releases its own roll (the game shows what was found, so it must stay); the same item
            // in both games shares a key, so picking it up removes it for both.
            addKey(ship,id,key);
        }
        ++it;
    }
}

// ---- Item previews: a hidden copy of an item in the partner's hands, for the game's own details panel ----
int ObjectsMakePreviewItem(const char* name,Reader& r) {
    if(*player<=0 || !live(*player)) return 0;
    int ship=(int)getReal(*player,"Parent"); if(!live(ship)) return 0;
    CInstance* me=api->ResolveCInstance(*player);
    GmArgs idx; idx.AddStr(api,name); double obj=Number(Engine(api,"asset_get_index",me,idx.Count(),idx.Build()));
    if(!(obj>=0)) return 0;
    GmArgs mk; mk.AddReal(obj); mk.AddReal(*player);
    int id=(int)Number(call("gml_Script_CreateContentsOnTopOf",api->ResolveCInstance(ship),mk));
    if(!live(id)) return 0;
    static const char* const LOCAL_COUNTS[]={"InventoryCount","ContentsCount","ItemCount"};
    double counts[3]; for(int i=0;i<3;i++) counts[i]=getReal(id,LOCAL_COUNTS[i]);
    SnapshotApply(id,r,true);
    for(int i=0;i<3;i++) if(counts[i]==counts[i]) setReal(id,LOCAL_COUNTS[i],counts[i]);
    if(getReal(id,"Armed")==getReal(id,"Armed")) setReal(id,"Armed",0);
    { double tc=getReal(id,"TextColour"); if(!(tc==tc)) setReal(id,"TextColour",16777215); }
    removeLocal(id);   // off the ship: never seen or picked up, only described
    return id;
}

// ---- Items a player drops ----
static int dropCounter;
static void sendDrop(int item) {
    if(!sharedShipNow || !onShip(sharedShipNow,item)) return;
    std::string key=ObjectsKeyOf(item);
    if(key.empty()) { char k[64]; snprintf(k,sizeof(k),"drop:%s:%d",NetIsHost()?"H":"C",++dropCounter); key=k; addKey(sharedShipNow,item,key); }
    Writer w; w.str(getString(sharedShipNow,"Name")); w.str(key); w.str(objectName(item));
    w.put<double>(getReal(item,"Fore")); w.put<double>(getReal(item,"Starboard"));
    SnapshotCapture(item,w);
    NetSendReliable(MSG_ITEM_DROP,w.bytes.data(),w.bytes.size());
    Known k{}; k.present=true; readState(item,k.v); known[item]=k;
    CoopLog("Objects: %s (%s) placed in the partner's game too",objectName(item).c_str(),key.c_str());
}
static void receiveDrop(Reader& r) {
    std::string ship=r.str(), key=r.str(), name=r.str(); double fore=r.get<double>(), starboard=r.get<double>();
    if(!r.ok || !sharedShipNow || getString(sharedShipNow,"Name")!=ship || *player<=0) return;
    if((int)getReal(*player,"Parent")!=sharedShipNow) return;   // created only aboard (a ship far away is not set up for it)
    CInstance* me=api->ResolveCInstance(*player); if(!me) return;
    auto& keys=byShip[sharedShipNow];
    auto it=keys.find(key);
    int id=it!=keys.end() && live(it->second)?it->second:0;
    if(id && isPlayerObject((int)getReal(id,"Parent"))) return;   // someone here holds it
    if(!id) {
        GmArgs idx; idx.AddStr(api,name.c_str()); double obj=Number(Engine(api,"asset_get_index",me,idx.Count(),idx.Build()));
        if(!(obj>=0)) return;
        // The game's own constructor (self = ship): items made with a bare instance_create miss setup the game does
        // there, and the inventory UI then fails on them.
        GmArgs mk; mk.AddReal(obj); mk.AddReal(*player);
        id=(int)Number(call("gml_Script_CreateContentsOnTopOf",api->ResolveCInstance(sharedShipNow),mk));
        if(!live(id)) return;
        static const char* const LOCAL_COUNTS[]={"InventoryCount","ContentsCount","ItemCount"};
        double counts[3]; for(int i=0;i<3;i++) counts[i]=getReal(id,LOCAL_COUNTS[i]);
        SnapshotApply(id,r,true);
        for(int i=0;i<3;i++) if(counts[i]==counts[i]) setReal(id,LOCAL_COUNTS[i],counts[i]);
        // A thrown grenade's copy stays armed: the game draws its blast radius and fuse ring only while it is (the partner
        // saw none, 2026-10-05). It must not go off by itself here, though: its explosion arrives from the thrower's
        // game, so its own is blocked (preExplodeOnShip) and it is removed soon after.
        if(isArmable(name)) copyGrenades[id]=0;
        // The ship an item belongs to arrives as an id of the other game, which is nothing here. The game needs it (a
        // keycard without one raises "Keycard created but not given a Ship var"; a key only opens its own ship's doors).
        // (Only for what the ship was generated with, and keycards: a guard's dropped weapon has none.)
        if(isKeycard(id) || (key.rfind("gdrop:",0)!=0 && key.rfind("drop:",0)!=0)) setReal(id,"OriginalShip",sharedShipNow);
        addKey(sharedShipNow,id,key);
        // A guard's drop (key gdrop:<guard>:<object>): the copy of that guard here gives up its own copy of the item.
        if(key.rfind("gdrop:",0)==0) { size_t a=6, b=key.find(':',a); if(b!=std::string::npos) GuardsCopyGivesUp(key.substr(a,b-a).c_str(),name.c_str()); }
    }
    // The inventory menu draws every item's name in its TextColour.
    { double tc=getReal(id,"TextColour"); if(!(tc==tc)) setReal(id,"TextColour",16777215); }
    CInstance* inst=api->ResolveCInstance(id);
    // The constructor already put a new item aboard (on the player's ship); an existing one is moved back aboard.
    if((int)getReal(id,"Parent")!=sharedShipNow) {
        GmArgs out; out.AddReal(id); call("gml_Script_RemoveFromParentsContents",inst,out);
        setReal(id,"Parent",sharedShipNow);
        GmArgs add; add.AddReal(id); call("gml_Script_AddToParentsContents",inst,add);
    }
    setReal(id,"Fore",fore); setReal(id,"Starboard",starboard);
    setReal(id,"ForeSpeed",0); setReal(id,"StarboardSpeed",0);
    PlaceAboard(api,inst);
    Known k{}; k.present=true; readState(id,k.v); k.heldUntil=GetTickCount()+HOLD_MS; known[id]=k;
    CoopLog("Objects: %s %d (%s) from the partner's game placed at %.0f,%.0f",name.c_str(),id,key.c_str(),fore,starboard);
}
// ---- Floor loot is the owner's ----
// Each game rolls the loot lying around a ship when it generates it, and the copy's roll differs from the owner's (two
// ammo pickups in one game, a money pickup somewhere else in the other). The game that generated the ship as its own
// sends what is really there: the list of its loot (the copy removes its own loot that is not on it) and each item
// (created in the copy where it is missing).
bool MissionsShipIsCopy(int ship);
static void setText(int id,const char* n,const char* text);
static bool isFloorLoot(int id){ std::string n=objectName(id); return n.rfind("oLoot",0)==0 && n.rfind("oLootBox",0)!=0; }
static void sendLoot(int ship) {
    if(MissionsShipIsCopy(ship)) return;
    std::vector<int> items; int count=(int)getReal(ship,"ContentsCount");
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); if(live(id) && isFloorLoot(id) && keyOf.count(id)) items.push_back(id);
    }
    Writer w; w.str(getString(ship,"Name")); w.put<uint16_t>((uint16_t)items.size());
    for(int id:items) w.str(keyOf[id]);
    // The mission's objective crate: the owner's game names it after the mission and gives it the mission's value; the
    // copy's is a plain "Cargo Crate". Its link to the mission (Mission) stays the owner's: the copy has no such mission.
    std::vector<int> boxes;
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); if(live(id) && objectName(id)=="oLootBox" && keyOf.count(id) && live((int)getReal(id,"Mission"))) boxes.push_back(id);
    }
    w.put<uint16_t>((uint16_t)boxes.size());
    for(int id:boxes) { w.str(keyOf[id]); w.str(getString(id,"Name")); w.put<double>(getReal(id,"Value")); }
    NetSendReliable(MSG_LOOT_LIST,w.bytes.data(),w.bytes.size());
    for(int id:items) sendDrop(id);
    CoopLog("Objects: sent this ship's floor loot to the partner (%zu items)",items.size());
}
static void receiveLootList(Reader& r) {
    std::string ship=r.str(); uint16_t n=r.get<uint16_t>(); std::set<std::string> theirs;
    for(int i=0;i<n;i++) theirs.insert(r.str());
    struct Box { std::string key,name; double value; }; std::vector<Box> boxes;
    uint16_t nb=r.get<uint16_t>(); for(int i=0;i<nb && r.ok;i++) { Box b; b.key=r.str(); b.name=r.str(); b.value=r.get<double>(); boxes.push_back(b); }
    if(!r.ok || !sharedShipNow || getString(sharedShipNow,"Name")!=ship || !MissionsShipIsCopy(sharedShipNow)) return;
    for(const Box& b:boxes) {
        int id=ObjectsIdOfKey(sharedShipNow,b.key.c_str());
        if(id<=0) { CoopLog("Objects: the owner's objective crate %s is not on this copy",b.key.c_str()); continue; }
        setText(id,"Name",b.name.c_str()); if(b.value==b.value) setReal(id,"Value",b.value);
        CoopLog("Objects: objective crate %s is '%s' here too",b.key.c_str(),b.name.c_str());
    }
    std::vector<int> gone; int count=(int)getReal(sharedShipNow,"ContentsCount");
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(sharedShipNow,"Contents",i,&item);
        int id=(int)Number(item); if(!live(id) || !isFloorLoot(id)) continue;
        auto k=keyOf.find(id); if(k!=keyOf.end() && theirs.count(k->second)) continue;
        gone.push_back(id);
    }
    for(int id:gone) removeLocal(id);
    if(!gone.empty()) CoopLog("Objects: removed %zu loot item(s) the owner's ship does not have",gone.size());
}
// Jammers (put down by guards, so they exist only in the game that runs the guards) are NOT sent like dropped items:
// tried 2026-10-04, the copy stopped the other game with "'with' failed in removefromcontents" (a jammer keeps
// references that do not survive being recreated from its variables). They need their own creation message.
void ObjectsShareNewItem(int item) { if(live(item)) pendingLoot[item]=GetTickCount()+700; }
// A copy of a guard never drops anything itself: its kit follows the guards' game's kit messages, and what that game's
// guard drops arrives as an item. The copy running the game's Down state dropped its weapon on its own, and when that
// came after the guard had got up and re-armed there, the copy stayed unarmed (guard_kits_match_client_mission).
static void preDropAtMyFeet(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && (GuardsIsMirroredGuard((int)self->id) || GuardsDownByAuthority((int)self->id))) api->RequestBypass();
}
// Throws: ThrowInventoryItem and EjectFromInventory take an item out of its owner's hands. Thrown from the planning
// screen, the drop that follows does not run with the player as self, and the thrown grenade was never sent (a playtester's
// partner saw neither the grenade nor its radius, 2026-10-05). Items this player owned before the call are sent after.
static std::vector<int> throwing;
static void preThrow(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(*player<=0) return;
    if(throwing.size()>16) throwing.clear();
    auto consider=[&](int id){ if(id>0 && live(id) && (int)getReal(id,"Owner")==*player && std::find(throwing.begin(),throwing.end(),id)==throwing.end()) throwing.push_back(id); };
    if(self) consider((int)self->id);
    for(int i=0;i<argc && i<4 && argv;i++) if(argv[i]) { RValueKind k=GetRValueKind(*argv[i]); if(k==RValueKind::Real || k==RValueKind::Int32 || k==RValueKind::Int64) consider((int)Number(*argv[i])); }
}
static void postThrow(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) {
    // (The calls nest: ThrowInventoryItem calls EjectFromInventory. An item still owned waits for the outer call.)
    for(auto it=throwing.begin();it!=throwing.end();) {
        int id=*it;
        if(!live(id)) { it=throwing.erase(it); continue; }
        if((int)getReal(id,"Owner")==*player) { ++it; continue; }
        it=throwing.erase(it);
        if(sharedShipNow && onShip(sharedShipNow,id)) sendDrop(id);
    }
}
// DropAtMyFeet runs with the dropper as self and the item as argument0.
static void postDropAtMyFeet(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!self || argc<1 || !argv || !argv[0] || !sharedShipNow) return;
    int item=(int)Number(*argv[0]), dropper=(int)self->id;
    if(!live(item)) return;
    if(dropper==*player) { sendDrop(item); return; }
    // A guard's drop (its weapon when it goes down) is decided by the game that runs the guards: it keys the item by
    // the guard and sends it once it has come to rest. The other game's copy of the guard drops its own copy of the
    // item, which is taken away again: the two differed (ammo left in a dropped gun, its worth, where it landed).
    const char* guardKey=GuardsKeyOfGuard(dropper);
    if(keyOf.count(item) && GuardsIsAuthority() && guardKey && *guardKey) { sendDrop(item); return; }   // picked up earlier, dropped again
    if(guardKey && *guardKey && !keyOf.count(item)) {
        if(!GuardsIsAuthority()) { pendingDiscard[item]=GetTickCount()+50; return; }
        std::string base=std::string("gdrop:")+guardKey+":"+objectName(item), key=base;
        auto& keys=byShip[sharedShipNow];
        for(int n=1;keys.count(key) && live(keys[key]);n++) key=base+"#"+std::to_string(n);
        addKey(sharedShipNow,item,key);
        pendingLoot[item]=GetTickCount()+700;
    }
}

// ---- Exchange ----
void ObjectsOnMessage(uint8_t type,const uint8_t* data,size_t size);
static bool resyncAll;
void ObjectsResync() { resyncAll=true; }
static void scan(int ship) {
    auto sit=byShip.find(ship); if(sit==byShip.end()) return;
    // When a ship becomes shared or the partner (re)connects, the game that runs the ship sends every object's state,
    // so a late arrival sees the crates opened, money taken and windows broken before they came.
    if(resyncAll && GuardsIsAuthority()) {
        resyncAll=false; std::vector<int> redrops;
        Writer w; w.str(getString(ship,"Name")); size_t countAt=w.bytes.size(); w.put<uint16_t>(0); uint16_t n=0;
        auto flush=[&]{ if(!n) return; memcpy(w.bytes.data()+countAt,&n,sizeof(n)); NetSendReliable(MSG_OBJECTS,w.bytes.data(),w.bytes.size()); w.bytes.resize(countAt); w.put<uint16_t>(0); n=0; };
        for(auto& kv:sit->second) {
            int id=kv.second; bool present=onShip(ship,id);
            double cur[VAR_COUNT]; if(present) readState(id,cur); else for(double& d:cur) d=NONE;
            if(!present && live(id) && (int)getReal(id,"Parent")!=id) continue;   // somewhere else (container, space): not reported
            w.str(kv.first); w.put<uint8_t>(present?1:0); for(double d:cur) w.put<double>(d);
            Known k{}; k.present=present; for(int i=0;i<VAR_COUNT;i++) k.v[i]=cur[i]; known[id]=k;
            if(++n>=60) flush();
            // Things dropped since the ship was made (a downed guard's weapon, a player's drop) exist in the other game
            // only through their drop message: a rebuilt copy there has none, so they are sent again.
            if(present && (kv.first.rfind("gdrop:",0)==0 || kv.first.rfind("drop:",0)==0)) redrops.push_back(id);
        }
        flush();
        for(int id:redrops) sendDrop(id);
        if(!redrops.empty()) CoopLog("Objects: %zu dropped items sent again",redrops.size());
        CoopLog("Objects: sent the full state of '%s' to the partner",getString(ship,"Name").c_str());
        return;
    }
    std::string shipName=getString(ship,"Name");
    Writer w; w.str(shipName); size_t countAt=w.bytes.size(); w.put<uint16_t>(0); uint16_t n=0;
    DWORD now=GetTickCount();
    for(auto& kv:sit->second) {
        int id=kv.second;
        bool present=onShip(ship,id);
        // Picked up (by a player or a guard), destroyed or gone overboard: it leaves the other game's ship too.
        bool taken=!present;
        double cur[VAR_COUNT]; if(present) readState(id,cur); else for(double& d:cur) d=NONE;
        auto kit=known.find(id);
        if(kit==known.end()) { Known k{}; k.present=present; for(int i=0;i<VAR_COUNT;i++) k.v[i]=cur[i]; known[id]=k; continue; }   // baseline
        Known& k=kit->second;
        if(!present && !taken) { k.present=false; continue; }
        // An armed copy of the partner's grenade or trap never reports back: what it does here (go off, be used up,
        // vanish) is the partner's item's business, whose own state arrives from their game.
        if((copyGrenades.count(id) || triggeredCopies.count(id)) && !(live(id) && isPlayerObject((int)getReal(id,"Parent")))) { k.present=present; for(int i=0;i<VAR_COUNT;i++) k.v[i]=cur[i]; continue; }
        bool changed=k.present!=present;
        for(int i=0;i<VAR_COUNT && !changed && present;i++) changed=cur[i]!=NONE && k.v[i]!=NONE ? differs(i,k.v[i],cur[i]) : cur[i]!=k.v[i];
        if(!changed) continue;
        if(present && k.v[0]==0 && cur[0]>0 && objectName(id)=="oLootBox") openingBoxes[id]=GetTickCount()+3000;
        // Right after the partner changed this object, this game's own logic is still catching up; adopt the result
        // instead of echoing the transition back.
        if(now<k.heldUntil && present) { for(int i=0;i<VAR_COUNT;i++) k.v[i]=cur[i]; continue; }
        w.str(kv.first); w.put<uint8_t>(present?1:0); for(double d:cur) w.put<double>(d);
        k.present=present; for(int i=0;i<VAR_COUNT;i++) k.v[i]=cur[i];
        if(++n>=60) break;
    }
    if(!n) return;
    memcpy(w.bytes.data()+countAt,&n,sizeof(n));
    NetSendReliable(MSG_OBJECTS,w.bytes.data(),w.bytes.size());
}

// ---- Sentries ----
static bool replayingSentry, targetingPuppet;
static bool sentryOnSharedShip(CInstance* self){ return self && sharedShipNow && (int)getReal((int)self->id,"Parent")==sharedShipNow && keyOf.count((int)self->id); }
static bool partnerHere(){ return *puppet>0 && live(*puppet) && CoopRemoteParent()==sharedShipNow && !CoopRemoteDown(); }
static void preSentryTries(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(sentryOnSharedShip(self) && !GuardsIsAuthority()) api->RequestBypass();   // the host's sentries decide
}
// The host's sentry also looks for the partner (whose hits are forwarded to them through their puppet).
static void postSentryTries(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(targetingPuppet || !sentryOnSharedShip(self) || !GuardsIsAuthority() || !partnerHere() || argc<1 || !argv || !argv[0]) return;
    if((int)Number(*argv[0])!=*player) return;
    targetingPuppet=true;
    // Same call as the game's (scripts read their arguments without checking the count), with the puppet as target.
    GmArgs a; a.AddReal(*puppet); for(int i=1;i<argc;i++) a.AddRValue(*argv[i]);
    call("gml_Script_SentryTriesToShootAt",self,a);
    targetingPuppet=false;
}
// A sentry slowing time when it spots the partner is meant for the partner's game.
static void preSetTimeScale(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { if(targetingPuppet) api->RequestBypass(); }
static void postSentryFires(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(replayingSentry || !sentryOnSharedShip(self) || !GuardsIsAuthority()) return;
    Writer w; w.str(getString(sharedShipNow,"Name")); w.str(keyOf[(int)self->id]); w.put<uint8_t>((uint8_t)(argc>8?8:argc));
    for(int i=0;i<argc && i<8;i++) w.put<double>(argv && argv[i]?Number(*argv[i]):0);
    NetSendUnreliable(MSG_SENTRY_FIRE,w.bytes.data(),w.bytes.size());
}
static void replaySentryShot(Reader& r) {
    std::string ship=r.str(), key=r.str(); uint8_t argc=r.get<uint8_t>(); double args[8]{};
    for(int i=0;i<argc;i++) args[i]=r.get<double>();
    if(!r.ok || !sharedShipNow || getString(sharedShipNow,"Name")!=ship) return;
    if(*player<=0 || (int)getReal(*player,"Parent")!=sharedShipNow) return;
    auto& keys=byShip[sharedShipNow]; auto it=keys.find(key); if(it==keys.end() || !live(it->second)) return;
    int sentry=it->second; int before=(int)getReal(sharedShipNow,"ContentsCount");
    // argument0 is the sentry itself (an instance id of the host's game): use this game's copy.
    GmArgs a; for(int i=0;i<argc;i++) a.AddReal(i==0?sentry:args[i]);
    replayingSentry=true; call("gml_Script_SentryFiresBulletInDirection",api->ResolveCInstance(sentry),a); replayingSentry=false;
    // The bullet is shown, but its damage already happened in the host's game (to the partner's puppet, forwarded here).
    int count=(int)getReal(sharedShipNow,"ContentsCount");
    for(int i=before>0?before-1:0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(sharedShipNow,"Contents",i,&item);
        int b=(int)Number(item);
        if(live(b) && (int)getReal(b,"Firer")==sentry) { setReal(b,"BulletDamageMask",0); setReal(b,"KnockbackSpeed",0); }
    }
    static DWORD lastLog; if(CoopVerbose() && GetTickCount()-lastLog>3000) { lastLog=GetTickCount(); CoopLog("Objects: host sentry %s fired (shown here; its hits arrive through your partner link)",key.c_str()); }
}

// ---- Effects one game creates are shown in the other ----
// The game makes its particle bursts, blood and sound rings through a few scripts. They run only in the game where the
// thing happened (a gun's muzzle blast, a melee hit, a door or guard reacting to a crash or subvert beam), so the
// partner saw the bullets but not the blast. Each such call is sent (script, arguments; the ship and instances by key)
// and the same script is run in the other game. Not sent: effects of bullets (the partner's copies of the bullets make
// their own), screen shake and screen warp (they belong to one viewer), and explosions (replayed by their own message).
struct EffectScript { const char* script; int shipArg, instArg; };
static const EffectScript EFFECTS[]={
    {"gml_Script_CreateParticlesOnShip",4,-1},            // type, fore, starboard, angle, ship
    {"gml_Script_CreateParticlesOnShipWithOffset",5,-1},  // type, fore, starboard, angle, offset, ship
    {"gml_Script_CreateParticlesFollowingObject",-1,1},   // type, the person it follows
    {"gml_Script_CreateBloodSplatter",2,-1},              // fore, starboard, ship
    {"gml_Script_VisualiseSoundInsideShip",3,-1},         // fore, starboard, radius, ship, important
};
static const int EFFECT_COUNT=sizeof(EFFECTS)/sizeof(EFFECTS[0]);
static bool replayingEffect;
static int effectsSent, effectsShown;
// Who an instance is, in terms the other game understands.
static std::string actorKey(int id) {
    if(id==*player) return "@";
    const char* g=GuardsKeyOfGuard(id); if(g && *g) return std::string("g:")+g;
    auto k=keyOf.find(id); if(k!=keyOf.end()) return std::string("o:")+k->second;
    return "";
}
static int actorOfKey(const std::string& key) {
    if(key=="@") return *puppet;
    if(key.rfind("g:",0)==0) return GuardsGuardByKey(key.c_str()+2);
    if(key.rfind("o:",0)==0) return ObjectsIdOfKey(sharedShipNow,key.c_str()+2);
    return 0;
}
// The same effect can arise in both games (an object's own reaction once its state arrives): one shown here by the
// partner's message is not made again locally, and one made locally is not shown again from a message.
struct RecentEffect { uint64_t hash; DWORD at; bool replay; };
static std::vector<RecentEffect> recentEffects;
static uint64_t effectHash(int idx,int argc,const double* a,const std::string& who) {
    uint64_t h=1469598103934665603ull; auto mix=[&](uint64_t v){ h^=v; h*=1099511628211ull; };
    mix((uint64_t)idx);
    const EffectScript& e=EFFECTS[idx];
    for(int i=0;i<argc;i++) { if(i==e.shipArg || i==e.instArg) continue; mix((uint64_t)(int64_t)std::floor(a[i]/16.0)); }
    for(char c:who) mix((uint8_t)c);
    return h;
}
static bool recentEffect(uint64_t h,bool replay) {
    DWORD now=GetTickCount();
    recentEffects.erase(std::remove_if(recentEffects.begin(),recentEffects.end(),[&](const RecentEffect& r){ return now-r.at>600; }),recentEffects.end());
    for(const RecentEffect& r:recentEffects) if(r.hash==h && r.replay==replay) return true;
    return false;
}
static void preEffect(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void* user) {
    if(replayingEffect || !sharedShipNow || !self || !argv) return;
    int idx=(int)(intptr_t)user; const EffectScript& e=EFFECTS[idx];
    if(argc>8 || argc<=e.shipArg || argc<=e.instArg) return;
    // A copy of a bullet fired in the other game makes no effects of its own: what it hits here can differ by a few
    // frames of lag, and the real bullet's effects arrive from that game (below).
    if(VisualsIsCopyBullet((int)self->id)) { api->RequestBypass(); return; }
    double a[8];
    for(int i=0;i<argc;i++) { if(!argv[i] || GetRValueKind(*argv[i])!=RValueKind::Real) return; a[i]=Number(*argv[i]); }
    if(e.shipArg>=0 && (int)a[e.shipArg]!=sharedShipNow) return;
    int me=(int)self->id, followed=e.instArg>=0?(int)a[e.instArg]:0;
    std::string who;
    if(e.instArg>=0) { who=actorKey(followed); if(who.empty()) return; }
    uint64_t h=effectHash(idx,argc,a,who);
    if(recentEffect(h,true)) { api->RequestBypass(); return; }   // already shown here from the partner's game
    recentEffects.push_back({h,GetTickCount(),false});
    if(CoopRemoteParent()!=sharedShipNow) return;
    // Whose effect it is: this player's (or their item's), a guard's in the game that runs the guards, or a ship
    // object's own reaction (bullets: below).
    int owner=(int)getReal(me,"Owner");
    // A bullet's effects (where it hits: sparks, a concussive blast, blood) go with its shooter: this player's own
    // shots, and the guards' in the game that runs them.
    if(objectName(me)=="oBullet") {
        int firer=(int)getReal(me,"Firer");
        if(!(firer==*player || (GuardsIsAuthority() && firer>=100000 && *GuardsKeyOfGuard(firer)))) return;
        Writer w; w.str(getString(sharedShipNow,"Name")); w.put<uint8_t>((uint8_t)idx); w.put<uint8_t>((uint8_t)argc);
        for(int i=0;i<argc;i++) w.put<double>(a[i]);
        w.str(who);
        NetSendUnreliable(MSG_EFFECT,w.bytes.data(),w.bytes.size()); effectsSent++;
        return;
    }
    bool ours= me==*player || owner==*player || followed==*player
        || (GuardsIsAuthority() && (*GuardsKeyOfGuard(me) || (owner>=100000 && *GuardsKeyOfGuard(owner)) || (followed && *GuardsKeyOfGuard(followed))))
        || (keyOf.count(me) && objectName(me)!="oBullet");
    if(!ours) return;
    Writer w; w.str(getString(sharedShipNow,"Name")); w.put<uint8_t>((uint8_t)idx); w.put<uint8_t>((uint8_t)argc);
    for(int i=0;i<argc;i++) w.put<double>(a[i]);
    w.str(who);
    NetSendUnreliable(MSG_EFFECT,w.bytes.data(),w.bytes.size()); effectsSent++;
}
static void receiveEffect(Reader& r) {
    std::string ship=r.str(); uint8_t idx=r.get<uint8_t>(), argc=r.get<uint8_t>();
    if(!r.ok || idx>=EFFECT_COUNT || argc>8) return;
    double a[8]{}; for(int i=0;i<argc;i++) a[i]=r.get<double>();
    std::string who=r.str();
    if(!r.ok || !sharedShipNow || getString(sharedShipNow,"Name")!=ship || *player<=0 || (int)getReal(*player,"Parent")!=sharedShipNow) return;
    const EffectScript& e=EFFECTS[idx];
    if(argc<=e.shipArg || argc<=e.instArg) return;
    uint64_t h=effectHash(idx,argc,a,who);
    if(recentEffect(h,false)) return;   // this game made the same one itself
    if(e.shipArg>=0) a[e.shipArg]=sharedShipNow;
    if(e.instArg>=0) { int inst=actorOfKey(who); if(!live(inst)) return; a[e.instArg]=inst; }
    recentEffects.push_back({h,GetTickCount(),true});
    GmArgs args; for(int i=0;i<argc;i++) args.AddReal(a[i]);
    replayingEffect=true; call(e.script,api->ResolveCInstance(sharedShipNow),args); replayingEffect=false;
    effectsShown++;
    static DWORD lastLog; if(GetTickCount()-lastLog>5000) { lastLog=GetTickCount(); CoopLog("Objects: effects sent %d, partner's effects shown %d",effectsSent,effectsShown); }
}

// ---- Keys are party-wide ----
// The game asks one question wherever a key matters (doors, airlocks, terminals, the level shown under the player):
// SecurityLevel(person, ship). Each game tells the other its own player's level on the shared ship, as state repeated
// twice a second (a key taken before the partner boarded, or before a reconnect, counts too), and answers that
// question for its own player with the higher of the two. (A real card is still handed over at pickup, see
// receiveKeycard; this is what makes it hold in every case.)
static double partnerKeyLevel; static std::string partnerKeyShip; static DWORD partnerKeyAt; static bool askingOwnLevel;
static void sendKeyLevel(int ship) {
    if(*player<=0 || !live(*player)) return;
    GmArgs a; a.AddReal(*player); a.AddReal(ship);
    askingOwnLevel=true; double lv=Number(call("gml_Script_SecurityLevel",api->ResolveCInstance(*player),a)); askingOwnLevel=false;
    Writer w; w.str(getString(ship,"Name")); w.put<double>(lv==lv?lv:0);
    NetSendUnreliable(MSG_KEY_LEVEL,w.bytes.data(),w.bytes.size());
}
static void postSecurityLevel(const char*,CInstance*,CInstance*,RValue* result,int argc,RValue** argv,void*) {
    if(askingOwnLevel || !result || argc<2 || !argv || !argv[0] || !argv[1] || !sharedShipNow) return;
    if((int)Number(*argv[0])!=*player || GetTickCount()-partnerKeyAt>3000 || !(partnerKeyLevel>0)) return;
    int ship=(int)Number(*argv[1]);
    if(ship!=sharedShipNow && ship!=(int)getReal(sharedShipNow,"OriginalID")) return;
    if(getString(sharedShipNow,"Name")!=partnerKeyShip) return;
    double mine=Number(*result);
    if(!(mine>=partnerKeyLevel)) { result->type=0; result->real=partnerKeyLevel; }
}

// ---- Sentry guns: the owner's rotation, mirrored ----
// A sentry gun does not track anyone. It faces one of its room's doorway directions and turns to the next every five
// game-seconds (state Armed -> Turning -> Arming -> Armed), starting from a random direction at a random point of that
// cycle, on its own game's clock. Left alone, each game showed its own rotation while the owner's decided who was shot:
// the partner was shot by a gun that, on their screen, faced away. The owner's game sends each gun's state; the other
// game applies it at the start of the gun's own step (UpdateUseHandle is the first thing that step calls).
static const char* const SENTRY_STATES[]={"Arming","Armed","Turning","Off","Destroyed"};
struct SentryState { uint8_t state; double index,angle,untilArmed,untilTurn,hacked; DWORD at; };
// The partner's own switch-off or subversion of a sentry: asked of the guards' game (MSG_SENTRY_ACTION), and kept here
// until that game's state shows it (or a moment passes), so the copy is not turned back on first.
struct SentryAsk { bool off; double hacked; DWORD until; };
static std::map<int,SentryAsk> sentryAsked;
static std::map<int,double> sentryHackedApplied;
static std::map<int,SentryState> sentryStates;   // this game's copy -> the owner's latest state
static bool isSentry(int id){ return objectName(id).rfind("oSentry",0)==0; }
static void setText(int id,const char* n,const char* text){ RValue v{}; v.type=1; api->SetString(&v,text); api->SetVarByName(id,n,0x80000000,&v); }
static void sendSentries(int ship) {
    auto sit=byShip.find(ship); if(sit==byShip.end()) return;
    Writer w; w.str(getString(ship,"Name")); size_t countAt=w.bytes.size(); w.put<uint8_t>(0); uint8_t n=0;
    for(auto& kv:sit->second) {
        int id=kv.second; if(!live(id) || !isSentry(id) || (int)getReal(id,"Parent")!=ship) continue;
        std::string st=getString(id,"State"); uint8_t code=255;
        for(uint8_t i=0;i<5;i++) if(st==SENTRY_STATES[i]) code=i;
        if(code==255) continue;
        w.str(kv.first); w.put<uint8_t>(code); w.put<double>(getReal(id,"DirectionIndex")); w.put<double>(getReal(id,"MyImageAngle"));
        w.put<double>(getReal(id,"SecondsUntilArmed")); w.put<double>(getReal(id,"SecondsUntilTurn")); w.put<double>(getReal(id,"Hacked"));
        if(++n==40) break;
    }
    if(!n) return;
    w.bytes[countAt]=n;
    NetSendUnreliable(MSG_SENTRY_STATE,w.bytes.data(),w.bytes.size());
}
static void receiveSentryAction(Reader& r) {
    std::string ship=r.str(), key=r.str(); uint8_t off=r.get<uint8_t>(); double hacked=r.get<double>();
    if(!r.ok || !sharedShipNow || !GuardsIsAuthority() || getString(sharedShipNow,"Name")!=ship) return;
    int id=ObjectsIdOfKey(sharedShipNow,key.c_str()); if(id<=0 || !isSentry(id)) return;
    std::string st=getString(id,"State"); if(st=="Destroyed") return;
    if(off) setText(id,"State","Off");
    if(hacked==hacked) setReal(id,"Hacked",hacked);
    CoopLog("Objects: the partner %s sentry %s; done here too",off?"switched off":"subverted",key.c_str());
}
static void receiveSentries(Reader& r) {
    std::string ship=r.str(); uint8_t n=r.get<uint8_t>();
    if(!r.ok || !sharedShipNow || GuardsIsAuthority() || getString(sharedShipNow,"Name")!=ship) return;
    auto& keys=byShip[sharedShipNow];
    for(int i=0;i<n;i++) {
        std::string key=r.str(); SentryState s{}; s.state=r.get<uint8_t>(); s.index=r.get<double>(); s.angle=r.get<double>(); s.untilArmed=r.get<double>(); s.untilTurn=r.get<double>(); s.hacked=r.get<double>();
        if(!r.ok) return;
        auto it=keys.find(key); if(it==keys.end() || !live(it->second) || s.state>4) continue;
        s.at=GetTickCount(); sentryStates[it->second]=s;
    }
}
static void preUpdateUseHandle(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || sentryStates.empty()) return;
    int id=(int)self->id; auto it=sentryStates.find(id); if(it==sentryStates.end()) return;
    const SentryState& s=it->second;
    if(GuardsIsAuthority() || GetTickCount()-s.at>1500 || (int)getReal(id,"Parent")!=sharedShipNow) { sentryStates.erase(it); return; }
    std::string mine=getString(id,"State");
    if(mine=="Destroyed" || s.state==4) return;   // destruction arrives through the object state (Dead) and its explosion
    {   // This player switched it off or subverted it here (the guards' game did not): ask that game, keep it meanwhile.
        double hackedHere=getReal(id,"Hacked"); auto ap=sentryHackedApplied.find(id);
        bool offHere=mine=="Off" && s.state!=3, hackHere=ap!=sentryHackedApplied.end() && hackedHere==hackedHere && hackedHere!=ap->second && hackedHere!=s.hacked;
        auto ask=sentryAsked.find(id);
        if((offHere || hackHere) && ask==sentryAsked.end()) {
            auto k=keyOf.find(id);
            if(k!=keyOf.end()) {
                Writer w; w.str(getString(sharedShipNow,"Name")); w.str(k->second); w.put<uint8_t>(offHere?1:0); w.put<double>(hackHere?hackedHere:NAN);
                NetSendReliable(MSG_SENTRY_ACTION,w.bytes.data(),w.bytes.size());
                sentryAsked[id]={offHere,hackHere?hackedHere:NAN,GetTickCount()+2000};
                CoopLog("Objects: sentry %s %s here; asked the guards' game",k->second.c_str(),offHere?"switched off":"subverted");
            }
            return;
        }
        if(ask!=sentryAsked.end()) {
            bool done=(!ask->second.off || s.state==3) && (!(ask->second.hacked==ask->second.hacked) || s.hacked==ask->second.hacked);
            if(!done && GetTickCount()<ask->second.until) return;
            sentryAsked.erase(ask);
        }
        if(s.hacked==s.hacked) { setReal(id,"Hacked",s.hacked); sentryHackedApplied[id]=s.hacked; }
    }
    if(s.index==s.index) setReal(id,"DirectionIndex",s.index);
    if(s.untilTurn==s.untilTurn) setReal(id,"SecondsUntilTurn",s.untilTurn);
    if(s.angle==s.angle) setReal(id,"MyImageAngle",s.angle);
    const char* theirs=SENTRY_STATES[s.state];
    if(mine==theirs) { if(s.state!=1 && s.untilArmed==s.untilArmed) setReal(id,"SecondsUntilArmed",s.untilArmed); return; }
    if(s.state==1) {
        // Armed is reached through the game's own Arming -> Armed step, which works out which rooms the gun covers and
        // its laser; with no arming time left that happens this frame.
        setText(id,"State","Arming"); setReal(id,"SecondsUntilArmed",0);
    } else {
        setText(id,"State",theirs); if(s.untilArmed==s.untilArmed) setReal(id,"SecondsUntilArmed",s.untilArmed);
    }
}

// ---- Explosions, guard sounds ----
// What explodes decides the explosion: ExplodeOnShip reads the exploding item's own variables (its damage type and
// flags: lethal, concussive, crash, breach). Replayed as a fuel barrel, a concussion grenade went off for the partner as
// a lethal, hull-breaking blast (test explosion_keeps_type_*). A grenade's type and variables are sent (before it
// explodes, while they are intact) and the other game sets off a copy of it through its own trigger (armed, impact):
// the grenade's own step explodes it. (Calling ExplodeOnShip directly stopped the game.)
static void preExplodeOnShip(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(self) {
        auto cg=copyGrenades.find((int)self->id);
        if(cg!=copyGrenades.end()) { api->RequestBypass(); if(!cg->second) cg->second=GetTickCount(); triggeredCopies.insert(cg->first); return; }
    }
    if(!self || !sharedShipNow) return;
    int id=(int)self->id;
    if((int)getReal(id,"Parent")!=sharedShipNow || replayedExplosions.count(id)) return;
    // Fuel barrels go off in both games through their own state; anything else that explodes (grenades, mines, charges,
    // items from crates or drops, which are keyed too) is replayed at the same spot.
    std::string name=objectName(id);
    if(name=="oFuelBarrel" || name.rfind("oSentry",0)==0) return;
    Writer w; w.str(getString(sharedShipNow,"Name")); w.put<double>(getReal(id,"Fore")); w.put<double>(getReal(id,"Starboard"));
    // The explosion's own arguments (the grenade passes its GrenadeDamageMask, BlastRadius, NoiseRadius and ship): the
    // other game calls ExplodeOnShip with the same ones on a copy of the grenade. (Called without them, it stopped the game.)
    bool argsOk=argc>0 && argc<=8 && argv;
    for(int i=0;argsOk && i<argc;i++) {
        RValueKind k=argv[i]?GetRValueKind(*argv[i]):RValueKind::Undefined;
        if(!(k==RValueKind::Real || k==RValueKind::Int32 || k==RValueKind::Int64 || k==RValueKind::Bool)) { argsOk=false; CoopLog("Objects: explosion argument %d is %s; replayed as a fuel barrel",i,GetTypeName(argv[i]?argv[i]->type&0xffffff:0)); }
    }
    w.str(argsOk?name:"");
    if(argsOk) {
        w.put<uint8_t>((uint8_t)argc);
        for(int i=0;i<argc;i++) { double d=Number(*argv[i]); bool ship=(int)d==sharedShipNow && d>=100000; w.put<uint8_t>(ship?1:0); w.put<double>(d); }
        SnapshotCapture(id,w);
    }
    NetSendReliable(MSG_EXPLOSION,w.bytes.data(),w.bytes.size());
    CoopLog("Objects: explosion of %s (%s) sent to the partner",name.c_str(),getString(id,"Name").c_str());
}
static void postExplodeOnShip(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || !sharedShipNow) return;
    int id=(int)self->id;
    if((int)getReal(id,"Parent")!=sharedShipNow) return;
    markExplosion(id);
}
static bool playingRemoteSound;
static void postPlaySound(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(playingRemoteSound || !self || argc<1 || !argv || !argv[0] || !sharedShipNow) return;
    // This player's own sounds (melee, gadgets) when the partner is aboard; the host's guards' sounds.
    const char* key=nullptr;
    if((int)self->id==*player) { if(CoopRemoteParent()!=sharedShipNow) return; key="@"; }
    else { if(!GuardsIsAuthority()) return; key=GuardsKeyOfGuard((int)self->id); }
    if(!key || !*key) return;
    // PlaySound(sound[, volume]) - forwarded with the same arguments (numbers only).
    if(argc>2) return;
    for(int i=0;i<argc;i++) if(!argv[i] || GetRValueKind(*argv[i])!=RValueKind::Real) return;
    Writer w; w.str(key); w.put<uint8_t>((uint8_t)argc); for(int i=0;i<argc;i++) w.put<double>(Number(*argv[i]));
    NetSendUnreliable(MSG_GUARD_SOUND,w.bytes.data(),w.bytes.size());
}

void ObjectsOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    Reader r(data,size);
    if(type==MSG_OBJECTS) {
        std::string ship=r.str(); uint16_t n=r.get<uint16_t>();
        if(!r.ok || !sharedShipNow || getString(sharedShipNow,"Name")!=ship) return;
        auto sit=byShip.find(sharedShipNow); if(sit==byShip.end()) return;
        for(uint16_t i=0;i<n && r.ok;i++) {
            std::string key=r.str(); uint8_t present=r.get<uint8_t>(); double v[VAR_COUNT]; for(double& d:v) d=r.get<double>();
            if(!r.ok) break;
            auto it=sit->second.find(key); if(it==sit->second.end()) continue;
            int id=it->second; Known& kn=known[id];
            kn.heldUntil=GetTickCount()+HOLD_MS;
            if(!present) { kn.present=false; goneRemotely(id,key); continue; }
            if(!live(id)) continue;
            double cur[VAR_COUNT]; readState(id,cur);
            for(int k=0;k<VAR_COUNT;k++) {
                if(v[k]==NONE || cur[k]==NONE || !differs(k,cur[k],v[k])) continue;
                // Money collected by the partner was credited to them; here it just disappears.
                if(!strcmp(VARS[k],"Collected") && v[k]!=0) { CoopLog("Objects: partner collected %s",key.c_str()); removeLocal(id); break; }
                applyVar(id,k,v[k]); if(CoopVerbose()) CoopLog("Objects: %s %s %g -> %g (partner)",key.c_str(),VARS[k],cur[k],v[k]);
            }
            if(!live(id)) { kn.present=false; continue; }
            kn.present=true; readState(id,kn.v);
            for(int k=0;k<VAR_COUNT;k++) if(v[k]!=NONE) kn.v[k]=v[k];
        }
        return;
    }
    if(type==MSG_DOORS) {
        std::string ship=r.str(); uint16_t n=r.get<uint16_t>(); std::set<std::string> doors;
        for(uint16_t i=0;i<n && r.ok;i++) doors.insert(r.str());
        if(!r.ok || !sharedShipNow || getString(sharedShipNow,"Name")!=ship) return;
        partnerDoors.swap(doors); partnerDoorsAt=GetTickCount();
        return;
    }
    if(type==MSG_ITEM_DROP) { receiveDrop(r); return; }
    if(type==MSG_GUARDS_TAKE_OVER) { std::string ship=r.str(); if(r.ok) GuardsTakeOver(ship.c_str()); return; }
    if(type==MSG_OBJECTS_RESYNC) {
        std::string ship=r.str();
        if(r.ok && sharedShipNow && getString(sharedShipNow,"Name")==ship) { resyncAll=true; CoopLog("Objects: the partner's copy of '%s' is new; sending everything aboard again",ship.c_str()); }
        return;
    }
    if(type==MSG_LOOT_LIST) { receiveLootList(r); return; }
    if(type==MSG_EFFECT) { receiveEffect(r); return; }
    if(type==MSG_KEYCARD) { receiveKeycard(r); return; }
    if(type==MSG_SENTRY_FIRE) { replaySentryShot(r); return; }
    if(type==MSG_SENTRY_STATE) { receiveSentries(r); return; }
    if(type==MSG_SENTRY_ACTION) { receiveSentryAction(r); return; }
    if(type==MSG_KEY_LEVEL) { std::string ship=r.str(); double lv=r.get<double>(); if(r.ok) { partnerKeyShip=ship; partnerKeyLevel=lv; partnerKeyAt=GetTickCount(); } return; }
    if(type==MSG_EXPLOSION) {
        std::string ship=r.str(); double fore=r.get<double>(), starboard=r.get<double>();
        if(!r.ok || !sharedShipNow || getString(sharedShipNow,"Name")!=ship || *player<=0) return;
        if((int)getReal(*player,"Parent")!=sharedShipNow) return;
        CInstance* me=api->ResolveCInstance(*player); if(!me) return;
        std::string what=r.str();
        if(r.ok && !what.empty()) {
            uint8_t argc=r.get<uint8_t>(); GmArgs eargs; std::string shown;
            for(int k=0;k<argc && r.ok;k++) { uint8_t kind=r.get<uint8_t>(); double d=r.get<double>(); eargs.AddReal(kind==1?sharedShipNow:d); char b[24]; snprintf(b,sizeof(b)," %g",kind==1?-1.0:d); shown+=b; }
            GmArgs gi; gi.AddStr(api,what.c_str()); double gobj=Number(Engine(api,"asset_get_index",me,gi.Count(),gi.Build()));
            GmArgs gm; gm.AddReal(0); gm.AddReal(0); gm.AddReal(gobj);
            int g=(r.ok && gobj>=0)?(int)Number(Engine(api,"instance_create",me,gm.Count(),gm.Build())):0;
            CInstance* gin=g>0?api->ResolveCInstance(g):nullptr;
            if(gin) {
                SnapshotApply(g,r,true);
                setReal(g,"Parent",sharedShipNow); setReal(g,"Fore",fore); setReal(g,"Starboard",starboard);
                GmArgs add; add.AddReal(g); call("gml_Script_AddToParentsContents",gin,add);
                PlaceAboard(api,gin);
                replayedExplosions.insert(g); markExplosion(g);
                call("gml_Script_ExplodeOnShip",gin,eargs);
                CoopLog("Objects: partner's %s replayed at %.0f,%.0f (ExplodeOnShip%s)",getString(g,"Name").c_str(),fore,starboard,shown.c_str());
                GmArgs ex; ex.AddReal(g);
                if(Number(Engine(api,"instance_exists",me,ex.Count(),ex.Build()))>0.5) {
                    GmArgs rm; rm.AddReal(g); call("gml_Script_RemoveFromParentsContents",gin,rm);
                    Engine(api,"instance_destroy",gin,0,nullptr);
                }
                return;
            }
        }
        GmArgs idx; idx.AddStr(api,"oFuelBarrel"); double obj=Number(Engine(api,"asset_get_index",me,idx.Count(),idx.Build()));
        if(!(obj>=0)) return;
        GmArgs mk; mk.AddReal(0); mk.AddReal(0); mk.AddReal(obj);
        int barrel=(int)Number(Engine(api,"instance_create",me,mk.Count(),mk.Build()));
        CInstance* b=barrel>0?api->ResolveCInstance(barrel):nullptr; if(!b) return;
        setReal(barrel,"Parent",sharedShipNow); setReal(barrel,"Fore",fore); setReal(barrel,"Starboard",starboard);
        GmArgs add; add.AddReal(barrel); call("gml_Script_AddToParentsContents",b,add);
        PlaceAboard(api,b);
        detonate(barrel);
        CoopLog("Objects: partner's explosion replayed at %.0f,%.0f",fore,starboard);
        return;
    }
    if(type==MSG_GUARD_SOUND) {
        std::string key=r.str(); uint8_t n=r.get<uint8_t>(); double args[2]{};
        for(int i=0;i<n && i<2;i++) args[i]=r.get<double>();
        if(!r.ok || n<1 || n>2) return;
        int guard=key=="@"?*puppet:GuardsGuardByKey(key.c_str()); if(!live(guard)) return;
        if(*player<=0 || (int)getReal(*player,"Parent")!=sharedShipNow) return;
        CInstance* self=api->ResolveCInstance(guard);
        GmArgs chk; chk.AddReal(args[0]); if(!Number(Engine(api,"audio_exists",self,chk.Count(),chk.Build()))) return;
        playingRemoteSound=true; GmArgs a; for(int i=0;i<n;i++) a.AddReal(args[i]); call("gml_Script_PlaySound",self,a); playingRemoteSound=false;
    }
}

// Test: hold the door nearest to the player open for a few seconds (a "local reason", like a guard walking through).
static int testDoor; static DWORD testDoorUntil;
const char* ObjectsTestHoldNearestDoor(double seconds) {
    static char result[96];
    if(!sharedShipNow || *player<=0) return "not aboard a shared ship";
    auto sit=byShip.find(sharedShipNow); if(sit==byShip.end()) return "no shared objects";
    double best=1e18; testDoor=0; std::string bestKey;
    for(auto& kv:sit->second) {
        int id=kv.second; if(!onShip(sharedShipNow,id) || !isDoor(id) || isAirlockDoor(id)) continue;
        double d=std::hypot(getReal(id,"Fore")-getReal(*player,"Fore"),getReal(id,"Starboard")-getReal(*player,"Starboard"));
        if(d<best){ best=d; testDoor=id; bestKey=kv.first; }
    }
    if(!testDoor) return "no door aboard";
    testDoorUntil=GetTickCount()+(DWORD)(seconds*1000);
    snprintf(result,sizeof(result),"holding %s open for %.0f s",bestKey.c_str(),seconds);
    return result;
}
// The inventory menu draws the hovered item's name in its TextColour; an item made or moved by co-op code may not have
// one yet. Give it the default before the menu reads it (and say which item it was).
static void preDrawCursorItemDetails(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self) return;
    double raw=getReal((int)self->id,"CursorItem"); int item=(int)raw;
    if(!live(item)) {
        if(raw>=100000) {   // an instance that no longer exists: the menu would read its name colour and fail
            static double lastRaw; if(raw!=lastRaw) { lastRaw=raw; CoopLog("Objects: the menu's hovered item %.0f no longer exists; cleared",raw); }
            setReal((int)self->id,"CursorItem",-4);
            api->RequestBypass();   // its details are not drawn this frame
        }
        return;
    }
    double tc=getReal(item,"TextColour");
    if(tc==tc) return;
    setReal(item,"TextColour",16777215);
    static int lastLogged; if(item!=lastLogged) { lastLogged=item; CoopLog("Objects: hovered %s (%d, key '%s', parent %g) had no name colour; set to white",objectName(item).c_str(),item,ObjectsKeyOf(item),getReal(item,"Parent")); }
}
bool ObjectsExplosionWindow(){ return GetTickCount()<explosionUntil; }
// Only things near the explosion were hit by it; other hits in the same moment are real.
bool ObjectsExplosionNear(int id){ return GetTickCount()<explosionUntil && std::hypot(getReal(id,"Fore")-explosionFore,getReal(id,"Starboard")-explosionStarboard)<700; }
static void watchKeycards() {
    static DWORD last; if(GetTickCount()-last<250 || *player<=0 || !live(*player)) return; last=GetTickCount();
    static int lastPlayer; if(lastPlayer!=*player) { lastPlayer=*player; knownCards.clear(); }
    int n=(int)getReal(*player,"PassiveInventoryCount");
    static bool first=true;
    for(int i=0;i<n;i++) {
        RValue v{}; v.type=5; api->GetVarByName(*player,"PassiveInventory",i,&v); int card=(int)Number(v);
        if(!live(card) || !isKeycard(card)) continue;
        // A new card, or a card of this ship upgraded in place (picking up a higher level raises the one already held).
        double level=getReal(card,"Security");
        auto known=knownCards.find(card);
        if(known!=knownCards.end() && !(level>known->second)) continue;
        knownCards[card]=level;
        if(first || givingKeycard) continue;   // cards held when the session started, and cards the partner gave us
        int ship=(int)getReal(card,"OriginalShip");
        Writer w; w.str(live(ship)?getString(ship,"Name"):""); w.put<double>(getReal(card,"Security"));
        NetSendReliable(MSG_KEYCARD,w.bytes.data(),w.bytes.size());
        CoopLog("Objects: you got a level %g keycard (%s); shared with your partner",getReal(card,"Security"),live(ship)?getString(ship,"Name").c_str():"?");
    }
    first=false;
}
// A keycard the player gets without a ship (the Key Cloner's copy) fails the game's own check ("Keycard created but not
// given a Ship var") and could not be shared: it is given the ship the player is on, the ship it was cloned on.
static void fixShiplessKeycards() {
    if(*player<=0 || !live(*player)) return;
    int ship=(int)getReal(*player,"Parent"); if(!live(ship) || getReal(ship,"Station")==1) return;
    int n=(int)getReal(*player,"PassiveInventoryCount");
    for(int i=0;i<n;i++) {
        RValue v{}; v.type=5; api->GetVarByName(*player,"PassiveInventory",i,&v); int card=(int)Number(v);
        if(!live(card) || !isKeycard(card)) continue;
        double s=getReal(card,"OriginalShip"); if(s==s && live((int)s)) continue;
        setReal(card,"OriginalShip",ship);
        CoopLog("Objects: a keycard without a ship (cloned?) now belongs to '%s'",getString(ship,"Name").c_str());
    }
}
void ObjectsTick(CInstance*,int sharedShip) {
    fixShiplessKeycards();
    watchKeycards();
    shortenFuses();
    spentCopyGrenades();
    { static DWORD lastGrenades; if(sharedShip && GetTickCount()-lastGrenades>100) { lastGrenades=GetTickCount(); sendNewGrenades(sharedShip); } }
    static DWORD lastScan,lastDoors;
    if(sharedShip!=sharedShipNow) {
        sharedShipNow=sharedShip; partnerDoors.clear(); resyncAll=true;
        // A game that follows: its copy may be new (rebuilt by a practice restart) while the guards' game kept its own;
        // that game is asked for everything aboard again (bodies, dropped items, doors), which it otherwise sends only
        // when its own shared ship changes.
        if(sharedShip && !GuardsIsAuthority()) { Writer w; w.str(getString(sharedShip,"Name")); NetSendReliable(MSG_OBJECTS_RESYNC,w.bytes.data(),w.bytes.size()); }
        if(sharedShip) CoopLog("Objects: sharing %zu objects on '%s'",byShip.count(sharedShip)?byShip[sharedShip].size():(size_t)0,getString(sharedShip,"Name").c_str());
    }
    if(!sharedShip) return;
    if(!pendingRemovals.empty()) { std::vector<int> retry; retry.swap(pendingRemovals); for(int id:retry) removeLocal(id); }
    collectWalkers(sharedShip);
    holdPartnerDoors(sharedShip);
    keyBoxLoot(sharedShip);
    if(testDoor && GetTickCount()<testDoorUntil && live(testDoor)) {
        setReal(testDoor,"OpenSignal",1);
        if(getReal(testDoor,"Open")!=1) { GmArgs a; a.AddReal(testDoor); call("gml_Script_OpenDoor",api->ResolveCInstance(testDoor),a); }
    }
    DWORD now=GetTickCount();
    if(now-lastDoors>=DOOR_REPORT_MS) { lastDoors=now; reportDoors(sharedShip); }
    if(now-lastScan>=SCAN_MS) { lastScan=now; scan(sharedShip); }
    // Sent a moment after both are aboard, and once more a few seconds later (the partner's game may still be setting
    // the copy up).
    { static int lootShip; static DWORD lootAt; static int lootSends;
      if(lootShip!=sharedShip) { lootShip=sharedShip; lootAt=now+1500; lootSends=0; }
      if(lootSends<2 && now>=lootAt && *player>0 && (int)getReal(*player,"Parent")==sharedShip && CoopRemoteParent()==sharedShip) { sendLoot(sharedShip); lootSends++; lootAt=now+5000; } }
    static DWORD lastKeys; if(now-lastKeys>=500) { lastKeys=now; sendKeyLevel(sharedShip); }
    static DWORD lastSentries; if(GuardsIsAuthority() && now-lastSentries>=100) { lastSentries=now; sendSentries(sharedShip); }
}
void ObjectsInit(const HS_ModApi* a,int* localPlayer,int* puppetId) {
    api=a; player=localPlayer; puppet=puppetId;
    api->SubscribeHookPost("gml_Script_FinishGeneratingShip",postFinishGeneratingShip,nullptr);
    api->SubscribeHook("gml_Script_DrawCursorItemDetails",preDrawCursorItemDetails,nullptr);
    api->SubscribeHook("gml_Script_ExplodeOnShip",preExplodeOnShip,nullptr);
    api->SubscribeHookPost("gml_Script_ExplodeOnShip",postExplodeOnShip,nullptr);
    api->SubscribeHookPost("gml_Script_PlaySound",postPlaySound,nullptr);
    api->SubscribeHook("gml_Script_DropAtMyFeet",preDropAtMyFeet,nullptr);
    api->SubscribeHook("gml_Script_SetDamageSignal",preCopyItemEffect,nullptr);
    api->SubscribeHook("gml_Script_TeleportTo",preCopyItemEffect,nullptr);
    for(const char* s_:{"gml_Script_ThrowInventoryItem","gml_Script_EjectFromInventory"}) { api->SubscribeHook(s_,preThrow,nullptr); api->SubscribeHookPost(s_,postThrow,nullptr); }
    api->SubscribeHookPost("gml_Script_DropAtMyFeet",postDropAtMyFeet,nullptr);
    for(int i=0;i<EFFECT_COUNT;i++) api->SubscribeHook(EFFECTS[i].script,preEffect,(void*)(intptr_t)i);
    api->SubscribeHook("gml_Script_UpdateUseHandle",preUpdateUseHandle,nullptr);
    api->SubscribeHookPost("gml_Script_SecurityLevel",postSecurityLevel,nullptr);
    api->SubscribeHook("gml_Script_SentryTriesToShootAt",preSentryTries,nullptr);
    api->SubscribeHookPost("gml_Script_SentryTriesToShootAt",postSentryTries,nullptr);
    api->SubscribeHookPost("gml_Script_SentryFiresBulletInDirection",postSentryFires,nullptr);
    api->SubscribeHook("gml_Script_SetTimeScale",preSetTimeScale,nullptr);
}
