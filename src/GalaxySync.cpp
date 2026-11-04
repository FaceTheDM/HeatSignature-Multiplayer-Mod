#include "GalaxySync.h"
#include "Console.h"
#include "Net.h"
#include "Wire.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs=std::filesystem;
static const char* const SLOT_A="Galaxy Coop";
static const char* const SLOT_B="Galaxy Coop B";
static char status[200]="";
static bool requested, installed;
static uint32_t transferTotal; static std::map<uint32_t,std::pair<std::string,std::string>> received;

// The game keeps its saves under %APPDATA%\Heat_Signature; some setups resolve it to a nested appdata\Heat_Signature
// folder. Every root that has an Options.txt is kept in step.
static std::vector<fs::path> saveRoots() {
    std::vector<fs::path> out;
    const char* appdata=getenv("APPDATA"); if(!appdata) return out;
    fs::path outer=fs::path(appdata)/"Heat_Signature";
    for(const fs::path& p:{outer,outer/"appdata"/"Heat_Signature"}) { std::error_code ec; if(fs::exists(p/"Options.txt",ec)) out.push_back(p); }
    return out;
}
static std::string readFile(const fs::path& p){ std::ifstream f(p,std::ios::binary); std::stringstream s; s<<f.rdbuf(); return s.str(); }
static void writeFile(const fs::path& p,const std::string& d){ std::ofstream f(p,std::ios::binary|std::ios::trunc); f<<d; }
static std::string selectedGalaxy(const fs::path& root) {
    std::string t=readFile(root/"Options.txt");
    size_t at=t.find("GalaxyFolder = "); if(at==std::string::npos) return "Galaxy 1";
    size_t end=t.find_first_of("\r\n",at); std::string v=t.substr(at+15,end==std::string::npos?std::string::npos:end-at-15);
    while(!v.empty() && (v.back()=='\\' || v.back()=='/' || v.back()==' ')) v.pop_back();
    return v;
}
static void selectGalaxy(const fs::path& root,const std::string& slot) {
    std::string t=readFile(root/"Options.txt");
    size_t at=t.find("GalaxyFolder = ");
    if(at==std::string::npos) { t+="GalaxyFolder = "+slot+"\\\r\n"; }
    else { size_t end=t.find_first_of("\r\n",at); t.replace(at,(end==std::string::npos?t.size():end)-at,"GalaxyFolder = "+slot+"\\"); }
    writeFile(root/"Options.txt",t);
}
static bool safeRelative(const std::string& rel) {
    if(rel.empty() || rel[0]=='/' || rel.find(':')!=std::string::npos || rel.find('\\')!=std::string::npos) return false;
    for(size_t i=0;;) { size_t j=rel.find('/',i); std::string part=rel.substr(i,j==std::string::npos?std::string::npos:j-i); if(part==".." || part.empty()) return false; if(j==std::string::npos) break; i=j+1; }
    return true;
}
static std::string pendingPath() {
    char base[MAX_PATH]; GetModuleFileNameA(nullptr,base,MAX_PATH); char* slash=strrchr(base,'\\'); if(slash) slash[1]=0;
    return std::string(base)+"coop-galaxy-pending.txt";
}

void GalaxySyncStartup() {
    std::string slot=readFile(pendingPath());
    if(slot.empty()) return;
    for(const fs::path& root:saveRoots()) selectGalaxy(root,slot);
    std::error_code ec; fs::remove(pendingPath(),ec);
    CoopLog("Galaxy: selected the partner's galaxy \"%s\" received last session",slot.c_str());
}

// ---- HOST: send the galaxy that is selected ----
static void sendGalaxy() {
    std::vector<fs::path> roots=saveRoots();
    for(const fs::path& root:roots) {
        fs::path dir=root/selectedGalaxy(root);
        std::error_code ec; if(!fs::exists(dir/"Galaxy.txt",ec)) continue;
        std::vector<fs::path> files;
        for(auto it=fs::recursive_directory_iterator(dir,ec);it!=fs::recursive_directory_iterator();it.increment(ec)) if(it->is_regular_file(ec)) files.push_back(it->path());
        uint32_t i=0; size_t bytes=0;
        for(const fs::path& f:files) {
            std::string rel=fs::relative(f,dir,ec).generic_string(), data=readFile(f);
            Writer w; w.put<uint32_t>(i++); w.put<uint32_t>((uint32_t)files.size()); w.str(rel); w.put<uint32_t>((uint32_t)data.size());
            w.bytes.insert(w.bytes.end(),data.begin(),data.end());
            NetSendReliable(MSG_GALAXY_FILE,w.bytes.data(),w.bytes.size());
            bytes+=data.size();
        }
        CoopLog("Galaxy: sent \"%s\" to the partner (%u files, %zu bytes)",selectedGalaxy(root).c_str(),(unsigned)files.size(),bytes);
        snprintf(status,sizeof(status),"Co-op: sending your galaxy to your partner...");
        return;
    }
    CoopLog("Galaxy: partner asked for the galaxy, but no saved galaxy was found");
}

