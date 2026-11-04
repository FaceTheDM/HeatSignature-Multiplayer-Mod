// Shared time policy. Heat Signature eases the global TimeScale toward oTime.TargetTimeScale (slow-mo, pause, fast-forward).
// While both players are aboard the same ship, the world there runs no faster than the partner's own target, so one
// player's slow-mo is not undone by the other's real time (guards are simulated on the HOST's clock). The partner's
// influence never goes below normal slow-mo speed, so one player pausing cannot freeze the other's game.
// coop.ini `time=independent` disables it.
#include "TimePolicy.h"
#include "Console.h"
#include "GmArgs.h"
#include <cstdio>
#include <cstring>

static const HS_ModApi* api;
static const double OBJ_TIME=42, GLOBAL_TIMESCALE=10, PARTNER_FLOOR=0.2;
static int timeObject;
static bool enabled=true;
int CoopRemoteParent();
double CoopRemoteTimeScaleWanted();
bool CoopRemotePaused();
int CoopRemotePauseKind();
bool CrateRemoteChoosing();

// ---- Shared pause: the game's pause menu (Escape) sets the control object's Mode to "Menu" and stops the clock. While the
// partner has it open, this game's clock is held at zero too (no menu here; the status line says who paused). ----
static const double OBJ_CONTROL=26;
static int controlObject;
static bool pausedByPartner;
// 0: playing; 1: the pause menu (Escape) is open; 2: the inventory (Space) is open (planning: time is stopped while the
// player picks an item or aims, e.g. a swapper or Glitcher teleport, and resumes when they commit or back out).
// Either stops both games; time resumes when neither player is in one. A player who never opened theirs has nothing to
// confirm.
static int rawKind;   // the mode right now (no settling delay)
static DWORD planningGraceUntil;   // our own inventory is being opened: no hold until it is
int TimePolicyLocalPauseKind() {
    if(!controlObject || !api->ResolveCInstance(controlObject)) {
        controlObject=0;
        const RValue* p=GlobalVar(GLOBAL_PLAYER);
        CInstance* self=p?api->ResolveCInstance((int)Number(*p)):nullptr; if(!self) return 0;
        GmArgs f; f.AddReal(OBJ_CONTROL); f.AddReal(0);
        controlObject=(int)Number(Engine(api,"instance_find",self,f.Count(),f.Build()));
        if(!api->ResolveCInstance(controlObject)) { controlObject=0; return 0; }
    }
    RValue m{}; m.type=5; api->GetVarByName(controlObject,"Mode",(int)0x80000000,&m);
    const char* mode=GetRValueKind(m)==RValueKind::String && m.str && m.str->text?m.str->text:"";
    int kind=!strcmp(mode,"Menu")?1:!strcmp(mode,"Inventory")?2:0;
    // The game passes through "Menu" briefly on its way to other screens (mission results): only one that stays open
    // counts.
    rawKind=kind;
    static DWORD since; static int sinceKind; if(!kind) { since=0; return 0; }
    if(!since || sinceKind!=kind) { since=GetTickCount(); sinceKind=kind; }
    return GetTickCount()-since>300?kind:0;
}
bool TimePolicyLocalPaused() { return TimePolicyLocalPauseKind()!=0; }
bool TimePolicyPausedByPartner() { return pausedByPartner; }
// Switches the control object's Mode from `from` to `to` (only when it is in `from`).
static void setMode(const char* from,const char* to) {
    if(!controlObject || !api->ResolveCInstance(controlObject)) { TimePolicyLocalPaused(); if(!controlObject) return; }
    RValue m{}; m.type=5; api->GetVarByName(controlObject,"Mode",(int)0x80000000,&m);
    if(GetRValueKind(m)!=RValueKind::String || !m.str || !m.str->text || strcmp(m.str->text,from)) return;
    RValue v{}; v.type=1; api->SetString(&v,to); api->SetVarByName(controlObject,"Mode",(int)0x80000000,&v);
}

