#include "Presence.h"
#include "Seed.h"
#include "GalaxySync.h"
#include "Console.h"
#include "Frame.h"
#include "GmArgs.h"
#include "Missions.h"
#include "Wire.h"
#include "Report.h"
unsigned CoopRemoteAgeMs();
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static const HS_ModApi* api;
static int* player;
static const double OBJ_SHIP=164, GLOBAL_PLAYERSHIP=7;
static const double RESCUE_RANGE=500;          // pod-to-drifting-partner distance that counts as a catch
static const int KEY_MENU=119, KEY_ACCEPT=120;  // F8 menu (8: bug report), F9 accept an invite (F7 is the loader's ImGui key)
static bool menuOpen;
static const char* pendingCommand;   // queued console command, run on the next tick
static int pendingReset;             // station both players should go to (from the partner's reset)
static DWORD rescueUntil; static std::string rescuer;
// After a rescue the caught player rides in the rescuer's pod: their own pod is kept on the rescuer's, so their camera
// follows the rescuer, until the rescuer arrives somewhere (station or ship), where they are put aboard too.
static bool passenger; static std::string passengerOf;
static int dockedPartnerShip; static double dockedPartnerX,dockedPartnerY;   // partner's docked pod, in this world
static void joinPartner(CInstance* me,const PlayerState& r);
static void runChoice(CInstance* self,const PlayerState& remote,bool fresh,int choice);
static bool attachAvailable(const PlayerState& r);
static void attachToPartner(CInstance* self,const PlayerState& r);
static void startTrip(int ship);
static DWORD inviteAt; static std::string inviteFrom;
// A partner's request to move this player (reset to a station, go to a mission ship) waits for F9, like an invite.
static int requestKind, requestTarget; static DWORD requestAt; static char requestText[160];
enum { REQUEST_RESET=1, REQUEST_TRIP=2 };
static DWORD menuOpenedAt;
static char notice[200]; static DWORD noticeAt;

