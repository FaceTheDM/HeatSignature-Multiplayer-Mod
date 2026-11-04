// Heat Signature co-op plugin: transform sync of one remote player rendered as a native oPlayer puppet.
#include "ModInterface.h"
#include <wincrypt.h>
#include <cstdio>

#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include "GmArgs.h"
#include "Console.h"
#include "Lobby.h"
#include "Report.h"
#include "Steam.h"
#include "Seed.h"
#include "Place.h"
#ifdef HS_COOP_DEVTOOLS   // developer builds only (build.py --dev): the private developer extension, not in this repository
#include "DevPort.h"
#endif
#include "Net.h"
#include "Seed.h"
#include "Missions.h"
#include "Guards.h"
#include "TimePolicy.h"
#include "Snapshot.h"
#include "Wire.h"
#include "Frame.h"
#include "Presence.h"
#include "Visuals.h"
#include "Crate.h"
#include "Hull.h"
#include "Objects.h"
#include "GalaxySync.h"
#include "TestMenu.h"
#include <string>
#include <set>
#include <vector>
#include <cctype>
#include <cstring>

static const HS_ModApi* api;
static CoopContext ctx;
static char baseDir[MAX_PATH];
static bool testWindow, netReady, debugMode, verboseMode;
bool CoopVerbose() { return verboseMode; }
static const double OBJ_PLAYER=137, OBJ_SHIP=164;
static const DWORD SEND_INTERVAL_MS=16, REMOTE_TIMEOUT_MS=3000;
static const double kPi=3.14159265358979323846;
// Build id: both players must run the same plugin build (protocol and generation seeds depend on it).
static const uint32_t BUILD_ID=HS_COOP_BUILD;   // content hash of the sources, passed in by build.py

static std::vector<std::string> logSecrets;
void CoopLogHide(const char* secret) {
    if(!secret || strlen(secret)<6) return;
    for(const std::string& s:logSecrets) if(s==secret) return;
    if(logSecrets.size()>=16) logSecrets.erase(logSecrets.begin());
    logSecrets.push_back(secret);
}
// Reads a dotted IPv4 address at s; returns its length (0 if there is none).
static size_t readIPv4(const char* s,unsigned out[4]) {
    const char* p=s;
    for(int i=0;i<4;i++) {
        if(!isdigit((unsigned char)*p)) return 0;
        unsigned v=0; int digits=0; while(isdigit((unsigned char)*p) && digits<4) { v=v*10+(*p-'0'); p++; digits++; }
        if(digits>3 || v>255) return 0;
        out[i]=v; if(i<3) { if(*p!='.') return 0; p++; }
    }
    return isdigit((unsigned char)*p)||*p=='.'&&isdigit((unsigned char)p[1]) ? 0 : (size_t)(p-s);
}
static std::string maskLogLine(const char* line) {
    std::string in=line;
    for(const std::string& s:logSecrets) for(size_t at;(at=in.find(s))!=std::string::npos;) in.replace(at,s.size(),"<code>");
    std::string out; out.reserve(in.size());
    for(size_t i=0;i<in.size();) {
        unsigned a[4]; size_t n=(i==0||!isdigit((unsigned char)in[i-1])&&in[i-1]!='.')?readIPv4(in.c_str()+i,a):0;
        if(!n) { out+=in[i++]; continue; }
        bool keep=a[0]==127||(a[0]==0&&a[1]==0&&a[2]==0&&a[3]==0);
        bool priv=a[0]==10||(a[0]==192&&a[1]==168)||(a[0]==172&&a[1]>=16&&a[1]<=31)||(a[0]==100&&a[1]>=64&&a[1]<=127)||(a[0]==169&&a[1]==254);
        if(keep) out.append(in,i,n);
        else if(priv) { char b[24]; snprintf(b,sizeof(b),"%u.%u.x.x",a[0],a[1]); out+=b; }
        else out+="<public-ip>";
        i+=n;
    }
    return out;
}
void CoopLog(const char* fmt,...){char raw[2048];va_list a;va_start(a,fmt);vsnprintf(raw,sizeof(raw),fmt,a);va_end(a);
    std::string masked; if(!debugMode) masked=maskLogLine(raw);
    const char* b=debugMode?raw:masked.c_str();
    api->Log("HS_Coop",b); ReportLogLine(b);
#ifdef HS_COOP_DEVTOOLS
    if(DevCapturing()) DevCaptureLine(b); else if(debugMode) EventsPush("%s",b);
#endif
}

static double getReal(int id,const char* name){ RValue v{}; v.type=5; api->GetVarByName(id,name,0x80000000,&v); return Number(v); }
static void setReal(int id,const char* name,double value){ RValue v{}; v.type=0; v.real=value; api->SetVarByName(id,name,0x80000000,&v); }
static bool getString(int id,const char* name,char* out,size_t size) {
    RValue v{}; v.type=5; api->GetVarByName(id,name,0x80000000,&v);
    if(GetRValueKind(v)!=RValueKind::String || !v.str || !v.str->text){ if(size) *out=0; return false; }
    snprintf(out,size,"%s",v.str->text); return true;
}
static CInstance* live(int id,double object=-1) {
    if(id<=0) return nullptr;
    CInstance* inst=api->ResolveCInstance(id);
    if(!inst || inst->id!=(uint32_t)id || (object>=0 && inst->type!=(uint32_t)object)) return nullptr;
    return inst;
}
static RValue script(const char* name,CInstance* self,GmArgs& args){ RValue r{}; r.type=5; api->CallScript(name,self,self,&r,args.Count(),args.Build()); return r; }

// ---- Test-window mode (only when launched by launch.py) ----
// The test window is 16:9, as large as 1920x1080 but never larger than the screen allows. A window bigger than the
// display is clamped by Windows and never reaches the size asked for; asking again every frame (as this did) made the
// game rebuild its screen surfaces every frame, which destroyed the ships' drawing surfaces over and over: sections
// drawn out of place, stutter, and the "Tried to draw module to non-existant surface" error box. (Seen on a portrait
// 1080x1920 screen, where both test copies ran like that.)
static void testWindowSize(CInstance* self,double& w,double& h) {
    GmArgs none; double dw=Number(Engine(api,"display_get_width",self,0,none.Build())), dh=Number(Engine(api,"display_get_height",self,0,none.Build()));
    double k=1;
    if(dw>200 && dh>200) k=std::min(1.0,std::min((dw-40)/1920.0,(dh-120)/1080.0));
    w=std::floor(1920*k); h=std::floor(1080*k);
}
// Test copies started for automated runs (HS_COOP_HIDDEN) are kept off the developer's screens: the window keeps its size
// (minimized, the game's cursor and input code broke: a window with no size) and sits just past the right edge of the
// virtual desktop, never taking focus. The game (resizing its window) can move it back, so it is moved again when it is.
static bool hiddenWindow=getenv("HS_COOP_HIDDEN")!=nullptr;
// The game's main window: this process's top-level GameMaker window (the loader's GetGameWindow may not know it yet).
static BOOL CALLBACK findGameWindow(HWND h,LPARAM out) {
    DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    if(pid!=GetCurrentProcessId() || GetWindow(h,GW_OWNER)) return TRUE;
    char cls[64]; GetClassNameA(h,cls,sizeof(cls));
    if(!strcmp(cls,"YYGameMakerYY")) { *(HWND*)out=h; return FALSE; }
    return TRUE;
}
static HWND ownGameWindow() {
    HWND found=nullptr;
    EnumWindows(findGameWindow,(LPARAM)&found);
    if(!found && api->GetGameWindow) found=api->GetGameWindow();
    return found;
}
static void keepHidden() {
    if(!hiddenWindow) return;
    HWND w=ownGameWindow(); if(!w) return;
    int x=GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN)+64, y=GetSystemMetrics(SM_YVIRTUALSCREEN);
    if(IsIconic(w)) {
        // Restored straight to the off-screen spot (it started minimized), so it never shows on screen in between.
        WINDOWPLACEMENT p{}; p.length=sizeof(p);
        if(GetWindowPlacement(w,&p)) { int cw=p.rcNormalPosition.right-p.rcNormalPosition.left, ch=p.rcNormalPosition.bottom-p.rcNormalPosition.top;
            p.rcNormalPosition={x,y,x+cw,y+ch}; p.showCmd=SW_SHOWNOACTIVATE; SetWindowPlacement(w,&p); }
        return;
    }
    RECT r; if(GetWindowRect(w,&r) && r.left==x && r.top==y) return;
    SetWindowPos(w,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
static void windowed(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    static bool configured=false;
    api->RequestBypass();
    if(configured) return;
    configured=true;
    double w,h; testWindowSize(self,w,h);
    GmArgs full; full.AddReal(0); Engine(api,"window_set_fullscreen",self,full.Count(),full.Build());
    GmArgs size; size.AddReal(w); size.AddReal(h); Engine(api,"window_set_size",self,size.Count(),size.Build());
    CoopLog("Windowed test mode requested: %.0fx%.0f%s",w,h,hiddenWindow?" (kept minimized)":"");
    keepHidden();
}
static void enforceTestWindow(CInstance* self) {
    keepHidden();
    // Hidden test copies are silent too (by request, 2026-10-05): the game's master volume is set to zero, again
    // every second in case the game's own settings set it back.
    if(hiddenWindow) { static DWORD lastMute; if(GetTickCount()-lastMute>1000) { lastMute=GetTickCount(); GmArgs g; g.AddReal(0); Engine(api,"audio_master_gain",self,g.Count(),g.Build()); } }
    // Corrected a few times at most, a second apart: if the window still is not that size, Windows will not give it.
    static int attempts; static DWORD last;
    if(attempts>=3 || GetTickCount()-last<1000) return;
    double tw,th; testWindowSize(self,tw,th);
    GmArgs none; double w=Number(Engine(api,"window_get_width",self,0,none.Build())), h=Number(Engine(api,"window_get_height",self,0,none.Build()));
    if(w==tw && h==th) return;
    last=GetTickCount(); attempts++;
    GmArgs a; a.AddReal(tw); a.AddReal(th); Engine(api,"window_set_size",self,a.Count(),a.Build());
    if(attempts==3) CoopLog("Test window: stays %.0fx%.0f (asked for %.0fx%.0f); leaving it",w,h,tw,th);
}

// ---- Executable guard ----
static bool supportedExecutable() {
    // Fixed-offset loader hooks are only safe for the researched executable.
    const BYTE expected[32]={0xa5,0xf8,0xbe,0xfe,0x28,0x78,0x62,0x06,0xed,0x1e,0x2a,0x52,0x38,0x50,0x51,0x04,0x95,0x24,0xfe,0xa5,0x1e,0x9e,0x80,0x66,0x0e,0x31,0x15,0xe7,0x12,0x62,0x74,0xd2};
    wchar_t path[32768];
    if(!GetModuleFileNameW(nullptr,path,32768)) return false;
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider=0; HCRYPTHASH hash=0; BYTE digest[32],buf[65536]; DWORD read=0,n=32;
    bool ok=CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT)!=0;
    if(ok) ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)!=0;
    while(ok) {
        if(!ReadFile(f,buf,sizeof(buf),&read,nullptr)){ok=false;break;}
        if(!read) break;
        ok=CryptHashData(hash,buf,read,0)!=0;
    }
    if(ok) ok=CryptGetHashParam(hash,HP_HASHVAL,digest,&n,0)!=0 && n==32 && memcmp(digest,expected,32)==0;
    if(hash) CryptDestroyHash(hash);
    if(provider) CryptReleaseContext(provider,0);
    CloseHandle(f); return ok;
}

