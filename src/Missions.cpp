#include "Hull.h"
#include "Missions.h"
#include "Console.h"
#include "GmArgs.h"
#include "Net.h"
#include "Snapshot.h"
#include "Wire.h"
#include "Frame.h"
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <array>
#include <set>
#include <string>
#include <vector>

static const HS_ModApi* api;
static int* player;
static const double OBJ_SHIP=164;
static const double REWARD_SHARE=0.5, REWARD_MINIMUM=10;   // the partner receives half the pay, at least 10
static bool inAccept, applyingRemote;
static std::string acceptedType;   // type of the mission being accepted locally
static std::deque<std::vector<uint8_t>> pendingShips;
static std::set<int> partnerShips;   // ships generated from the partner's missions
static std::set<int> ownShips;
static std::set<int> trafficShips;        // host: traffic ships this game spawned (sent to the partner)
static bool inSpawn;
static std::string finishedShipName; static DWORD finishedAt;
// Where the partner's game has each of its ships: followed smoothly every frame.
// vx/vy/spin are the game's own speed variables (set on the copy so the game moves it the same way); mvx/mvy/mspin are
// the ship's measured motion per second on the owner, used to predict where it is now (more exact than the speed
// variables, which only approximate the motion when the ship accelerates or turns).
struct ShipTrack { double x,y,angle,vx,vy,spin,mvx,mvy,mspin; double at; };
// Milliseconds on the precise clock: GetTickCount moves in ~15.6 ms steps, which (times a fast ship's speed) made the
// copy's target jump about 190 units at a time, and the copy move unevenly (choppy) as it followed.
static double preciseMs() { static LARGE_INTEGER f; LARGE_INTEGER n; if(!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&n); return n.QuadPart*1000.0/f.QuadPart; }
static std::map<int,ShipTrack> tracks;
static std::map<int,std::vector<uint8_t>> ownShipMessages;   // the generation message of each own ship, for a reconnect       // ships generated for this player's missions (this game flies them for both)
static int latestPartnerShip; static std::string latestOwner, latestType; static DWORD latestPartnerAt;
static char status[160]="no shared missions";