static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static void setReal(int id,const char* n,double d){ RValue v{}; v.type=0; v.real=d; api->SetVarByName(id,n,0x80000000,&v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static RValue call(const char* script,CInstance* self,GmArgs& a){ RValue r{}; r.type=5; api->CallScript(script,self,self,&r,a.Count(),a.Build()); return r; }
static double eng(const char* fn,CInstance* self,GmArgs& a){ return Number(Engine(api,fn,self,a.Count(),a.Build())); }
static void say(const char* fmt,...) { va_list a; va_start(a,fmt); vsnprintf(notice,sizeof(notice),fmt,a); va_end(a); noticeAt=GetTickCount(); CoopLog("Presence: %s",notice); }
static int podId() { const RValue* g=GlobalVar((int)GLOBAL_PLAYERSHIP); int id=g?(int)Number(*g):0; return id>0&&api->ResolveCInstance(id)?id:0; }
static bool live(int id){ return id>0 && api->ResolveCInstance(id); }

static int homeStation();
static int startStation;
// Galaxy stations (oStation) by name; galaxy objects load first, so ids are scanned in the low range (this also reaches
// deactivated stations, which instance_find skips).
static int stationByName(const char* name) {
    if(!name || !*name) return 0;
    for(int id=100000;id<100600;id++) {
        CInstance* inst=api->ResolveCInstance(id);
        if(!inst || inst->type!=192) continue;
        if(getString(id,"Name")==name) return id;
    }
    return 0;
}
// Ship orientation and ship-local -> world, as the game's GetX/GetY compute it (AnchorX/Y + Fore/Starboard units).
static bool shipToWorld(int ship,double fore,double starboard,double& x,double& y) {
    double ax=getReal(ship,"AnchorX"), ay=getReal(ship,"AnchorY"), fux=getReal(ship,"ForeUnitX"), fuy=getReal(ship,"ForeUnitY");
    double sux=getReal(ship,"StarboardUnitX"), suy=getReal(ship,"StarboardUnitY");
    x=ax+fore*fux+starboard*sux; y=ay+fore*fuy+starboard*suy;
    return x==x && y==y;
}
static double shipAngle(int ship) { return std::atan2(-getReal(ship,"ForeUnitY"),getReal(ship,"ForeUnitX"))*180/3.14159265358979; }

// ---- Local state ----
static int localMode() {
    if(!live(*player)) return 0;
    std::string state=getString(*player,"State");
    if(state=="Floating") return 2;
    int pod=podId(), parent=(int)getReal(*player,"Parent"), controlling=(int)getReal(*player,"Controlling");
    if(pod && (parent==pod || (state=="Controlling" && controlling==pod))) return 1;
    return 0;
}
// Slipstream: the Slipstream item in the player's inventory counts its seconds down (SecondsOfSlipstreamLeft of
// SecondsOfSlipstreamMax) while it runs, and its draw whites the screen out in proportion. Read by variable id (lookup by
// name does not reach these variables). Sent as the fraction left (0 = not slipstreaming).
static double varById(int id,const char* name,int fallback) {
    int var=api->GetVarId(name); if(var<0) var=fallback;
    RValue v{}; v.type=5; api->GetVar(id,var,(int)0x80000000,&v); double d=Number(v); return d==d?d:0;
}
static float slipstreamFraction() {
    if(!live(*player)) return 0;
    int n=(int)getReal(*player,"InventoryCount");
    for(int i=0;i<n;i++) {
        RValue v{}; v.type=5; api->GetVarByName(*player,"Inventory",i,&v); int item=(int)Number(v);
        if(!live(item)) continue;
        // The Slipstream gadget (oTimeGadget): on while SecondsOn (game seconds) has not reached its Duration.
        { double on=getReal(item,"SecondsOn"), dur=getReal(item,"Duration");
          if(on>0 && dur>0 && on<dur && getString(item,"Name").find("Slipstream")!=std::string::npos) return (float)(1-on/dur); }
        double left=varById(item,"SecondsOfSlipstreamLeft",3004);
        if(left>0) { double max=varById(item,"SecondsOfSlipstreamMax",3005); return (float)(max>0?left/max:1); }
    }
    return 0;
}
// The ship of this player's practice (the practice mission's Ship), or 0.
static int practiceShipHere() {
    if(!SeedPracticeScenario()[0]) return 0;
    const RValue* m=GlobalVar(2); int mission=m?(int)Number(*m):0;   // global Mission
    if(!live(mission)) return 0;
    int ship=(int)getReal(mission,"Ship");
    return live(ship)?ship:0;
}
static double podShownX,podShownY; static DWORD podShownAt;   // where the partner's pod was last drawn (frame-relative; test query)
void PresenceFill(PlayerState& s) {
    s.slipstream=slipstreamFraction();
    s.riding=passenger?1:0;
    snprintf(s.practice,sizeof(s.practice),"%s",SeedPracticeScenario());
    { int ship=practiceShipHere(); snprintf(s.practiceShip,sizeof(s.practiceShip),"%s",ship?getString(ship,"Name").c_str():""); }
    s.danger=-1; s.mode=0; s.station=0; s.homeStation=homeStation();
    double fx,fy; if(!live(*player) || !FrameRef(fx,fy)) return;
    int p=*player, pod=podId();
    s.mode=(uint8_t)localMode();
    int at=(s.mode==1 && pod)?pod:p;
    s.worldX=getReal(at,"xReal")-fx; s.worldY=getReal(at,"yReal")-fy;
    if(pod){ s.podX=getReal(pod,"xReal")-fx; s.podY=getReal(pod,"yReal")-fy; s.podAngle=getReal(pod,"MyImageAngle"); s.podVx=getReal(pod,"xSpeed"); s.podVy=getReal(pod,"ySpeed"); }
    if(s.mode==2) { double o=getReal(p,"SecondsOfOxygenLeft"); if(o==o) s.danger=(float)o; }
    if(getReal(p,"Bleeding")==1) { double b=getReal(p,"SecondsUntilBleedOut"); if(b==b && (s.danger<0 || b<s.danger)) s.danger=(float)b; }
    int parent=(int)getReal(p,"Parent");
    if(live(parent) && getReal(parent,"Station")==1) { int st=(int)getReal(parent,"MyStation"); if(live(st)) { s.station=st; if(!startStation) startStation=st; snprintf(s.stationName,sizeof(s.stationName),"%s",getString(st,"Name").c_str()); } }
    if(live(homeStation())) snprintf(s.homeStationName,sizeof(s.homeStationName),"%s",getString(homeStation(),"Name").c_str());
    if(pod) {
        int dockShip=(int)getReal(pod,"Parent"), airlock=(int)getReal(pod,"MyAirlock");
        if(dockShip!=pod && live(dockShip) && api->ResolveCInstance(dockShip)->type==(uint32_t)OBJ_SHIP && live(airlock)) {
            s.podDocked=1;
            snprintf(s.podShipName,sizeof(s.podShipName),"%s",getString(dockShip,"Name").c_str());
            s.podFore=getReal(pod,"Fore"); s.podStarboard=getReal(pod,"Starboard");
            s.podRelAngle=getReal(pod,"MyImageAngle")-shipAngle(dockShip);
            s.airlockFore=getReal(airlock,"Fore"); s.airlockStarboard=getReal(airlock,"Starboard");
        }
    }
}

// Where the partner is in this world. Aboard a ship or station this world shares, their position comes from their
// ship-local Fore/Starboard on our copy (copies can be rotated or moving differently in each game); otherwise from their
// position relative to the shared frame.
static bool partnerWorld(CInstance* self,const PlayerState& r,double& x,double& y) {
    double fx,fy; if(!FrameRef(fx,fy)) return false;
    if(r.mode==0 && r.parentName[0]) {
        int ship=MissionsFindShip(r.parentName,r.parentX,r.parentY);
        if(ship) {
            // GetX/GetY in plain arithmetic (they raise a GML error if the ship is still being generated).
            double ax=getReal(ship,"AnchorX"), ay=getReal(ship,"AnchorY"), fux=getReal(ship,"ForeUnitX"), fuy=getReal(ship,"ForeUnitY");
            double sux=getReal(ship,"StarboardUnitX"), suy=getReal(ship,"StarboardUnitY");
            double sx=ax+r.fore*fux+r.starboard*sux, sy=ay+r.fore*fuy+r.starboard*suy;
            if(sx==sx && sy==sy) { x=sx; y=sy; return true; }
        }
    }
    x=fx+r.worldX; y=fy+r.worldY; return true;
}

// Teleport to a galaxy station. GlitchPlayerToStation takes no argument: it always sends the player to the global
// HomeStation (id 1), so that is pointed at the target for the call and restored afterwards.
static void teleportToStation(CInstance* me,int station) {
    RValue* home=const_cast<RValue*>(GlobalVar(1));
    if(!home || GetRValueKind(*home)!=RValueKind::Real) return;
    double saved=home->real; home->real=station;
    int before=live(*player)?(int)getReal(*player,"Parent"):0;
    GmArgs none; call("gml_Script_GlitchPlayerToStation",me,none);
    home->real=saved;
    // Leaving a station ship this way does not remove it (the game expects to glitch back from a mission ship); arriving
    // by pod, DockAtStation removes the previous station's ship. Left running, its mission computer kept deleting the new
    // station's job offers as "extra" (the board opened on an empty galaxy map). It is removed the same way here.
    int after=live(*player)?(int)getReal(*player,"Parent"):0;
    if(before!=after && live(before) && getReal(before,"Station")==1) {
        CoopLog("Presence: left the station ship '%s' behind; removed as when docking elsewhere",getString(before,"Name").c_str());
        GmArgs d; d.AddReal(before); call("gml_Script_DestroyShip",api->ResolveCInstance(before),d);
    }
}

// The ship this player rides on (a copy of the partner's) arrived at a station in the partner's game, where it docked
// and was consumed. This player goes into that station too, with their pod, as the pilot did.
bool PresenceArriveAtStation(const char* stationName,const char* shipName) {
    CInstance* me=live(*player)?api->ResolveCInstance(*player):nullptr; if(!me) return false;
    int station=stationByName(stationName);
    if(!station) { say("%s docked at %s, which this game does not have",shipName,stationName); return false; }
    teleportToStation(me,station);
    say("%s docked at %s",shipName,stationName);
    return true;
}

// ---- Joining the partner ----
// Board a ship through its first airlock, the way a player floating up to an airlock arrives.
static bool boardShip(CInstance* me,int ship) {
    int airlock=(int)getReal(ship,"FirstAirlock");
    if(!live(airlock)) { GmArgs r; r.AddReal(ship); call("gml_Script_ReactivateShip",me,r); }
    if(!live(airlock)) return false;
    GmArgs leave; leave.AddReal(*player); call("gml_Script_RemoveFromParentsContents",me,leave);
    GmArgs arrive; arrive.AddReal(airlock); call("gml_Script_PlayerArrivesAtThatAirlock",me,arrive);
    return true;
}
// Fly (teleport) the local pod next to a point, matching nothing else; only while piloting it.
static bool podTo(double fx,double fy,double x,double y,double offset) {
    int pod=podId(); if(!pod || localMode()!=1) return false;
    setReal(pod,"xReal",fx+x+offset); setReal(pod,"yReal",fy+y+offset);
    setReal(pod,"xSpeed",0); setReal(pod,"ySpeed",0);
    return true;
}
static void joinPartner(CInstance* me,const PlayerState& r) {
    double fx,fy; if(!me || !FrameRef(fx,fy)) return;
    if(r.mode==0 && r.parentName[0]) {
        int station=stationByName(r.stationName);
        if(station) {
            teleportToStation(me,station);
            say("Joined %s at %s",r.name,r.stationName); return;
        }
        int ship=MissionsFindShip(r.parentName,r.parentX,r.parentY);
        // With the pod (docked at the ship's airlock) when there is one, so it is there when you want to leave.
        if(ship && podId()) { startTrip(ship); say("Joining %s on %s with your pod",r.name,r.parentName); return; }
        if(ship && boardShip(me,ship)) { say("Boarded %s to join %s",r.parentName,r.name); return; }
    }
    // Partner in their pod, drifting, or on a ship this world does not have: bring our pod next to them.
    // A drifting partner is reached directly (within catching range); otherwise park alongside.
    if(podTo(fx,fy,r.worldX,r.worldY,r.mode==2?150:700)) { say("Your pod is now next to %s",r.name); return; }
    say("Get in your pod first, then use Join partner again (F8)");
}

// ---- Resetting ----
static void goToStation(CInstance* me,int station,const char* why) {
    if(!live(station)) { say("That station is not available"); return; }
    teleportToStation(me,station);
    say("%s: %s",why,getString(station,"Name").c_str());
}
// "Home" for a reset is the station this session started at (where the crew was), else the global HomeStation.
static int homeStation() {
    if(live(startStation)) return startStation;
    const RValue* g=GlobalVar(1); return g?(int)Number(*g):0;
}
// This player's ship no longer exists (see CoopProbe's orphan check): they go to their home station right now.
bool PresenceRescueToHome() {
    CInstance* me=live(*player)?api->ResolveCInstance(*player):nullptr; int st=homeStation();
    if(!me || !live(st)) return false;
    // A ship that still lists the player in its contents (only the player's own link to it was lost) keeps them there:
    // arriving at a station that is that ship raised "Adding oPlayer to parents contents but I'm already in there!".
    // The player is taken off every ship that lists them first, as leaving it would.
    {
        GmArgs n; n.AddReal(OBJ_SHIP); int count=(int)Number(Engine(api,"instance_number",me,n.Count(),n.Build()));
        for(int i=0;i<count;i++) {
            GmArgs f; f.AddReal(OBJ_SHIP); f.AddReal(i); int ship=(int)Number(Engine(api,"instance_find",me,f.Count(),f.Build()));
            if(ship<=0) continue;
            int contents=(int)getReal(ship,"ContentsCount");
            for(int c=0;c<contents;c++) {
                RValue v{}; v.type=5; api->GetVarByName(ship,"Contents",c,&v);
                if((int)Number(v)!=*player) continue;
                setReal(*player,"Parent",ship);
                GmArgs r; r.AddReal(*player); call("gml_Script_RemoveFromParentsContents",me,r);
                CoopLog("Presence: taken off '%s', which still listed this player",getString(ship,"Name").c_str());
                break;
            }
        }
    }
    setReal(*player,"Parent",*player);   // as a player adrift: nothing is left to be removed from
    teleportToStation(me,st);
    say("Your ship was gone: back at %s",getString(st,"Name").c_str());
    int parent=(int)getReal(*player,"Parent");
    return parent!=*player && live(parent);
}
static void cancelMission(CInstance* me) {
    const RValue* m=GlobalVar(2);   // global Mission
    int mission=m?(int)Number(*m):0;
    if(!live(mission)) { say("You have no active mission"); return; }
    GmArgs none; call("gml_Script_CancelMission",api->ResolveCInstance(mission),none);
    say("Mission cancelled");
}

// ---- Messages ----
void PresenceOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    Reader r(data,size);
    if(type==MSG_INVITE) { inviteFrom=r.str(); inviteAt=GetTickCount(); say("%s asks you to join them: press F9 to accept",inviteFrom.c_str()); return; }
    if(type==MSG_GOTO_MISSION) {
        std::string name=r.str(); double x=r.get<double>(), y=r.get<double>();
        int ship=r.ok?MissionsFindShip(name.c_str(),x,y):0;
        if(!ship) { say("Your partner's mission ship %s is not here yet",name.c_str()); return; }
        requestKind=REQUEST_TRIP; requestTarget=ship; requestAt=GetTickCount();
        snprintf(requestText,sizeof(requestText),"Your partner wants to take you (with your pod) to %s - press F9 to go",name.c_str());
        return;
    }
    if(type==MSG_RESET) {
        std::string name=r.str(); int st=stationByName(name.c_str());
        if(!st) { say("Reset: station %s not found here",name.c_str()); return; }
        requestKind=REQUEST_RESET; requestTarget=st; requestAt=GetTickCount();
        snprintf(requestText,sizeof(requestText),"Your partner wants both of you back at %s - press F9 to go",name.c_str());
        return;
    }
    if(type==MSG_SCOOP_SOUND) {
        // This player's pod caught the partner: the scoop-up the partner's game played is heard here too.
        double sound=r.get<double>(); if(!r.ok || !(sound>=0) || !live(*player)) return;
        CInstance* me=api->ResolveCInstance(*player);
        GmArgs chk; chk.AddReal(sound); if(!(eng("audio_exists",me,chk)>0.5)) return;
        GmArgs a; a.AddReal(sound); a.AddReal(10); a.AddReal(0); Engine(api,"audio_play_sound",me,a.Count(),a.Build());
        CoopLog("Presence: your pod caught your partner (scoop-up sound %.0f)",sound);
        return;
    }
    if(type==MSG_RESCUE) {
        std::string who=r.str();
        if(localMode()!=2) return;
        // In this world the rescuer's pod does not exist; our own pod comes to us instead and the game scoops us up.
        rescueUntil=GetTickCount()+2000; rescuer=who;
        return;
    }
}

// ---- Docking behind the partner ----
// Each game has one pod per airlock; the partner's docked pod is only drawn here, so our pod docks to the same airlock in
// our own world, where it is free.
static bool attachAvailable(const PlayerState& r) {
    int pod=podId();
    if(!dockedPartnerShip || !pod || localMode()!=1 || (int)getReal(pod,"Parent")==dockedPartnerShip) return false;
    return std::hypot(getReal(pod,"xReal")-dockedPartnerX,getReal(pod,"yReal")-dockedPartnerY)<2500;
}
static void attachToPartner(CInstance* self,const PlayerState& r) {
    int ship=dockedPartnerShip, pod=podId(), best=0; double bestD=1e18;
    int count=(int)getReal(ship,"ContentsCount");
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
        if(!inst || inst->GetObjectName(api)!="oAirlock") continue;
        double d=std::hypot(getReal(id,"Fore")-r.airlockFore,getReal(id,"Starboard")-r.airlockStarboard);
        if(d<bestD){ bestD=d; best=id; }
    }
    if(!best) { say("No airlock found on %s",r.podShipName); return; }
    // Wake the ship, bring the pod onto the airlock (docking only succeeds from close by), then dock as the game does.
    GmArgs wake; wake.AddReal(ship); call("gml_Script_ReactivateShip",self,wake);
    setReal(pod,"xReal",getReal(best,"xReal")); setReal(pod,"yReal",getReal(best,"yReal"));
    setReal(pod,"xSpeed",0); setReal(pod,"ySpeed",0);
    GmArgs a; a.AddReal(best); call("gml_Script_DockWithAirlock",api->ResolveCInstance(pod),a);
    say("Docked behind %s's pod on %s",r.name,r.podShipName);
}