// ---- Local player ----
static bool localGameplay() {
    if(!live(ctx.player,OBJ_PLAYER)) return false;
    char state[64]; getString(ctx.player,"State",state,sizeof(state));
    return strcmp(state,"CharacterSelect")!=0 && strcmp(state,"Resting")!=0;
}
static PlayerState readLocal() {
    PlayerState s{};
    s.inGameplay=localGameplay();
    s.timeScaleWanted=TimePolicyLocalWanted();
    s.paused=s.inGameplay?(uint8_t)TimePolicyLocalPauseKind():0;
    s.build=BUILD_ID;
    {
        int parent=(int)getReal(ctx.player,"Parent");
        s.galaxy=FrameGalaxyFingerprint(live(ctx.player,OBJ_PLAYER),getReal(parent,"Station")==1?parent:0);
    }
    if(!s.inGameplay) return s;
    int p=ctx.player;
    s.fore=getReal(p,"Fore"); s.starboard=getReal(p,"Starboard"); s.imageAngle=getReal(p,"MyImageAngle");
    // Speeds are sent per REAL second: the game keeps them per game second, so in this player's slow motion (the
    // Slipstream runs their world at 0.1 while they move at full pace) they read ten times their real pace, and the
    // other game's prediction overshot and snapped back each report (the partner rubber-banded, 2026-10-05).
    { const RValue* ts=GlobalVar(10); double k=ts?Number(*ts):1; if(!(k>=0) || k>1) k=1;
      s.foreSpeed=getReal(p,"ForeSpeed")*k; s.starboardSpeed=getReal(p,"StarboardSpeed")*k; s.measuredSpeed=getReal(p,"MeasuredSpeed")*k; }
    s.parentId=(int32_t)getReal(p,"Parent");
    CInstance* parent=live(s.parentId); s.parentObject=parent?(int32_t)parent->type:-1;
    getString(p,"Name",s.name,sizeof(s.name));
    char state[48]; getString(p,"State",state,sizeof(state));
    // Carried (by a guard or the partner) counts as down: the body hangs limp, it must not be shown standing.
    s.down=!strcmp(state,"Unconscious") || !strcmp(state,"Dead") || !strcmp(state,"Carried") || getReal(p,"Dead")==1;
    if(parent) { getString(s.parentId,"Name",s.parentName,sizeof(s.parentName)); double fx=0,fy=0; FrameRef(fx,fy); s.parentX=getReal(s.parentId,"xReal")-fx; s.parentY=getReal(s.parentId,"yReal")-fy; }
    s.seenAsThreat=(float)getReal(p,"SeenAsThreat"); getString(p,"State",s.state,sizeof(s.state));
    getString(p,"Skin",s.skin,sizeof(s.skin)); getString(p,"Head",s.head,sizeof(s.head)); s.female=getReal(p,"Female")==1;
    PresenceFill(s);
    VisualsFill(s);
    // A completed mission (objective done; the reward is claimed at home) has no ship to go to any more: the partner's
    // marker followed this and kept saying to go to the ship (2026-10-05).
    { const RValue* g=GlobalVar(2); int m=g?(int)Number(*g):0; int ship=live(m)?(int)getReal(m,"Ship"):0;
      if(live(ship) && getReal(m,"Completed")!=1) getString(ship,"Name",s.missionShip,sizeof(s.missionShip)); }
    return s;
}

// ---- Remote puppet ----
static PlayerState remote;
static DWORD remoteAt;
static bool haveRemote;
static double shownFore,shownStarboard,shownAngle;
static int puppetParent;
static bool editingPuppet;   // true while the plugin itself moves or reconfigures the puppet

