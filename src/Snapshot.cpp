#include "Snapshot.h"
#include "Console.h"
#include "GmArgs.h"
#include "HandleVars.h"
#include <cmath>
#include <set>
#include <string>

static const HS_ModApi* api;
static const int MAX_VAR_ID=4000, MAX_ARRAY=256;
static const int NO_INDEX=(int)0x80000000;
static const double NOONE=49;   // Heat Signature's placeholder for "no instance" (object index of oNoone)
// Generation bookkeeping that the receiver's own generation creates; never copied.
static std::set<int> skipped;
static int generatedVar=-1;
static const char* const SKIP_NAMES[]={"Contents","ContentsCount","ModuleList","ModuleCount","ModuleGrid","Generated","OriginalShip"};
static int parentVar=-1;
enum : uint8_t { KIND_REAL=0, KIND_STRING=1, KIND_REF_NAMED=2, KIND_REF_NONE=3 };

void SnapshotInit(const HS_ModApi* a) { api=a; }
// Variable ids are resolved on first use: ModInit runs while the game is starting, and hundreds of lookups would delay it.
static void resolveSkipped() {
    static bool done; if(done) return; done=true;
    for(const char* n:SKIP_NAMES){ int id=api->GetVarId(n); if(id>=0) skipped.insert(id); }
    generatedVar=api->GetVarId("Generated");
    for(const char* n:HANDLE_VARS){ int id=api->GetVarId(n); if(id>=0) skipped.insert(id); }
    parentVar=api->GetVarId("Parent");
}
// Instance ids are positive integers from 100000; galaxy coordinates are not integers in practice.
static bool looksLikeInstance(double d) { return d>=100000 && d<10000000 && d==std::floor(d) && api->ResolveCInstance((int)d); }
static double real(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return Number(v); }
static std::string text(int id,const char* n){ RValue v{}; v.type=5; api->GetVarByName(id,n,0x80000000,&v); return GetRValueKind(v)==RValueKind::String&&v.str&&v.str->text?v.str->text:""; }

