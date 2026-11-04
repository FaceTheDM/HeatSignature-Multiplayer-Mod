#include "Visuals.h"
#include "Console.h"
#include "Place.h"
#include "GmArgs.h"
#include "Wire.h"
#include "Guards.h"
#include "Net.h"
#include "Snapshot.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <map>

int CoopRemoteParent();
static const HS_ModApi* api;
static int* player;
static int* puppet;

static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static RValue call(const char* script,CInstance* self,GmArgs& a){ RValue r{}; r.type=5; api->CallScript(script,self,self,&r,a.Count(),a.Build()); return r; }
static bool live(int id){ return id>0 && api->ResolveCInstance(id); }
static void setReal(int id,const char* n,double d){ RValue v{}; v.type=0; v.real=d; api->SetVarByName(id,n,0x80000000,&v); }
static std::string objectName(int id){ CInstance* i=id>0?api->ResolveCInstance(id):nullptr; return i?i->GetObjectName(api):""; }
static double shipAngle(int ship){ return std::atan2(-getReal(ship,"ForeUnitY"),getReal(ship,"ForeUnitX"))*180/3.14159265358979; }

// ---- Attachments (head, backpack, ...): recorded from the game's SetAttachment(slot, name) calls on our player ----
// Recorded for every character (by instance id): the crew is created before the player picks one of them.
static std::map<int,std::map<std::string,std::string>> attachmentsOf;
static std::map<std::string,std::string> partnerAttachments;
static bool attachmentsDirty, partnerAttachmentsChanged;
static void preSetAttachment(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!self || self->type!=137 || (puppet && (int)self->id==*puppet) || argc<2 || !argv || !argv[0] || !argv[1] || GetRValueKind(*argv[0])!=RValueKind::String || !argv[0]->str) return;
    std::string slot=argv[0]->str->text?argv[0]->str->text:"";
    std::string name=(GetRValueKind(*argv[1])==RValueKind::String && argv[1]->str && argv[1]->str->text)?argv[1]->str->text:"";
    auto& mine=attachmentsOf[(int)self->id];
    if(!mine.count(slot) || mine[slot]!=name) { mine[slot]=name; if((int)self->id==*player) attachmentsDirty=true; }
}
static void sendAttachments() {
    // Characters loaded with the save got their attachments before this hook existed; the puppet would otherwise keep
    // the random backpack it was created with, so "no backpack" is the default unless one was recorded.
    auto& myAttachments=attachmentsOf[*player];
    if(!myAttachments.count("Backpack")) myAttachments["Backpack"]="";
    static size_t lastLogged=(size_t)-1; if(myAttachments.size()!=lastLogged){ lastLogged=myAttachments.size(); CoopLog("Visuals: sending %u attachments of player %d",(unsigned)myAttachments.size(),*player); }
    Writer w; w.put<uint16_t>((uint16_t)myAttachments.size());
    for(auto& kv:myAttachments){ w.str(kv.first); w.str(kv.second); }
    NetSendReliable(MSG_LOOK,w.bytes.data(),w.bytes.size());
}