// Every stand-in this game made: one that is not the current stand-in is removed as soon as it can be (a playtest saw
// the partner several times, frozen in place). A stand-in the mod lost track of (the game had deactivated it, or it
// seemed removed) used to stay in the world when a new one was made.
static std::set<int> madePuppets;
static void removeStrayPuppets() {
    for(auto it=madePuppets.begin();it!=madePuppets.end();) {
        int id=*it;
        if(id==ctx.puppet) { ++it; continue; }
        CInstance* inst=api->ResolveCInstance(id);
        if(!inst) { it=madePuppets.erase(it); continue; }   // gone for good
        GmArgs e; e.AddReal(id);
        CInstance* me=live(ctx.player,OBJ_PLAYER);
        if(!me || !(Number(Engine(api,"instance_exists",me,e.Count(),e.Build()))>0.5)) { ++it; continue; }   // inactive: later
        if(inst->type!=(uint32_t)OBJ_PLAYER) { it=madePuppets.erase(it); continue; }   // the id now belongs to something else
        editingPuppet=true;
        int current=(int)getReal(id,"Parent");
        if(current!=id && live(current)) { GmArgs a; a.AddReal(id); script("gml_Script_RemoveFromParentsContents",inst,a); }
        int legs=(int)getReal(id,"MyLegs");
        if(legs>0 && legs!=id && live(legs)) Engine(api,"instance_destroy",live(legs),0,nullptr);
        Engine(api,"instance_destroy",inst,0,nullptr);
        editingPuppet=false;
        CoopLog("Puppet %d removed: an earlier stand-in still in the world (current %d)",id,ctx.puppet);
        it=madePuppets.erase(it);
    }
}
// (Developers) how many stand-ins this game has in the world, active or not, and which is current.
int CoopStandinCount(int* current) {
    if(current) *current=ctx.puppet;
    int n=0; for(int id:madePuppets) if(api->ResolveCInstance(id) && api->ResolveCInstance(id)->type==(uint32_t)OBJ_PLAYER) n++;
    return n;
}
static void despawnPuppet(const char* why) {
    CInstance* puppet=live(ctx.puppet,OBJ_PLAYER);
    if(puppet) {
        editingPuppet=true;
        int current=(int)getReal(ctx.puppet,"Parent");
        if(current!=ctx.puppet && live(current)) { GmArgs a; a.AddReal(ctx.puppet); script("gml_Script_RemoveFromParentsContents",puppet,a); }
        int legs=(int)getReal(ctx.puppet,"MyLegs");   // a separate instance, not destroyed with its owner
        if(legs>0 && legs!=ctx.puppet && live(legs)) Engine(api,"instance_destroy",live(legs),0,nullptr);
        Engine(api,"instance_destroy",puppet,0,nullptr);
        editingPuppet=false;
        CoopLog("Puppet %d removed: %s",ctx.puppet,why);
    }
    ctx.puppet=0; puppetParent=0;
}
static void reparentPuppet(int parent);
static bool spawnPuppet(int parent) {
    CInstance* self=live(ctx.player,OBJ_PLAYER); if(!self || !live(parent,OBJ_SHIP)) return false;
    // Created directly and registered on the partner's ship. Copying the local player's parent (the game's
    // CreateObjectAtLattersOnboardPosition) fails when the local player is drifting: it has no parent then, and the
    // game's "Add to parents contents, but I don't have a parent" check fires.
    GmArgs c; c.AddReal(0); c.AddReal(0); c.AddReal(OBJ_PLAYER);
    int id=(int)Number(Engine(api,"instance_create",self,c.Count(),c.Build()));
    if(!live(id,OBJ_PLAYER)) { CoopLog("Puppet spawn failed (returned %d)",id); return false; }
    ctx.puppet=id; puppetParent=id;   // a fresh oPlayer is its own parent until registered aboard
    madePuppets.insert(id);
    shownFore=remote.fore; shownStarboard=remote.starboard; shownAngle=remote.imageAngle;
    char name[64]; snprintf(name,sizeof(name),"%s",remote.name[0]?remote.name:"Co-op partner");
    RValue s{}; s.type=1; api->SetString(&s,name); api->SetVarByName(id,"Name",0x80000000,&s);
    CoopLog("Puppet %d spawned for remote '%s' on parent %d",id,name,puppetParent);
    if(puppetParent!=parent) reparentPuppet(parent);
    return true;
}
static void reparentPuppet(int parent) {
    CInstance* puppet=live(ctx.puppet,OBJ_PLAYER); if(!puppet || !live(parent)) return;
    editingPuppet=true;
    int current=(int)getReal(ctx.puppet,"Parent");
    if(current!=ctx.puppet && live(current)) { GmArgs r; r.AddReal(ctx.puppet); script("gml_Script_RemoveFromParentsContents",puppet,r); }
    setReal(ctx.puppet,"Parent",parent); setReal(ctx.puppet,"Fore",remote.fore); setReal(ctx.puppet,"Starboard",remote.starboard);
    GmArgs a; a.AddReal(ctx.puppet); script("gml_Script_AddToParentsContents",puppet,a);
    editingPuppet=false;
    CoopLog("Puppet %d moved from parent %d to %d",ctx.puppet,puppetParent,parent);
    puppetParent=parent;
    shownFore=remote.fore; shownStarboard=remote.starboard;
}
static double angleLerp(double from,double to,double t) {
    double d=fmod(to-from+540.0,360.0)-180.0; return from+d*t;
}
// The local ship/station the remote player is aboard, matched by name and galaxy position (instance ids are process-local).
// Ships the game has begun destroying (DestroyShip). One can linger for a frame or two with its name, and a stand-in put
// on it then stopped the game ("Trying to removefromcontents oPlayer but parent doesn't exist"), seen when practice
// was restarted (it destroys the practice ship and builds it again under the same name).
static std::set<int> dyingShips;
static int remoteParentHere() {
    // Only ships and stations (oShip) are shared; a floating player is their own parent.
    if(!remote.parentName[0] || remote.parentObject!=(int32_t)OBJ_SHIP) return 0;
    int localParent=(int)getReal(ctx.player,"Parent");
    if(dyingShips.count(localParent)) return 0;
    char localName[48]; getString(localParent,"Name",localName,sizeof(localName));
    if(!strcmp(localName,remote.parentName)) return localParent;
    // A station's interior ship is generated from the shared seed (same layout), but its name is picked by each game:
    // aboard the same galaxy station counts as the same ship.
    if(remote.stationName[0] && live(localParent) && getReal(localParent,"Station")==1) {
        int st=(int)getReal(localParent,"MyStation");
        char stName[48]; getString(st,"Name",stName,sizeof(stName));
        if(live(st) && !strcmp(stName,remote.stationName)) {
            // The partner's game (client) takes the host's name for it, so everything matched by ship name (doors,
            // objects, guards, the stand-in) lines up.
            if(!NetIsHost()) {
                RValue v{}; v.type=1; api->SetString(&v,remote.parentName); api->SetVarByName(localParent,"Name",0x80000000,&v);
                CoopLog("Station ship '%s' at %s is '%s' in the host's game; renamed to match",localName,stName,remote.parentName);
            }
            return localParent;
        }
    }
    // (Never one being destroyed: leaving practice with the partner still on the practice ship found it by name as the
    // game destroyed it, and the stand-in put on it stopped the game, 2026-10-05.)
    int found=MissionsFindShip(remote.parentName,remote.parentX,remote.parentY);
    return dyingShips.count(found)?0:found;
}
const char* CoopRemoteMissionShip() { return haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS && remote.inGameplay ? remote.missionShip : nullptr; }
bool CoopPartnerConnected() { return haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS && remote.inGameplay; }
bool CoopRemoteWorld(double& x,double& y) { if(!CoopPartnerConnected()) return false; x=remote.worldX; y=remote.worldY; return true; }
int CoopRemoteParent() { return ctx.puppet && puppetParent!=ctx.puppet?puppetParent:0; }
// The puppet is a freshly generated character; give it the partner's look the way InitialiseSpineAnimation does:
// SetSkin on the body (the legs skeleton has its own skins) and the head as three attachments (HeadTop/HeadSide/HeadBack).
static void applyAppearance() {
    static int appliedTo; static std::string appliedLook;
    std::string look=std::string(remote.skin)+"|"+remote.head;
    if(!remote.skin[0] || !remote.head[0] || (appliedTo==ctx.puppet && appliedLook==look)) return;
    CInstance* puppet=live(ctx.puppet,OBJ_PLAYER); if(!puppet) return;
    int p=ctx.puppet;
    RValue sk{}; sk.type=1; api->SetString(&sk,remote.skin); api->SetVarByName(p,"Skin",0x80000000,&sk);
    RValue hd{}; hd.type=1; api->SetString(&hd,remote.head); api->SetVarByName(p,"Head",0x80000000,&hd);
    setReal(p,"Female",remote.female);
    GmArgs body; body.AddStr(api,remote.skin); script("gml_Script_SetSkin",puppet,body);
    for(const char* slot:{"HeadTop","HeadSide","HeadBack"}) {
        std::string attachment=std::string(remote.head)+"/"+slot;
        GmArgs a; a.AddStr(api,slot); a.AddStr(api,attachment.c_str()); script("gml_Script_SetAttachment",puppet,a);
    }
    appliedTo=ctx.puppet; appliedLook=look;
    CoopLog("Puppet %d now looks like the partner (%s, %s)",p,remote.skin,remote.head);
}
static double nowPreciseMs() { static LARGE_INTEGER f; if(!f.QuadPart) QueryPerformanceFrequency(&f); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart*1000.0/f.QuadPart; }
static void floatPuppet() {
    double fx,fy; if(!FrameRef(fx,fy)) return;
    if(ctx.puppet && puppetParent!=ctx.puppet) despawnPuppet("partner is drifting in space");
    if(!live(ctx.puppet,OBJ_PLAYER)) {
        CInstance* self=live(ctx.player,OBJ_PLAYER); if(!self) return;
        GmArgs c; c.AddReal(0); c.AddReal(0); c.AddReal(OBJ_PLAYER);
        int id=(int)Number(Engine(api,"instance_create",self,c.Count(),c.Build()));
        if(!live(id,OBJ_PLAYER)) return;
        ctx.puppet=id; puppetParent=id; madePuppets.insert(id);
        char name[64]; snprintf(name,sizeof(name),"%s",remote.name[0]?remote.name:"Co-op partner");
        RValue s{}; s.type=1; api->SetString(&s,name); api->SetVarByName(id,"Name",0x80000000,&s);
        CoopLog("Puppet %d shown drifting in space for '%s'",id,name);
    }
    applyAppearance();
    int p=ctx.puppet; CInstance* puppet=live(p,OBJ_PLAYER); if(!puppet) return;
    // Drawn smoothly (placed only at each report, the drifting partner jumped): their velocity from the last two reports,
    // carried on since the last one, and eased toward.
    static double shownX,shownY,lastWX,lastWY,vx,vy; static DWORD lastAt; static double lastDraw; static bool have;
    if(remoteAt!=lastAt) {
        if(have && lastAt && remoteAt>lastAt) { double dt=(remoteAt-lastAt)/1000.0; if(dt>0.005 && dt<1) { vx=(remote.worldX-lastWX)/dt; vy=(remote.worldY-lastWY)/dt; } }
        lastWX=remote.worldX; lastWY=remote.worldY; lastAt=remoteAt;
    }
    double age=(GetTickCount()-remoteAt)/1000.0; if(age>0.3) age=0.3;
    double tx=remote.worldX+vx*age, ty=remote.worldY+vy*age;
    double nowMs=nowPreciseMs(), fdt=lastDraw?(nowMs-lastDraw)/1000.0:0; lastDraw=nowMs;
    if(!have || std::hypot(shownX-tx,shownY-ty)>3000) { shownX=tx; shownY=ty; have=true; }
    double k=fdt*12; if(k>1) k=1; shownX+=(tx-shownX)*k; shownY+=(ty-shownY)*k;
    setReal(p,"xReal",fx+shownX); setReal(p,"yReal",fy+shownY);
    setReal(p,"xSpeed",0); setReal(p,"ySpeed",0); setReal(p,"MyImageAngle",remote.imageAngle);
    GmArgs none; script("gml_Script_SetImageAngle",puppet,none);
    // Its torso needs an animation every frame, or the game stops with "Torso animation not found".
    VisualsApplyToPuppet(puppet,remote);
    script("gml_Script_ProgressSpineLegAndTorsoFrames",puppet,none);
}
static double speedToGame=1;   // real-second speeds (as sent) to this game's per-game-second units
static void applyPuppet(double dt) {
    { static DWORD lastStray; if(GetTickCount()-lastStray>500) { lastStray=GetTickCount(); removeStrayPuppets(); } }
    // The game can destroy the stand-in itself (it is a person aboard: an airlock that ejects its contents takes it
    // too). A destroyed instance still resolves by id for a while, with every variable blank; using it raised "Add to
    // parents contents, but I don't have a parent. I'm a oPlayer". It is forgotten here and a new one is made below.
    if(ctx.puppet) {
        CInstance* me=live(ctx.player,OBJ_PLAYER);
        GmArgs e; e.AddReal(ctx.puppet);
        if(me && !(Number(Engine(api,"instance_exists",me,e.Count(),e.Build()))>0.5)) {
            CoopLog("Puppet %d no longer exists (removed by the game); a new one will be made",ctx.puppet);
            ctx.puppet=0; puppetParent=0;
        }
    }
    bool fresh=haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS;
    if(!fresh || !remote.inGameplay || !localGameplay()) {
        if(ctx.puppet && haveRemote && GetTickCount()-remoteAt>=REMOTE_TIMEOUT_MS) { despawnPuppet("remote timed out"); haveRemote=false; }
        // Partner at character select or dead: guards must not keep fighting a stand-in whose hits would land on
        // whatever character they pick next.
        else if(ctx.puppet && fresh && !remote.inGameplay) despawnPuppet("partner left gameplay");
        return;
    }
    static std::string lastWhere;
    int parent=remoteParentHere();
    std::string where=parent?std::string("aboard '")+remote.parentName+"'":std::string("not on a shared ship ('")+remote.parentName+"')";
    if(where!=lastWhere){ CoopLog("Remote player %s",where.c_str()); lastWhere=where; }
    // A partner drifting in space is shown floating where they are (a fresh oPlayer is its own parent, as a drifting
    // player is). A partner in a pod is drawn as their pod; on a ship this world does not share, only by arrow.
    bool inDockedPod=remote.mode==1 && remote.podDocked;
    if(!parent && (remote.mode==2 || inDockedPod)) { floatPuppet(); return; }
    if(!parent) { if(ctx.puppet) despawnPuppet("remote left the shared ship"); return; }
    if(ctx.puppet && puppetParent==ctx.puppet) despawnPuppet("partner came aboard");
    if(!live(ctx.puppet,OBJ_PLAYER) && !spawnPuppet(parent)) return;
    // Nothing else may move the puppet to another parent (e.g. a guard carrying a "body"); re-assert it. If the game keeps
    // pushing it off (the partner stands where our copy has no floor, e.g. inside their docked pod), stop fighting it for
    // a few seconds and let the arrow show them instead.
    static DWORD windowStart, pausedUntil; static int reattaches, pauses;
    DWORD nowMs=GetTickCount();
    if(nowMs<pausedUntil) { if(ctx.puppet) despawnPuppet("partner is where this copy of the ship has no floor"); return; }
    if(nowMs-pausedUntil>20000) pauses=0;
    if(puppetParent!=parent || (int)getReal(ctx.puppet,"Parent")!=parent) {
        if(nowMs-windowStart>2000){ windowStart=nowMs; reattaches=0; }
        if(++reattaches>4) {
            int n=pauses<4?pauses:4; pausedUntil=nowMs+(3000u<<n); reattaches=0; pauses++;
            CoopLog("Puppet keeps leaving '%s' at %.0f,%.0f; showing the partner by arrow for %us",remote.parentName,remote.fore,remote.starboard,(3000u<<n)/1000);
            return;
        }
        reparentPuppet(parent);
    }
    applyAppearance();
    // The partner's position is predicted to now from their speed (the network delay is made up) and eased lightly, so
    // their steps here keep pace with their own screen.
    double age=(GetTickCount()-remoteAt)/1000.0+0.03; if(age>0.2) age=0.2;
    double tf=remote.fore+remote.foreSpeed*age, ts=remote.starboard+remote.starboardSpeed*age;
    if(std::hypot(tf-shownFore,ts-shownStarboard)>300) { shownFore=tf; shownStarboard=ts; }
    double t=dt*25.0; if(t>1) t=1;
    shownFore+=(tf-shownFore)*t; shownStarboard+=(ts-shownStarboard)*t; shownAngle=angleLerp(shownAngle,remote.imageAngle,t);
    int p=ctx.puppet;
    setReal(p,"Fore",shownFore); setReal(p,"Starboard",shownStarboard); setReal(p,"MyImageAngle",shownAngle);
    // Back to this game's units (per game second) for the game's own use of them (legs, guards leading their aim).
    { const RValue* ts=GlobalVar(10); double k=ts?Number(*ts):1; if(!(k>0.05)) k=0.05; if(k>1) k=1; speedToGame=1/k; }
    setReal(p,"ForeSpeed",remote.foreSpeed*speedToGame); setReal(p,"StarboardSpeed",remote.starboardSpeed*speedToGame);
    CInstance* puppet=live(p,OBJ_PLAYER);
    GmArgs none;
    PlaceAboard(api,puppet);
    script("gml_Script_SetImageAngle",puppet,none);
    // The puppet keeps its own Resting state machine (bypassing it leaves no torso animation and the game raises a modal
    // error), but its LookDead pose is blocked. Give it a standing torso and let the game animate its legs from the remote speed.
    static bool shownDown; static int shownDownOn;
    if(shownDownOn!=ctx.puppet) { shownDownOn=ctx.puppet; shownDown=false; }
    if(remote.down) {
        if(!shownDown){ shownDown=true; VisualsPuppetLookDead(puppet); CoopLog("Partner is down"); }
        return;
    }
    if(shownDown){ shownDown=false; CoopLog("Partner is up again"); }
    // The partner's weapon and torso animation (idle/aim/shoot/swing); legs from their speed.
    VisualsApplyToPuppet(puppet,remote);
    setReal(p,"MeasuredSpeed",remote.measuredSpeed*speedToGame);
    // The stand-in's own step already advances its torso (spine) frames; advancing them here too played its animations
    // at double speed. Its legs are only animated here (its Resting state does not walk).
    script("gml_Script_AnimateLegsOnGround",puppet,none);
}
bool CoopRemoteDown() { return remote.down!=0; }
double CoopRemoteSeenAsThreat() { return remote.seenAsThreat; }
const char* CoopRemoteStateName() { return remote.state; }
unsigned CoopRemoteAgeMs() { return haveRemote?GetTickCount()-remoteAt:100000; }
static void scriptedGoto() {
    if(!ctx.gotoActive || !live(ctx.player,OBJ_PLAYER)) return;
    DWORD now=GetTickCount(); double dt=(now-ctx.gotoLastMs)/1000.0; ctx.gotoLastMs=now; if(dt>0.1) dt=0.1;
    double f=getReal(ctx.player,"Fore"), s=getReal(ctx.player,"Starboard"), df=ctx.gotoFore-f, ds=ctx.gotoStarboard-s, d=std::hypot(df,ds);
    double step=ctx.gotoSpeed*dt;
    // Blocked (a wall or furniture between here and the target: the game pushes the player back out each frame, or slides
    // them along it): give up rather than pull the player against it for ever. Each second must close at least half of
    // what walking at gotoSpeed would.
    if(ctx.gotoBest>1e17) { ctx.gotoBest=d; ctx.gotoBestMs=now; }
    else if(now-ctx.gotoBestMs>=1000) {
        bool stuck=ctx.gotoBest-d < ctx.gotoSpeed*0.5*(now-ctx.gotoBestMs)/1000.0;
        ctx.gotoBest=d; ctx.gotoBestMs=now;
        if(stuck) { ctx.gotoActive=false; setReal(ctx.player,"MeasuredSpeed",0); CoopLog("goto blocked at (%g,%g), %g short of (%g,%g): stopped",f,s,d,ctx.gotoFore,ctx.gotoStarboard); return; }
    }
    if(d<=step) { f=ctx.gotoFore; s=ctx.gotoStarboard; ctx.gotoActive=false; setReal(ctx.player,"MeasuredSpeed",0); CoopLog("goto arrived at (%g,%g)",f,s); }
    else { f+=df/d*step; s+=ds/d*step; setReal(ctx.player,"MeasuredSpeed",ctx.gotoSpeed); setReal(ctx.player,"MyImageAngle",atan2(-ds,df)*180/kPi); }
    setReal(ctx.player,"Fore",f); setReal(ctx.player,"Starboard",s);
    GmArgs none; script("gml_Script_UpdatePosition",live(ctx.player,OBJ_PLAYER),none);   // dev-only walk of the local player
}
static void scriptedWalk() {
    if(ctx.walkSeconds<=0 || !live(ctx.player,OBJ_PLAYER)) return;
    double t=(GetTickCount()-ctx.walkStartMs)/1000.0;
    if(t>ctx.walkSeconds){ ctx.walkSeconds=0; setReal(ctx.player,"MeasuredSpeed",0); CoopLog("walk finished"); return; }
    // One lap every 6 seconds at walking pace; Fore/Starboard are moved per frame like real movement.
    double a=t/6.0*2*kPi, speed=ctx.walkRadius*2*kPi/6.0;
    setReal(ctx.player,"Fore",ctx.walkFore+cos(a)*ctx.walkRadius-ctx.walkRadius);
    setReal(ctx.player,"Starboard",ctx.walkStarboard+sin(a)*ctx.walkRadius);
    setReal(ctx.player,"MyImageAngle",fmod(a*180/kPi+90,360));
    setReal(ctx.player,"MeasuredSpeed",speed);
}