// ---- Both players to the mission ship, with their pods ----
// Each player's pod is brought to the mission ship and docked at its first airlock (each game has its own copy of the
// airlock, so both pods fit), then the player walks aboard. Steps run on consecutive frames: a pod docked at a station
// must be undocked before it can dock elsewhere.
static int tripShip, tripStep; static DWORD tripAt;
static int settleShip; static DWORD settleUntil;
static int missionShip(std::string* owner) {
    const RValue* g=GlobalVar(2); int mission=g?(int)Number(*g):0;
    if(live(mission)) { int ship=(int)getReal(mission,"Ship"); if(live(ship)) { if(owner) *owner="your"; return ship; } }
    const char* who=nullptr; int ship=MissionsPartnerShip(&who,nullptr);
    if(live(ship)) { if(owner) *owner=std::string(who?who:"partner")+"'s"; return ship; }
    return 0;
}
static void startTrip(int ship) {
    if(!podId()) { say("You have no pod to bring"); return; }
    tripShip=ship; tripStep=0; tripAt=GetTickCount();
}
static void tripTick(CInstance* me) {
    if(!tripShip) return;
    int pod=podId(), ship=tripShip;
    if(!live(ship) || !pod || GetTickCount()-tripAt>5000) { say("Could not bring your pod to the mission ship"); tripShip=0; return; }
    CInstance* podInst=api->ResolveCInstance(pod);
    if(tripStep==0) {
        // A docked pod belongs to the station (state 2); release it first.
        if(getReal(pod,"State")==2) { GmArgs none; call("gml_Script_UndockFromStation",podInst,none); }
        tripStep=1; return;
    }
    if(tripStep==1) {
        GmArgs wake; wake.AddReal(ship); call("gml_Script_ReactivateShip",me,wake);
        int airlock=(int)getReal(ship,"FirstAirlock");
        if(!live(airlock)) return;   // retried next frame until the ship is awake
        setReal(pod,"xReal",getReal(airlock,"xReal")); setReal(pod,"yReal",getReal(airlock,"yReal"));
        setReal(pod,"xSpeed",0); setReal(pod,"ySpeed",0);
        GmArgs a; a.AddReal(airlock); call("gml_Script_DockWithAirlock",podInst,a);
        tripStep=2; return;
    }
    // Walk aboard through the airlock. Undocking from the station left the player remote-controlling the pod
    // ("Controlling"); they are put back on their feet, or they would stay stuck looking at the pod.
    boardShip(me,ship);
    settleShip=ship; settleUntil=GetTickCount()+4000;
    say("You and your pod are at %s",getString(ship,"Name").c_str());
    tripShip=0;
}
// The docking finishes over the next frames and puts the player back to remote-controlling the pod; while they are
// aboard the ship, they are put on their feet again until it settles.
static void settleTick(CInstance* me) {
    if(!settleShip) return;
    if(GetTickCount()>settleUntil) { settleShip=0; return; }
    // Boarding lands a moment later than the call (the airlock arrival): wait for it rather than giving up at once, which
    // left the player sitting in their docked pod ("Controlling"): the playtest report "put into their own pod" on docking.
    if((int)getReal(*player,"Parent")!=settleShip) {
        // Still sitting in the pod (arriving from drifting in space, the boarding did not take; seen by the session
        // recorder: state Controlling on the pod, 2026-10-05): board again through the airlock the pod docked at.
        static DWORD lastBoard;
        if(getString(*player,"State")=="Controlling" && GetTickCount()-lastBoard>500) {
            lastBoard=GetTickCount(); setReal(*player,"Controlling",49);
            if(boardShip(me,settleShip)) CoopLog("Presence: still in the pod after docking at %s; boarded again",getString(settleShip,"Name").c_str());
        }
        return;
    }
    if(getString(*player,"State")=="Controlling") {
        // What the player controls goes too (noone, 49): left pointing at the docked pod, the game put them back in it
        // every frame.
        setReal(*player,"Controlling",49);
        GmArgs st; st.AddStr(api,"Free"); call("gml_Script_ChangeStateTo",me,st);
        static DWORD lastLog; if(GetTickCount()-lastLog>1000) { lastLog=GetTickCount(); CoopLog("Presence: out of the docked pod and aboard %s",getString(settleShip,"Name").c_str()); }
    }
}

