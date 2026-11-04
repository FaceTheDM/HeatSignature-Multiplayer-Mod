// Ship damage both players share. A module destroyed in one game (an explosion, a station's guns, a Foundry pod) breaks
// the ship, and a ship cut in two splits into pieces (SplitShip). Each game would otherwise do this on its own, so one
// player saw a broken ship and docked to a piece the other game never had.
//  - The game that flies a ship (its owner: own mission ships, the host's traffic) decides: every module it loses is sent
//    over and destroyed the game's own way in the partner's copy, whose own module losses are blocked.
//  - Modules are recognised by where generation put them (identical in both games).
//  - When the owner's ship splits, each new piece's name is sent with one of its modules; the partner's game gives its
//    matching piece that name, so pods docked to it and ship movement line up again.
#include "Hull.h"
#include "Console.h"
#include "GmArgs.h"
#include "Net.h"
#include "Wire.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

static const HS_ModApi* api;
bool MissionsShipIsCopy(int ship);
void MissionsAdoptPiece(int ship,bool own);
bool CoopPartnerConnected();

static bool live(int id){ return id>0 && api->ResolveCInstance(id); }
static double getReal(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static std::string getString(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }
static void setString(int id,const char* n,const std::string& s){ RValue v{}; v.type=1; api->SetString(&v,s.c_str()); api->SetVarByName(id,n,0x80000000,&v); }

// `ship` is the ship's name when the module was recorded. A ship can be renamed afterwards (a copy takes the owner's
// name when the two games generated different ones), so the name used for matching is the origin ship's name NOW.
// Matching on the remembered name found no module of a renamed copy: the owner's lost sections were never applied
// there, the copy's doors were then removed one by one, and a door without a ship stopped the partner's game with
// "REAL argument is unset" in oDoor's Draw.
struct ModuleId { std::string ship, key; int origin=0; };
static std::map<int,ModuleId> modules;          // module instance -> the ship it was generated on, and where
static std::set<int> sent;                      // modules whose loss was sent
static bool replaying;
struct Piece { std::string ship, key, name; DWORD until; };
static std::vector<Piece> pendingPieces;        // partner's pieces still to be matched here
static std::map<std::string,DWORD> scanSplit;   // own ships that split, scanned for new pieces until then
static std::set<int> announced;                 // own pieces already announced
static std::map<std::string,int> wrecks;         // pieces named per original ship

static std::string keyOf(int module){ char k[48]; snprintf(k,sizeof(k),"m%.0f,%.0f",getReal(module,"Fore"),getReal(module,"Starboard")); return k; }
static std::string shipNameOf(const ModuleId& id) {
    if(live(id.origin)) { std::string now=getString(id.origin,"Name"); if(!now.empty()) return now; }
    return id.ship;
}
static ModuleId idOf(int module) {
    auto it=modules.find(module); if(it!=modules.end()) { ModuleId id=it->second; id.ship=shipNameOf(id); return id; }
    int parent=(int)getReal(module,"Parent");
    return {getString(parent,"Name"),keyOf(module),parent};
}
static int findModule(const std::string& ship,const std::string& key) {
    for(auto& kv:modules) if(kv.second.key==key && live(kv.first) && (shipNameOf(kv.second)==ship || kv.second.ship==ship)) return kv.first;
    return 0;
}

// Generation done: remember every module by its generated position.
static void postFinishGenerating(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self) return;
    int ship=(int)self->id; std::string name=getString(ship,"Name");
    int n=(int)getReal(ship,"ModuleCount");
    for(int i=0;i<n;i++) {
        RValue v{}; v.type=5; api->GetVarByName(ship,"ModuleList",i,&v); int m=(int)Number(v);
        if(live(m)) modules[m]={name,keyOf(m),ship};
    }
}

// The partner's game flies this ship: its module losses arrive from there, local ones are blocked.
static void preDestroyModule(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(replaying || argc<1 || !argv || !argv[0]) return;
    int module=(int)Number(*argv[0]); if(!live(module)) return;
    if(!modules.count(module)) modules[module]=idOf(module);   // its identity, while it still exists
    int ship=(int)getReal(module,"Parent"); if(!live(ship)) return;
    if(MissionsShipIsCopy(ship)) {
        static DWORD lastLog;
        if(GetTickCount()-lastLog>2000) { lastLog=GetTickCount(); CoopLog("Hull: damage to '%s' here follows the partner's game (it flies that ship)",getString(ship,"Name").c_str()); }
        api->RequestBypass();
    }
}
static void postDestroyModule(const char*,CInstance*,CInstance*,RValue*,int argc,RValue** argv,void*) {
    if(replaying || argc<1 || !argv || !argv[0] || !CoopPartnerConnected()) return;
    int module=(int)Number(*argv[0]);
    if(live(module) && live((int)getReal(module,"Parent"))) return;   // it survived this hit
    if(!sent.insert(module).second) return;
    ModuleId id=idOf(module);
    std::string cause=argc>1&&argv[1]&&GetRValueKind(*argv[1])==RValueKind::String&&argv[1]->str&&argv[1]->str->text?argv[1]->str->text:"";
    Writer w; w.str(id.ship); w.str(id.key); w.str(cause);
    NetSendReliable(MSG_MODULE_GONE,w.bytes.data(),w.bytes.size());
    CoopLog("Hull: module %s of '%s' destroyed (%s); sent to the partner",id.key.c_str(),id.ship.c_str(),cause.c_str());
}