static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static void setReal(int id,const char* n,double d){ RValue v{}; v.type=0; v.real=d; api->SetVarByName(id,n,0x80000000,&v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static RValue call(const char* script,CInstance* self,GmArgs& a){ RValue r{}; r.type=5; api->CallScript(script,self,self,&r,a.Count(),a.Build()); return r; }

int MissionsFindShip(const char* name,double x,double y) {
    if(!name || !*name) return 0;
    // Called every frame from several places; ships do not change identity, so the answer is reused for half a second.
    struct Found { int id; DWORD at; }; static std::map<std::string,Found> cache;
    auto hit=cache.find(name);
    if(hit!=cache.end() && GetTickCount()-hit->second.at<500 && (!hit->second.id || api->ResolveCInstance(hit->second.id))) return hit->second.id;
    int found=0;
    struct Store { std::map<std::string,Found>& c; std::string n; int& f; ~Store(){ c[n]={f,GetTickCount()}; } } store{cache,name,found};
    CInstance* self=api->ResolveCInstance(*player); if(!self) return 0;
    GmArgs n; n.AddReal(OBJ_SHIP);
    int count=(int)Number(Engine(api,"instance_number",self,n.Count(),n.Build()));
    int best=0; double bestDistance=1e18;
    for(int i=0;i<count;i++) {
        GmArgs f; f.AddReal(OBJ_SHIP); f.AddReal(i);
        int id=(int)Number(Engine(api,"instance_find",self,f.Count(),f.Build()));
        if(id<=0 || getString(id,"Name")!=name) continue;
        double fx=0,fy=0; FrameRef(fx,fy);
        double d=std::hypot(getReal(id,"xReal")-fx-x,getReal(id,"yReal")-fy-y);
        if(!(d==d)) d=1e17;
        if(d<bestDistance){ bestDistance=d; best=id; }
    }
    // Ships are matched by name; x/y (relative to the frame reference) only choose between same-named ships, since
    // ships fly independently in each world.
    found=best;
    return best;
}

// ---- Owner side ----
static void preAcceptMission(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    inAccept=true;
    acceptedType=(argc>0 && argv && argv[0])?getString((int)Number(*argv[0]),"Type"):"";
}
// AcceptMission runs with the accepting player as self and the mission as argument0.
static void postAcceptMission(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    inAccept=false;
    if(applyingRemote || argc<1 || !argv || !argv[0]) return;
    int mission=(int)Number(*argv[0]);
    snprintf(status,sizeof(status),"your mission: %s (shared with partner)",getString(mission,"Type").c_str());
}
// The game names a ship during its generation, from a random draw that is not the same in both games: the owner's
// final name follows the generation message, and the copy takes it.
static std::map<int,std::string> namesBefore;
static uint64_t messageHash(const std::vector<uint8_t>& m){ uint64_t h=1469598103934665603ull; for(uint8_t b:m) h=(h^b)*1099511628211ull; return h; }
static std::map<uint64_t,int> copyOfMessage;   // partner ship copies by the generation message they came from
static void postGenerateShip(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self) return;
    auto it=namesBefore.find((int)self->id); if(it==namesBefore.end()) return;
    std::string after=getString(self->id,"Name"); namesBefore.erase(it);
    auto msg=ownShipMessages.find((int)self->id); if(msg==ownShipMessages.end()) return;
    Writer w; w.put<uint64_t>(messageHash(msg->second)); w.str(after); NetSendReliable(MSG_SHIP_RENAME,w.bytes.data(),w.bytes.size());
}
static void preGenerateShip(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    // Only ships generated for a locally accepted mission are shared; the partner's own arrive through messages.
    bool traffic=inSpawn;
    if((!inAccept && !traffic) || applyingRemote || !self) return;
    // A practice ship is not sent: a partner who starts the same practice builds the same ship (Seed.cpp), and a copy
    // sent along was drawn over the partner's station and stayed after the practice ended.
    if(!traffic && acceptedType=="Practice") { CoopLog("Mission: practice ship not shared"); return; }
    Writer w;
    w.str(traffic?"":getString(*player,"Name"));
    w.str(traffic?"":acceptedType);
    w.put<double>(getReal(self->id,"FactionIndex"));
    double fx=0,fy=0; FrameRef(fx,fy); w.put<double>(fx); w.put<double>(fy);
    SnapshotCapture(self->id,w);
    ownShips.insert((int)self->id); ownShipMessages[(int)self->id]=w.bytes;
    namesBefore[(int)self->id]=getString(self->id,"Name");
    if(traffic) trafficShips.insert((int)self->id);
    NetSendReliable(MSG_MISSION_SHIP,w.bytes.data(),w.bytes.size());
    if(traffic) { CoopLog("Traffic: ship spawned here, created in the partner's game too"); return; }
    CoopLog("Mission: shared ship generation for accepted mission (faction %g at %.0f,%.0f, %u bytes)",getReal(self->id,"FactionIndex"),getReal(self->id,"xReal"),getReal(self->id,"yReal"),(unsigned)w.bytes.size());
}
static void postCompleteMission(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(applyingRemote || argc<1 || !argv || !argv[0]) return;
    int mission=(int)Number(*argv[0]);
    // The game calls this on more than one frame for the same mission; it is reported (and paid) once.
    static std::set<int> reported; if(!reported.insert(mission).second) return;
    double pay=getReal(mission,"Pay"), bonus=getReal(mission,"Bonus");
    if(std::isnan(pay)) pay=0; if(std::isnan(bonus)) bonus=0;
    // Personal missions keep their pay on the mission ship.
    int ship=(int)getReal(mission,"Ship");
    if(ship>0 && api->ResolveCInstance(ship)) { double shipPay=getReal(ship,"Pay"); if(shipPay==shipPay && shipPay>pay) pay=shipPay; }
    Writer w; w.str(getString(*player,"Name")); w.str(getString(mission,"Type")); w.put<double>(pay+bonus);
    w.str(ship>0 && api->ResolveCInstance(ship)?getString(ship,"Name"):"");
    NetSendReliable(MSG_MISSION_DONE,w.bytes.data(),w.bytes.size());
    CoopLog("Mission: completed %s paying %g; partner share sent",getString(mission,"Type").c_str(),pay+bonus);
}

// A failed mission is reported to the partner too (they see it, and stop keeping its ship).
static int failingMission;
static void preFailMission(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) {
    const RValue* g=GlobalVar(2); failingMission=g?(int)Number(*g):0;
}
static void postFailMission(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) {
    int mission=failingMission; failingMission=0;
    if(!api->ResolveCInstance(mission)) return;
    static std::set<int> reported; if(!reported.insert(mission).second) return;
    int ship=(int)getReal(mission,"Ship");
    Writer w; w.str(getString(*player,"Name")); w.str(getString(mission,"Type"));
    w.str(ship>0 && api->ResolveCInstance(ship)?getString(ship,"Name"):"");
    NetSendReliable(MSG_MISSION_FAILED,w.bytes.data(),w.bytes.size());
    CoopLog("Mission: %s failed; partner told",getString(mission,"Type").c_str());
}
static char banner[160]; static DWORD bannerUntil; static bool bannerFailed;
const char* MissionsBanner(bool* failed) { if(GetTickCount()>bannerUntil) return nullptr; if(failed) *failed=bannerFailed; return banner; }

// ---- Partner side ----
struct Rename { uint64_t message; std::string after; DWORD until; };
static std::vector<Rename> pendingRenames;
static void applyRenames() {
    for(auto it=pendingRenames.begin();it!=pendingRenames.end();) {
        bool done=false;
        auto c=copyOfMessage.find(it->message);
        int ship=c!=copyOfMessage.end()?c->second:0;
        if(ship && api->ResolveCInstance(ship)) {
            std::string before=getString(ship,"Name");
            if(before!=it->after) {
                RValue v{}; v.type=1; api->SetString(&v,it->after.c_str()); api->SetVarByName(ship,"Name",0x80000000,&v);
                if(ship==latestPartnerShip) snprintf(status,sizeof(status),"%s's mission ship: %s",latestOwner.c_str(),it->after.c_str());
                CoopLog("Mission: partner's ship came out '%s' here and '%s' in their game; renamed to match",before.c_str(),it->after.c_str());
            }
            done=true;
        }
        if(done || GetTickCount()>it->until) it=pendingRenames.erase(it); else ++it;
    }
}
// ---- A ship that docks at a station is consumed there, in both games ----
// When a ship flown to a station arrives (its autopilot, or the player at the helm), the game's DockAtStation moves the
// local player into the station, hands over objectives and captives, and destroys the ship. Only the game that flies
// the ship does that. The other game was told nothing: its copy stopped being steered, kept its last speed, and flew on
// with the passenger aboard. Now the arrival is announced before the ship is destroyed, and the other game takes its
// player (and pod) into the same station and removes the copy.
bool PresenceArriveAtStation(const char* stationName,const char* shipName);
static void preDockAtStation(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!self || argc<1 || !argv || !argv[0] || !ownShips.count((int)self->id)) return;
    int station=(int)Number(*argv[0]); if(station<100000 || !api->ResolveCInstance(station)) return;
    Writer w; w.str(getString((int)self->id,"Name")); w.str(getString(station,"Name"));
    NetSendReliable(MSG_SHIP_ARRIVED,w.bytes.data(),w.bytes.size());
    CoopLog("Mission: '%s' docks at %s; the partner is told",getString((int)self->id,"Name").c_str(),getString(station,"Name").c_str());
}
static void receiveShipArrived(Reader& r) {
    std::string shipName=r.str(), stationName=r.str(); if(!r.ok) return;
    int copy=0; for(int ship:partnerShips) if(api->ResolveCInstance(ship) && getString(ship,"Name")==shipName) { copy=ship; break; }
    if(!copy) { CoopLog("Mission: partner's '%s' docked at %s (no copy of it here)",shipName.c_str(),stationName.c_str()); return; }
    const RValue* g=GlobalVar(7); int pod=g?(int)Number(*g):0;
    bool aboard=(*player>0 && (int)getReal(*player,"Parent")==copy) || (pod>0 && api->ResolveCInstance(pod) && (int)getReal(pod,"Parent")==copy);
    // No longer steered or kept: it must not fly on.
    tracks.erase(copy);
    for(const char* v:{"xSpeed","ySpeed","xSpeedDesired","ySpeedDesired","Spin"}) setReal(copy,v,0);
    if(aboard && !PresenceArriveAtStation(stationName.c_str(),shipName.c_str())) { CoopLog("Mission: could not follow '%s' into %s; staying aboard the copy",shipName.c_str(),stationName.c_str()); return; }
    partnerShips.erase(copy); if(copy==latestPartnerShip) { latestPartnerShip=0; snprintf(status,sizeof(status),"no shared missions"); }
    CInstance* inst=api->ResolveCInstance(copy);
    bool stillAboard=(*player>0 && (int)getReal(*player,"Parent")==copy);
    if(inst && !stillAboard) { GmArgs d; d.AddReal(copy); call("gml_Script_DestroyShip",inst,d); }
    CoopLog("Mission: partner's '%s' docked at %s; %s",shipName.c_str(),stationName.c_str(),aboard?"this player went into the station with it":"its copy here is removed");
}
// ---- Mission boards ----
// The game wipes every open offer in one go (DestroyExtraMissions: on accepting a mission, when a board regenerates, and
// whenever any mission computer is destroyed, e.g. with a station left behind). A board that had already generated its
// list then pointed at offers that no longer exist and showed an empty map with only BACK, because it still counted as
// generated. Every board is marked ungenerated after a wipe, so the next use makes a fresh list (the game's own path).
static const double OBJ_MISSION_COMPUTER=182;
static void postDestroyExtraMissions(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    CInstance* any=self; if(!any && *player>0) any=api->ResolveCInstance(*player); if(!any) return;
    GmArgs n; n.AddReal(OBJ_MISSION_COMPUTER); int count=(int)Number(Engine(api,"instance_number",any,n.Count(),n.Build()));
    for(int i=0;i<count;i++) {
        GmArgs f; f.AddReal(OBJ_MISSION_COMPUTER); f.AddReal(i); int id=(int)Number(Engine(api,"instance_find",any,f.Count(),f.Build()));
        if(id>=100000 && api->ResolveCInstance(id)) setReal(id,"GeneratedMissions",0);
    }
}
void MissionsOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    if(type==MSG_SHIP_ARRIVED) { Reader r(data,size); receiveShipArrived(r); return; }
    if(type==MSG_SHIP_RENAME) {
        Reader r(data,size); uint64_t message=r.get<uint64_t>(); std::string after=r.str();
        if(r.ok) pendingRenames.push_back({message,after,GetTickCount()+30000});
        return;
    }
    if(type==MSG_MISSION_SHIP) { pendingShips.emplace_back(data,data+size); return; }
    if(type==MSG_SHIP_MOVE) {
        // The partner's game flies this ship; this copy follows it every frame (see followShips).
        Reader r(data,size); std::string name=r.str();
        double x=r.get<double>(), y=r.get<double>(), angle=r.get<double>(), vx=r.get<double>(), vy=r.get<double>(), spin=r.get<double>();
        if(!r.ok) return;
        double mvx=r.get<double>(), mvy=r.get<double>(), mspin=r.get<double>(); if(!r.ok) { mvx=mvy=mspin=0; }
        for(int ship:partnerShips) if(api->ResolveCInstance(ship) && getString(ship,"Name")==name) { tracks[ship]={x,y,angle,vx,vy,spin,mvx,mvy,mspin,preciseMs()}; return; }
        return;
    }
    if(type==MSG_SHIP_GONE) {
        Reader r(data,size); std::string name=r.str(); if(!r.ok) return;
        for(auto it=partnerShips.begin();it!=partnerShips.end();) {
            if(api->ResolveCInstance(*it) && getString(*it,"Name")==name && *it!=latestPartnerShip) { tracks.erase(*it); it=partnerShips.erase(it); CoopLog("Traffic: '%s' is gone in the partner's game; released here",name.c_str()); }
            else ++it;
        }
        return;
    }
    if(type==MSG_MISSION_FAILED) {
        Reader r(data,size); std::string who=r.str(), what=r.str(), shipName=r.str();
        if(!r.ok) return;
        if(!shipName.empty()) { finishedShipName=shipName; finishedAt=GetTickCount(); }
        snprintf(status,sizeof(status),"%s's mission failed: %s",who.c_str(),what.c_str());
        snprintf(banner,sizeof(banner),"%s's mission failed: %s",who.c_str(),what.c_str()); bannerFailed=true; bannerUntil=GetTickCount()+12000;
        CoopLog("Mission: partner %s's %s failed",who.c_str(),what.c_str());
        return;
    }
    if(type==MSG_MISSION_DONE) {
        Reader r(data,size); std::string who=r.str(), what=r.str(); double pay=r.get<double>(); std::string shipName=r.str();
        if(!r.ok || *player<=0) return;
        // The mission is over: its ship is no longer kept alive here, and a player still aboard it goes where the
        // partner is (Presence brings them over once the partner has arrived somewhere).
        if(!shipName.empty()) { finishedShipName=shipName; finishedAt=GetTickCount(); }
        double share=std::floor(pay*REWARD_SHARE); if(share<REWARD_MINIMUM) share=REWARD_MINIMUM;
        double money=getReal(*player,"Money"); if(std::isnan(money)) money=0;
        setReal(*player,"Money",money+share);
        snprintf(status,sizeof(status),"%s completed %s: +%g to you",who.c_str(),what.c_str(),share);
        snprintf(banner,sizeof(banner),"%s's mission complete: %s  (+%g to you)",who.c_str(),what.c_str(),share); bannerFailed=false; bannerUntil=GetTickCount()+12000;
        CoopLog("Mission: partner %s completed %s; received share %g (money %g -> %g)",who.c_str(),what.c_str(),share,money,money+share);
    }
}
static void generatePartnerShip(const std::vector<uint8_t>& message) {
    CInstance* self=api->ResolveCInstance(*player); if(!self) return;
    Reader r(message.data(),message.size());
    std::string owner=r.str(); std::string missionType=r.str(); double faction=r.get<double>();
    double senderFx=r.get<double>(), senderFy=r.get<double>();
    if(!r.ok) return;
    // Sent again after a reconnect: the same message yields the copy already made.
    uint64_t messageId=messageHash(message);
    { static std::set<uint64_t> seen; if(!seen.insert(messageId).second) { CoopLog("Mission: partner's ship already here (sent again after a reconnect)"); return; } }
    GmArgs c; c.AddReal(0); c.AddReal(0); c.AddReal(OBJ_SHIP);
    int ship=(int)Number(Engine(api,"instance_create",self,c.Count(),c.Build()));
    CInstance* inst=api->ResolveCInstance(ship);
    if(!inst){ CoopLog("Mission: could not create partner ship"); return; }
    GmArgs f; f.AddReal(faction); call("gml_Script_SetFactionFromIndex",inst,f);
    Reader body=r; int applied=SnapshotApply(ship,body);
    // Move the ship from the owner's coordinate frame into ours.
    double fx=0,fy=0;
    if(FrameRef(fx,fy)) { setReal(ship,"xReal",getReal(ship,"xReal")-senderFx+fx); setReal(ship,"yReal",getReal(ship,"yReal")-senderFy+fy); }
    // The owner's ship got its name when its job offer was made; generating the copy here picks a new random one, so the
    // owner's name is put back (everything matched by ship name depends on it).
    std::string ownerName=getString(ship,"Name");
    applyingRemote=true;
    GmArgs none; call("gml_Script_GenerateShip",inst,none);
    applyingRemote=false;
    if(!ownerName.empty() && getString(ship,"Name")!=ownerName) {
        CoopLog("Mission: copy came out named '%s'; named '%s' as in the owner's game",getString(ship,"Name").c_str(),ownerName.c_str());
        RValue v{}; v.type=1; api->SetString(&v,ownerName.c_str()); api->SetVarByName(ship,"Name",0x80000000,&v);
    }
    // A ship is its own "original" (pieces split off it keep pointing back). The game looks keycards up by that id:
    // the owner's value arrived as an id of the owner's game, which is nothing here, so on a copy no key ever counted
    // (SecurityLevel answered 0 and every locked door stayed shut for the partner).
    setReal(ship,"OriginalID",ship);
    partnerShips.insert(ship); copyOfMessage[messageId]=ship;
    std::string name=getString(ship,"Name");
    if(owner.empty()) { CoopLog("Traffic: '%s' created (spawned in the partner's game)",name.c_str()); return; }
    latestPartnerShip=ship; latestOwner=owner; latestType=missionType; latestPartnerAt=GetTickCount();
    snprintf(status,sizeof(status),"%s's mission ship: %s",owner.c_str(),name.c_str());
    CoopLog("Mission: generated %s's mission ship %d '%s' (%d values) at %.0f,%.0f",owner.c_str(),ship,name.c_str(),applied,getReal(ship,"xReal"),getReal(ship,"yReal"));
}
// Each frame the copy is placed where the partner's ship is now (last report moved on by its velocity), approached
// gently so the view never jumps, also while this player is aboard: the ship takes them along (home, too).
// Partner copies of ships are moved inside the game's own ship integration (a pre-hook on UpdateSpacePosition, which
// each ship runs once per frame in End Step right before it integrates and rebuilds its anchor; the people and objects
// aboard are then placed from that anchor). There the copy gets the owner's true speeds and turn rate (the copy's own
// autopilot would set its turn rate otherwise) plus a small correction toward where the owner's ship is now, aimed at the
// end-of-frame prediction and sized in game time (slow-mo and pause slow it like everything else).
// Earlier, writes from the mod's tick (sometimes in Draw, after the anchor was rebuilt) drew sections out of place and slid
// the partner around; pacing by wall-clock moved the copy at full speed during the partner's aim slow-mo, which led a
// locked melee aim off in the direction of travel; inflating the speeds to steer jolted people out into space.
static void preUpdateSpacePosition(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self) return;
    int ship=(int)self->id;
    auto it=tracks.find(ship); if(it==tracks.end() || !partnerShips.count(ship)) return;
    const ShipTrack& tr=it->second;
    double ms=preciseMs(); if(ms-tr.at>3000) return;
    double fx=0,fy=0; if(!FrameRef(fx,fy)) return;
    GmArgs none; RValue gs{}; gs.type=5; api->CallScript("gml_Script_GameSecondsPassed",self,self,&gs,none.Count(),none.Build());
    double g=Number(gs); if(!(g>=0) || g>0.5) g=0;
    double age=(ms-tr.at)/1000.0;
    double tx=fx+tr.x+tr.mvx*age, ty=fy+tr.y+tr.mvy*age, ta=tr.angle+tr.mspin*age;
    setReal(ship,"xSpeed",tr.vx); setReal(ship,"ySpeed",tr.vy); setReal(ship,"xSpeedDesired",tr.vx); setReal(ship,"ySpeedDesired",tr.vy);
    setReal(ship,"Spin",tr.spin);
    // Where the game will put it this frame, and how far that is from the owner's ship now.
    double px=getReal(ship,"xReal")+tr.vx*g, py=getReal(ship,"yReal")+tr.vy*g, pa=getReal(ship,"MyImageAngle")+tr.spin*g;
    double ex=tx-px, ey=ty-py, ea=std::fmod(ta-pa+540.0,360.0)-180.0;
    bool jump=std::hypot(ex,ey)>30000;
    // The gap is measured against the wall clock, but each frame the game moves the ship a fixed game-time amount, and real
    // frames vary (12-29 ms): fed straight in, every long frame's gap became a visible lurch. Smoothed over several frames
    // and corrected gently, only the lasting difference is closed.
    static std::map<int,std::array<double,3>> gap;
    auto& e=gap[ship];
    if(jump) e={0,0,0}; else if(g>0) { e[0]+=(ex-e[0])*0.05; e[1]+=(ey-e[1])*0.05; e[2]+=(ea-e[2])*0.05; }
    if(!jump) { ex=e[0]; ey=e[1]; if(std::fabs(ea)<=30) ea=e[2]; }
    double k=jump?1.0:1.0-std::exp(-g*1.5);   // game time: no correction while paused
    if(ex!=0 || ey!=0) { setReal(ship,"xReal",getReal(ship,"xReal")+ex*k); setReal(ship,"yReal",getReal(ship,"yReal")+ey*k); }
    if(std::fabs(ea)>0.0001) setReal(ship,"MyImageAngle",std::fmod(getReal(ship,"MyImageAngle")+ea*(std::fabs(ea)>30?1.0:k)+360.0,360.0));
}
static void followShips() {
    double now=preciseMs();
    for(auto it=tracks.begin();it!=tracks.end();) {
        if(!api->ResolveCInstance(it->first) || !partnerShips.count(it->first) || now-it->second.at>3000) it=tracks.erase(it); else ++it;
    }
}
// A finished partner mission's ship is let go (the game may then remove it) only once this player is off it: removing a
// ship with the player aboard breaks the game.
static void releaseFinishedShip() {
    static std::string pending; static DWORD since;
    if(!finishedShipName.empty()) { pending=finishedShipName; since=GetTickCount(); }
    if(pending.empty()) return;
    const RValue* g=GlobalVar(7); int pod=g?(int)Number(*g):0;
    for(auto it=partnerShips.begin();it!=partnerShips.end();) {
        int ship=*it;
        if(!api->ResolveCInstance(ship) || getString(ship,"Name")!=pending) { ++it; continue; }
        bool aboard=(*player>0 && (int)getReal(*player,"Parent")==ship) || (pod>0 && api->ResolveCInstance(pod) && (int)getReal(pod,"Parent")==ship);
        if(aboard) return;
        // The copy stays protected from culling: run unprotected, its own ship logic reaches for data only the owner's
        // game has (oBridgeModule failed on an undefined distance). Only its mission marker goes.
        if(ship==latestPartnerShip) { latestPartnerShip=0; snprintf(status,sizeof(status),"no shared missions"); CoopLog("Mission: the partner's mission on '%s' is over",pending.c_str()); }
        ++it;
    }
    if(finishedShipName.empty() || GetTickCount()-since>600000) pending.clear();
}
bool MissionsShipIsCopy(int ship) { return partnerShips.count(ship)>0; }
// A piece split off a ship: flown here (own) or following the partner's game (copy), like the ship it came from.
void MissionsAdoptPiece(int ship,bool own) { if(own) ownShips.insert(ship); else partnerShips.insert(ship); }
void MissionsTick(CInstance*) {
    HullTick();
    if(!pendingRenames.empty()) applyRenames();
    followShips();
    releaseFinishedShip();
    static DWORD lastMove;
    if(GetTickCount()-lastMove>=100) {
        lastMove=GetTickCount();
        double fx=0,fy=0;
        for(auto it=ownShips.begin();it!=ownShips.end();) {
            int ship=*it;
            if(!api->ResolveCInstance(ship)) { it=ownShips.erase(it); continue; }
            ++it;
            if(!FrameRef(fx,fy)) break;
            Writer w; w.str(getString(ship,"Name"));
            w.put<double>(getReal(ship,"xReal")-fx); w.put<double>(getReal(ship,"yReal")-fy); w.put<double>(getReal(ship,"MyImageAngle"));
            w.put<double>(getReal(ship,"xSpeed")); w.put<double>(getReal(ship,"ySpeed")); w.put<double>(getReal(ship,"Spin"));
            // Measured motion since the last report (per second of wall time), for the copy's prediction.
            struct Last { double x,y,a; LARGE_INTEGER t; }; static std::map<int,Last> last;
            static LARGE_INTEGER freq; if(!freq.QuadPart) QueryPerformanceFrequency(&freq);
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            double x=getReal(ship,"xReal"), y=getReal(ship,"yReal"), a=getReal(ship,"MyImageAngle"), mvx=0, mvy=0, mspin=0;
            auto l=last.find(ship);
            if(l!=last.end()) {
                double dt=(double)(now.QuadPart-l->second.t.QuadPart)/freq.QuadPart;
                if(dt>0.02 && dt<1.0) { mvx=(x-l->second.x)/dt; mvy=(y-l->second.y)/dt; mspin=(std::fmod(a-l->second.a+540.0,360.0)-180.0)/dt; }
            }
            last[ship]={x,y,a,now};
            w.put<double>(mvx); w.put<double>(mvy); w.put<double>(mspin);
            NetSendUnreliable(MSG_SHIP_MOVE,w.bytes.data(),w.bytes.size());
        }
    }
    if(pendingShips.empty() || *player<=0) return;
    std::vector<uint8_t> next=std::move(pendingShips.front()); pendingShips.pop_front();
    generatePartnerShip(next);
}
// Ship culling destroys any ship that is not important to the local player. Ships created for the partner's game stay
// until the partner's game releases them; the host also keeps the ships the partner is on or near.
bool CoopRemoteWorld(double& x,double& y);
int CoopRemoteParent();
static void preIsImportant(const char*,CInstance* self,CInstance*,RValue* result,int,RValue**,void*) {
    if(!self || !result) return;
    int id=(int)self->id; bool keep=partnerShips.count(id)>0 || CoopRemoteParent()==id;
    if(!keep && ownShips.count(id)) { double px,py,fx,fy; if(CoopRemoteWorld(px,py) && FrameRef(fx,fy)) keep=std::hypot(getReal(id,"xReal")-fx-px,getReal(id,"yReal")-fy-py)<150000; }
    if(keep){ result->type=0; result->real=1; api->RequestBypass(); }
}
// Traffic: the host's game spawns the passing ships for both players (each spawned ship is created in the partner's
// game the same way as mission ships); the client's own spawner is off while the host is connected.
bool CoopPartnerConnected();
// Far from the host (more than two station spacings) the client spawns its own traffic around itself, sent to the host
// the same way, so neither player flies through empty space.
static const double OWN_TRAFFIC_DISTANCE=1200000;
static void preSpawnShipsAtIntervals(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) {
    if(NetIsHost() || !CoopPartnerConnected()) return;
    double px,py,fx,fy;
    if(CoopRemoteWorld(px,py) && FrameRef(fx,fy) && *player>0 &&
       std::hypot(getReal(*player,"xReal")-fx-px,getReal(*player,"yReal")-fy-py)>OWN_TRAFFIC_DISTANCE) return;
    api->RequestBypass();
}
static void preSpawnShip(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { inSpawn=true; }
// Spawned ships are generated a step per frame; the first step (inside SpawnShip) is where the ship is captured.
static void preGenerateShip(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*);
static void preGenerateShipInSteps(const char* n,CInstance* self,CInstance* o,RValue* r,int argc,RValue** argv,void* u) {
    if(inSpawn && argc>0 && argv && argv[0] && Number(*argv[0])==1) preGenerateShip(n,self,o,r,argc,argv,u);
}
static void postSpawnShip(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { inSpawn=false; }
static void preDestroyShipMissions(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*);
void MissionsInit(const HS_ModApi* a,int* localPlayer) {
    api=a; player=localPlayer;
    api->SubscribeHook("gml_Script_UpdateSpacePosition",preUpdateSpacePosition,nullptr);
    api->SubscribeHook("gml_Script_DockAtStation",preDockAtStation,nullptr);
    api->SubscribeHookPost("gml_Script_DestroyExtraMissions",postDestroyExtraMissions,nullptr);
    api->SubscribeHook("gml_Script_AcceptMission",preAcceptMission,nullptr);
    api->SubscribeHookPost("gml_Script_AcceptMission",postAcceptMission,nullptr);
    api->SubscribeHook("gml_Script_GenerateShip",preGenerateShip,nullptr);
    api->SubscribeHookPost("gml_Script_GenerateShip",postGenerateShip,nullptr);
    api->SubscribeHookPost("gml_Script_CompleteMission",postCompleteMission,nullptr);
    api->SubscribeHook("gml_Script_FailMission",preFailMission,nullptr);
    api->SubscribeHookPost("gml_Script_FailMission",postFailMission,nullptr);
    api->SubscribeHook("gml_Script_IsImportant",preIsImportant,nullptr);
    api->SubscribeHook("gml_Script_SpawnShipsAtIntervals",preSpawnShipsAtIntervals,nullptr);
    api->SubscribeHook("gml_Script_SpawnShip",preSpawnShip,nullptr);
    api->SubscribeHookPost("gml_Script_SpawnShip",postSpawnShip,nullptr);
    api->SubscribeHook("gml_Script_GenerateShipInSteps",preGenerateShipInSteps,nullptr);
    api->SubscribeHook("gml_Script_DestroyShip",preDestroyShipMissions,nullptr);
}
const char* CoopRemoteMissionShip();
const char* MissionsStatus() {
    // Rebuilt from what is true now: this player's active mission, and the partner's (cleared when they no longer have it).
    std::string own;
    { const RValue* g=GlobalVar(2); int m=g?(int)Number(*g):0; if(api->ResolveCInstance(m)) { std::string type=getString(m,"Type"); if(!type.empty()) own="your mission: "+type; } }
    if(latestPartnerShip && api->ResolveCInstance(latestPartnerShip)) {
        const char* theirs=CoopRemoteMissionShip();
        if(theirs && GetTickCount()-latestPartnerAt>5000 && strcmp(theirs,getString(latestPartnerShip,"Name").c_str())) latestPartnerShip=0;   // their mission is over
    } else latestPartnerShip=0;
    std::string partner=latestPartnerShip?latestOwner+"'s mission: "+(latestType.empty()?getString(latestPartnerShip,"Name"):latestType):"";
    snprintf(status,sizeof(status),"%s%s%s",own.c_str(),!own.empty()&&!partner.empty()?" | ":"",partner.c_str());
    return status;
}
// Test: tells the partner this player's current mission is complete (as a real completion does), without completing it.
void MissionsTestDone() {
    const RValue* g=GlobalVar(2); int mission=g?(int)Number(*g):0; if(!api->ResolveCInstance(mission)) { CoopLog("Mission: no active mission"); return; }
    int ship=(int)getReal(mission,"Ship");
    Writer w; w.str(getString(*player,"Name")); w.str(getString(mission,"Type")); w.put<double>(0);
    w.str(ship>0 && api->ResolveCInstance(ship)?getString(ship,"Name"):"");
    NetSendReliable(MSG_MISSION_DONE,w.bytes.data(),w.bytes.size());
    CoopLog("Mission: test completion sent");
}
const char* MissionsFinishedShip() { return GetTickCount()-finishedAt<60000?finishedShipName.c_str():""; }
void MissionsClearFinished() { finishedShipName.clear(); }
// A ship this game sent to the partner is being removed (culled, destroyed): the partner's copy is released too.
static void preDestroyShipMissions(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || !ownShips.count((int)self->id)) return;
    Writer w; w.str(getString((int)self->id,"Name")); NetSendReliable(MSG_SHIP_GONE,w.bytes.data(),w.bytes.size());
    ownShips.erase((int)self->id); trafficShips.erase((int)self->id); ownShipMessages.erase((int)self->id);
}
void MissionsResend() {
    for(auto it=ownShipMessages.begin();it!=ownShipMessages.end();) {
        if(!api->ResolveCInstance(it->first)) { it=ownShipMessages.erase(it); continue; }
        NetSendReliable(MSG_MISSION_SHIP,it->second.data(),it->second.size());
        { Writer w; w.put<uint64_t>(messageHash(it->second)); w.str(getString(it->first,"Name")); NetSendReliable(MSG_SHIP_RENAME,w.bytes.data(),w.bytes.size()); }
        ++it;
    }
}
int MissionsPartnerShip(const char** owner,const char** type) {
    if(latestPartnerShip && !api->ResolveCInstance(latestPartnerShip)) latestPartnerShip=0;
    if(owner) *owner=latestOwner.c_str(); if(type) *type=latestType.c_str();
    return latestPartnerShip;
}