// ---- Network dispatch ----
static bool statusNow;
void CoopRequestStatus() { statusNow=true; }
static void onState(const PlayerState& in) {
    if(!haveRemote) CoopLog("Net: first state from %s (gameplay=%d)",NetPeerText(),in.inGameplay);
    remote=in; remoteAt=GetTickCount(); haveRemote=true;
}
static void onMessage(uint8_t type,const uint8_t* data,size_t size,bool) {
    switch(type) {
    case MSG_MISSION_SHIP: case MSG_MISSION_DONE: case MSG_SHIP_RENAME: case MSG_MISSION_FAILED: case MSG_SHIP_MOVE: case MSG_SHIP_ARRIVED: case MSG_SHIP_GONE: MissionsOnMessage(type,data,size); break;
    case MSG_GUARDS: case MSG_ALARM_REQUEST: case MSG_GUARD_HIT: case MSG_PLAYER_HIT: case MSG_BODY: case MSG_GUARD_REQUEST: case MSG_GUARD_SPAWN: case MSG_GUARD_ALERT: case MSG_MISSION_EVENT: case MSG_GUARD_ANIM: case MSG_TARGET_DELIVERED: case MSG_SLOWMO: case MSG_GUARD_KIT: case MSG_REINFORCEMENT: GuardsOnMessage(type,data,size); break;
    case MSG_INVITE: case MSG_RESCUE: case MSG_SCOOP_SOUND: case MSG_RESET: case MSG_GOTO_MISSION: PresenceOnMessage(type,data,size); break;
    case MSG_SHOT: case MSG_SOUND: case MSG_LOOK: case MSG_BULLET: case MSG_TELEPORT_FX: VisualsOnMessage(type,data,size); break;
    case MSG_TEST: if(debugMode) TestMenuOnMessage(type,data,size); break;
    case MSG_GALAXY_REQUEST: case MSG_GALAXY_FILE: GalaxySyncOnMessage(type,data,size); break;
    case MSG_MODULE_GONE: case MSG_SHIP_PIECE: HullOnMessage(type,data,size); break;
    case MSG_REPORT_REQUEST: case MSG_REPORT_LOG: ReportOnMessage(type,data,size); break;
    case MSG_CRATE_MENU: case MSG_CRATE_SUGGEST: case MSG_CRATE_ITEM: CrateOnMessage(type,data,size); break;
    case MSG_OBJECTS: case MSG_EXPLOSION: case MSG_GUARD_SOUND: case MSG_DOORS: case MSG_ITEM_DROP: case MSG_LOOT_LIST: case MSG_EFFECT: case MSG_KEYCARD: case MSG_KEY_LEVEL: case MSG_SENTRY_STATE: case MSG_SENTRY_FIRE: case MSG_SENTRY_ACTION: case MSG_OBJECTS_RESYNC: case MSG_GUARDS_TAKE_OVER: ObjectsOnMessage(type,data,size); break;
    default: break;
    }
}
static void onSessionReset() {
    CoopLog("Net: partner restarted; reliable channel reset");
    MissionsResend();        // the (re)connected partner needs our mission ships again
    ObjectsResync();         // and the full state of the shared ship's objects
    GalaxySyncReset();
}
const char* CoopRemoteName() { return haveRemote?remote.name:""; }
bool CoopRemotePaused() { return haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS && remote.inGameplay && remote.paused; }
const char* CoopRemotePractice() { return haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS && remote.inGameplay ? remote.practice : ""; }
int CoopRemotePauseKind() { return CoopRemotePaused()?remote.paused:0; }
double CoopRemoteTimeScaleWanted() { return haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS && remote.inGameplay ? remote.timeScaleWanted : 0; }