// ---- Local: recorded from the game's own calls on our player ----
static std::string torsoAnim, oneShotAnim; static uint16_t oneShotSeq;
static bool settingPuppetTorso;
static void preSetTorsoAnim(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(self && *puppet>0 && (int)self->id==*puppet && !settingPuppetTorso) {
        // The stand-in's own state (Resting) also sets torso animations; only the partner's animation is shown.
        static DWORD lastLog; static int logged; if((logged++<3 || CoopVerbose()) && GetTickCount()-lastLog>5000 && argc>0 && argv && argv[0] && GetRValueKind(*argv[0])==RValueKind::String && argv[0]->str) { lastLog=GetTickCount(); CoopLog("Visuals: stand-in's own torso animation %s blocked",argv[0]->str->text?argv[0]->str->text:""); }
        api->RequestBypass(); return;
    }
    if(!self || (int)self->id!=*player || argc<1 || !argv || !argv[0] || GetRValueKind(*argv[0])!=RValueKind::String || !argv[0]->str) return;
    torsoAnim=argv[0]->str->text?argv[0]->str->text:"";
}
static void prePlayTorsoAnimOnce(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!self || (int)self->id!=*player || argc<1 || !argv || !argv[0] || GetRValueKind(*argv[0])!=RValueKind::String || !argv[0]->str) return;
    oneShotAnim=argv[0]->str->text?argv[0]->str->text:""; oneShotSeq++;
}
// The partner is down: the stand-in takes the game's own down pose (LookDead sets the "Shot" torso and "LegsShot" legs).
// Its torso is otherwise only the partner's, so the torso half is let through explicitly (it was blocked: dead legs
// under a standing torso).
void VisualsPuppetLookDead(CInstance* puppetInst) {
    if(!puppetInst) return;
    settingPuppetTorso=true;
    GmArgs none; RValue r{}; r.type=5; api->CallScript("gml_Script_LookDead",puppetInst,puppetInst,&r,none.Count(),none.Build());
    settingPuppetTorso=false;
}
void VisualsFill(PlayerState& s) {
    static DWORD lastLook;
    // Attachments change rarely: sent on change, and every few seconds so a partner who (re)connects gets them.
    if(live(*player) && (attachmentsDirty || GetTickCount()-lastLook>5000)) { sendAttachments(); attachmentsDirty=false; lastLook=GetTickCount(); }
    int p=*player; if(!live(p)) return;
    snprintf(s.torsoAnim,sizeof(s.torsoAnim),"%s",torsoAnim.c_str());
    snprintf(s.oneShotAnim,sizeof(s.oneShotAnim),"%s",oneShotAnim.c_str()); s.oneShotSeq=oneShotSeq;
    int item=(int)getReal(p,"EquippedItem");
    if(live(item)) {
        snprintf(s.weaponSlot,sizeof(s.weaponSlot),"%s",getString(item,"AttachmentSlot").c_str());
        snprintf(s.weaponName,sizeof(s.weaponName),"%s",getString(item,"AttachmentName").c_str());
    }
}

// ---- Puppet: weapon attachment and torso animation ----
static std::string appliedTorso;
const char* VisualsPuppetTorso() { return appliedTorso.c_str(); }
void VisualsApplyToPuppet(CInstance* pup,const PlayerState& r) {
    static int appliedTo; static std::string appliedSlot,appliedName; static uint16_t appliedShot;
    if(!pup) return;
    int id=(int)pup->id;
    if(appliedTo!=id){ appliedTo=id; appliedSlot.clear(); appliedName.clear(); appliedTorso.clear(); appliedShot=r.oneShotSeq; partnerAttachmentsChanged=true; }
    if(partnerAttachmentsChanged) {
        partnerAttachmentsChanged=false;
        for(auto& kv:partnerAttachments) {
            GmArgs a; a.AddStr(api,kv.first.c_str()); if(kv.second.empty()) a.AddReal(-4); else a.AddStr(api,kv.second.c_str());
            call("gml_Script_SetAttachment",pup,a);
        }
    }
    // The weapon: the same engine call EquipInventoryItem makes (skeleton_attachment_set(slot, name)); -1 clears a slot.
    if(appliedSlot!=r.weaponSlot || appliedName!=r.weaponName) {
        if(!appliedSlot.empty() && appliedSlot!=r.weaponSlot) { GmArgs clear; clear.AddStr(api,appliedSlot.c_str()); clear.AddReal(-1); Engine(api,"skeleton_attachment_set",pup,clear.Count(),clear.Build()); }
        if(r.weaponSlot[0]) {
            GmArgs set; set.AddStr(api,r.weaponSlot);
            if(r.weaponName[0]) set.AddStr(api,r.weaponName); else set.AddReal(-1);
            Engine(api,"skeleton_attachment_set",pup,set.Count(),set.Build());
        }
        appliedSlot=r.weaponSlot; appliedName=r.weaponName;
    }
    // Torso animation (falls back to the casual idle until the partner's first animation arrives).
    const char* torso=r.torsoAnim[0]?r.torsoAnim:"Casual_Idle1";
    GmArgs t; t.AddStr(api,torso); settingPuppetTorso=true; call("gml_Script_SetTorsoAnim",pup,t); settingPuppetTorso=false;
    appliedTorso=torso;
    if(r.oneShotSeq!=appliedShot && r.oneShotAnim[0]) {
        appliedShot=r.oneShotSeq;
        GmArgs once; once.AddStr(api,r.oneShotAnim); once.AddStr(api,torso); call("gml_Script_PlayTorsoAnimOnce",pup,once);
    }
}