// ---- Menu ----
static void runChoice(CInstance* self,const PlayerState& remote,bool fresh,int choice) {
    switch(choice) {
    case 1: if(fresh) joinPartner(self,remote); else say("No partner connected"); break;
    case 2: if(fresh) { Writer w; w.str(getString(*player,"Name")); NetSendReliable(MSG_INVITE,w.bytes.data(),w.bytes.size()); say("Asked %s to join you",remote.name); } else say("No partner connected"); break;
    case 3: goToStation(self,homeStation(),"Back at your starting station"); break;
    case 4: {
        // Both players start over at the host's home station (galaxy station ids match across games).
        int st=NetIsHost()?homeStation():(fresh?stationByName(remote.homeStationName):0);
        if(!live(st)) { say("The host's starting station is not known yet"); break; }
        if(fresh) { Writer w; w.str(getString(st,"Name")); NetSendReliable(MSG_RESET,w.bytes.data(),w.bytes.size()); }
        goToStation(self,st,fresh?"Asked your partner to come too; you are at":"Back at");
        break; }
    case 5: cancelMission(self); break;
    case 6: {
        std::string owner; int ship=missionShip(&owner);
        if(!ship) { say("Neither of you has an active mission"); break; }
        double fx=0,fy=0; FrameRef(fx,fy);
        if(fresh) { Writer w; w.str(getString(ship,"Name")); w.put<double>(getReal(ship,"xReal")-fx); w.put<double>(getReal(ship,"yReal")-fy); NetSendReliable(MSG_GOTO_MISSION,w.bytes.data(),w.bytes.size()); }
        startTrip(ship);
        say("Going to %s mission ship %s%s",owner.c_str(),getString(ship,"Name").c_str(),fresh?" (your partner is asked to come)":"");
        break; }
    case 7:
        if(NetIsHost()) { say("Only the partner downloads the host's galaxy"); break; }
        GalaxySyncRequest(); say("Downloading the host's galaxy - restart the game when it says so");
        break;
    case 8: ReportSave(""); say(fresh?"Bug report saved, with your partner's log (coop-reports folder in the game folder)":"Bug report saved (coop-reports folder in the game folder); no partner connected"); break;
    default: break;
    }
}