// ---- Per-frame tick ----
static double lastTickMs;   // when the mod's frame last ran (high-resolution ms)
static void tick(CInstance* self) {
    static DWORD lastSend,lastConsole,lastStatus;
    // tick is reachable from more than one per-frame hook; run it at most once per frame. GetTickCount (~16 ms
    // resolution) would skip real frames here, so frames are told apart with the high-resolution counter.
    static LARGE_INTEGER frequency; if(!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
    double nowMs=counter.QuadPart*1000.0/frequency.QuadPart;
    if(lastTickMs && nowMs-lastTickMs<4) return;
    double dt=lastTickMs?(nowMs-lastTickMs)/1000.0:0; lastTickMs=nowMs;
    if(dt>0.25) dt=0.25;
    DWORD now=GetTickCount();
#ifdef HS_COOP_DEVTOOLS
    ctx.api=api; if(debugMode) DevPortTick(ctx,self);
#endif
    if(now-lastConsole>=250) {
        lastConsole=now;
        if(testWindow) enforceTestWindow(self);
        char path[MAX_PATH]; snprintf(path,sizeof(path),"%scoop-command.txt",baseDir);
        ctx.api=api; if(debugMode) ConsoleRun(ctx,self,path);
    }
    if(!netReady) return;
    scriptedWalk(); scriptedGoto();
    NetHandlers handlers{onState,onMessage,onSessionReset};
    NetPump(handlers);
    if(now-lastSend>=SEND_INTERVAL_MS) { lastSend=now; NetSendState(readLocal()); }
    MissionsTick(self);
    applyPuppet(dt);
    // Test commands first: hits they set must be seen by the guard sync before the guards' own step consumes them.
    if(debugMode) TestMenuTick(self,remote,haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS);
    GuardsTick(self,localGameplay());
    ObjectsTick(self,GuardsSharedShip());
    {
        bool fresh=haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS;
        GalaxySyncTick(NetIsHost(),FrameGalaxyFingerprint(nullptr,0),remote.galaxy,fresh,remote.build==BUILD_ID);
    }
    TimePolicyTick(self);
    PresenceTick(self,remote,haveRemote && GetTickCount()-remoteAt<REMOTE_TIMEOUT_MS);
    NetPump(handlers);   // flush messages produced this frame
    {   // Session recorder (see Report.h): once a second.
        static DWORD lastRecord;
        if(now-lastRecord>=1000) {
            lastRecord=now;
            PlayerState l=readLocal(); bool fresh=haveRemote && now-remoteAt<REMOTE_TIMEOUT_MS;
            char st[48]={0}; if(live(ctx.player,OBJ_PLAYER)) getString(ctx.player,"State",st,sizeof(st));
            int shared=GuardsSharedShip(); char sharedName[96]={0}; if(shared) getString(shared,"Name",sharedName,sizeof(sharedName));
            char line[600];
            snprintf(line,sizeof(line),"me state=%s mode=%d ship='%s' fore=%.0f stb=%.0f | partner %s state=%s mode=%d down=%d ship='%s' fore=%.0f stb=%.0f standin=%d | shared='%s' %s alarm=%.0f | net %s",
                st,l.mode,l.parentName,l.fore,l.starboard,fresh?"here":"absent",remote.state,(int)remote.mode,(int)remote.down,remote.parentName,remote.fore,remote.starboard,ctx.puppet,
                sharedName,shared?(GuardsIsAuthority()?"runs-guards":"follows"):"-",shared?getReal(shared,"Alarm"):0.0,haveRemote?NetPeerText():"none");
            ReportRecord(line);
        }
    }
    if(statusNow || now-lastStatus>=(verboseMode?10000u:60000u)) {
        lastStatus=now; statusNow=false;
        PlayerState l=readLocal();
        CoopLog("status local=%d gameplay=%d fore=%.1f stb=%.1f parent='%s' | remote=%s gameplay=%d parent='%s' | puppet=%d | %s | %s",
            ctx.player,l.inGameplay,l.fore,l.starboard,l.parentName,haveRemote?NetPeerText():"none",remote.inGameplay,remote.parentName,ctx.puppet,MissionsStatus(),GuardsStatus());
    }
}

// ---- Hooks ----
// The game's global `Player` is authoritative; hook-derived ids are only a fallback.
static void refreshPlayer() {
    const RValue* g=GlobalVar(GLOBAL_PLAYER);
    int id=g?(int)Number(*g):0;
    if(id>0 && id!=ctx.puppet && live(id,OBJ_PLAYER) && id!=ctx.player) { if(ctx.player) CoopLog("Local player now %d (global Player)",id); ctx.player=id; }
}
// ---- One apply phase ----
// The game moves every ship, everyone aboard and the camera in one End Step chain (each End Step calls UpdatePosition or,
// for a ship, UpdateSpacePosition; nothing else calls them). The mod's whole frame (network, mirror writes, sends) runs
// once, at the first of those calls: after every Step (the game's aim, hit and sight code, and the player's own input)
// and before anything is placed. Mirrored Fore/Starboard written here are then placed by the game itself, in the same
// moment as everything else. Frames with no End Step chain (menus, character select) tick from the Draw pass instead.
static bool endStepTicked;
static bool stepTick=getenv("HS_COOP_TICK_STEP")!=nullptr;   // (developers) the old placement, in the player's Step, to compare
static void preEndStepChain(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(stepTick || endStepTicked) return;
    endStepTicked=true;   // before tick: scripts it calls may run UpdatePosition again
    refreshPlayer();
    if(ctx.player && live(ctx.player,OBJ_PLAYER)) tick(live(ctx.player,OBJ_PLAYER));
    else if(self) tick(self);
}
static void drawPassTick(CInstance* self) {
    static double lastDrawMs;
    static LARGE_INTEGER frequency; if(!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter; QueryPerformanceCounter(&counter);
    double nowMs=counter.QuadPart*1000.0/frequency.QuadPart;
    bool sameFrame=lastDrawMs && nowMs-lastDrawMs<4; lastDrawMs=nowMs;   // drawn more than once in a frame
    if(sameFrame) return;
    if(stepTick) { tick(self); return; }   // at most once per frame (tick's own check)
    if(endStepTicked) { endStepTicked=false; return; }
    refreshPlayer(); tick(self);
}
static void frame(CInstance* self) { refreshPlayer(); if(stepTick) tick(self); }
// A dialog (e.g. the practice terminal's) runs neither the End Step chain nor the inventory draw: its own draw ticks
// instead (the dev channel and the connection stopped while one was open).
static void drawPassTick(CInstance* self);
static void postDialogDraw(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) { if(self) drawPassTick(self); }
static void onPlayAsCharacter(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(argc>0 && argv && argv[0]) ctx.player=api->ResolveInstance((uint32_t*)argv[0]);
    if(ctx.player==ctx.puppet) ctx.puppet=0;
    CoopLog("PlayAsCharacter self=%u player=%d",self?self->id:0,ctx.player);
}
static void onMovementControls(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && (int)self->id!=ctx.puppet) { if(!ctx.player) ctx.player=self->id; frame(self); }
}
static void onCharacterSelect(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self) return;
    if(!ctx.player) ctx.player=self->id;
    refreshPlayer();
    if(!endStepTicked) tick(self);   // character select has no End Step chain to tick from (tick runs once a frame)
}
static void postUpdateState(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && (int)self->id==ctx.player) frame(self);
}
static void preCharacterIsChooseable(const char*,CInstance*,CInstance*,RValue* result,int argc,RValue** argv,void*) {
    // The puppet is never a selectable roster member.
    if(argc>0 && argv && argv[0] && ctx.puppet && (int)Number(*argv[0])==ctx.puppet) { if(result){ result->type=0; result->real=0; } api->RequestBypass(); }
}
static void preLookDead(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    // The puppet is drawn standing unless the partner is actually down.
    if(self && ctx.puppet && (int)self->id==ctx.puppet && !remote.down) api->RequestBypass();
}
// The puppet only mirrors the partner: no game logic may change its state, kill it or move it between contents lists.
// ---- A player whose ship no longer exists ----
// The game stops with a fatal error ("STRING argument is unset" from Crash, via ShipwiseCheckParentIsUpdated; or from
// AlignViewAngleToParentShip) when a player's Step finds its Parent gone. Seen in a real session, 2026-10-04, right when a
// mission ship docked at a station; which removal left a player behind is not known yet, so this both records it and
// keeps the game alive: this game's own player is taken to their home station at once, and a stand-in is removed and
// made again where the partner really is.
static bool orphanDespawn;
static void preOrphanCheck(const char* name,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || self->type!=(uint32_t)OBJ_PLAYER) return;
    int id=(int)self->id; int parent=(int)getReal(id,"Parent");
    if(parent==id || live(parent)) return;   // adrift (its own parent) or aboard something that exists
    char state[64]; getString(id,"State",state,sizeof(state));
    bool isPuppet=ctx.puppet && id==ctx.puppet;
    CoopLog("DIAG orphan: %s %d (state %s) has Parent %d, which does not exist (in %s); stand-in parent %d; partner %s on '%s' mode %d",
        isPuppet?"the stand-in":id==ctx.player?"this player":"a player",id,state,parent,name,puppetParent,haveRemote?"known":"unknown",remote.parentName,(int)remote.mode);
    if(id==ctx.player) {
        bool home=PresenceRescueToHome();
        CoopLog("DIAG orphan: this player %s",home?"was taken to their home station":"could not be taken home");
        if(home && !strcmp(name,"gml_Script_PlayerStateFree")) api->RequestBypass();   // the rest of this frame's walking code is skipped
    } else {
        // A stand-in (or another character): adrift for this frame (its own parent, as the game has it for people in
        // space); the stand-in is then removed and made again where the partner really is.
        setReal(id,"Parent",id);
        if(isPuppet) orphanDespawn=true;
        if(!strcmp(name,"gml_Script_PlayerStateFree")) api->RequestBypass();
    }
}
// (Developers) leaves a player without a ship between two frames, as a ship removed under them does: applied from the
// draw pass, so the player's next Step is the first code to see it.
static int testOrphanId;
void CoopTestOrphan(int id) { testOrphanId=id; }
static void orphanTick() {
    if(testOrphanId) { if(live(testOrphanId,OBJ_PLAYER)) setReal(testOrphanId,"Parent",999999); testOrphanId=0; }
    if(orphanDespawn) { orphanDespawn=false; if(ctx.puppet) { puppetParent=0; despawnPuppet("its ship no longer existed"); } }
}
static bool argIsPuppet(int argc,RValue** argv){ return ctx.puppet && argc>0 && argv && argv[0] && (int)Number(*argv[0])==ctx.puppet; }
// Only guard logic running with the puppet as `Player` is blocked: the game's own bookkeeping (e.g. destroying a ship and
// its contents when the player undocks) must still be able to remove the puppet, or its Crash check fires.
static void prePuppetContents(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(!GuardsTargetingPuppet() || editingPuppet) return;
    if(argIsPuppet(argc,argv) || (self && ctx.puppet && (int)self->id==ctx.puppet && argc==0)) api->RequestBypass();
}
static void prePuppetSelf(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(GuardsTargetingPuppet() && !editingPuppet && self && ctx.puppet && (int)self->id==ctx.puppet) api->RequestBypass();
}
// A breach or airlock cycling ejects everything near it; the puppet only goes where the partner really is (their own game
// ejects them if they were caught in it), and an ejected puppet re-added by the mirror broke the game's contents lists.
static void prePuppetSucked(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if((self && ctx.puppet && (int)self->id==ctx.puppet) || argIsPuppet(argc,argv)) {
        if(ctx.puppet) { setReal(ctx.puppet,"Sucking",0); setReal(ctx.puppet,"Suckable",0); }
        api->RequestBypass();
    }
}
static void preEjectContents(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(argIsPuppet(argc,argv)) api->RequestBypass();
}
// ---- A new life with nobody to choose ----
// After a death the game asks StationWithPlayerOnboard where the new life starts: a station where someone of the crew
// rests. Only when there is none does it make new recruits. Then character select offers the resting characters that
// CharacterIsChooseable allows; with none it stops ("Reached SwitchToCharacterSelect and no chooseable players exist",
// the host in a playtest after a death, 2026-10-05). Not reproduced yet: when a station is found but nobody anywhere can be
// chosen, every character in the world is logged, to learn which one held the station.
static bool forceNoCrew;
void CoopForceNoCrew() { forceNoCrew=true; }
static bool anyoneChoosable(CInstance* self) {
    GmArgs c; c.AddReal(OBJ_PLAYER); int n=(int)Number(Engine(api,"instance_number",self,c.Count(),c.Build()));
    for(int i=0;i<n;i++) {
        GmArgs f; f.AddReal(OBJ_PLAYER); f.AddReal(i); int id=(int)Number(Engine(api,"instance_find",self,f.Count(),f.Build()));
        if(!live(id,OBJ_PLAYER) || id==ctx.puppet || madePuppets.count(id) || getReal(id,"Resting")!=1) continue;
        GmArgs a; a.AddReal(id); RValue r{}; api->CallScript("gml_Script_CharacterIsChooseable",self,self,&r,a.Count(),a.Build());
        if(Number(r)>0.5) return true;
    }
    return false;
}
static void postStationWithPlayer(const char*,CInstance* self,CInstance*,RValue* result,int,RValue**,void*) {
    // (49 is the game's "nobody / none" value here, as for MyAirlock: only a real station counts.)
    if(!result || !self) return; int found=(int)Number(*result); if(found<100000 || !live(found)) return;
    bool force=forceNoCrew; forceNoCrew=false;
    if(!force && anyoneChoosable(self)) return;
    GmArgs c; c.AddReal(OBJ_PLAYER); int n=(int)Number(Engine(api,"instance_number",self,c.Count(),c.Build()));
    CoopLog("DIAG new life: station %d has someone onboard but nobody can be chosen%s; %d characters:",(int)Number(*result),force?" (test)":"",n);
    for(int i=0;i<n && i<40;i++) {
        GmArgs f; f.AddReal(OBJ_PLAYER); f.AddReal(i); int id=(int)Number(Engine(api,"instance_find",self,f.Count(),f.Build()));
        if(!live(id)) continue; int par=(int)getReal(id,"Parent");
        char nm[64]="",st[32]="",stat[32]="",pn[64]="";
        getString(id,"Name",nm,sizeof(nm)); getString(id,"State",st,sizeof(st)); getString(id,"Status",stat,sizeof(stat)); if(live(par)) getString(par,"Name",pn,sizeof(pn));
        CoopLog("DIAG   %d '%s' state %s status %s resting %g parent %d '%s' station %g%s",id,nm,st,stat,getReal(id,"Resting"),par,pn,live(par)?getReal(par,"Station"):-1.0,
                (id==ctx.puppet||madePuppets.count(id))?" (stand-in)":"");
    }
    // Not overridden: telling the game "no station" (so it makes recruits) left character select unable to place its
    // camera ("STRING argument is unset" in GetX). Logged only, to find which character holds the station.
    CoopLog("Presence: nobody to choose from after the death; see the DIAG lines above (please send a bug report)");
}
// A destroyed module returns everyone standing in it to "their" pod: DestroyModule runs PlayerReturnToShip for each
// person there, and the script moves the local player into a pod. Run for the partner's stand-in, it moved this
// player into the partner's pod copy, or left them with no ship at all (a playtest host, then the game stopped at
// character select: "no chooseable players", 2026-10-05). The partner's own game returns the real partner.
static void preReturnToShip(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(self && (madePuppets.count((int)self->id) || (int)self->id==ctx.puppet)) { api->RequestBypass(); CoopLog("Puppet: the game's return-to-pod for the partner's stand-in skipped (their game does it)"); }
}
// Before the game destroys the ship the puppet stands on, take the puppet off it cleanly.
static void preDestroyShip(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    { int s_=argc>0 && argv && argv[0] ? (int)Number(*argv[0]) : 0; if(s_>0) dyingShips.insert(s_); if(self) dyingShips.insert((int)self->id); }
    if(!ctx.puppet) return;
    int ship=argc>0 && argv && argv[0] ? (int)Number(*argv[0]) : 0;
    if(puppetParent==ship || (self && puppetParent==(int)self->id)) despawnPuppet("its ship is being destroyed here");
}
static void preSaveCharacter(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    // Never persist the puppet into this player's save.
    if(self && ctx.puppet && (int)self->id==ctx.puppet) { CoopLog("Blocked save of puppet %d",ctx.puppet); api->RequestBypass(); }
}

