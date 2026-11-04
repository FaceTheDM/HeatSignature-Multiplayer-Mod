#include "TestMenu.h"
#include "Guards.h"
#include "Objects.h"
#include "Console.h"
#include "Place.h"
#include "GmArgs.h"
#include "Missions.h"
#include "Presence.h"
#include "Wire.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static const HS_ModApi* api;
static int* player;
static bool open;
static std::string pending;          // command to run on the next tick (console, menu or partner)
static DWORD boardPartnerAt;         // retry window for "board the partner's mission ship"
static char notice[200]; static DWORD noticeAt;
static const int KEY_TEST=121;       // F10
static const double OBJ_PLAYER=137, OBJ_SHIP=164, OBJ_MISSION=27, GLOBAL_PLAYERSHIP=7, GLOBAL_GODMODE=127;

static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static void setReal(int id,const char* n,double d){ RValue v{}; v.type=0; v.real=d; api->SetVarByName(id,n,0x80000000,&v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static RValue call(const char* script,CInstance* self,GmArgs& a){ RValue r{}; r.type=5; api->CallScript(script,self,self,&r,a.Count(),a.Build()); return r; }
static bool live(int id){ return id>0 && api->ResolveCInstance(id); }
static void say(const char* fmt,...) { va_list a; va_start(a,fmt); vsnprintf(notice,sizeof(notice),fmt,a); va_end(a); noticeAt=GetTickCount(); CoopLog("Test: %s",notice); }
static int contentOfType(int ship,const char* object,double nearFore=NAN,double nearStarboard=NAN) {
    int count=(int)getReal(ship,"ContentsCount"), best=0; double bestD=1e18;
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item);
        int id=(int)Number(item); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
        if(!inst || inst->GetObjectName(api)!=object) continue;
        double d=std::isnan(nearFore)?0:std::hypot(getReal(id,"Fore")-nearFore,getReal(id,"Starboard")-nearStarboard);
        if(d<bestD){ bestD=d; best=id; }
    }
    return best;
}
static bool boardShip(CInstance* me,int ship) {
    int airlock=(int)getReal(ship,"FirstAirlock");
    if(!live(airlock)) { GmArgs r; r.AddReal(ship); call("gml_Script_ReactivateShip",me,r); }
    if(!live(airlock)) return false;
    GmArgs leave; leave.AddReal(*player); call("gml_Script_RemoveFromParentsContents",me,leave);
    GmArgs arrive; arrive.AddReal(airlock); call("gml_Script_PlayerArrivesAtThatAirlock",me,arrive);
    return true;
}