// ---- Per frame ----
void PresenceTick(CInstance* self,const PlayerState& remote,bool fresh) {
    if(!self) return;
    if(pendingCommand) {
        std::string c=pendingCommand; free((void*)pendingCommand); pendingCommand=nullptr;
        if(c=="join") runChoice(self,remote,fresh,1);
        else if(c=="invite") runChoice(self,remote,fresh,2);
        else if(c=="home") runChoice(self,remote,fresh,3);
        else if(c=="reset") runChoice(self,remote,fresh,4);
        else if(c=="cancel") runChoice(self,remote,fresh,5);
        else if(c=="mission") runChoice(self,remote,fresh,6);
        else if(c=="accept" && inviteAt) { inviteAt=0; if(fresh) joinPartner(self,remote); }
        else if(c=="attach") { if(attachAvailable(remote)) attachToPartner(self,remote); else say("Not near your partner's docked pod (or not flying your pod)"); }
        else if(c=="menu") menuOpen=!menuOpen;
        else if(c=="report") runChoice(self,remote,fresh,8);
        else if(c=="status") { void CoopRequestStatus(); CoopRequestStatus(); }
        else if(c=="podshown") CoopLog("podshown %.2f %.2f age %u",podShownX,podShownY,(unsigned)(GetTickCount()-podShownAt));
        else if(c=="nocrew") { void CoopForceNoCrew(); CoopForceNoCrew(); say("(test) the next new life treats no character as choosable"); }
        else if(c=="caught") { if(localMode()==2) { rescueUntil=GetTickCount()+2000; rescuer=remote.name; say("(test) caught by %s's pod",remote.name); } else say("Not drifting"); }
    }
    GmArgs menuKey; menuKey.AddReal(KEY_MENU);
    if(eng("keyboard_check_pressed",self,menuKey)) { menuOpen=!menuOpen; menuOpenedAt=GetTickCount(); }
    if(menuOpen && GetTickCount()-menuOpenedAt>15000) menuOpen=false;
    if(menuOpen) {
        int choice=0;
        for(int k=1;k<=9 && !choice;k++){
            GmArgs key; key.AddReal('0'+k);
            if(eng("keyboard_check_pressed",self,key)) { choice=k; GmArgs clr; clr.AddReal('0'+k); eng("keyboard_clear",self,clr); }   // the game does not see it
        }
        if(choice) { menuOpen=false; runChoice(self,remote,fresh,choice); }
    }
    tripTick(self); settleTick(self);
    // The partner's mission is done and they are home: a player still aboard that mission ship comes along.
    if(const char* fin=MissionsFinishedShip(); fin && *fin && fresh) {
        int parent=(int)getReal(*player,"Parent");
        if(getString(parent,"Name")!=fin) MissionsClearFinished();
        else if(remote.mode==0 && remote.stationName[0]) {
            int st=stationByName(remote.stationName);
            if(st) { teleportToStation(self,st); say("%s finished the mission; you came back with them to %s",remote.name,remote.stationName); MissionsClearFinished(); }
        }
    }
    if(pendingReset) { int st=pendingReset; pendingReset=0; goToStation(self,st,"Reset by your partner, now at"); }
    GmArgs accept; accept.AddReal(KEY_ACCEPT);
    bool acceptPressed=eng("keyboard_check_pressed",self,accept)!=0;
    if(requestKind && GetTickCount()-requestAt>30000) requestKind=0;
    if(requestKind && acceptPressed) {
        int kind=requestKind; requestKind=0; acceptPressed=false;
        if(kind==REQUEST_RESET && live(requestTarget)) goToStation(self,requestTarget,"Back at");
        if(kind==REQUEST_TRIP && live(requestTarget)) startTrip(requestTarget);
    }
    if(inviteAt && GetTickCount()-inviteAt<30000 && acceptPressed) { inviteAt=0; acceptPressed=false; if(fresh) joinPartner(self,remote); }
    if(acceptPressed && attachAvailable(remote)) attachToPartner(self,remote);
    // Being rescued: keep our pod on top of us (undocking it first) until the game's scoop-up catches us.
    if(rescueUntil) {
        int pod=podId();
        if(localMode()!=2 || !pod || GetTickCount()>rescueUntil) {
            if(localMode()==1) { say("%s caught you with their pod - riding along (F9: take your own pod)",rescuer.c_str()); passenger=true; passengerOf=rescuer; }
            rescueUntil=0;
        } else {
            // A station-docked pod (state 2) is held by the station; undock it once (the game then flies it, state 0).
            if(getReal(pod,"State")==2) { GmArgs none; call("gml_Script_UndockFromStation",api->ResolveCInstance(pod),none); }
            setReal(pod,"xReal",getReal(*player,"xReal")); setReal(pod,"yReal",getReal(*player,"yReal"));
            setReal(pod,"xSpeed",getReal(*player,"xSpeed")); setReal(pod,"ySpeed",getReal(*player,"ySpeed"));
        }
    }
    // Practising together: both games built the same practice ship (Seed.cpp), but its name is drawn differently in each;
    // the partner who started the practice first keeps theirs, the one who joined takes it.
    if(fresh && remote.practice[0] && remote.practiceShip[0] && !strcmp(SeedPracticeScenario(),remote.practice) && SeedPracticeJoined()) {
        int ship=practiceShipHere();
        if(ship && getString(ship,"Name")!=remote.practiceShip) {
            CoopLog("Presence: practice ship '%s' takes the partner's name '%s'",getString(ship,"Name").c_str(),remote.practiceShip);
            RValue v{}; v.type=1; api->SetString(&v,remote.practiceShip); api->SetVarByName(ship,"Name",0x80000000,&v);
        }
    }
    // The partner riding in this player's pod (they were caught drifting): their own pod is not drawn beside ours.
    { static bool was; bool now=fresh && remote.riding; if(now!=was) CoopLog(now?"Presence: partner rides in your pod":"Presence: partner left your pod"); was=now; }
    // Riding in the partner's pod.
    if(passenger) {
        int pod=podId(); double pfx,pfy;
        if(!fresh || !pod || localMode()==2 || acceptPressed) {
            passenger=false;
            if(acceptPressed) say("You took your own pod");
        } else if(remote.podDocked && remote.podShipName[0] && (dockedPartnerShip || (dockedPartnerShip=MissionsFindShip(remote.podShipName,0,0)))) {
            // The pilot docked at a ship's airlock: this player's own pod docks at the same airlock in this world (the
            // game's DockWithAirlock), so both arrive there, and each later leaves in their own pod, undocking only it.
            // (Boarding through the ship's first airlock left this player's pod out in space.)
            attachToPartner(self,remote);
            say("Arrived at %s with %s",remote.podShipName,passengerOf.c_str());
            passenger=false;
        } else if(remote.mode==1 && FrameRef(pfx,pfy)) {
            if(getReal(pod,"State")==2) { GmArgs none; call("gml_Script_UndockFromStation",api->ResolveCInstance(pod),none); }
            // Where the partner's pod is now: their last report carried on by its velocity (set from each report alone,
            // the pod jumped back at every packet and flew on by itself between them).
            double age=CoopRemoteAgeMs()/1000.0; if(age>0.3) age=0.3;
            setReal(pod,"xReal",pfx+remote.podX+remote.podVx*age); setReal(pod,"yReal",pfy+remote.podY+remote.podVy*age);
            setReal(pod,"xSpeed",remote.podVx); setReal(pod,"ySpeed",remote.podVy);
            setReal(pod,"MyImageAngle",remote.podAngle);
        } else if(remote.mode==0) {
            // The rescuer arrived: come aboard where they are.
            int station=stationByName(remote.stationName);
            if(station) { teleportToStation(self,station); say("Arrived at %s with %s",remote.stationName,passengerOf.c_str()); }
            else { int ship=MissionsFindShip(remote.parentName,remote.parentX,remote.parentY); if(ship && boardShip(self,ship)) say("Arrived at %s with %s",remote.parentName,passengerOf.c_str()); }
            passenger=false;
        } else passenger=false;
    }
    // Rescue: our pod reached the partner drifting in space.
    static DWORD lastRescue;
    double fx,fy;
    if(fresh && remote.mode==2 && localMode()==1 && GetTickCount()-lastRescue>2000 && FrameRef(fx,fy)) {
        int pod=podId();
        double d=std::hypot(getReal(pod,"xReal")-fx-remote.worldX,getReal(pod,"yReal")-fy-remote.worldY);
        if(d<RESCUE_RANGE) { lastRescue=GetTickCount(); Writer w; w.str(getString(*player,"Name")); NetSendReliable(MSG_RESCUE,w.bytes.data(),w.bytes.size()); say("You caught %s!",remote.name); }
    }
}