// ---- In-game status line (drawn in the GUI pass, bottom left) ----
// A character floating in space is not drawn by the game unless it runs the Floating state (which would also make it
// breathe, drift and be scooped up); the stand-in for a drifting partner is drawn here from its own skeleton instead.
static void drawFloatingPuppet(CInstance* self) {
    if(!ctx.puppet || puppetParent!=ctx.puppet || !(remote.mode==2 || (remote.mode==1 && remote.podDocked))) return;
    CInstance* pup=live(ctx.puppet,OBJ_PLAYER); if(!pup) return;
    double wx=getReal(ctx.puppet,"xReal"), wy=getReal(ctx.puppet,"yReal"), ang=remote.imageAngle;
    const double rad=ang*kPi/180; double ax=wx+cos(rad)*100, ay=wy-sin(rad)*100;
    GmArgs x0; x0.AddReal(wx); x0.AddReal(wy); GmArgs y0; y0.AddReal(wx); y0.AddReal(wy);
    GmArgs x1; x1.AddReal(ax); x1.AddReal(ay); GmArgs y1; y1.AddReal(ax); y1.AddReal(ay);
    double sx=Number(script("gml_Script_GUIxOfXY",self,x0)), sy=Number(script("gml_Script_GUIyOfXY",self,y0));
    double tx=Number(script("gml_Script_GUIxOfXY",self,x1)), ty=Number(script("gml_Script_GUIyOfXY",self,y1));
    if(!(sx==sx) || !(tx==tx)) return;
    double pxPerUnit=hypot(tx-sx,ty-sy)/100.0, scale=getReal(ctx.puppet,"Scale"); if(!(scale>0)) scale=0.5;
    scale*=pxPerUnit; if(scale<0.05) scale=0.05;
    double rot=atan2(-(ty-sy),tx-sx)*180/kPi;
    const char* anim=VisualsPuppetTorso(); if(!anim || !*anim) return;
    if(!remote.skin[0]) return;
    GmArgs d; d.AddReal(ctx.puppet); d.AddStr(api,anim); d.AddStr(api,remote.skin); d.AddReal(0);
    d.AddReal(sx); d.AddReal(sy); d.AddReal(scale); d.AddReal(scale); d.AddReal(rot); d.AddReal(0xffffff); d.AddReal(1);
    Engine(api,"draw_skeleton_instance",self,d.Count(),d.Build());
    static DWORD lastLog; if(verboseMode && GetTickCount()-lastLog>5000){ lastLog=GetTickCount(); CoopLog("Puppet drawn drifting at screen %.0f,%.0f scale %.3f (%s, %s)",sx,sy,scale,anim,remote.skin); }
}
static void postDrawInventoryList(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    // Drawn on every call: a time-based "once per frame" guard skipped real frames (GetTickCount has ~16 ms resolution),
    // which made the line flicker. A second call in the same frame just redraws the same text.
    DWORD now=GetTickCount();
    if(!self) return;
    LobbyTick();
    if(localGameplay()) {
        LobbyInviteTick();
        GmArgs none0; double gh=Number(Engine(api,"display_get_gui_height",self,0,none0.Build())); if(!(gh>0)) gh=1080;
        bool partnerHere=netReady && haveRemote && now-remoteAt<REMOTE_TIMEOUT_MS;
        LobbyDraw(self,TimePolicyLocalPauseKind()==1,partnerHere,remote.name,gh);
        // Joined with a code: brought to the host's station once both are in the game.
        static bool wantJoin; if(LobbyTakeJoinRequest()) wantJoin=true;
        if(wantJoin && partnerHere && remote.inGameplay) { wantJoin=false; PresenceCommand("join"); }
        if(!LobbyState()) wantJoin=false;
    }
    orphanTick();
    ReportTick(self);
    // Only when no End Step chain ran this frame: menus and character select (the dev channel and console too, with or
    // without a connection: a game started with none hosts or joins later).
    drawPassTick(self);
    if(!netReady) return;
    char line[320];
    bool fresh=haveRemote && now-remoteAt<REMOTE_TIMEOUT_MS;
    uint32_t myGalaxy=FrameGalaxyFingerprint(nullptr,0);
    static bool everConnected; if(fresh) everConnected=true;
    const char* rejected=NetRejectedText();
    if(!fresh && rejected[0]) snprintf(line,sizeof(line),"Co-op: %s",rejected);
    else if(!fresh && everConnected) snprintf(line,sizeof(line),"Co-op: connection to your partner lost (%us) - waiting for them",(unsigned)((now-remoteAt)/1000));
    else if(!fresh) snprintf(line,sizeof(line),"Co-op: waiting for your partner to connect | F8: co-op menu");
    else if(remote.build!=BUILD_ID) snprintf(line,sizeof(line),"Co-op: your partner has a different HS_Coop version - both need the same HS_Coop zip");
    else if(GalaxySyncStatus()[0] && (remote.galaxy!=myGalaxy || strstr(GalaxySyncStatus(),"restart"))) snprintf(line,sizeof(line),"%s",GalaxySyncStatus());
    else if(remote.galaxy && myGalaxy && remote.galaxy!=myGalaxy) snprintf(line,sizeof(line),"Co-op: you are in a different galaxy than your partner - the host's galaxy is being sent");
    else if(!remote.inGameplay) snprintf(line,sizeof(line),"Co-op: your partner is choosing a character | F8: co-op menu");
    else {
        char where[96];
        if(remote.mode==2) snprintf(where,sizeof(where),"drifting in space");
        else if(remote.mode==1) snprintf(where,sizeof(where),"in their pod");
        else snprintf(where,sizeof(where),"%s%s",ctx.puppet?"aboard ":"on ",remote.parentName[0]?remote.parentName:"a ship");
        char myName[48]; getString(ctx.player,"Name",myName,sizeof(myName));
        bool sameCharacter=myName[0] && !strcmp(myName,remote.name);
        const char* missions=MissionsStatus();
        snprintf(line,sizeof(line),"Co-op: %s %s%s%s%s%s%s%s | F8: co-op menu",remote.name[0]?remote.name:"partner",where,remote.down?" (down)":"",
            missions[0]?" | ":"",missions,strstr(GuardsStatus(),"ALARM")?" | ALARM":"",TimePolicyPartnerSlowing()?" | SLOW-MO (partner)":"",
            sameCharacter?" | you are both playing the same character - pick different ones":"");
    }
    GmArgs none; double h=Number(Engine(api,"display_get_gui_height",self,0,none.Build()));
    if(!(h>0)) h=1080;
    GmArgs colour; colour.AddReal(fresh?0x88ff88:0x8888ff); Engine(api,"draw_set_colour",self,colour.Count(),colour.Build());
    // The game leaves its own alignment set (this GameMaker has no draw_get_halign); draw left/top, the GML defaults.
    GmArgs left; left.AddReal(0); Engine(api,"draw_set_halign",self,left.Count(),left.Build());
    GmArgs top; top.AddReal(0); Engine(api,"draw_set_valign",self,top.Count(),top.Build());
    GmArgs text; text.AddReal(24); text.AddReal(h-40); text.AddStr(api,line); Engine(api,"draw_text",self,text.Count(),text.Build());
    CrateDraw(self);
    // The partner's mission ended (completed or failed): shown here too, the way the game announces it.
    { bool failed=false; const char* b=MissionsBanner(&failed);
      if(b) {
        double w=Number(Engine(api,"display_get_gui_width",self,0,none.Build())); if(!(w>0)) w=1920;
        GmArgs c; c.AddReal(failed?0x3333ff:0x55dd55); Engine(api,"draw_set_colour",self,c.Count(),c.Build());
        GmArgs ce; ce.AddReal(1); Engine(api,"draw_set_halign",self,ce.Count(),ce.Build());
        GmArgs t; t.AddReal(w/2); t.AddReal(h*0.22); t.AddStr(api,b); Engine(api,"draw_text",self,t.Count(),t.Build());
        GmArgs l0; l0.AddReal(0); Engine(api,"draw_set_halign",self,l0.Count(),l0.Build());
        GmArgs wc; wc.AddReal(0xffffff); Engine(api,"draw_set_colour",self,wc.Count(),wc.Build());
      } }
    // The partner's slipstream whites this screen out too, the way the Slipstream item does for its user.
    // Held for half a second past the last report that had it: a late report must not blink the wash off and on.
    static DWORD slipSeenAt; static float slipShown;
    if(fresh && remote.inGameplay && remote.slipstream>0) { slipSeenAt=GetTickCount(); slipShown=remote.slipstream; }
    bool slipOn=slipSeenAt && GetTickCount()-slipSeenAt<500 && remote.inGameplay;
    { static bool was; if(slipOn!=was) CoopLog("Partner's slipstream %s",slipOn?"started":"ended"); was=slipOn; }
    if(slipOn) {
        // The item calls DrawWhiteOut(0.3 x seconds left / max, ExcitingOrange) in its world draw; the same wash is drawn
        // over the whole screen here.
        const RValue* orange=GlobalVar(340);
        double gw=Number(Engine(api,"display_get_gui_width",self,0,none.Build())); if(!(gw>0)) gw=1920;
        GmArgs a; a.AddReal(0.3*slipShown); Engine(api,"draw_set_alpha",self,a.Count(),a.Build());
        GmArgs c; c.AddReal(orange?Number(*orange):33023); Engine(api,"draw_set_colour",self,c.Count(),c.Build());
        GmArgs r; r.AddReal(0); r.AddReal(0); r.AddReal(gw); r.AddReal(h); r.AddReal(0); Engine(api,"draw_rectangle",self,r.Count(),r.Build());
        GmArgs one; one.AddReal(1); Engine(api,"draw_set_alpha",self,one.Count(),one.Build());
        GmArgs wc; wc.AddReal(0xffffff); Engine(api,"draw_set_colour",self,wc.Count(),wc.Build());
    }
    if(TimePolicyPausedByPartner() && !CrateRemoteChoosing()) {
        // This game stands still because the partner opened the pause menu; only they can resume.
        double w=Number(Engine(api,"display_get_gui_width",self,0,none.Build())); if(!(w>0)) w=1920;
        char note[160]; const char* who=remote.name[0]?remote.name:"your partner";
        if(remote.paused==2) snprintf(note,sizeof(note),"Paused - %s is planning - time resumes when they act or close it (Space: plan too)",who);
        else snprintf(note,sizeof(note),"Paused - initiated by %s",who);
        GmArgs yellow; yellow.AddReal(0x66ddff); Engine(api,"draw_set_colour",self,yellow.Count(),yellow.Build());
        GmArgs centre; centre.AddReal(1); Engine(api,"draw_set_halign",self,centre.Count(),centre.Build());
        GmArgs t2; t2.AddReal(w/2); t2.AddReal(h-90); t2.AddStr(api,note); Engine(api,"draw_text",self,t2.Count(),t2.Build());
        Engine(api,"draw_set_halign",self,left.Count(),left.Build());
    }
    {   // Still planning while the partner has finished: say they are ready, and with what in hand.
        static bool partnerPlanned; static DWORD readySince;
        if(TimePolicyLocalPauseKind()==2 && fresh) {
            if(remote.paused==2) { partnerPlanned=true; readySince=0; }
            else if(partnerPlanned) {
                if(!readySince) readySince=GetTickCount();
                double w=Number(Engine(api,"display_get_gui_width",self,0,none.Build())); if(!(w>0)) w=1920;
                char item[64]={0}; int held=ctx.puppet?(int)getReal(ctx.puppet,"CurrentlySelectedItem"):0;
                if(held>=100000 && api->ResolveCInstance(held)) getString(held,"Name",item,sizeof(item));
                char note[200]; const char* who=remote.name[0]?remote.name:"Your partner";
                if(item[0]) snprintf(note,sizeof(note),"%s is ready (%s) - time resumes when you act or close this",who,item);
                else snprintf(note,sizeof(note),"%s is ready - time resumes when you act or close this",who);
                GmArgs yellow; yellow.AddReal(0x66ddff); Engine(api,"draw_set_colour",self,yellow.Count(),yellow.Build());
                GmArgs centre; centre.AddReal(1); Engine(api,"draw_set_halign",self,centre.Count(),centre.Build());
                GmArgs t2; t2.AddReal(w/2); t2.AddReal(h-90); t2.AddStr(api,note); Engine(api,"draw_text",self,t2.Count(),t2.Build());
                GmArgs left; left.AddReal(0); Engine(api,"draw_set_halign",self,left.Count(),left.Build());
            }
        } else { partnerPlanned=false; readySince=0; }
    }
    if(TimePolicyLocalPauseKind()==2 && remote.paused==2 && fresh) {
        // Both planning: this player's own inventory is open; say that time also waits for the partner.
        double w=Number(Engine(api,"display_get_gui_width",self,0,none.Build())); if(!(w>0)) w=1920;
        char note[160]; snprintf(note,sizeof(note),"%s is planning too - time resumes when neither of you is",remote.name[0]?remote.name:"Your partner");
        GmArgs yellow; yellow.AddReal(0x66ddff); Engine(api,"draw_set_colour",self,yellow.Count(),yellow.Build());
        GmArgs centre; centre.AddReal(1); Engine(api,"draw_set_halign",self,centre.Count(),centre.Build());
        GmArgs t2; t2.AddReal(w/2); t2.AddReal(h-90); t2.AddStr(api,note); Engine(api,"draw_text",self,t2.Count(),t2.Build());
        GmArgs left; left.AddReal(0); Engine(api,"draw_set_halign",self,left.Count(),left.Build());
    }
    GmArgs white; white.AddReal(0xffffff); Engine(api,"draw_set_colour",self,white.Count(),white.Build());
    drawFloatingPuppet(self);
    PresenceDraw(self,remote,fresh);
    VisualsDraw(self);
    if(debugMode) TestMenuDraw(self);
}