static void postSplitShip(const char*,CInstance* self,CInstance*,RValue*,int,RValue**,void*) {
    if(!self || MissionsShipIsCopy((int)self->id)) return;
    scanSplit[getString((int)self->id,"Name")]=GetTickCount()+2000;
}

void HullOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    Reader r(data,size);
    if(type==MSG_MODULE_GONE) {
        std::string ship=r.str(), key=r.str(), cause=r.str(); if(!r.ok) return;
        int m=findModule(ship,key);
        if(!m) { CoopLog("Hull: partner's module %s of '%s' is not here",key.c_str(),ship.c_str()); return; }
        CInstance* inst=api->ResolveCInstance(m);
        GmArgs a; a.AddReal(m); a.AddStr(api,cause.c_str());
        RValue out{}; replaying=true; api->CallScript("gml_Script_DestroyModule",inst,inst,&out,a.Count(),a.Build()); replaying=false;
        CoopLog("Hull: module %s of '%s' destroyed as in the partner's game",key.c_str(),ship.c_str());
    } else if(type==MSG_SHIP_PIECE) {
        Piece p; p.ship=r.str(); p.key=r.str(); p.name=r.str(); if(!r.ok) return;
        p.until=GetTickCount()+5000; pendingPieces.push_back(p);
    }
}

void HullTick() {
    DWORD now=GetTickCount();
    // Owner: pieces split off an own ship are announced with one of their modules, and flown (and sent) like own ships.
    for(auto it=scanSplit.begin();it!=scanSplit.end();) {
        if(now>it->second) { it=scanSplit.erase(it); continue; }
        for(auto& kv:modules) {
            if((kv.second.ship!=it->first && shipNameOf(kv.second)!=it->first) || !live(kv.first)) continue;
            int piece=(int)getReal(kv.first,"Parent");
            if(!live(piece) || getString(piece,"Name")==it->first || announced.count(piece)) continue;
            announced.insert(piece);
            MissionsAdoptPiece(piece,true);
            // The game leaves a broken-off piece unnamed; it gets a name both games use for it.
            std::string name=getString(piece,"Name");
            if(name.empty()) { name=it->first+" (wreck "+std::to_string(++wrecks[it->first])+")"; setString(piece,"Name",name); }
            Writer w; w.str(it->first); w.str(kv.second.key); w.str(name);
            NetSendReliable(MSG_SHIP_PIECE,w.bytes.data(),w.bytes.size());
            CoopLog("Hull: '%s' broke off '%s'; sent to the partner",name.c_str(),it->first.c_str());
        }
        ++it;
    }
    // Partner's copy: its piece is found through the same module and takes the owner's name.
    for(auto it=pendingPieces.begin();it!=pendingPieces.end();) {
        int m=findModule(it->ship,it->key), piece=m?(int)getReal(m,"Parent"):0;
        if(live(piece) && getString(piece,"Name")!=it->ship) {
            if(getString(piece,"Name")!=it->name) setString(piece,"Name",it->name);
            MissionsAdoptPiece(piece,false);
            CoopLog("Hull: the piece that broke off '%s' is '%s' as in the partner's game",it->ship.c_str(),it->name.c_str());
            it=pendingPieces.erase(it); continue;
        }
        if(now>it->until) { CoopLog("Hull: no piece here for '%s' (%s)",it->name.c_str(),it->ship.c_str()); it=pendingPieces.erase(it); continue; }
        ++it;
    }
}

void HullInit(const HS_ModApi* a) {
    api=a;
    api->SubscribeHookPost("gml_Script_FinishGeneratingShip",postFinishGenerating,nullptr);
    api->SubscribeHook("gml_Script_DestroyModule",preDestroyModule,nullptr);
    api->SubscribeHookPost("gml_Script_DestroyModule",postDestroyModule,nullptr);
    api->SubscribeHookPost("gml_Script_SplitShip",postSplitShip,nullptr);
}
