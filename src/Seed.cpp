// Deterministic procedural generation: before selected generation scripts run, reseed GameMaker's RNG from the shared
// co-op password plus a cross-process identity of what is being generated. Both games then produce identical stations,
// mission offers and mission ships without exchanging the generated data.
#include "Seed.h"
#include "Guards.h"
#include <windows.h>
#include "Console.h"
#include "GmArgs.h"
#include "Frame.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

static const HS_ModApi* api;
static std::string secret;
static std::map<std::string,int> uses;

// identityArg: -1 = self, n = argv[n] is an instance. post: reseed after the script instead of before.
struct SeedPoint { const char* script; int identityArg; bool post; };
// Each player's own save (roster, personal missions) consumes random numbers differently, so a single early seed drifts.
// Shared-world generation is therefore reseeded at every sub-step, from a key that depends only on what is generated.
static const SeedPoint points[]={
    {"gml_Script_LoadOrCreateGalaxy",-1,false},            // startup: home-station choice
    {"gml_Script_CreateStationBuilding",0,false},          // station interior, keyed by the galaxy station being built
    {"gml_Script_GenerateCharactersOnStation",0,true},     // roster characters are per player; realign afterwards
    {"gml_Script_AcceptMission",-1,false},
    {"gml_Script_GenerateShipInSteps",-1,false},
    {"gml_Script_GenerateShipShapeForOffworld",-1,false},
    {"gml_Script_GenerateShipShapeForSovereign",-1,false},
    {"gml_Script_GenerateShipShapeForGlitchers",-1,false},
    {"gml_Script_GenerateShipShapeForFoundry",-1,false},
    {"gml_Script_GenerateSegments",-1,false},
    {"gml_Script_AssignShipSecurityLevels",-1,false},
    {"gml_Script_AddAdditionalAirlocks",-1,false},
    {"gml_Script_AddTeleporterRoom",-1,false},
    {"gml_Script_AddTerminalsToShip",-1,false},
    {"gml_Script_AddGuardsToShip",-1,false},
    {"gml_Script_PlaceLootInDeadEnds",-1,false},
    {"gml_Script_AddObjectiveToHighestSecurityModule",-1,false},
    {"gml_Script_FinishGeneratingShip",-1,false},
};

