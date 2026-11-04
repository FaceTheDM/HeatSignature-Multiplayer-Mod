#include "Console.h"
#include "Lobby.h"
int CoopStandinCount(int* current);
void CoopTestOrphan(int id);
#include "Steam.h"
#include "GalaxySync.h"
#include "Missions.h"
#include "GmArgs.h"
#include "Guards.h"
#include "Presence.h"
#include "TestMenu.h"
#ifdef HS_COOP_DEVTOOLS   // developer builds only (build.py --dev): the private developer extension, not in this repository
#include "DevPort.h"
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

RValue Engine(const HS_ModApi* api, const char* name, CInstance* self, int argc, RValue** argv) {
    RValue out{}; out.type=5; api->CallEngineScript(name,self,nullptr,&out,argc,argv); return out;
}
// YYC reads globals from the object at RVA 0x453D610: a fast array at +4 (16-byte RValues by variable id), else vtable[1](id).
const RValue* GlobalVar(int id) {
    uint8_t* base=(uint8_t*)GetModuleHandleA(nullptr);
    void* globals=*(void**)(base+0x453D610);
    if(!globals || id<0) return nullptr;
    RValue* fast=*(RValue**)((uint8_t*)globals+4);
    if(fast) return fast+id;
    typedef RValue* (__thiscall* Getter)(void*,int);
    return (*(Getter**)globals)[1](globals,id);
}
double Number(const RValue& v) {
    switch(GetRValueKind(v)) {
    case RValueKind::Real: case RValueKind::Bool: return v.real;
    case RValueKind::Int32: return v.i32;
    case RValueKind::Int64: return (double)v.i64;
    default: return NAN;
    }
}
static void describe(const char* label, const RValue& v) {
    if(GetRValueKind(v)==RValueKind::String) CoopLog("%s = \"%s\"",label,v.str&&v.str->text?v.str->text:"");
    else CoopLog("%s = %.10g (type %u)",label,Number(v),v.type&0xffffff);
}
static double idFor(CoopContext& c, const char* t) {
    if(!strcmp(t,"@")) return c.player;
    if(!strcmp(t,"!")) return c.puppet;
    if(!strcmp(t,"$")) { RValue p{}; c.api->GetVarByName(c.player,"Parent",0x80000000,&p); return Number(p); }
    return atof(t);
}
// Splits on spaces; a token starting with '"' runs to the next '"'.
static int tokenize(char* line, char** out, int max) {
    int n=0;
    while(*line && n<max) {
        while(*line==' '||*line=='\t'||*line=='\r'||*line=='\n') line++;
        if(!*line) break;
        if(*line=='"') { out[n++]=line++; while(*line && *line!='"') line++; if(*line) *line++=0; continue; }
        out[n++]=line;
        while(*line && *line!=' ' && *line!='\t' && *line!='\r' && *line!='\n') line++;
        if(*line) *line++=0;
    }
    return n;
}
static CInstance* selfFor(CoopContext& c, const char* t, CInstance* hookSelf) {
    if(!strcmp(t,"0")) return hookSelf;
    return c.api->ResolveCInstance((int)idFor(c,t));
}
static const HS_ModApi* traceApi;
static std::map<std::string,int> traceBudget;
static void argText(const RValue* v, char* out, size_t size) {
    if(!v) { snprintf(out,size,"null"); return; }
    if(GetRValueKind(*v)==RValueKind::String) snprintf(out,size,"\"%s\"",v->str&&v->str->text?v->str->text:"");
    else snprintf(out,size,"%.10g/t%u",Number(*v),v->type&0xffffff);
}
static void traceCall(const char* when, const char* hook, CInstance* self, CInstance* other, int argc, RValue** argv, const RValue* ret) {
    auto& left=traceBudget[hook]; if(left<=0) return; if(*when=='<') left--;
    char buf[1500]; int len=snprintf(buf,sizeof(buf),"trace%s %s self=%u(obj %u) other=%u argc=%d",when,hook,self?self->id:0,self?self->type:0,other?other->id:0,argc);
    for(int i=0;i<argc && i<8 && len<1200;i++){ char a[200]; argText(argv?argv[i]:nullptr,a,sizeof(a)); len+=snprintf(buf+len,sizeof(buf)-len," a%d=%s",i,a); }
    if(ret){ char a[200]; argText(ret,a,sizeof(a)); snprintf(buf+len,sizeof(buf)-len," -> %s",a); }
    CoopLog("%s",buf);
}
// The loader may pass a null hook name (e.g. for CallScript-initiated calls), so the name travels in userData.
static void tracePre(const char*,CInstance* self,CInstance* other,RValue*,int argc,RValue** argv,void* name) { traceCall(">",(const char*)name,self,other,argc,argv,nullptr); }
static void tracePost(const char*,CInstance* self,CInstance* other,RValue* ret,int argc,RValue** argv,void* name) { traceCall("<",(const char*)name,self,other,argc,argv,ret); }
#ifdef HS_COOP_DEVTOOLS   // developer builds only (build.py --dev): the private developer extension, not in this repository
int CoopRemoteParent();
bool CoopRemoteDown();
const char* CoopRemoteStateName();
static std::string str(int id,const char* n){ RValue v{}; v.type=5; traceApi->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static double num(int id,const char* n){ RValue v{}; v.type=5; traceApi->GetVarByName(id,n,0x80000000,&v); double d=Number(v); return d==d?d:0; }
// One JSON object describing the scene as this game sees it (dev snapshot).
static std::string snapshotJson(CoopContext& c) {
    traceApi=c.api;
    auto person=[&](int id)->std::string{
        if(id<=0 || !c.api->ResolveCInstance(id)) return "null";
        char b[600];
        snprintf(b,sizeof(b),"{\"id\":%d,\"name\":\"%s\",\"state\":\"%s\",\"fore\":%.1f,\"stb\":%.1f,\"parent\":%d,\"parentName\":\"%s\",\"seenAsThreat\":%g,\"dead\":%g,\"teleported\":%g}",
            id,JsonEscape(str(id,"Forename")+" "+str(id,"Surname")).c_str(),JsonEscape(str(id,"State")).c_str(),num(id,"Fore"),num(id,"Starboard"),(int)num(id,"Parent"),
            JsonEscape(str((int)num(id,"Parent"),"Name")).c_str(),num(id,"SeenAsThreat"),num(id,"Dead"),num(id,"Teleported"));
        return b;
    };
    int ship=GuardsSharedShip(); if(!ship && c.player>0) ship=(int)num(c.player,"Parent");
    const RValue* m=GlobalVar(2); int mission=m?(int)Number(*m):0;
    const RValue* god=GlobalVar(127);
    std::string mis="null";
    if(mission>0 && c.api->ResolveCInstance(mission)) { char b[300]; snprintf(b,sizeof(b),"{\"id\":%d,\"type\":\"%s\",\"ship\":\"%s\",\"started\":%g}",mission,JsonEscape(str(mission,"Type")).c_str(),JsonEscape(str((int)num(mission,"Ship"),"Name")).c_str(),num(mission,"Started")); mis=b; }
    char head[700];
    snprintf(head,sizeof(head),"{\"frame\":%u,\"god\":%g,\"guards\":\"%s\",\"authority\":%s,\"sharedShip\":%d,\"ship\":%d,\"shipName\":\"%s\",\"alarm\":%g,"
        "\"partnerParent\":%d,\"partnerDown\":%s,\"partnerState\":\"%s\",\"mission\":%s,",
        DevFrame(),god?Number(*god):0,JsonEscape(GuardsStatus()).c_str(),GuardsIsAuthority()?"true":"false",GuardsSharedShip(),ship,JsonEscape(str(ship,"Name")).c_str(),num(ship,"Alarm"),
        CoopRemoteParent(),CoopRemoteDown()?"true":"false",JsonEscape(CoopRemoteStateName()).c_str(),mis.c_str());
    return std::string(head)+"\"player\":"+person(c.player)+",\"standin\":"+person(c.puppet)+",\"guardList\":"+(ship?GuardsSnapshotJson(ship):"[]")+"}";
}
#endif
static void execute(CoopContext& c, char* line, CInstance* hookSelf) {
    char* tok[24]; int n=tokenize(line,tok,24);
    if(!n || tok[0][0]=='#') return;
    const char* cmd=tok[0];
#ifdef HS_COOP_DEVTOOLS
    if(!strcmp(cmd,"snapshot")) { ConsoleEmit(snapshotJson(c)); return; }
#endif
    if(!strcmp(cmd,"capture")) { GmArgs a; a.AddStr(c.api,"coop-capture.png"); Engine(c.api,"screen_save",hookSelf,a.Count(),a.Build()); CoopLog("Capture requested"); return; }
    if(!strcmp(cmd,"quit")) { Engine(c.api,"game_end",hookSelf,0,nullptr); return; }
    if(!strcmp(cmd,"play") && c.player>0) {
        GmArgs a; a.AddReal(c.player); RValue r{};
        c.api->CallScript("gml_Script_PlayAsCharacter",hookSelf,nullptr,&r,a.Count(),a.Build());
        CoopLog("Play requested id=%d",c.player); return;
    }
    if(!strcmp(cmd,"get") && n>=3) {
        RValue v{}; v.type=5;
        int index=n>3?atoi(tok[3]):(int)0x80000000;
        c.api->GetVarByName((int)idFor(c,tok[1]),tok[2],index,&v);
        char label[160]; snprintf(label,sizeof(label),"get %s.%s",tok[1],tok[2]); describe(label,v); return;
    }
    if(!strcmp(cmd,"set") && n>=4 && tok[3][0]=='"') {
        RValue v{}; v.type=1; c.api->SetString(&v,tok[3]+1);
        c.api->SetVarByName((int)idFor(c,tok[1]),tok[2],n>4?atoi(tok[4]):(int)0x80000000,&v);
        CoopLog("set %s.%s = \"%s\"",tok[1],tok[2],tok[3]+1); return;
    }
    if(!strcmp(cmd,"setid") && n>=4) {   // setid <instance> <variable id> <value>: for variables the name lookup misses
        RValue v{}; v.type=0; v.real=idFor(c,tok[3]);
        c.api->SetVar((int)idFor(c,tok[1]),atoi(tok[2]),(int)0x80000000,&v);
        CoopLog("setid %s.#%s = %g",tok[1],tok[2],v.real); return;
    }
    if(!strcmp(cmd,"set") && n>=4) {
        RValue v{}; v.type=0; v.real=idFor(c,tok[3]);
        int index=n>4?atoi(tok[4]):(int)0x80000000;
        c.api->SetVarByName((int)idFor(c,tok[1]),tok[2],index,&v);
        CoopLog("set %s.%s = %g",tok[1],tok[2],v.real); return;
    }
    if((!strcmp(cmd,"call")||!strcmp(cmd,"eng")) && n>=3) {
        CInstance* self=selfFor(c,tok[2],hookSelf);
        if(!self) { CoopLog("%s %s: no instance for self %s",cmd,tok[1],tok[2]); return; }
        GmArgs a;
        for(int i=3;i<n;i++) { if(tok[i][0]=='"') a.AddStr(c.api,tok[i]+1); else a.AddReal(idFor(c,tok[i])); }
        RValue r{}; r.type=5;
        if(cmd[0]=='c') c.api->CallScript(tok[1],self,self,&r,a.Count(),a.Build());
        else c.api->CallEngineScript(tok[1],self,self,&r,a.Count(),a.Build());
        char label[160]; snprintf(label,sizeof(label),"%s %s self=%u",cmd,tok[1],self->id); describe(label,r); return;
    }
    if(!strcmp(cmd,"trace") && n>=2) {
        // Hooks can only be subscribed during ModInit; this only adjusts the budget of a startup trace.
        if(traceBudget.find(tok[1])==traceBudget.end()) { CoopLog("trace %s: not subscribed at startup (add it to coop-trace.txt)",tok[1]); return; }
        traceBudget[tok[1]]=n>2?atoi(tok[2]):10; CoopLog("tracing %s for %d calls",tok[1],traceBudget[tok[1]]); return;
    }
    if(!strcmp(cmd,"walk") && n>=3) {
        RValue f{},s{}; f.type=s.type=5;
        c.api->GetVarByName(c.player,"Fore",0x80000000,&f); c.api->GetVarByName(c.player,"Starboard",0x80000000,&s);
        c.walkRadius=atof(tok[1]); c.walkSeconds=atof(tok[2]); c.walkStartMs=GetTickCount(); c.walkFore=Number(f); c.walkStarboard=Number(s);
        CoopLog("walk radius=%g seconds=%g around (%g,%g)",c.walkRadius,c.walkSeconds,c.walkFore,c.walkStarboard); return;
    }
    if(!strcmp(cmd,"goto") && n>=2 && !strcmp(tok[1],"stop")) { if(c.gotoActive) CoopLog("goto stopped"); c.gotoActive=false; return; }
    if(!strcmp(cmd,"goto") && n>=3) {
        c.gotoFore=atof(tok[1]); c.gotoStarboard=atof(tok[2]); c.gotoSpeed=n>3?atof(tok[3]):150; c.gotoLastMs=GetTickCount(); c.gotoActive=true;
        c.gotoBest=1e18; c.gotoBestMs=c.gotoLastMs;
        CoopLog("goto (%g,%g) at %g/s",c.gotoFore,c.gotoStarboard,c.gotoSpeed); return;
    }
    if(!strcmp(cmd,"test") && n>=2) { std::string all=tok[1]; for(int i=2;i<n;i++){ all+=' '; all+=tok[i]; } TestMenuCommand(all.c_str()); CoopLog("test %s queued",all.c_str()); return; }
    if(!strcmp(cmd,"coop") && n>=2 && !strcmp(tok[1],"getgalaxy")) { GalaxySyncRequest(); return; }
    if(!strcmp(cmd,"lobby") && n>=2) {
        char err[160]="";
        if(!strcmp(tok[1],"hostip")) { if(!LobbyHostByIp(err,sizeof(err))) CoopLog("lobby host failed: %s",err); else CoopLog("lobby hosting started"); }
        else if(!strcmp(tok[1],"host")) { if(!LobbyHost(err,sizeof(err))) CoopLog("lobby host failed: %s",err); else CoopLog("lobby hosting started"); }
        else if(!strcmp(tok[1],"join") && n>=3) { if(!LobbyJoin(tok[2],err,sizeof(err))) CoopLog("lobby join failed: %s",err); else CoopLog("lobby join started"); }
        else if(!strcmp(tok[1],"leave")) { LobbyLeave(); CoopLog("lobby left"); }
        else if(!strcmp(tok[1],"id")) CoopLog("lobby steam id %llu",(unsigned long long)SteamLobbyId());
        else if(!strcmp(tok[1],"invited") && n>=3) { SteamSimulateInvite(_strtoui64(tok[2],nullptr,10)); CoopLog("lobby invite simulated"); }
        else if(!strcmp(tok[1],"code")) CoopLog("lobby state %d code %s",LobbyState(),LobbyCode());
        return;
    }
    if(!strcmp(cmd,"standins")) { int cur=0; int n=CoopStandinCount(&cur); CoopLog("standins count=%d current=%d",n,cur); return; }
    if(!strcmp(cmd,"orphan") && n>=2) { CoopTestOrphan((int)idFor(c,tok[1])); CoopLog("orphan queued"); return; }
    if(!strcmp(cmd,"coop") && n>=2 && !strcmp(tok[1],"testdone")) { MissionsTestDone(); return; }
    if(!strcmp(cmd,"coop") && n>=2) { PresenceCommand(tok[1]); CoopLog("coop %s queued",tok[1]); return; }
    if(!strcmp(cmd,"guards") && n>=2) { GuardsDebugDump((int)idFor(c,tok[1])); return; }
    if(!strcmp(cmd,"contents") && n>=2) {
        int ship=(int)idFor(c,tok[1]);
        RValue count{}; count.type=5; c.api->GetVarByName(ship,"ContentsCount",0x80000000,&count);
        std::map<std::string,int> byType; std::string example;
        for(int i=0;i<(int)Number(count);i++) {
            RValue item{}; item.type=5; c.api->GetVarByName(ship,"Contents",i,&item);
            CInstance* inst=c.api->ResolveCInstance((int)Number(item)); if(!inst) continue;
            std::string name=inst->GetObjectName(c.api); byType[name]++;
            if(byType[name]==1){ char b[64]; snprintf(b,sizeof(b)," %s=%d",name.c_str(),(int)inst->id); example+=b; }
        }
        std::string summary; for(auto& kv:byType){ char b[64]; snprintf(b,sizeof(b)," %s x%d",kv.first.c_str(),kv.second); summary+=b; }
        CoopLog("contents %d:%s | first of each:%s",ship,summary.c_str(),example.c_str()); return;
    }
    if(!strcmp(cmd,"board") && n>=2) {
        // Dev: put the local player aboard a ship at its first airlock, through the game's own arrival path
        // (leave the current parent, then PlayerArrivesAtThatAirlock as when floating up to an airlock).
        int ship=(int)idFor(c,tok[1]);
        CInstance* me=c.api->ResolveCInstance(c.player); if(!me) return;
        RValue al{}; al.type=5; c.api->GetVarByName(ship,"FirstAirlock",0x80000000,&al);
        int airlock=(int)Number(al);
        if(!c.api->ResolveCInstance(airlock)) { GmArgs r; r.AddReal(ship); RValue out{}; c.api->CallScript("gml_Script_ReactivateShip",me,me,&out,r.Count(),r.Build()); }
        if(!c.api->ResolveCInstance(airlock)) { CoopLog("board: ship %d airlock %d not available",ship,airlock); return; }
        GmArgs leave; leave.AddReal(c.player); RValue out{}; c.api->CallScript("gml_Script_RemoveFromParentsContents",me,me,&out,leave.Count(),leave.Build());
        GmArgs arrive; arrive.AddReal(airlock); c.api->CallScript("gml_Script_PlayerArrivesAtThatAirlock",me,me,&out,arrive.Count(),arrive.Build());
        RValue p{}; p.type=5; c.api->GetVarByName(c.player,"Parent",0x80000000,&p);
        CoopLog("board: player %d now aboard %g via airlock %d",c.player,Number(p),airlock); return;
    }
    if(!strcmp(cmd,"dumpvars") && n>=2) {
        // Every defined instance variable by numeric id; names are resolved offline from known_variable_properties.txt.
        int id=(int)idFor(c,tok[1]); int shown=0;
        for(int v=0;v<4000;v++) {
            RValue out{}; out.type=0x00ffffff; c.api->GetVar(id,v,(int)0x80000000,&out);
            RValueKind k=GetRValueKind(out);
            if(k==RValueKind::Unset || k==RValueKind::Undefined) continue;
            if(k==RValueKind::String) CoopLog("var %d #%d = \"%s\"",id,v,out.str&&out.str->text?out.str->text:"");
            else if(k==RValueKind::Array) CoopLog("var %d #%d = <array>",id,v);
            else CoopLog("var %d #%d = %.10g",id,v,Number(out));
            shown++;
        }
        CoopLog("dumpvars %d: %d defined",id,shown); return;
    }
    if(!strcmp(cmd,"fingerprint") && n>=2) {
        // Layout hash of a ship: every content's object type and rounded ship-local position, in contents order.
        int ship=(int)idFor(c,tok[1]);
        RValue count{}; count.type=5; c.api->GetVarByName(ship,"ContentsCount",0x80000000,&count);
        RValue modules{}; modules.type=5; c.api->GetVarByName(ship,"ModuleCount",0x80000000,&modules);
        RValue name{}; name.type=5; c.api->GetVarByName(ship,"Name",0x80000000,&name);
        uint64_t h=1469598103934665603ull; int total=(int)Number(count),seen=0;
        for(int i=0;i<total;i++) {
            RValue item{}; item.type=5; c.api->GetVarByName(ship,"Contents",i,&item);
            int id=(int)Number(item); CInstance* inst=id>0?c.api->ResolveCInstance(id):nullptr; if(!inst) continue;
            RValue f{},s{}; f.type=s.type=5; c.api->GetVarByName(id,"Fore",0x80000000,&f); c.api->GetVarByName(id,"Starboard",0x80000000,&s);
            long long parts[3]={(long long)inst->type,(long long)floor(Number(f)+0.5),(long long)floor(Number(s)+0.5)};
            for(long long p:parts) for(int b=0;b<8;b++){ h^=(uint8_t)(p>>(b*8)); h*=1099511628211ull; }
            seen++;
        }
        CoopLog("fingerprint ship=%d name=\"%s\" modules=%g contents=%d/%d hash=%016llx",ship,GetRValueKind(name)==RValueKind::String&&name.str?name.str->text:"",Number(modules),seen,total,(unsigned long long)h);
        return;
    }
    if(!strcmp(cmd,"gset") && n>=3) {
        RValue* v=const_cast<RValue*>(GlobalVar(atoi(tok[1])));
        if(v && GetRValueKind(*v)==RValueKind::Real) { v->real=idFor(c,tok[2]); CoopLog("global[%s] = %g",tok[1],v->real); }
        else CoopLog("global[%s] not a real; unchanged",tok[1]);
        return;
    }
    if(!strcmp(cmd,"gget") && n>=2) {
        const RValue* v=GlobalVar(atoi(tok[1]));
        char label[64]; snprintf(label,sizeof(label),"global[%s]",tok[1]);
        if(v) describe(label,*v); else CoopLog("%s unavailable",label); return;
    }
    if(!strcmp(cmd,"list") && n>=2) {
        GmArgs a; a.AddReal(atof(tok[1]));
        int count=(int)Number(Engine(c.api,"instance_number",hookSelf,a.Count(),a.Build()));
        char buf[1800]; int len=snprintf(buf,sizeof(buf),"list object=%s count=%d:",tok[1],count);
        for(int i=0;i<count && len<(int)sizeof(buf)-16;i++) {
            GmArgs f; f.AddReal(atof(tok[1])); f.AddReal(i);
            len+=snprintf(buf+len,sizeof(buf)-len," %.0f",Number(Engine(c.api,"instance_find",hookSelf,f.Count(),f.Build())));
        }
        CoopLog("%s",buf); return;
    }
    CoopLog("Unknown console command: %s",cmd);
}
void ConsoleExecute(CoopContext& c, char* line, CInstance* hookSelf) { traceApi=traceApi?traceApi:c.api; execute(c,line,hookSelf); }
void ConsoleRun(CoopContext& c, CInstance* hookSelf, const char* path) {
    FILE* f=fopen(path,"r"); if(!f) return;
    char line[512];
    static char lines[64][512]; int count=0;
    while(count<64 && fgets(line,sizeof(line),f)) strcpy(lines[count++],line);
    fclose(f); remove(path);
    for(int i=0;i<count;i++) { CoopLog("> %s",strtok(lines[i],"\r\n")?lines[i]:""); execute(c,lines[i],hookSelf); }
}

void ConsoleSubscribeTraces(const HS_ModApi* api, const char* path) {
    FILE* f=fopen(path,"r"); if(!f) return;
    char line[256];
    const char* blanks=" \t\r\n";
    while(fgets(line,sizeof(line),f)) {
        char* name=strtok(line,blanks); if(!name || name[0]=='#') continue;
        char* budget=strtok(nullptr,blanks);
        name=_strdup(name); traceBudget[name]=budget?atoi(budget):10;
        api->SubscribeHook(name,tracePre,name); api->SubscribeHookPost(name,tracePost,name);
        CoopLog("startup trace %s budget %d",name,traceBudget[name]);
    }
    fclose(f);
}
