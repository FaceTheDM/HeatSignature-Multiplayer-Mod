#include "Frame.h"
#include "Console.h"
#include "GmArgs.h"
#include <cmath>
#include <cstdio>
#include <string>

static const HS_ModApi* api;
static int anchor; static std::string anchorName;

void FrameInit(const HS_ModApi* a) { api=a; }
static double real(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
// The shared frame of reference: one station both games have, the one whose name sorts first.
//
// Each game shifts its whole world to keep its own camera near zero, so raw xReal/yReal mean nothing to the other
// game; positions are sent relative to a point both games agree on. That point used to be the game's "galaxy" object,
// which is NOT shifted with the world: relative to it the stations sat about 4 million units apart between the two
// games, and a mission ship placed through it showed up in a different part of the map for each player. Stations are
// shifted with everything else and never move within the galaxy, and both players share one galaxy.
static int findAnchor(CInstance* self) {
    GmArgs n; n.AddStr(api,"oStation"); int type=(int)Number(Engine(api,"asset_get_index",self,n.Count(),n.Build()));
    if(type<0) return 0;
    int best=0; std::string bestName;
    // Galaxy objects are created first when the save loads; resolving by id also reaches deactivated stations, which
    // instance_find skips (and which differ between the games, depending on each camera).
    for(int id=100000;id<101000;id++) {
        CInstance* inst=api->ResolveCInstance(id);
        if(!inst || inst->type!=(uint32_t)type) continue;
        RValue nm{}; nm.type=5; api->GetVarByName(id,"Name",0x80000000,&nm);
        if(GetRValueKind(nm)!=RValueKind::String || !nm.str || !nm.str->text || !nm.str->text[0]) continue;
        double x=real(id,"xReal"); if(!(x==x)) continue;
        if(!best || bestName.compare(nm.str->text)>0) { best=id; bestName=nm.str->text; }
    }
    if(best && bestName!=anchorName) { anchorName=bestName; CoopLog("Frame: positions are shared relative to station '%s'",anchorName.c_str()); }
    return best;
}
bool FrameRef(double& x,double& y,CInstance* hint) {
    if(!anchor || !api->ResolveCInstance(anchor)) {
        anchor=0;
        const RValue* p=GlobalVar(GLOBAL_PLAYER);
        CInstance* self=p?api->ResolveCInstance((int)Number(*p)):nullptr;
        if(!self) self=hint;
        if(!self) return false;
        anchor=findAnchor(self);
        if(!anchor) return false;
    }
    x=real(anchor,"xReal"); y=real(anchor,"yReal");
    return x==x && y==y;
}
const char* FrameAnchorName() { return anchorName.c_str(); }

// Fingerprint of the galaxy: names of every active galaxy station (the object type of a station
// ship's MyStation). Two players must share a galaxy for co-op; differing fingerprints mean the CLIENT should import the
// HOST's galaxy. Computed once, the first time the player is aboard a station.
uint32_t FrameGalaxyFingerprint(CInstance* self, int stationShip) {
    static uint32_t cached;
    if(cached || !self || stationShip<=0) return cached;
    int station=(int)real(stationShip,"MyStation");
    CInstance* st=station>0?api->ResolveCInstance(station):nullptr;
    double fx,fy;
    if(!st || !FrameRef(fx,fy,self)) return 0;
    // Galaxy objects are created first when the save loads; resolving by id also reaches deactivated stations, which
    // instance_find skips (and which differ between processes, depending on each camera).
    uint64_t sum=0; int used=0;
    for(int id=100000;id<100600;id++) {
        CInstance* inst=api->ResolveCInstance(id);
        if(!inst || inst->type!=st->type) continue;
        RValue nm{}; nm.type=5; api->GetVarByName(id,"Name",0x80000000,&nm);
        if(GetRValueKind(nm)!=RValueKind::String || !nm.str || !nm.str->text) continue;
        uint64_t h=1469598103934665603ull; for(const char* c=nm.str->text;*c;c++){ h^=(uint8_t)*c; h*=1099511628211ull; }
        sum+=h; used++;
    }
    if(used) { cached=(uint32_t)(sum^(sum>>32)); if(!cached) cached=1; CoopLog("Galaxy fingerprint %08x over %d stations",cached,used); }
    return cached;
}