// While the partner's slow-mo holds this game back, this game also shows the slow-mo screen effect: the game sets the
// effect's strength every frame from its own target time scale, which the partner's slow-mo does not change.
static bool partnerSlowing; static double partnerScale=1;
static const double FULL_EFFECT_STRENGTH=1.0, SLOWMO_SCALE=0.2;
static void preEffectStrength(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!partnerSlowing || argc<1 || !argv || !argv[0]) return;
    double want=(1-partnerScale)/(1-SLOWMO_SCALE)*FULL_EFFECT_STRENGTH; if(want>FULL_EFFECT_STRENGTH) want=FULL_EFFECT_STRENGTH;
    double cur=Number(*argv[0]);
    if(!(cur>=want)) { argv[0]->type=0; argv[0]->real=want; }
}
void TimePolicyInit(const HS_ModApi* a) { api=a; api->SubscribeHook("gml_Script_SetSlowMotionEffectStrength",preEffectStrength,nullptr); }
bool TimePolicyPartnerSlowing() { return partnerSlowing; }
void TimePolicyConfigure(bool shared) { enabled=shared; CoopLog("Time policy: %s",shared?"shared on the same ship (min of both, partner floor 0.2)":"independent"); }
static double target() {
    if(!timeObject || !api->ResolveCInstance(timeObject)) {
        timeObject=0;
        const RValue* p=GlobalVar(GLOBAL_PLAYER);
        CInstance* self=p?api->ResolveCInstance((int)Number(*p)):nullptr;
        if(!self) return 1.0;
        GmArgs f; f.AddReal(OBJ_TIME); f.AddReal(0);
        timeObject=(int)Number(Engine(api,"instance_find",self,f.Count(),f.Build()));
        if(!api->ResolveCInstance(timeObject)) { timeObject=0; return 1.0; }
    }
    // TargetTimeScale keeps its last value after slow-mo ends; the game's own slow-mo is on while its visuals are.
    // Read by variable id: lookup by name does not reach these variables.
    static int strengthVar=api->GetVarId("SlowMoVisualsStrength"), targetVar=api->GetVarId("TargetTimeScale");
    if(strengthVar<0) strengthVar=2911;   // ids from known_variable_properties.txt (exact executable only)
    if(targetVar<0) targetVar=2606;
    RValue s{}; s.type=5; api->GetVar(timeObject,strengthVar,(int)0x80000000,&s);
    double strength=Number(s); if(!(strength>0.01)) return 1.0;
    RValue v{}; v.type=5; api->GetVar(timeObject,targetVar,(int)0x80000000,&v);
    double d=Number(v); if(!(d==d) || d>1) d=1.0;
    return d;
}
// While the partner holds this game back, its slow-mo visuals are on because of the partner; reporting them as this
// player's own slow-mo would keep both games slowed. The last value from before is reported instead.
static double ownWanted=1.0; static bool holding;
double TimePolicyLocalWanted() { return holding?ownWanted:(ownWanted=target()); }
// Moves `from` toward `to` by at most `step`.
static double approach(double from,double to,double step){ return from<to?(from+step<to?from+step:to):(from-step>to?from-step:to); }
void TimePolicyTick(CInstance*) {
    static bool overriding; static DWORD last; static double lastReported=-1;
    DWORD now=GetTickCount(); double dt=last?(now-last)/1000.0:0; last=now;
    RValue* scale=const_cast<RValue*>(GlobalVar((int)GLOBAL_TIMESCALE));
    if(!scale || GetRValueKind(*scale)!=RValueKind::Real) return;
    // Planning (Space, the inventory): time stops for both while either player plans, and resumes as soon as nobody is
    // planning. A player who is not planning counts as ready: they do not have to confirm anything. While held by the
    // partner's planning, Space here opens this player's own inventory (the game's own ShowInventoryMenu), so both can
    // plan in the same stopped moment; then time waits for both.
    { static bool wantOpen;
      if(pausedByPartner && CoopRemotePauseKind()==2) {
        const RValue* pl=GlobalVar(GLOBAL_PLAYER); CInstance* me=pl?api->ResolveCInstance((int)Number(*pl)):nullptr;
        GmArgs k; k.AddReal(VK_SPACE);
        bool pressed=me && Number(Engine(api,"keyboard_check_pressed",me,k.Count(),k.Build()))>0.5;
        // Opened on the frame AFTER the press: the game's own inventory button sees the same press, and would close an
        // inventory opened in that frame straight away.
        if(pressed) wantOpen=true;
        else if(wantOpen && me) {
            wantOpen=false;
            setMode("CoopPause","Game");
            GmArgs none; RValue r{}; r.type=5; api->CallScript("gml_Script_ShowInventoryMenu",me,me,&r,none.Count(),none.Build());
            planningGraceUntil=GetTickCount()+500;
            CoopLog("Time policy: planning too (inventory opened during the partner's planning pause)");
        }
      } else wantOpen=false; }
    TimePolicyLocalPauseKind();   // refreshes rawKind
    if((CoopRemotePaused() || CrateRemoteChoosing()) && !TimePolicyLocalPaused() && rawKind==0 && GetTickCount()>=planningGraceUntil) {
        if(!pausedByPartner) CoopLog("Time policy: paused by the partner");
        pausedByPartner=true; scale->real=0;
        // A zero time scale alone does not stop the world (ships keep flying): the game only stands still when its control
        // object leaves "Game" mode, as the pause menu does. A mode of our own stops it the same way without opening the
        // menu here (the status line says who paused). Escape here still opens the real menu over it.
        setMode("Game","CoopPause");
        return;
    }
    if(pausedByPartner) { pausedByPartner=false; setMode("CoopPause","Game"); scale->real=target(); CoopLog("Time policy: the partner resumed"); }
    // This player's own menu or inventory is open: the game stops time itself (its MenuPriority outranks every slow
    // motion, a guard's aim included). The partner's slow motion must not set it going again: it did, and the planner's
    // world ran at 0.2 while a guard closed in and killed them (playtest, 2026-10-05).
    if(rawKind!=0) {
        if(overriding) { overriding=false; lastReported=-1; CoopLog("Time policy: own %s open; the game's own stop applies",rawKind==1?"menu":"inventory"); }
        partnerSlowing=false;
        return;
    }
    double local=holding?ownWanted:(ownWanted=target());
    double effective=local;
    int remoteShip=CoopRemoteParent(); const RValue* pl=GlobalVar(GLOBAL_PLAYER);
    bool sameShip=remoteShip && pl && (int)Number(*pl)>0 && [&]{ RValue v{}; v.type=5; api->GetVarByName((int)Number(*pl),"Parent",0x80000000,&v); return (int)Number(v)==remoteShip; }();
    if(enabled && sameShip) {
        double remote=CoopRemoteTimeScaleWanted();
        if(remote>0) { if(remote<PARTNER_FLOOR) remote=PARTNER_FLOOR; if(remote<effective) effective=remote; }
    }
    // The game eases TimeScale only when its own target changes, so while the partner's target holds this world back the
    // plugin does the easing itself, and hands control back once the speed is back at the local target.
    const double rate=4.0*dt;
    partnerSlowing=effective<local-1e-6 && effective<0.999; partnerScale=scale->real;
    // The visuals fade out for a moment after the partner's slow-mo ends; keep reporting the old value meanwhile.
    static DWORD holdUntil; if(overriding || partnerSlowing) holdUntil=now+1500;
    holding=now<holdUntil;
    if(partnerSlowing && timeObject && api->ResolveCInstance(timeObject)) {
        // The partner's slow-mo shows here too: the effect is set every frame while it holds this game back (the game
        // only sets it itself during this player's own slow-mo).
        double want=(1-scale->real)/(1-SLOWMO_SCALE)*FULL_EFFECT_STRENGTH; if(want>FULL_EFFECT_STRENGTH) want=FULL_EFFECT_STRENGTH; if(want<0) want=0;
        GmArgs e; e.AddReal(want); RValue out{}; api->CallScript("gml_Script_SetSlowMotionEffectStrength",api->ResolveCInstance(timeObject),api->ResolveCInstance(timeObject),&out,e.Count(),e.Build());
    }
    if(effective<local-1e-6) {
        overriding=true;
        scale->real=scale->real>effective?effective:approach(scale->real,effective,rate);
        if(effective!=lastReported){ CoopLog("Time policy: partner's time scale %.2f applies here",effective); lastReported=effective; }
    } else if(overriding) {
        scale->real=approach(scale->real,local,rate);
        if(scale->real==local){ overriding=false; lastReported=-1; CoopLog("Time policy: back to this game's own time scale %.2f",local); }
    }
}