// Networking (re)started while the game runs: hosting or joining with a lobby code.
static void onSessionReset();
bool CoopStartNetwork(bool host,const char* bind,const char* peer,const char* altPeer,const char* password) {
    if(netReady) { NetClose(); netReady=false; }
    NetConfig c{}; c.host=host; c.sharedTime=true;
    snprintf(c.bind,sizeof(c.bind),"%s",bind); snprintf(c.peer,sizeof(c.peer),"%s",peer?peer:""); snprintf(c.password,sizeof(c.password),"%s",password);
    haveRemote=false; onSessionReset();
    SeedSetSecret(password);
    netReady=NetOpen(c);
    if(netReady && altPeer) NetSetAltPeer(altPeer);
    return netReady;
}
bool CoopStartNetworkSteam(bool host,const char* password) {
    if(netReady) { NetClose(); netReady=false; }
    NetConfig c{}; c.host=host; c.sharedTime=true; snprintf(c.password,sizeof(c.password),"%s",password);
    haveRemote=false; onSessionReset();
    SeedSetSecret(password);
    netReady=NetOpenTransport(c,SteamSend,SteamReceive,"Steam");
    return netReady;
}
void CoopStopNetwork() {
    if(netReady) { NetClose(); netReady=false; }
    haveRemote=false; SeedSetSecret("");
}
HS_EXPORT_MOD_API_VERSION()
extern "C" __declspec(dllexport) void ModInit(const HS_ModApi* a) {
    api=a; CoopLog("ModInit executed pid=%lu ABI=%u; co-op transform sync",GetCurrentProcessId(),HS_API_VERSION);
    GetModuleFileNameA(nullptr,baseDir,MAX_PATH); char* slash=strrchr(baseDir,'\\'); if(slash) slash[1]=0;
    if(!supportedExecutable()){ CoopLog("Unsupported executable SHA256; refusing to subscribe hooks"); return; }
    GalaxySyncStartup();
    char path[MAX_PATH];
    // The game keeps starting up while ModInit runs: the generation seed points must be subscribed before anything slow,
    // or the galaxy loads unseeded.
    NetConfig config; char error[160];
    snprintf(path,sizeof(path),"%scoop.ini",baseDir);
    bool configured=NetLoadConfig(path,config,error,sizeof(error));
    ReportInit(api,baseDir);
    FrameInit(api); SeedInit(api,configured?config.password:""); LobbyInit(api,baseDir);
    if(!configured) config.sharedTime=true;
    // Developer options hold with or without a configured connection (a game may be hosted or joined later).
    debugMode=config.debug; verboseMode=config.verbose;
    if(debugMode) { snprintf(path,sizeof(path),"%scoop-trace.txt",baseDir); ConsoleSubscribeTraces(api,path); }
#ifdef HS_COOP_DEVTOOLS
    if(debugMode) { DevPortSubscribeEvents(api); if(config.devPort) DevPortOpen(config.devPort); }
#endif
    testWindow=getenv("HS_COOP_ROLE")!=nullptr;
    if(testWindow) api->SubscribeHook("gml_Script_UpdateFullscreen",windowed,nullptr);
    api->SubscribeHookPost("gml_Script_PlayAsCharacter",onPlayAsCharacter,nullptr);
    api->SubscribeHookPost("gml_Script_PlayerMovementControls",onMovementControls,nullptr);
    api->SubscribeHookPost("gml_Script_PlayerStateCharacterSelect",onCharacterSelect,nullptr);
    api->SubscribeHookPost("gml_Script_UpdateState",postUpdateState,nullptr);
    api->SubscribeHook("gml_Script_CharacterIsChooseable",preCharacterIsChooseable,nullptr);
    api->SubscribeHook("gml_Script_SaveCharacter",preSaveCharacter,nullptr);
    api->SubscribeHook("gml_Script_LookDead",preLookDead,nullptr);
    for(const char* s:{"gml_Script_RemoveFromParentsContents","gml_Script_AddToParentsContents"}) api->SubscribeHook(s,prePuppetContents,nullptr);
    for(const char* s:{"gml_Script_ChangeStateTo","gml_Script_KillPlayer"}) api->SubscribeHook(s,prePuppetSelf,nullptr);
    api->SubscribeHookPost("gml_Script_DrawInventoryList",postDrawInventoryList,nullptr);
    api->SubscribeHookPost("gml_Script_DialogDrawGUI",postDialogDraw,nullptr);
    for(const char* n:{"gml_Script_UpdatePosition","gml_Script_UpdateSpacePosition"}) api->SubscribeHook(n,preEndStepChain,nullptr);
    for(const char* n:{"gml_Script_ShipwiseCheckParentIsUpdated","gml_Script_AlignViewAngleToParentShip","gml_Script_PlayerStateFree"}) api->SubscribeHook(n,preOrphanCheck,nullptr);
    api->SubscribeHook("gml_Script_DestroyShip",preDestroyShip,nullptr);
    api->SubscribeHook("gml_Script_EjectContentsFromAirlock",preEjectContents,nullptr);
    // A breach (destroyed module, broken window) sucks people out with GetSucked and the Sucking state; the stand-in is
    // never sucked out here: the partner's own game does that to them, and the stand-in follows where they end up.
    api->SubscribeHook("gml_Script_GetSucked",prePuppetSucked,nullptr);
    api->SubscribeHook("gml_Script_PlayerStateSucking",prePuppetSucked,nullptr);
    api->SubscribeHook("gml_Script_DestroyContents",preDestroyShip,nullptr);
    api->SubscribeHook("gml_Script_PlayerReturnToShip",preReturnToShip,nullptr);
    api->SubscribeHookPost("gml_Script_StationWithPlayerOnboard",postStationWithPlayer,nullptr);
    { SnapshotInit(api); MissionsInit(api,&ctx.player); GuardsInit(api,&ctx.player,&ctx.puppet); TimePolicyInit(api); TimePolicyConfigure(config.sharedTime); PresenceInit(api,&ctx.player); VisualsInit(api,&ctx.player,&ctx.puppet); ObjectsInit(api,&ctx.player,&ctx.puppet); CrateInit(api,&ctx.player); HullInit(api); TestMenuInit(api,&ctx.player); if(debugMode) { snprintf(path,sizeof(path),"%scoop-probe.txt",baseDir); SeedProbes(path); } }
    if(configured) netReady=NetOpen(config);
    else CoopLog("Net: %s (host or join from the Escape menu: F5 / F6)",error);
    CoopLog("Hooks subscribed; restart process to unload safely");
}