static void appendString(std::string& out,int id,const char* name) {
    RValue v{}; v.type=5; api->GetVarByName(id,name,0x80000000,&v);
    out+='|';
    if(GetRValueKind(v)==RValueKind::String && v.str && v.str->text) out+=v.str->text;
}
static void appendRounded(std::string& out,int id,const char* name) {
    RValue v{}; v.type=5; api->GetVarByName(id,name,0x80000000,&v);
    double d=Number(v); char b[32];
    if(std::isnan(d)) snprintf(b,sizeof(b),"|-"); else snprintf(b,sizeof(b),"|%.0f",std::floor(d+0.5));
    out+=b;
}
// Cross-process identity: never instance ids, which differ between processes.
std::string SeedIdentity(int id) {
    std::string s;
    CInstance* inst=id>0?api->ResolveCInstance(id):nullptr;
    if(!inst) return "none";
    char b[16]; snprintf(b,sizeof(b),"o%u",inst->type); s+=b;
    appendString(s,id,"Name");
    // Positions relative to the shared frame reference: raw xReal/yReal depend on where this process's camera is.
    double fx=0,fy=0; FrameRef(fx,fy,inst);
    RValue xr{},yr{}; xr.type=yr.type=5; api->GetVarByName(id,"xReal",0x80000000,&xr); api->GetVarByName(id,"yReal",0x80000000,&yr);
    double rx=Number(xr), ry=Number(yr);
    if(inst->type==20) rx=ry=0;   // the reference itself
    char pos[64]; if(rx!=rx||ry!=ry) snprintf(pos,sizeof(pos),"|-|-"); else snprintf(pos,sizeof(pos),"|%.0f|%.0f",std::floor(rx-fx+0.5),std::floor(ry-fy+0.5)); s+=pos;
    appendRounded(s,id,"FactionIndex");
    appendRounded(s,id,"Type"); appendRounded(s,id,"Difficulty");
    RValue station{}; station.type=5; api->GetVarByName(id,"MyStation",0x80000000,&station);
    int st=(int)Number(station);
    if(st>0 && st!=id && api->ResolveCInstance(st)) appendString(s,st,"Name");
    RValue src{}; src.type=5; api->GetVarByName(id,"Station",0x80000000,&src);
    int so=(int)Number(src);
    if(so>0 && so!=id && api->ResolveCInstance(so)) appendString(s,so,"Name");
    return s;
}
static uint32_t hash32(const std::string& text) {
    uint64_t h=1469598103934665603ull;
    for(unsigned char c:text){ h^=c; h*=1099511628211ull; }
    return (uint32_t)(h^(h>>32))&0x7fffffff;
}
void SeedNow(CInstance* self,const std::string& key) {
    if(secret.empty() || !self) return;
    // No per-session use counter: a station or ship built again later in one game (e.g. after undocking and coming back)
    // must come out the same as the first build in the partner's game, so the seed depends only on what is built.
    const char* suffix="";
    uint32_t seed=hash32(secret+"\x1f"+key+suffix);
    GmArgs a; a.AddReal(seed); Engine(api,"random_set_seed",self,a.Count(),a.Build());
    if(CoopVerbose()) CoopLog("Seed %u for %s%s",seed,key.c_str(),suffix);
}
static std::string practiceScenario;
static bool buildingPractice;   // inside CreatePractice...: every step is keyed by the practice, not by what is built
static void preSeed(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void* data) {
    const SeedPoint* p=(const SeedPoint*)data;
    if(buildingPractice && !practiceScenario.empty()) {
        // The practice ship's identity (its name above all) is drawn differently in each game before the joiner's ship
        // takes the first one's name; keyed by it, each step was seeded differently and the two ships came out with
        // rooms turned and other pieces (playtest, 2026-10-05). Keyed by the practice and the step only, they match.
        std::string key=std::string(p->script+11)+"|practice|"+practiceScenario;
        for(int i=0;i<argc && i<4;i++) if(argv && argv[i] && GetRValueKind(*argv[i])==RValueKind::Real && std::fabs(argv[i]->real)<100000) { char b[24]; snprintf(b,sizeof(b),"|a%d=%g",i,argv[i]->real); key+=b; }
        SeedNow(self,key);
        return;
    }
    int id=0;
    if(p->identityArg<0) id=self?(int)self->id:0;
    else if(argc>p->identityArg && argv && argv[p->identityArg]) id=(int)Number(*argv[p->identityArg]);
    std::string key=std::string(p->script+11)+(p->post?"/post:":":")+SeedIdentity(id);
    // Numeric arguments are part of the context (e.g. generation step), instance-valued ones are not reliable.
    for(int i=0;i<argc && i<4;i++) if(argv && argv[i] && GetRValueKind(*argv[i])==RValueKind::Real && std::fabs(argv[i]->real)<100000) { char b[24]; snprintf(b,sizeof(b),"|a%d=%g",i,argv[i]->real); key+=b; }
    SeedNow(self,key);
}
// Diagnostic: logs the RNG position (one draw) at selected scripts, so two processes' logs can be compared.
static void preProbe(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void* name) {
    if(!self) return;
    GmArgs a; a.AddReal(1000000); double v=Number(Engine(api,"irandom",self,a.Count(),a.Build()));
    CoopLog("Probe %s self=%s rng=%.0f",(const char*)name,SeedIdentity(self->id).c_str(),v);
}
static const char* BLANKS=" \t\r\n";
void SeedProbes(const char* path) {
    FILE* f=fopen(path,"r"); if(!f) return;
    char line[160];
    while(fgets(line,sizeof(line),f)) {
        char* n=strtok(line,BLANKS); if(!n||*n=='#') continue;
        char* name=_strdup(n); api->SubscribeHook(name,preProbe,name); CoopLog("Seed probe on %s",name);
    }
    fclose(f);
}
// Practice: the scenario (its file name) decides everything, so both games start the same practice identically (the
// same practice ship, under the same name, at the same place): a partner who starts the same practice is on the same ship.
const char* CoopRemotePractice();
static bool practiceJoined;   // this player's practice is the partner's, started after theirs (this ship takes their name)
bool SeedPracticeJoined() { return practiceJoined && !practiceScenario.empty(); }
const char* SeedPracticeScenario() { return practiceScenario.c_str(); }
static void preCreatePractice(const char*,CInstance* self,CInstance*,RValue*,int argc,RValue** argv,void*) {
    std::string name=argc>0 && argv && argv[0] && GetRValueKind(*argv[0])==RValueKind::String && argv[0]->str && argv[0]->str->text?argv[0]->str->text:"";
    practiceScenario=name; practiceJoined=name==CoopRemotePractice(); buildingPractice=true;
    CoopLog("Practice: scenario '%s' starting",name.c_str());
    SeedNow(self,"practice|"+name);
}
// Free practice (CreatePracticePlayerAndShipAndMission) builds a random ship each time: each one gets a fresh key, and a
// partner who starts free practice while this player's is running uses that key, so both build the same ship.
static void preCreateFreePractice(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    std::string partner=CoopRemotePractice();
    std::string key;
    if(partner.rfind("free#",0)==0) key=partner;
    else { char b[32]; snprintf(b,sizeof(b),"free#%08lx",(unsigned long)(GetTickCount()*2654435761u)); key=b; }
    practiceScenario=key; practiceJoined=partner==key; buildingPractice=true;
    CoopLog("Practice: free practice '%s' starting%s",key.c_str(),partner==key?" (the partner's)":"");
    SeedNow(self,"practice|"+key);
}
// "Restart practice mode" rebuilds the practice ship. With the partner still playing on it, their game keeps the guards
// (and sends what lies aboard): the rebuilt copy here starts fresh, with every guard up (a playtest saw a body on one
// screen only, 2026-10-05).
static void preRestartPractice(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) {
    if(!practiceScenario.empty() && practiceScenario==CoopRemotePractice()) { GuardsFollowRebuiltShip(); CoopLog("Practice: restarted while the partner plays it; following their game"); }
}
static void postBuildPractice(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { buildingPractice=false; }
static void postEndPractice(const char*,CInstance*,CInstance*,RValue*,int,RValue**,void*) { practiceScenario.clear(); practiceJoined=false; CoopLog("Practice: ended"); }
void SeedInit(const HS_ModApi* a,const char* password) {
    api=a; secret=password?password:"";
    // Subscribed even without a secret: one arrives when a game is hosted or joined with a lobby code. Without one the
    // seed points do nothing and generation stays random.
    if(secret.empty()) CoopLog("Seed: no shared secret yet; generation stays random until a game is hosted or joined");
    for(const SeedPoint& p:points) {
        if(p.post) api->SubscribeHookPost(p.script,(HS_HookPostCallback)preSeed,(void*)&p);
        else api->SubscribeHook(p.script,preSeed,(void*)&p);
    }
    api->SubscribeHook("gml_Script_CreatePracticeScenario",preCreatePractice,nullptr);
    api->SubscribeHook("gml_Script_CreatePracticePlayerAndShipAndMission",preCreateFreePractice,nullptr);
    api->SubscribeHookPost("gml_Script_CreatePracticePlayerAndShipAndMission",postBuildPractice,nullptr);
    api->SubscribeHookPost("gml_Script_CreatePracticeScenario",postBuildPractice,nullptr);
    api->SubscribeHook("gml_Script_RestartPracticeMode",preRestartPractice,nullptr);
    api->SubscribeHookPost("gml_Script_EndPracticeMode",postEndPractice,nullptr);
    CoopLog("Seed: deterministic generation enabled for %u scripts",(unsigned)(sizeof(points)/sizeof(points[0])));
}
void SeedSetSecret(const char* password) { secret=password?password:""; CoopLog("Seed: shared secret %s",secret.empty()?"cleared":"set"); }