// ---- Drawing ----
static void colour(CInstance* self,int bgr){ GmArgs c; c.AddReal(bgr); Engine(api,"draw_set_colour",self,c.Count(),c.Build()); }
static void text(CInstance* self,double x,double y,const char* s){ GmArgs a; a.AddReal(x); a.AddReal(y); a.AddStr(api,s); Engine(api,"draw_text",self,a.Count(),a.Build()); }
static void align(CInstance* self,int h,int v){ GmArgs a; a.AddReal(h); Engine(api,"draw_set_halign",self,a.Count(),a.Build()); GmArgs b; b.AddReal(v); Engine(api,"draw_set_valign",self,b.Count(),b.Build()); }
static void triangle(CInstance* self,double x1,double y1,double x2,double y2,double x3,double y3) {
    GmArgs a; a.AddReal(x1); a.AddReal(y1); a.AddReal(x2); a.AddReal(y2); a.AddReal(x3); a.AddReal(y3); a.AddReal(0);
    Engine(api,"draw_triangle",self,a.Count(),a.Build());
}
static std::string distanceText(double d) { char b[32]; if(d>=1e6) snprintf(b,sizeof(b),"%.1fM",d/1e6); else if(d>=1e3) snprintf(b,sizeof(b),"%.1fk",d/1e3); else snprintf(b,sizeof(b),"%.0f",d); return b; }
// Points at a world position: a marker above it when on screen, otherwise an arrow on the screen edge.
static void pointAt(CInstance* self,double wx,double wy,double meX,double meY,int bgr,const std::string& label,double w,double h) {
    GmArgs gx; gx.AddReal(wx); gx.AddReal(wy); GmArgs gy; gy.AddReal(wx); gy.AddReal(wy);
    double x=Number(call("gml_Script_GUIxOfXY",self,gx)), y=Number(call("gml_Script_GUIyOfXY",self,gy));
    if(!(x==x) || !(y==y)) return;
    std::string line=label+"  "+distanceText(std::hypot(wx-meX,wy-meY));
    colour(self,bgr);
    const double margin=48;
    if(x>margin && x<w-margin && y>margin && y<h-margin) {
        triangle(self,x-12,y-34,x+12,y-34,x,y-16);
        align(self,1,2); text(self,x,y-38,line.c_str()); align(self,0,0);
        return;
    }
    double cx=w/2, cy=h/2, dx=x-cx, dy=y-cy;
    double scale=std::fmin((cx-margin)/std::fmax(std::fabs(dx),1e-6),(cy-margin)/std::fmax(std::fabs(dy),1e-6));
    double ex=cx+dx*scale, ey=cy+dy*scale, len=std::hypot(dx,dy); double ux=dx/len, uy=dy/len;
    triangle(self,ex+ux*22,ey+uy*22,ex-uy*14,ey+ux*14,ex+uy*14,ey-ux*14);
    align(self,ex>cx?2:0,ey>cy?2:0); text(self,ex-ux*24,ey-uy*24,line.c_str()); align(self,0,0);
}
// The partner's pod as the game draws pods (sPlayerShip at the pod's Scale), projected through the camera: position,
// zoom and rotation all come from the game's own GUIxOfXY/GUIyOfXY.
static void drawPod(CInstance* self,double wx,double wy,double angle);
// The partner's slipstream: a fading vapour trail behind their pod, along its flight direction.
static void drawSlipstream(CInstance* self,double wx,double wy,double vx,double vy) {
    double speed=std::hypot(vx,vy); if(speed<1) return;
    double ux=-vx/speed, uy=-vy/speed;
    for(int i=0;i<12;i++) {
        double a0=i*180.0, a1=(i+1)*180.0;
        GmArgs p0; p0.AddReal(wx+ux*a0); p0.AddReal(wy+uy*a0); GmArgs q0; q0.AddReal(wx+ux*a0); q0.AddReal(wy+uy*a0);
        GmArgs p1; p1.AddReal(wx+ux*a1); p1.AddReal(wy+uy*a1); GmArgs q1; q1.AddReal(wx+ux*a1); q1.AddReal(wy+uy*a1);
        double sx=Number(call("gml_Script_GUIxOfXY",self,p0)), sy=Number(call("gml_Script_GUIyOfXY",self,q0));
        double tx=Number(call("gml_Script_GUIxOfXY",self,p1)), ty=Number(call("gml_Script_GUIyOfXY",self,q1));
        if(!(sx==sx) || !(tx==tx)) return;
        GmArgs al; al.AddReal(0.6*(1.0-i/12.0)); Engine(api,"draw_set_alpha",self,al.Count(),al.Build());
        GmArgs col; col.AddReal(0xffe8c0); Engine(api,"draw_set_colour",self,col.Count(),col.Build());
        GmArgs l; l.AddReal(sx); l.AddReal(sy); l.AddReal(tx); l.AddReal(ty); l.AddReal(6*(1.0-i/12.0)+1); Engine(api,"draw_line_width",self,l.Count(),l.Build());
    }
    GmArgs one; one.AddReal(1); Engine(api,"draw_set_alpha",self,one.Count(),one.Build());
}
static void drawPod(CInstance* self,double wx,double wy,double angle) {
    static double sprite=-1; if(sprite<0){ GmArgs o; o.AddReal(163); sprite=eng("object_get_sprite",self,o); }
    if(!(sprite>=0)) return;
    const double rad=angle*3.14159265358979/180;
    double ax=wx+std::cos(rad)*100, ay=wy-std::sin(rad)*100;
    GmArgs x0; x0.AddReal(wx); x0.AddReal(wy); GmArgs y0; y0.AddReal(wx); y0.AddReal(wy);
    GmArgs x1; x1.AddReal(ax); x1.AddReal(ay); GmArgs y1; y1.AddReal(ax); y1.AddReal(ay);
    double sx=Number(call("gml_Script_GUIxOfXY",self,x0)), sy=Number(call("gml_Script_GUIyOfXY",self,y0));
    double tx=Number(call("gml_Script_GUIxOfXY",self,x1)), ty=Number(call("gml_Script_GUIyOfXY",self,y1));
    if(!(sx==sx) || !(tx==tx)) return;
    double pxPerUnit=std::hypot(tx-sx,ty-sy)/100.0;
    double scale=0.32*pxPerUnit; if(scale<0.03) scale=0.03;   // stays visible when zoomed far out
    double rot=std::atan2(-(ty-sy),tx-sx)*180/3.14159265358979;
    GmArgs d; d.AddReal(sprite); d.AddReal(0); d.AddReal(sx); d.AddReal(sy); d.AddReal(scale); d.AddReal(scale); d.AddReal(rot); d.AddReal(0xffffff); d.AddReal(1);
    Engine(api,"draw_sprite_ext",self,d.Count(),d.Build());
}
void PresenceDraw(CInstance* self,const PlayerState& remote,bool fresh) {
    if(!self || !live(*player)) return;
    GmArgs none; double w=Number(Engine(api,"display_get_gui_width",self,0,none.Build())), h=Number(Engine(api,"display_get_gui_height",self,0,none.Build()));
    if(!(w>0) || !(h>0)) return;
    double fx,fy; if(!FrameRef(fx,fy)) return;
    double meX=getReal(*player,"xReal"), meY=getReal(*player,"yReal");
    int pod=podId(); if(localMode()==1 && pod){ meX=getReal(pod,"xReal"); meY=getReal(pod,"yReal"); }
    DWORD now=GetTickCount(); bool blink=(now/300)%2==0;
    // Partner's mission ship (orange).
    const char *owner=nullptr,*type=nullptr; int ship=MissionsPartnerShip(&owner,&type);
    // Not drawn while the partner is aboard it: their own arrow says so.
    bool partnerAboard=fresh && remote.mode==0 && ship && getString(ship,"Name")==remote.parentName;
    if(ship && !partnerAboard) {
        char label[160]; snprintf(label,sizeof(label),"%s's mission%s%s",owner,type&&*type?": ":"",type?type:"");
        pointAt(self,getReal(ship,"xReal"),getReal(ship,"yReal"),meX,meY,0x2a9cff,label,w,h);
    }
    // Partner (cyan; flashing red while in danger). Not drawn when the puppet already shows them on our ship.
    if(fresh && remote.inGameplay) {
        int parent=(int)getReal(*player,"Parent");
        bool sameShip=remote.mode==0 && remote.parentName[0] && getString(parent,"Name")==remote.parentName;
        bool danger=remote.danger>=0;
        // The partner's docked pod is drawn attached to our copy of that ship, at the same airlock. Once we are aboard that
        // ship too (or our own pod is docked there), only our own pod is shown: one pod for both players, and each player
        // still leaves in their own.
        {
            int dockShip=remote.podDocked?MissionsFindShip(remote.podShipName,0,0):0;
            int myPod=podId(), myAirlock=myPod?(int)getReal(myPod,"MyAirlock"):0;
            bool together=dockShip && (dockShip==parent || (live(myAirlock) && (int)getReal(myAirlock,"Parent")==dockShip));
            double dx,dy;
            if(dockShip && shipToWorld(dockShip,remote.podFore,remote.podStarboard,dx,dy)) {
                if(!together) drawPod(self,dx,dy,shipAngle(dockShip)+remote.podRelAngle);
                dockedPartnerShip=dockShip; dockedPartnerX=dx; dockedPartnerY=dy;
            } else {
                dockedPartnerShip=0;
                // Docked to a ship this game does not have (yet): their pod is still shown where it is, not as a
                // drifting player.
                if(remote.podDocked && remote.mode!=1) drawPod(self,fx+remote.podX,fy+remote.podY,remote.podAngle);
            }
        }
        // Riding in each other's pod: the pods are one, no arrow.
        bool together=passenger || (remote.riding && localMode()==1) || (remote.mode==1 && localMode()==1 && podId() && std::hypot(getReal(podId(),"xReal")-fx-remote.podX,getReal(podId(),"yReal")-fy-remote.podY)<400);
        if((!sameShip || danger) && !together) {
            const char *o=nullptr,*ty=nullptr; int ms=MissionsPartnerShip(&o,&ty);
            bool onMission=remote.mode==0 && ms && getString(ms,"Name")==remote.parentName;
            char label[160]; snprintf(label,sizeof(label),"%s%s",remote.name,remote.mode==1?" (pod)":remote.mode==2?" (drifting)":onMission?" (on their mission)":"");
            double px,py; if(!partnerWorld(self,remote,px,py)) { px=fx+remote.worldX; py=fy+remote.worldY; }
            if(remote.mode==1 && dockedPartnerShip) { px=dockedPartnerX; py=dockedPartnerY; }
            if(remote.mode==1 && !dockedPartnerShip) {
                // Extrapolate by the pod's velocity since the packet, then ease the drawn pod toward that.
                static double shownX,shownY; static bool have; static DWORD last;
                double age=CoopRemoteAgeMs()/1000.0; if(age>0.3) age=0.3;
                double tx=remote.podX+remote.podVx*age, ty=remote.podY+remote.podVy*age;
                DWORD nowMs=GetTickCount(); double dt=last?(nowMs-last)/1000.0:0; last=nowMs;
                if(!have || std::hypot(shownX-tx,shownY-ty)>3000) { shownX=tx; shownY=ty; have=true; }
                double k=dt*15; if(k>1) k=1; shownX+=(tx-shownX)*k; shownY+=(ty-shownY)*k;
                int myPod=podId();
                bool riding=localMode()==1 && myPod && std::hypot(fx+shownX-getReal(myPod,"xReal"),fy+shownY-getReal(myPod,"yReal"))<400;
                { static bool was; bool now=!riding && remote.slipstream>0; if(now!=was) CoopLog("Presence: partner's slipstream %s",now?"shown":"ended"); was=now; }
                if(!riding && remote.slipstream>0) drawSlipstream(self,fx+shownX,fy+shownY,remote.podVx,remote.podVy);
                if(!riding) drawPod(self,fx+shownX,fy+shownY,remote.podAngle);
                px=fx+shownX; py=fy+shownY;
                podShownX=shownX; podShownY=shownY; podShownAt=GetTickCount();
            }
            pointAt(self,px,py,meX,meY,danger?(blink?0x3030ff:0xffffff):0xffd040,label,w,h);
        }
        if(danger) {
            char line[200];
            snprintf(line,sizeof(line),remote.mode==2?"%s is drifting in space: %.0fs of oxygen left - catch them with your pod!":"%s is bleeding out: %.0fs left!",remote.name,remote.danger);
            colour(self,blink?0x3030ff:0xffffff); align(self,1,2); text(self,w/2,h-70,line); align(self,0,0);
        }
    }
    // The partner is practising: this player's practice terminal is highlighted, with how to join them (the same practice
    // builds the same ship in both games, so starting it here puts both on one ship).
    if(fresh && remote.inGameplay && remote.practice[0] && strcmp(SeedPracticeScenario(),remote.practice)) {
        static double obj=-2; if(obj==-2) { GmArgs n; n.AddStr(api,"oPracticeTerminal"); obj=eng("asset_get_index",self,n); }
        int parent=(int)getReal(*player,"Parent"), term=0;
        if(obj>=0) { GmArgs c; c.AddReal(obj); int count=(int)eng("instance_number",self,c);
            for(int i=0;i<count && !term;i++) { GmArgs f; f.AddReal(obj); f.AddReal(i); int id=(int)eng("instance_find",self,f); if(id>0 && (int)getReal(id,"Parent")==parent) term=id; } }
        char label[200];
        if(SeedPracticeScenario()[0]) snprintf(label,sizeof(label),"%s practises '%s' (you: '%s')",remote.name,remote.practice,SeedPracticeScenario());
        else snprintf(label,sizeof(label),"%s is practising: talk to the practice terminal to join",remote.name);
        if(term) pointAt(self,getReal(term,"xReal"),getReal(term,"yReal"),meX,meY,blink?0x60ff60:0x30c030,label,w,h);
        colour(self,0x60ff60); align(self,1,2); text(self,w/2,h-120,label); align(self,0,0);
        static std::string logged; if(logged!=remote.practice) { logged=remote.practice; CoopLog("Presence: partner practises '%s'%s",remote.practice,term?"; terminal highlighted":""); }
    }
    // Co-op menu, invite prompt, notices.
    colour(self,0xffffff);
    if(menuOpen) {
        const char* lines[]={"CO-OP  (F8 to close)","1  Join my partner (with my pod)","2  Ask my partner to come to me",
            "3  Return to my starting station","4  Both of us to the host's starting station","5  Cancel my current mission",
            "6  Both of us (with our pods) to the active mission ship",
            NetIsHost()?"7  -":"7  Download the host's galaxy (then restart)","8  Save a bug report (both players' logs)","9  Close"};
        for(int i=0;i<10;i++) text(self,w/2-300,h/2-90+i*26,lines[i]);
    }
    if(requestKind && now-requestAt<30000) { colour(self,0x40ffff); align(self,1,0); text(self,w/2,60,requestText); align(self,0,0); }
    if(inviteAt && now-inviteAt<30000) { colour(self,0x40ffff); align(self,1,0); char b[160]; snprintf(b,sizeof(b),"%s asks you to join them - press F9 to accept",inviteFrom.c_str()); text(self,w/2,90,b); align(self,0,0); }
    if(fresh && attachAvailable(remote)) { colour(self,0x40ffff); align(self,1,0); char b[160]; snprintf(b,sizeof(b),"Press F9 to dock behind %s's pod",remote.name); text(self,w/2,150,b); align(self,0,0); }
    if(noticeAt && now-noticeAt<5000) { colour(self,0x80ff80); align(self,1,0); text(self,w/2,120,notice); align(self,0,0); }
    colour(self,0xffffff);
}
// A rider (caught by the partner's pod) does not fly it: the game's own check for "may this player steer the pod".
static void prePlayerCanControlPod(const char*,CInstance*,CInstance*,RValue* result,int,RValue**,void*) {
    if(!passenger) return;
    if(result) { result->type=0; result->real=0; }
    api->RequestBypass();
}
bool PresenceRiding() { return passenger; }
// The game's scoop-up (CanBeScoopedUp, this player caught by their pod) plays its sounds; when the partner's pod is the
// one that caught them (a rescue), those sounds go to the partner too.
static bool inScoop;
static void preScoop(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { inScoop=rescueUntil!=0; }
static void postScoop(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { inScoop=false; }
static void postScoopSound(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!inScoop || argc<1 || !argv || !argv[0]) return;
    Writer w; w.put<double>(Number(*argv[0])); NetSendReliable(MSG_SCOOP_SOUND,w.bytes.data(),w.bytes.size());
}
void PresenceInit(const HS_ModApi* a,int* localPlayer) {
    api=a; player=localPlayer;
    api->SubscribeHook("gml_Script_PlayerCanControlPod",prePlayerCanControlPod,nullptr);
    api->SubscribeHook("gml_Script_CanBeScoopedUp",preScoop,nullptr);
    api->SubscribeHookPost("gml_Script_CanBeScoopedUp",postScoop,nullptr);
    api->SubscribeHookPost("gml_Script_PlaySound",postScoopSound,nullptr);
}
// Console equivalents of the menu (test windows have no keyboard focus): join, invite, accept.
void PresenceCommand(const char* what) { pendingCommand=_strdup(what); }