// ---- Actions ----
static void getInPod(CInstance* me) {
    int p=*player, parent=(int)getReal(p,"Parent");
    if(getString(p,"State")=="Controlling") { say("Already flying your pod"); return; }
    // On a station: the game's own route, the lift to the docking bay.
    int lift=live(parent)?contentOfType(parent,"oStationDockLift"):0;
    if(lift) {
        setReal(p,"Fore",getReal(lift,"Fore")); setReal(p,"Starboard",getReal(lift,"Starboard"));
        GmArgs a; a.AddReal(lift); call("gml_Script_InteractWith",me,a);
        pending="launch";   // next frame: undock, so the pod is flying free
        say("Took the lift to your pod"); return;
    }
    // Elsewhere: back to the pod the way the game does when your module is destroyed.
    GmArgs none; call("gml_Script_PlayerReturnToShip",me,none);
    say(getString(p,"State")=="Controlling"?"Back in your pod":"Could not reach your pod from here (try from a station)");
}
static void eject(CInstance* me) {
    // Aboard a ship: step into its airlock and open it, as a player would.
    int parent=(int)getReal(*player,"Parent"), airlock=live(parent)?(int)getReal(parent,"FirstAirlock"):0;
    if(live(airlock) && getString(*player,"State")!="Controlling") {
        double d=std::hypot(getReal(*player,"Fore")-getReal(airlock,"Fore"),getReal(*player,"Starboard")-getReal(airlock,"Starboard"));
        if(d>60) { boardShip(me,parent); pending="eject"; say("Walking to the airlock..."); return; }   // arrive at it the game's way
        GmArgs a; a.AddReal(airlock); call("gml_Script_InteractWith",me,a);
        say("Opened the airlock: you are drifting, your partner should see your oxygen timer"); return;
    }
    const RValue* g=GlobalVar((int)GLOBAL_PLAYERSHIP); int pod=g?(int)Number(*g):0;
    if(!live(pod) || getString(*player,"State")!="Controlling") { say("Get in your pod first (test 1)"); return; }
    // Ejecting next to a station or ship just lands you on it: first move the pod into empty space, eject next frame.
    setReal(pod,"xReal",getReal(pod,"xReal")+400000); setReal(pod,"yReal",getReal(pod,"yReal")+400000);
    setReal(pod,"xSpeed",0); setReal(pod,"ySpeed",0);
    pending="eject2";
    say("Moving your pod into empty space...");
}
// Accept the first unstarted mission offered here and board its ship.
static bool newMissionAndBoard(CInstance* me,bool board=true) {
    // The game allows one active mission; accepting another (even right after cancelling) breaks its mission checks.
    { const RValue* g=GlobalVar(2); int active=g?(int)Number(*g):0;
      if(live(active) && getReal(active,"Started")==1) { say("You already have an active mission: finish it, or cancel it first (F8, then 5)"); return false; } }
    GmArgs n; n.AddReal(OBJ_MISSION);
    int count=(int)Number(Engine(api,"instance_number",me,n.Count(),n.Build()));
    for(int i=0;i<count;i++) {
        GmArgs f; f.AddReal(OBJ_MISSION); f.AddReal(i);
        int m=(int)Number(Engine(api,"instance_find",me,f.Count(),f.Build()));
        if(!live(m) || getReal(m,"Started")==1) continue;
        int ship=(int)getReal(m,"Ship"); if(!live(ship)) continue;
        GmArgs a; a.AddReal(m); call("gml_Script_AcceptMission",me,a);
        if(!board) { say("Accepted %s (ship %s)",getString(m,"Type").c_str(),getString(ship,"Name").c_str()); return true; }
        if(boardShip(me,ship)) { say("Accepted %s and boarded %s",getString(m,"Type").c_str(),getString(ship,"Name").c_str()); return true; }
    }
    say("No mission offered here (go to a station, or use test 7 to reset)"); return false;
}
static void toggleGod() {
    RValue* g=const_cast<RValue*>(GlobalVar((int)GLOBAL_GODMODE));
    if(!g || GetRValueKind(*g)!=RValueKind::Real) { say("God mode not available"); return; }
    g->real=g->real?0:1; say("God mode %s",g->real?"ON":"off");
}
// Walks to the nearest knocked-out guard and picks it up (body carrying is shared with the partner).
static void carryNearestBody(CInstance* me) {
    int p=*player, parent=(int)getReal(p,"Parent");
    if(!live(parent)) { say("Not aboard a ship"); return; }
    int count=(int)getReal(parent,"ContentsCount"), best=0; double bestD=1e18;
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(parent,"Contents",i,&item);
        int id=(int)Number(item); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
        if(!inst || inst->GetObjectName(api)!="oEnemy" || getReal(id,"Conscious")==1) continue;
        double d=std::hypot(getReal(id,"Fore")-getReal(p,"Fore"),getReal(id,"Starboard")-getReal(p,"Starboard"));
        if(d<bestD){ bestD=d; best=id; }
    }
    if(!best) { say("No knocked-out guard aboard (try ko first)"); return; }
    setReal(p,"Fore",getReal(best,"Fore")+20); setReal(p,"Starboard",getReal(best,"Starboard"));
    // A one-off teleport of the local player: the game's full update (walls, contents), not PlaceAboard.
    GmArgs none; call("gml_Script_UpdatePosition",me,none);
    GmArgs a; a.AddReal(best); call("gml_Script_InteractWith",me,a);
    say("Picked up %s (carried=%g)",getString(best,"Name").c_str(),getReal(p,"CarriedObject"));
}
static void knockOutNearestGuard(CInstance* me) {
    int p=*player, parent=(int)getReal(p,"Parent");
    if(!live(parent)) { say("Not aboard a ship"); return; }
    int count=(int)getReal(parent,"ContentsCount"), best=0; double bestD=1e18;
    for(int i=0;i<count;i++) {
        RValue item{}; item.type=5; api->GetVarByName(parent,"Contents",i,&item);
        int id=(int)Number(item); CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
        if(!inst || inst->GetObjectName(api)!="oEnemy" || getReal(id,"Conscious")!=1) continue;
        double d=std::hypot(getReal(id,"Fore")-getReal(p,"Fore"),getReal(id,"Starboard")-getReal(p,"Starboard"));
        if(d<bestD){ bestD=d; best=id; }
    }
    if(!best) { say("No conscious guard aboard"); return; }
    // A concussive hit through the normal damage path (forwarded to the host when this game follows).
    setReal(best,"DamageMask",2); setReal(best,"DamageSignal",1);
    say("Hit %s (concussive)",getString(best,"Name").c_str());
}