// ---- CLIENT: install it ----
static void install() {
    std::vector<fs::path> roots=saveRoots();
    if(roots.empty()) { snprintf(status,sizeof(status),"Co-op: could not find your Heat Signature save folder to install the partner's galaxy"); return; }
    char stamp[32]; time_t now=time(nullptr); strftime(stamp,sizeof(stamp),"%Y%m%d-%H%M%S",localtime(&now));
    std::string current=selectedGalaxy(roots[0]);
    std::string slot=current==SLOT_A?SLOT_B:SLOT_A;   // never overwrite the galaxy the running game will save into
    for(const fs::path& root:roots) {
        std::error_code ec;
        fs::path target=root/slot;
        if(fs::exists(target,ec)) fs::rename(target,root/(slot+".coop-backup-"+stamp),ec);
        fs::create_directories(target,ec);
        for(auto& kv:received) {
            fs::path p=target/fs::path(kv.second.first);
            fs::create_directories(p.parent_path(),ec);
            writeFile(p,kv.second.second);
        }
        std::string mine=selectedGalaxy(root);
        if(mine!=SLOT_A && mine!=SLOT_B) writeFile(root/".coop-previous-galaxy",mine+"\\");
        writeFile(root/("Options.txt.coop-backup-"+std::string(stamp)),readFile(root/"Options.txt"));
        selectGalaxy(root,slot);
    }
    writeFile(pendingPath(),slot);
    installed=true;
    snprintf(status,sizeof(status),"Co-op: your partner's galaxy is installed as \"%s\" - quit and restart Heat Signature to join it",slot.c_str());
    CoopLog("Galaxy: installed the partner's galaxy (%u files) as \"%s\"; restart to play in it (your galaxy \"%s\" is kept)",(unsigned)received.size(),slot.c_str(),current.c_str());
}

void GalaxySyncOnMessage(uint8_t type,const uint8_t* data,size_t size) {
    Reader r(data,size);
    if(type==MSG_GALAXY_REQUEST) { if(NetIsHost()) sendGalaxy(); return; }
    if(type!=MSG_GALAXY_FILE || NetIsHost() || installed) return;
    uint32_t index=r.get<uint32_t>(), total=r.get<uint32_t>(); std::string rel=r.str(); uint32_t n=r.get<uint32_t>();
    if(!r.ok || n>r.left || !safeRelative(rel) || total>100000) { CoopLog("Galaxy: rejected a malformed galaxy file"); return; }
    if(index==0) received.clear();
    transferTotal=total; received[index]={rel,std::string((const char*)r.p,n)};
    snprintf(status,sizeof(status),"Co-op: receiving your partner's galaxy (%u/%u files)",(unsigned)received.size(),total);
    if(received.size()==total) install();
}

void GalaxySyncTick(bool host,uint32_t localGalaxy,uint32_t remoteGalaxy,bool partnerFresh,bool sameBuild) {
    if(host || requested || installed || !partnerFresh || !sameBuild || !localGalaxy || !remoteGalaxy || localGalaxy==remoteGalaxy) return;
    requested=true;
    NetSendReliable(MSG_GALAXY_REQUEST,"",0);
    snprintf(status,sizeof(status),"Co-op: your galaxy differs from the host's - downloading theirs...");
    CoopLog("Galaxy: different galaxy than the host (%08x vs %08x); requesting theirs",localGalaxy,remoteGalaxy);
}
void GalaxySyncRequest() {
    if(NetIsHost()) { CoopLog("Galaxy: only the CLIENT downloads the host's galaxy"); return; }
    requested=true; installed=false; received.clear();
    NetSendReliable(MSG_GALAXY_REQUEST,"",0);
    snprintf(status,sizeof(status),"Co-op: downloading the host's galaxy...");
    CoopLog("Galaxy: requested the host's galaxy");
}
void GalaxySyncReset() { if(!installed) requested=false; }
const char* GalaxySyncStatus() { return status; }