static bool writeValue(Writer& w,const RValue& v) {
    switch(GetRValueKind(v)) {
    case RValueKind::Real: case RValueKind::Bool: case RValueKind::Int32: case RValueKind::Int64: {
        double d=Number(v);
        if(looksLikeInstance(d)) {
            // References are re-linked by identity on the receiver; ships and stations are identified by name and position.
            int ref=(int)d; std::string name=text(ref,"Name");
            if(!name.empty()) { w.put<uint8_t>(KIND_REF_NAMED); w.put<int32_t>(ref); w.put<uint32_t>(api->ResolveCInstance(ref)->type); w.str(name); w.put<double>(real(ref,"xReal")); w.put<double>(real(ref,"yReal")); }
            else w.put<uint8_t>(KIND_REF_NONE);
            return true;
        }
        w.put<uint8_t>(KIND_REAL); w.put<double>(d); return true; }
    case RValueKind::String:
        w.put<uint8_t>(KIND_STRING); w.str(v.str&&v.str->text?v.str->text:""); return true;
    default: return false;
    }
}
void SnapshotCapture(int id, Writer& w) {
    resolveSkipped();
    size_t countAt=w.bytes.size(); w.put<uint32_t>(0);
    uint32_t count=0;
    for(int var=0;var<MAX_VAR_ID;var++) {
        if((skipped.count(var) && var!=generatedVar) || var==parentVar) continue;
        RValue v{}; v.type=0x00ffffff; api->GetVar(id,var,NO_INDEX,&v);
        RValueKind k=GetRValueKind(v);
        if(k==RValueKind::Array) {
            GmArgs a; a.AddRValue(v);
            int len=(int)Number(Engine(api,"array_length_1d",api->ResolveCInstance(id),a.Count(),a.Build()));
            for(int i=0;i<len && i<MAX_ARRAY;i++) {
                RValue e{}; e.type=0x00ffffff; api->GetVar(id,var,i,&e);
                size_t mark=w.bytes.size(); w.put<uint16_t>((uint16_t)var); w.put<int16_t>((int16_t)i);
                if(writeValue(w,e)) count++; else w.bytes.resize(mark);
            }
            continue;
        }
        size_t mark=w.bytes.size(); w.put<uint16_t>((uint16_t)var); w.put<int16_t>(-1);
        if(writeValue(w,v)) count++; else w.bytes.resize(mark);
    }
    memcpy(w.bytes.data()+countAt,&count,4);
}
// Finds a local instance of `object` with this name nearest to (x,y).
static int findNamed(CInstance* any,uint32_t object,const std::string& name,double x,double y) {
    GmArgs n; n.AddReal(object);
    int count=(int)Number(Engine(api,"instance_number",any,n.Count(),n.Build()));
    int best=0; double bestDistance=1e18;
    for(int i=0;i<count;i++) {
        GmArgs f; f.AddReal(object); f.AddReal(i);
        int id=(int)Number(Engine(api,"instance_find",any,f.Count(),f.Build()));
        if(id<=0 || text(id,"Name")!=name) continue;
        double d=std::hypot(real(id,"xReal")-x,real(id,"yReal")-y);
        if(d<bestDistance){ bestDistance=d; best=id; }
    }
    if(best) return best;
    // Galaxy objects (stations, sectors) far from the player are deactivated, which instance_find skips; they are among
    // the first instances created, so the low id range is searched directly.
    for(int id=100000;id<101000;id++) {
        CInstance* inst=api->ResolveCInstance(id);
        if(!inst || inst->type!=object || text(id,"Name")!=name) continue;
        return id;
    }
    return 0;
}
int SnapshotApply(int id, Reader& r, bool keepLocalRefs) {
    resolveSkipped();
    uint32_t count=r.get<uint32_t>(); int applied=0;
    for(uint32_t i=0;i<count && r.ok;i++) {
        int var=r.get<uint16_t>(); int index=r.get<int16_t>(); uint8_t kind=r.get<uint8_t>();
        RValue v{}; v.type=0;
        switch(kind) {
        case KIND_REAL: v.real=r.get<double>(); break;
        case KIND_STRING: { std::string s=r.str(); v.type=1; api->SetString(&v,s.c_str()); break; }
        case KIND_REF_NAMED: { int32_t senderId=r.get<int32_t>(); uint32_t object=r.get<uint32_t>(); std::string name=r.str(); double x=r.get<double>(), y=r.get<double>();
            // Galaxy objects load from the same save in the same order, so the sender's id usually names the same object here,
            // even when it is deactivated (and so invisible to instance_find). Accept it only if the name agrees.
            if(keepLocalRefs) { v.type=5; break; }
            int local=(api->ResolveCInstance(senderId) && text(senderId,"Name")==name)?senderId:findNamed(api->ResolveCInstance(id),object,name,x,y); v.real=local?local:NOONE;
            if(!local && CoopVerbose()) CoopLog("Snapshot: var #%d[%d] refers to %s '%s' not found here",var,index,"object",name.c_str()); break; }
        case KIND_REF_NONE: if(keepLocalRefs) { v.type=5; break; } v.real=NOONE; if(CoopVerbose()) CoopLog("Snapshot: var #%d[%d] was an unnamed instance reference; set to none",var,index); break;
        default: r.ok=false; break;
        }
        if(!r.ok) break;
        // An item (keepLocalRefs) takes the sender's Generated: an item generates itself in its first Step (rolls its
        // traits, puts them in its name), and a copy that did so again got a second roll and a doubled name prefix
        // ("Self-Charging Self-Charging Emergency Shield"). A ship's Generated stays this game's own.
        if((skipped.count(var) && !(keepLocalRefs && var==generatedVar)) || v.type==5) continue;
        api->SetVar(id,var,index<0?NO_INDEX:index,&v); applied++;
    }
    if(parentVar>=0 && !keepLocalRefs){ RValue self{}; self.type=0; self.real=id; api->SetVar(id,parentVar,NO_INDEX,&self); }
    return applied;
}