static void run(CInstance* me,const std::string& c,const PlayerState& remote,bool fresh) {
    if(c=="pod") getInPod(me);
    else if(c=="eject") eject(me);
    else if(c=="launch") {
        const RValue* g=GlobalVar((int)GLOBAL_PLAYERSHIP); int pod=g?(int)Number(*g):0;
        if(live(pod) && getReal(pod,"DockedStation")>0) { GmArgs none; call("gml_Script_UndockFromStation",api->ResolveCInstance(pod),none); say("Undocked: you are flying your pod"); }
    }
    else if(c=="eject2") {
        const RValue* g=GlobalVar((int)GLOBAL_PLAYERSHIP); int pod=g?(int)Number(*g):0;
        if(live(pod)) { GmArgs none; call("gml_Script_EjectPlayerFromPod",api->ResolveCInstance(pod),none); setReal(pod,"xReal",getReal(pod,"xReal")+3000); setReal(pod,"xSpeed",0); setReal(pod,"ySpeed",0); say("Ejected into space: your partner should see your oxygen timer"); }
    }
    else if(c=="mission") newMissionAndBoard(me);
    else if(c=="accept") newMissionAndBoard(me,false);
    else if(c.rfind("deliver ",0)==0) say("%s",GuardsTestDeliver(c.substr(8).c_str()));
    else if(c=="bothmission") {
        if(newMissionAndBoard(me) && fresh) { NetSendReliable(MSG_TEST,"boardpartner",12); say("Partner will board this ship too"); }
    }
    else if(c=="boardpartner") { boardPartnerAt=GetTickCount(); say("Boarding your partner's mission ship..."); }
    else if(c=="bringpartner") { if(fresh) { NetSendReliable(MSG_TEST,"join",4); say("Pulling %s to you",remote.name); } }
    else if(c=="join") PresenceCommand("join");
    else if(c=="god") toggleGod();
    else if(c=="ko") knockOutNearestGuard(me);
    else if(c=="carry") carryNearestBody(me);
    else if(c=="door") say("%s",ObjectsTestHoldNearestDoor(5));
    else if(c=="reset") PresenceCommand("reset");
    else say("Unknown test command %s",c.c_str());
}
void TestMenuCommand(const char* name) { pending=name; }
void TestMenuOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    if(type==MSG_TEST) pending.assign((const char*)data,size);
}
void TestMenuTick(CInstance* self,const PlayerState& remote,bool fresh) {
    if(!self) return;
    GmArgs key; key.AddReal(KEY_TEST);
    if(Number(Engine(api,"keyboard_check_pressed",self,key.Count(),key.Build()))) open=!open;
    if(open) {
        static const char* const keys="1234567890";
        static const char* const commands[]={"pod","eject","bothmission","mission","bringpartner","join","reset","god","ko","carry"};
        for(int k=0;k<10;k++){ GmArgs d; d.AddReal(keys[k]); if(Number(Engine(api,"keyboard_check_pressed",self,d.Count(),d.Build()))) { open=false; pending=commands[k]; break; } }
    }
    CInstance* me=api->ResolveCInstance(*player);
    if(!pending.empty() && me) { std::string c=pending; pending.clear(); run(me,c,remote,fresh); }
    // The partner's mission ship arrives as a message and is generated a moment later: retry boarding for a few seconds.
    if(boardPartnerAt) {
        if(GetTickCount()-boardPartnerAt>8000) { boardPartnerAt=0; say("Could not find your partner's mission ship"); }
        else if(fresh && remote.parentName[0]) {
            int ship=MissionsFindShip(remote.parentName,remote.parentX,remote.parentY);
            if(ship && me && ship!=(int)getReal(*player,"Parent") && boardShip(me,ship)) { boardPartnerAt=0; say("Boarded %s with your partner",remote.parentName); }
        }
    }
}
void TestMenuDraw(CInstance* self) {
    GmArgs none; double w=Number(Engine(api,"display_get_gui_width",self,0,none.Build())), h=Number(Engine(api,"display_get_gui_height",self,0,none.Build()));
    if(!(w>0)) return;
    GmArgs white; white.AddReal(0xffffff); Engine(api,"draw_set_colour",self,white.Count(),white.Build());
    auto text=[&](double x,double y,const char* s){ GmArgs a; a.AddReal(x); a.AddReal(y); a.AddStr(api,s); Engine(api,"draw_text",self,a.Count(),a.Build()); };
    if(open) {
        const char* lines[]={"CO-OP TEST  (F10 to close)",
            "1  Get in my pod (from a station: takes the dock lift)","2  Drift in space (opens the airlock you are at; or ejects from your pod)",
            "3  BOTH: accept a mission here and board its ship together","4  Me: accept a mission here and board its ship",
            "5  Pull my partner to me","6  Go to my partner","7  Reset both to the host's starting station",
            "8  God mode on/off","9  Knock out the nearest guard","0  Pick up the nearest knocked-out guard"};
        for(int i=0;i<11;i++) text(w/2-330,h/2-130+i*26,lines[i]);
    }
    if(noticeAt && GetTickCount()-noticeAt<5000) { GmArgs c; c.AddReal(0x80ffff); Engine(api,"draw_set_colour",self,c.Count(),c.Build()); text(w/2-330,180,notice); }
    Engine(api,"draw_set_colour",self,white.Count(),white.Build());
}
void TestMenuInit(const HS_ModApi* a,int* localPlayer) { api=a; player=localPlayer; }