// ---- Shots and gun sounds ----
// A tracer is kept relative to the shooter's ship (ship-local position and angle) when the shooter is aboard one: a ship
// moves ~200 units a frame, so a tracer kept in room coordinates was left behind in space while it was shown.
struct Tracer { double x,y,angle; DWORD at; int ship; double fore,stb,rel; };
static std::deque<Tracer> tracers;
static bool sending;
// GunFiresBulletInDirection(gun) runs from the gun's use event: argument0 is the gun, its Owner the shooter. The shot goes
// where the shooter faces; MyImageAngle is relative to the ship they are aboard, which the partner converts through
// their own copy of that ship.
// The partner sees real bullets: right after a shot, the bullets it created here (each records its Firer) are sent with
// their variables, and the partner's game creates the same bullets from its copy of the shooter (the partner's character
// or a guard). Those copies are harmless: the shot's hits already happen in the shooter's game.
static DWORD lastReplayAt;
static std::set<int> copyBullets;   // bullets made here as copies of the partner's (or the guards' game's) shots
bool VisualsIsCopyBullet(int id) { return copyBullets.count(id)!=0; }
static void preCopyBulletDamage(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && copyBullets.count((int)self->id)) api->RequestBypass();
}
static const char* const BULLET_LOOK[]={"sprite_index","TrailSprite","TrailSpriteLength","image_blend","image_xscale","image_yscale"};
// Built-in variables (sprite_index, image_blend...) go through the engine's variable_instance_get/set.
static double builtinReal(int id,const char* n) {
    CInstance* inst=api->ResolveCInstance(id); if(!inst) return NAN;
    GmArgs a; a.AddReal(id); a.AddStr(api,n); RValue v=Engine(api,"variable_instance_get",inst,a.Count(),a.Build());
    return GetRValueKind(v)==RValueKind::String?NAN:Number(v);
}
static void setBuiltinReal(int id,const char* n,double value) {
    CInstance* inst=api->ResolveCInstance(id); if(!inst) return;
    GmArgs a; a.AddReal(id); a.AddStr(api,n); a.AddReal(value); Engine(api,"variable_instance_set",inst,a.Count(),a.Build());
}
static int shotsSent, shotsShown;
static void shotStats() {
    static DWORD last; if(!CoopVerbose() || GetTickCount()-last<5000 || !(shotsSent||shotsShown)) return;
    last=GetTickCount(); CoopLog("Visuals: bullets sent %d, partner's bullets shown %d",shotsSent,shotsShown);
    shotsSent=shotsShown=0;
}
static void postGunFires(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(sending || argc<1 || !argv || !argv[0]) return;
    int gun=(int)Number(*argv[0]); if(!live(gun)) return;
    int owner=(int)getReal(gun,"Owner");
    std::string shooterKey;
    if(owner==*player) shooterKey="@";
    else if(GuardsIsAuthority()) { const char* k=GuardsKeyOfGuard(owner); if(k && *k) shooterKey=k; }
    if(shooterKey.empty()) return;
    if(owner==*player) {
        int parent=(int)getReal(owner,"Parent");
        Writer w; w.put<uint8_t>(live(parent) && parent!=owner?1:0); w.put<double>(getReal(owner,"MyImageAngle"));
        NetSendUnreliable(MSG_SHOT,w.bytes.data(),w.bytes.size());
    }
    int ship=(int)getReal(owner,"Parent"); if(!live(ship) || ship==owner) return;
    if(CoopRemoteParent()!=ship) return;   // nobody there to see them
    // The new bullets are the last entries of the ship's contents with this shooter as Firer.
    int count=(int)getReal(ship,"ContentsCount");
    for(int i=count-1;i>=0 && i>=count-12;i--) {
        RValue item{}; item.type=5; api->GetVarByName(ship,"Contents",i,&item); int b=(int)Number(item);
        if(!live(b) || (int)getReal(b,"Firer")!=owner) continue;
        static std::map<int,DWORD> sent; DWORD now=GetTickCount();
        if(sent.count(b) && now-sent[b]<2000) continue;
        sent[b]=now; if(sent.size()>256) sent.clear();
        Writer w; w.str(getString(ship,"Name")); w.str(shooterKey); w.str(objectName(b));
        w.put<double>(getReal(b,"Fore")-getReal(owner,"Fore")); w.put<double>(getReal(b,"Starboard")-getReal(owner,"Starboard"));
        SnapshotCapture(b,w);
        // How it looks (the gun's BulletSprite and the trail): sprites are left out of snapshots as per-process handles,
        // but asset indices are the same in both games. Without them the partner saw plain bullets (playtest, 2026-10-05).
        for(const char* n:BULLET_LOOK) w.put<double>(builtinReal(b,n));
        NetSendReliable(MSG_BULLET,w.bytes.data(),w.bytes.size());
        shotsSent++;
    }
    shotStats();
}
static void showBullet(Reader& r) {
    std::string shipName=r.str(), shooterKey=r.str(), name=r.str(); double df=r.get<double>(), ds=r.get<double>();
    if(!r.ok) return;
    int shooter=shooterKey=="@"?*puppet:GuardsGuardByKey(shooterKey.c_str());
    if(!live(shooter)) return;
    int ship=(int)getReal(shooter,"Parent"); if(!live(ship) || getString(ship,"Name")!=shipName) return;
    if((int)getReal(*player,"Parent")!=ship) return;   // only shown aboard (a ship far away is not set up to create things)
    CInstance* s=api->ResolveCInstance(ship);
    GmArgs idx; idx.AddStr(api,name.c_str()); double obj=Number(Engine(api,"asset_get_index",s,idx.Count(),idx.Build()));
    if(!(obj>=0)) return;
    GmArgs mk; mk.AddReal(obj); mk.AddReal(shooter);
    int b=(int)Number(call("gml_Script_CreateContentsOnTopOf",s,mk));
    if(!live(b)) return;
    SnapshotApply(b,r,true);
    for(const char* n:BULLET_LOOK) { double v=r.get<double>(); if(r.ok && v==v) setBuiltinReal(b,n,v); }
    setReal(b,"Fore",getReal(shooter,"Fore")+df); setReal(b,"Starboard",getReal(shooter,"Starboard")+ds);
    // The copy keeps its BulletDamageMask: its own step picks its hit effects by it (a concussive bullet's blue blast;
    // with the mask zeroed the partner saw plain bullets, 2026-10-05). Its damage is stopped instead (preCopyBulletDamage):
    // the shot's hits already happen in the shooter's game.
    setReal(b,"Firer",shooter); setReal(b,"KnockbackSpeed",0);
    copyBullets.insert(b); if(copyBullets.size()>512) copyBullets.clear();
    PlaceAboard(api,api->ResolveCInstance(b));
    lastReplayAt=GetTickCount(); shotsShown++; shotStats();
}
static void postGunSound(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(sending || !self || argc<1 || !argv || !argv[0]) return;
    int shooter=(int)self->id;
    if(shooter!=*player && (int)getReal(shooter,"Owner")!=*player) return;
    Writer w; w.put<double>(Number(*argv[0]));
    NetSendUnreliable(MSG_SOUND,w.bytes.data(),w.bytes.size());
}
// ---- Gadget effects (glitch, swap, teleport): the game's teleport effect replayed on the partner's copies ----
// Arguments are sent as they were: this game's player becomes "@" (the partner's stand-in there), the ship "$", a
// guard its key; a call with any other instance argument is not sent (it could not be matched, and scripts read their
// arguments unchecked).
static bool replayingFx;
static void prePlayTeleportEffect(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(replayingFx || *player<=0 || argc>12) return;
    int ship=GuardsSharedShip(); if(!ship) return;
    Writer w; w.put<uint8_t>((uint8_t)argc);
    bool involvesMe=self && (int)self->id==*player;
    for(int i=0;i<argc;i++) {
        if(!argv || !argv[i] || GetRValueKind(*argv[i])!=RValueKind::Real) return;
        double d=Number(*argv[i]); int id=(int)d;
        if(d==id && id>=100000 && live(id)) {
            if(id==*player) { w.put<uint8_t>(1); involvesMe=true; }
            else if(id==ship) w.put<uint8_t>(2);
            else { const char* k=GuardsKeyOfGuard(id); if(!k || !*k) return; w.put<uint8_t>(3); w.str(k); }
        } else { w.put<uint8_t>(0); w.put<double>(d); }
    }
    // This player's own gadgets, and teleports of the guards this game runs (the partner's copies do not teleport).
    std::string selfKey;
    if(!involvesMe) { if(!GuardsIsAuthority() || !self) return; const char* k=GuardsKeyOfGuard((int)self->id); if(!k || !*k) return; selfKey=k; }
    w.str(selfKey);
    NetSendReliable(MSG_TELEPORT_FX,w.bytes.data(),w.bytes.size());
}
// The screen warp belongs to whoever teleported; the partner's replayed teleport shows only what happens aboard (trail,
// particles, scorch), not the warp over this player's whole screen.
static void prePlayTeleportScreenEffect(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { if(replayingFx) api->RequestBypass(); }
static void replayTeleportEffect(Reader& r) {
    uint8_t argc=r.get<uint8_t>(); int ship=GuardsSharedShip();
    if(!r.ok || !ship || !live(*puppet)) return;
    GmArgs a;
    for(int i=0;i<argc;i++) {
        uint8_t kind=r.get<uint8_t>();
        if(kind==0) a.AddReal(r.get<double>());
        else if(kind==1) a.AddReal(*puppet);
        else if(kind==2) a.AddReal(ship);
        else { std::string k=r.str(); int g=GuardsGuardByKey(k.c_str()); if(!live(g)) return; a.AddReal(g); }
        if(!r.ok) return;
    }
    std::string selfKey=r.str(); int self=*puppet;
    if(r.ok && !selfKey.empty()) { self=GuardsGuardByKey(selfKey.c_str()); if(!live(self)) return; }
    if((int)getReal(*player,"Parent")!=ship) return;   // shown aboard only
    replayingFx=true; call("gml_Script_PlayTeleportEffectOnShip",api->ResolveCInstance(self),a); replayingFx=false;
}
void VisualsOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    Reader r(data,size);
    if(type==MSG_LOOK) {
        std::map<std::string,std::string> next; uint16_t n=r.get<uint16_t>();
        for(uint16_t i=0;i<n && r.ok;i++){ std::string slot=r.str(), name=r.str(); if(r.ok) next[slot]=name; }
        if(r.ok && next!=partnerAttachments){
            partnerAttachments.swap(next); partnerAttachmentsChanged=true;
            std::string list; for(auto& kv:partnerAttachments) list+=" "+kv.first+"="+(kv.second.empty()?"none":kv.second);
            CoopLog("Visuals: partner attachments:%s",list.c_str());
        }
        return;
    }
    if(!live(*puppet)) return;   // only seen and heard when the partner is shown aboard the same ship
    int pup=*puppet;
    if(type==MSG_BULLET) { showBullet(r); return; }
    if(type==MSG_TELEPORT_FX) { replayTeleportEffect(r); return; }
    if(type==MSG_SHOT) {
        uint8_t relative=r.get<uint8_t>(); double angle=r.get<double>(); if(!r.ok) return;
        if(GetTickCount()-lastReplayAt<300) return;   // real bullets are shown
        int parent=(int)getReal(pup,"Parent");
        bool aboard=live(parent) && parent!=pup;
        double rel=aboard?(relative?angle:angle-shipAngle(parent)):0;
        if(relative && live(parent)) angle+=shipAngle(parent);
        tracers.push_back({getReal(pup,"xReal"),getReal(pup,"yReal"),angle,GetTickCount(),aboard?parent:0,getReal(pup,"Fore"),getReal(pup,"Starboard"),rel});
        if(tracers.size()>32) tracers.pop_front();
    } else if(type==MSG_SOUND) {
        double sound=r.get<double>(); if(!r.ok || !(sound>=0)) return;
        if(GetTickCount()-lastReplayAt<300) return;   // the replayed shot made its own sound
        // Played with the engine directly, and only if it really is a sound: a wrong value passed to the game's own
        // sound scripts would raise a GML error here.
        CInstance* self=api->ResolveCInstance(pup);
        GmArgs chk; chk.AddReal(sound);
        if(!Number(Engine(api,"audio_exists",self,chk.Count(),chk.Build()))) return;
        GmArgs a; a.AddReal(sound); a.AddReal(10); a.AddReal(0); Engine(api,"audio_play_sound",self,a.Count(),a.Build());
    }
}
void VisualsDraw(CInstance* self) {
    DWORD now=GetTickCount();
    while(!tracers.empty() && now-tracers.front().at>150) tracers.pop_front();
    for(Tracer t:tracers) {
        if(t.ship && live(t.ship)) {   // where the ship is now
            t.x=getReal(t.ship,"AnchorX")+t.fore*getReal(t.ship,"ForeUnitX")+t.stb*getReal(t.ship,"StarboardUnitX");
            t.y=getReal(t.ship,"AnchorY")+t.fore*getReal(t.ship,"ForeUnitY")+t.stb*getReal(t.ship,"StarboardUnitY");
            t.angle=t.rel+shipAngle(t.ship);
        }
        double rad=t.angle*3.14159265358979/180, ex=t.x+std::cos(rad)*1500, ey=t.y-std::sin(rad)*1500;
        GmArgs a0; a0.AddReal(t.x); a0.AddReal(t.y); GmArgs b0; b0.AddReal(t.x); b0.AddReal(t.y);
        GmArgs a1; a1.AddReal(ex); a1.AddReal(ey); GmArgs b1; b1.AddReal(ex); b1.AddReal(ey);
        double sx=Number(call("gml_Script_GUIxOfXY",self,a0)), sy=Number(call("gml_Script_GUIyOfXY",self,b0));
        double tx=Number(call("gml_Script_GUIxOfXY",self,a1)), ty=Number(call("gml_Script_GUIyOfXY",self,b1));
        if(!(sx==sx) || !(tx==tx)) continue;
        GmArgs c; c.AddReal(0x60ffff); Engine(api,"draw_set_colour",self,c.Count(),c.Build());
        GmArgs l; l.AddReal(sx); l.AddReal(sy); l.AddReal(tx); l.AddReal(ty); l.AddReal(2); Engine(api,"draw_line_width",self,l.Count(),l.Build());
    }
}
// Diagnostics: how often each character's animation advances (the stand-in should match the real player).
static int progressPuppet, progressPlayer, legsPuppet, legsPlayer, progressEnemy, legsEnemy; static std::set<int> enemies;
static void preProgressFrames(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void* legs) {
    if(!self) return;
    if(*puppet>0 && (int)self->id==*puppet) (legs?legsPuppet:progressPuppet)++;
    else if(*player>0 && (int)self->id==*player) (legs?legsPlayer:progressPlayer)++;
    else if(self->GetObjectName(api)=="oEnemy") { (legs?legsEnemy:progressEnemy)++; enemies.insert((int)self->id); }
    static DWORD since; DWORD now=GetTickCount(); if(!since) since=now;
    if(now-since>=5000) {
        if(CoopVerbose()) CoopLog("Visuals: per second - torso/spine progress: stand-in %.1f, player %.1f, per guard %.1f; legs: stand-in %.1f, player %.1f, per guard %.1f",progressPuppet/5.0,progressPlayer/5.0,enemies.empty()?0:progressEnemy/5.0/enemies.size(),legsPuppet/5.0,legsPlayer/5.0,enemies.empty()?0:legsEnemy/5.0/enemies.size());
        progressPuppet=progressPlayer=legsPuppet=legsPlayer=progressEnemy=legsEnemy=0; enemies.clear(); since=now;
    }
}
void VisualsInit(const HS_ModApi* a,int* localPlayer,int* puppetId) {
    api=a; player=localPlayer; puppet=puppetId;
    api->SubscribeHook("gml_Script_SetTorsoAnim",preSetTorsoAnim,nullptr);
    api->SubscribeHook("gml_Script_ProgressSpineLegAndTorsoFrames",preProgressFrames,nullptr);
    api->SubscribeHook("gml_Script_AnimateLegsOnGround",preProgressFrames,(void*)1);
    api->SubscribeHook("gml_Script_SetAttachment",preSetAttachment,nullptr);
    api->SubscribeHook("gml_Script_PlayTorsoAnimOnce",prePlayTorsoAnimOnce,nullptr);
    api->SubscribeHookPost("gml_Script_GunFiresBulletInDirection",postGunFires,nullptr);
    api->SubscribeHook("gml_Script_PlayTeleportEffectOnShip",prePlayTeleportEffect,nullptr);
    api->SubscribeHook("gml_Script_PlayTeleportScreenEffect",prePlayTeleportScreenEffect,nullptr);
    api->SubscribeHook("gml_Script_SetDamageSignal",preCopyBulletDamage,nullptr);
    api->SubscribeHookPost("gml_Script_PlayGunSoundAtAppropriatePitch",postGunSound,nullptr);
}
