// Lobby codes: see Lobby.h.
#include "Lobby.h"
#include "Console.h"
#include "GmArgs.h"
#include "Net.h"
#include "Steam.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>

// Implemented in CoopProbe.cpp: (re)starts networking with these settings; false if the socket could not be opened.
bool CoopStartNetwork(bool host,const char* bind,const char* peer,const char* altPeer,const char* password);
void CoopStopNetwork();
bool CoopStartNetworkSteam(bool host,const char* password);

static const HS_ModApi* api;
static std::string baseDir;
static int state;                       // see LobbyState
static std::string code, note, lastCode;
static uint16_t hostPort;
static uint8_t sessionKey[6];
static bool joinRequest;
static char panelError[160]; static DWORD panelErrorAt;
static bool viaSteam, steamNetStarted;   // this lobby runs through Steam (else by IP address)
// A Steam lobby's code: its 64-bit id and a check byte, as 15 letters and digits.
static std::string steamCode(uint64_t id);
static bool readSteamCode(const char* text,uint64_t& id);

// ---- The code: 17 bytes as 28 letters and digits (Crockford base32: no I, L, O or U), shown in groups of four ----
// internet address (4, zero when unknown), local address (4), port (2), session key (6), check byte (1).
static const char ALPHABET[]="0123456789ABCDEFGHJKMNPQRSTVWXYZ";
static uint8_t checkByte(const uint8_t* d,int n){ uint8_t c=0x5a; for(int i=0;i<n;i++){ c^=d[i]; for(int b=0;b<8;b++) c=(uint8_t)((c&0x80)?((c<<1)^0x07):(c<<1)); } return c; }
static std::string encode(const uint8_t* data,int n) {
    std::string out; uint32_t acc=0; int bits=0;
    for(int i=0;i<n;i++){ acc=(acc<<8)|data[i]; bits+=8; while(bits>=5){ out+=ALPHABET[(acc>>(bits-5))&31]; bits-=5; } }
    if(bits) out+=ALPHABET[(acc<<(5-bits))&31];
    std::string grouped; for(size_t i=0;i<out.size();i++){ if(i && i%4==0) grouped+='-'; grouped+=out[i]; }
    return grouped;
}
static bool decode(const char* text,uint8_t* data,int n) {
    uint32_t acc=0; int bits=0, got=0;
    for(const char* p=text;*p;p++) {
        char ch=(char)toupper((unsigned char)*p);
        if(ch=='-'||ch==' '||ch=='\t'||ch=='\r'||ch=='\n') continue;
        if(ch=='O') ch='0'; if(ch=='I'||ch=='L') ch='1'; if(ch=='U') ch='V';
        const char* at=strchr(ALPHABET,ch); if(!at) return false;
        acc=(acc<<5)|(uint32_t)(at-ALPHABET); bits+=5;
        if(bits>=8){ if(got<n) data[got]=(uint8_t)(acc>>(bits-8)); got++; bits-=8; }
    }
    return got==n;
}
struct CodeData { uint8_t internet[4], local[4]; uint16_t port; uint8_t key[6]; };
static std::string makeCode(const CodeData& c) {
    uint8_t d[17]; memcpy(d,c.internet,4); memcpy(d+4,c.local,4); d[8]=(uint8_t)(c.port>>8); d[9]=(uint8_t)c.port; memcpy(d+10,c.key,6); d[16]=checkByte(d,16);
    return encode(d,17);
}
static bool readCode(const char* text,CodeData& c) {
    uint8_t d[17]; if(!decode(text,d,17) || d[16]!=checkByte(d,16)) return false;
    memcpy(c.internet,d,4); memcpy(c.local,d+4,4); c.port=(uint16_t)((d[8]<<8)|d[9]); memcpy(c.key,d+10,6);
    return c.port!=0;
}
static std::string steamCode(uint64_t id) { uint8_t d[9]; for(int i=0;i<8;i++) d[i]=(uint8_t)(id>>(56-8*i)); d[8]=checkByte(d,8); return encode(d,9); }
static bool readSteamCode(const char* text,uint64_t& id) {
    uint8_t d[9]; if(!decode(text,d,9) || d[8]!=checkByte(d,8)) return false;
    id=0; for(int i=0;i<8;i++) id=(id<<8)|d[i];
    return id!=0;
}
static std::string passwordOf(const uint8_t key[6]) { char b[40]; snprintf(b,sizeof(b),"hs-lobby-%02x%02x%02x%02x%02x%02x",key[0],key[1],key[2],key[3],key[4],key[5]); return b; }
static std::string dotted(const uint8_t a[4]) { char b[24]; snprintf(b,sizeof(b),"%u.%u.%u.%u",a[0],a[1],a[2],a[3]); return b; }
static bool isZero(const uint8_t a[4]) { return !(a[0]|a[1]|a[2]|a[3]); }
// Addresses that are not reachable from the internet: the router itself sits behind another one (or a carrier's).
static bool isPrivate(const uint8_t a[4]) { return a[0]==10 || (a[0]==172 && a[1]>=16 && a[1]<32) || (a[0]==192 && a[1]==168) || (a[0]==100 && a[1]>=64 && a[1]<128) || a[0]==127 || (a[0]==169 && a[1]==254); }

// ---- This PC's address on the local network (the one its default route uses; nothing is sent) ----
static bool localAddress(uint8_t out[4]) {
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); if(s==INVALID_SOCKET) return false;
    sockaddr_in to{}; to.sin_family=AF_INET; to.sin_port=htons(53); inet_pton(AF_INET,"198.51.100.1",&to.sin_addr);
    bool ok=false; sockaddr_in me{}; int len=sizeof(me);
    if(!connect(s,(sockaddr*)&to,sizeof(to)) && !getsockname(s,(sockaddr*)&me,&len)) { memcpy(out,&me.sin_addr,4); ok=!isZero(out); }
    closesocket(s); return ok;
}

// ---- Asking the router to open the port (UPnP), on its own thread: it waits on the network ----
struct Router { std::atomic<int> done{0}; std::mutex m; uint8_t internet[4]{}; bool mapped=false; std::string detail, control, service, host; int port=0; };
static Router* router;
static std::string httpRequest(const std::string& host,int port,const std::string& request,int timeoutMs=3000) {
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); if(s==INVALID_SOCKET) return "";
    DWORD t=(DWORD)timeoutMs; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&t,sizeof(t)); setsockopt(s,SOL_SOCKET,SO_SNDTIMEO,(const char*)&t,sizeof(t));
    sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons((u_short)port);
    std::string reply;
    if(inet_pton(AF_INET,host.c_str(),&a.sin_addr)==1 && !connect(s,(sockaddr*)&a,sizeof(a)) && send(s,request.data(),(int)request.size(),0)>0) {
        char buf[4096]; int n; while((n=recv(s,buf,sizeof(buf),0))>0 && reply.size()<200000) reply.append(buf,n);
    }
    closesocket(s); return reply;
}
static std::string between(const std::string& s,const std::string& a,const std::string& b,size_t from=0) {
    size_t i=s.find(a,from); if(i==std::string::npos) return ""; i+=a.size();
    size_t j=s.find(b,i); return j==std::string::npos?"":s.substr(i,j-i);
}
static std::string soap(Router& r,const char* action,const std::string& args) {
    std::string body="<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:"
        +std::string(action)+" xmlns:u=\""+r.service+"\">"+args+"</u:"+action+"></s:Body></s:Envelope>";
    char head[700]; snprintf(head,sizeof(head),"POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\nConnection: close\r\nContent-Length: %u\r\n\r\n",
        r.control.c_str(),r.host.c_str(),r.port,r.service.c_str(),action,(unsigned)body.size());
    return httpRequest(r.host,r.port,head+body);
}
// This PC's internet address when the router does not tell (no UPnP): asked of a public STUN server, the standard way
// to learn one's own address. One small UDP question; the server learns nothing but the address it answers with.
// Off with HS_COOP_NO_STUN (and never asked in the test copies).
static bool publicAddress(uint8_t out[4]) {
    if(getenv("HS_COOP_NO_STUN") || getenv("HS_COOP_NO_UPNP")) return false;
    for(const char* server:{"stun.l.google.com:19302","stun.cloudflare.com:3478"}) {
        std::string host=server; int port=atoi(host.c_str()+host.find(':')+1); host.resize(host.find(':'));
        addrinfo hints{},*res=nullptr; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_DGRAM;
        if(getaddrinfo(host.c_str(),nullptr,&hints,&res)!=0 || !res) continue;
        sockaddr_in to=*(sockaddr_in*)res->ai_addr; to.sin_port=htons((u_short)port); freeaddrinfo(res);
        SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP); if(s==INVALID_SOCKET) continue;
        DWORD t=1200; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&t,sizeof(t));
        uint8_t q[20]{0,1,0,0,0x21,0x12,0xA4,0x42}; BCryptGenRandom(nullptr,q+8,12,BCRYPT_USE_SYSTEM_PREFERRED_RNG);   // binding request
        uint8_t a[512]; int n=-1;
        if(sendto(s,(const char*)q,20,0,(sockaddr*)&to,sizeof(to))==20) n=recv(s,(char*)a,sizeof(a),0);
        closesocket(s);
        if(n<20 || memcmp(a+8,q+8,12)) continue;
        for(int i=20;i+4<=n;) {
            int type=(a[i]<<8)|a[i+1], len=(a[i+2]<<8)|a[i+3];
            if(type==0x0020 && len>=8 && a[i+5]==1) { out[0]=a[i+8]^0x21; out[1]=a[i+9]^0x12; out[2]=a[i+10]^0xA4; out[3]=a[i+11]^0x42; return true; }   // XOR-MAPPED-ADDRESS
            if(type==0x0001 && len>=8 && a[i+5]==1) { memcpy(out,a+i+8,4); return true; }
            i+=4+((len+3)&~3);
        }
    }
    return false;
}
static void routerThread(Router* r,uint8_t local[4],uint16_t port) {
    // 1. Find the router: one multicast question, answers for two seconds.
    SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    std::vector<std::string> locations;
    if(s!=INVALID_SOCKET) {
        sockaddr_in bindTo{}; bindTo.sin_family=AF_INET; memcpy(&bindTo.sin_addr,local,4); bind(s,(sockaddr*)&bindTo,sizeof(bindTo));
        DWORD t=500; setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,(const char*)&t,sizeof(t));
        sockaddr_in to{}; to.sin_family=AF_INET; to.sin_port=htons(1900); inet_pton(AF_INET,"239.255.255.250",&to.sin_addr);
        for(const char* target:{"urn:schemas-upnp-org:device:InternetGatewayDevice:1","urn:schemas-upnp-org:service:WANIPConnection:1","urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
            char q[300]; int n=snprintf(q,sizeof(q),"M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n",target);
            sendto(s,q,n,0,(sockaddr*)&to,sizeof(to));
        }
        DWORD start=GetTickCount(); char buf[2048];
        while(GetTickCount()-start<2200 && locations.size()<12) {
            int n=recv(s,buf,sizeof(buf)-1,0); if(n<=0) continue; buf[n]=0;
            std::string reply=buf; for(char& ch:reply) if(ch>='A'&&ch<='Z') ch=(char)(ch+32);
            size_t i=reply.find("location:"); if(i==std::string::npos) continue;
            size_t e=reply.find("\r\n",i); if(e==std::string::npos) continue;
            std::string url=std::string(buf).substr(i+9,e-i-9);
            while(!url.empty() && url[0]==' ') url.erase(0,1);
            if(url.rfind("http://",0)==0 && url.find('/',7)!=std::string::npos && std::find(locations.begin(),locations.end(),url)==locations.end()) locations.push_back(url);
        }
        closesocket(s);
    }
    auto finish=[&](const char* why){ uint8_t pub[4]{}; bool known=publicAddress(pub); std::lock_guard<std::mutex> g(r->m); if(known) memcpy(r->internet,pub,4); r->detail=why; r->done=1; };
    // 2. Other things answer too (TVs, lamps): the router is the one that offers a port-mapping service.
    for(const std::string& location:locations) {
        std::string hostPort=between(location,"http://","/"); std::string path=location.substr(location.find('/',7));
        size_t colon=hostPort.find(':'); std::string host=hostPort.substr(0,colon); int port=colon==std::string::npos?80:atoi(hostPort.c_str()+colon+1);
        std::string desc=httpRequest(host,port,"GET "+path+" HTTP/1.1\r\nHost: "+hostPort+"\r\nConnection: close\r\n\r\n",2000);
        for(const char* kind:{"urn:schemas-upnp-org:service:WANIPConnection:2","urn:schemas-upnp-org:service:WANIPConnection:1","urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
            size_t i=desc.find(kind); if(i==std::string::npos) continue;
            std::string control=between(desc,"<controlURL>","</controlURL>",i); if(control.empty()) continue;
            if(control.rfind("http://",0)==0) control=control.substr(control.find('/',7));
            if(control[0]!='/') control="/"+control;
            r->control=control; r->service=kind; r->host=host; r->port=port; break;
        }
        if(!r->control.empty()) break;
    }
    if(r->control.empty()) return finish(locations.empty()?"no router answered (UPnP is off or not supported)":"the router does not offer to open ports (UPnP is off or not supported)");
    // 3. The internet address the router has, and the port opened to this PC for two hours.
    std::string ip=between(soap(*r,"GetExternalIPAddress",""),"<NewExternalIPAddress>","</NewExternalIPAddress>");
    uint8_t internet[4]{}; if(inet_pton(AF_INET,ip.c_str(),internet)!=1) memset(internet,0,4);
    if(isZero(internet) || isPrivate(internet)) { uint8_t pub[4]; if(publicAddress(pub)) memcpy(internet,pub,4); }
    char args[600];
    auto mapPort=[&](int lease){
        snprintf(args,sizeof(args),"<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>%u</NewInternalPort><NewInternalClient>%u.%u.%u.%u</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>HS_Coop</NewPortMappingDescription><NewLeaseDuration>%d</NewLeaseDuration>",
            port,port,local[0],local[1],local[2],local[3],lease);
        std::string reply=soap(*r,"AddPortMapping",args);
        return reply.find("AddPortMappingResponse")!=std::string::npos;
    };
    bool mapped=mapPort(7200) || mapPort(0);   // some routers only accept mappings without an end time
    { std::lock_guard<std::mutex> g(r->m); memcpy(r->internet,internet,4); r->mapped=mapped;
      r->detail=mapped?"the router opened the port":"the router refused to open the port"; r->done=1; }
}
static void closeRouterPort() {
    if(!router || !router->done || !router->mapped) return;
    Router* r=router; uint16_t port=hostPort;
    std::thread([r,port]{ char args[200]; snprintf(args,sizeof(args),"<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>UDP</NewProtocol>",port); soap(*r,"DeletePortMapping",args); }).detach();
    router->mapped=false;
}

static void copyToClipboard(const std::string& text);
static std::string clipboardText();
static std::string codeInClipboard();
// ---- Hosting and joining ----
void LobbyInit(const HS_ModApi* a,const char* dir) {
    api=a; baseDir=dir;
    FILE* f=fopen((baseDir+"coop-lobby.txt").c_str(),"r");
    if(f) { char b[80]{}; if(fgets(b,sizeof(b),f)) { b[strcspn(b,"\r\n")]=0; lastCode=b; } fclose(f); }
}
static uint8_t hostLocal[4];
static void finishHosting() {
    CodeData c{}; memcpy(c.local,hostLocal,4); c.port=hostPort; memcpy(c.key,sessionKey,6);
    bool reachable=false;
    if(router) { std::lock_guard<std::mutex> g(router->m);
        if(!isZero(router->internet) && !isPrivate(router->internet)) { memcpy(c.internet,router->internet,4); reachable=router->mapped; }
        if(reachable) note="Players anywhere can join with this code.";
        else if(!isZero(router->internet) && isPrivate(router->internet)) note="Your router is behind another router: the code works on your network or a VPN.";
        else if(!isZero(c.internet)) { char b[200]; snprintf(b,sizeof(b),"For a partner on another network: forward UDP port %u to this PC in your router (it would not open it itself), or use a VPN.",hostPort); note=b; }
        else { char b[160]; snprintf(b,sizeof(b),"No answer from your router: the code works on your network or a VPN (or forward UDP port %u).",hostPort); note=b; }
        CoopLog("Lobby: %s; internet address %s",router->detail.c_str(),isZero(router->internet)?"unknown":"known");
    } else note="The code works on your network or a VPN.";
    code=makeCode(c); state=2; CoopLogHide(code.c_str());
    copyToClipboard(code);
    CoopLog("Lobby: hosting; code %s (%s)",code.c_str(),note.c_str());
}
// The Windows clipboard, used directly (the game engine's own clipboard functions did not see text copied elsewhere).
static void copyToClipboard(const std::string& text) {
    if(!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL mem=GlobalAlloc(GMEM_MOVEABLE,text.size()+1);
    if(mem) { char* p=(char*)GlobalLock(mem); if(p) { memcpy(p,text.c_str(),text.size()+1); GlobalUnlock(mem); SetClipboardData(CF_TEXT,mem); } }
    CloseClipboard();
}
static std::string clipboardText() {
    std::string out; if(!OpenClipboard(nullptr)) return out;
    HANDLE h=GetClipboardData(CF_UNICODETEXT);
    if(h) { const wchar_t* w=(const wchar_t*)GlobalLock(h); if(w) { for(;*w && out.size()<400;w++) out+=(*w<128)?(char)*w:' '; GlobalUnlock(h); } }
    CloseClipboard(); return out;
}
// A lobby code found in the clipboard's text ("" if none): the text itself, or any word of it (a code pasted from a
// chat often comes with a name or a sentence around it).
static std::string codeInClipboard() {
    std::string clip=clipboardText(); if(clip.empty()) return "";
    auto valid=[](const std::string& s){ uint64_t id; CodeData c{}; return !s.empty() && s.size()<64 && (readSteamCode(s.c_str(),id) || readCode(s.c_str(),c)); };
    std::string all; for(char ch:clip) if(!isspace((unsigned char)ch)) all+=ch;
    if(valid(all)) return all;
    std::string word;
    for(size_t i=0;i<=clip.size();i++) {
        char ch=i<clip.size()?clip[i]:' ';
        if(isalnum((unsigned char)ch) || ch=='-') { word+=ch; continue; }
        if(valid(word)) return word;
        word.clear();
    }
    return "";
}
bool LobbyHost(char* error,size_t errorSize) {
    LobbyLeave();
    // Through Steam when the game runs under Steam: no address, no router setting. By IP address otherwise.
    if(!getenv("HS_COOP_NO_STEAM") && SteamAvailable() && SteamHostLobby()) {
        viaSteam=true; steamNetStarted=false; state=1; code.clear(); note="Creating a Steam lobby...";
        CoopLog("Lobby: hosting through Steam"); return true;
    }
    return LobbyHostByIp(error,errorSize);
}
bool LobbyHostByIp(char* error,size_t errorSize) {
    LobbyLeave();
    if(!localAddress(hostLocal)) { snprintf(error,errorSize,"this PC has no network address"); return false; }
    BCryptGenRandom(nullptr,sessionKey,sizeof(sessionKey),BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    bool open=false;
    for(int port=27015;port<27025 && !open;port++) {
        char bindTo[32]; snprintf(bindTo,sizeof(bindTo),"0.0.0.0:%d",port);
        if(CoopStartNetwork(true,bindTo,"",nullptr,passwordOf(sessionKey).c_str())) { hostPort=(uint16_t)port; open=true; }
    }
    if(!open) { snprintf(error,errorSize,"could not open a network port (27015-27024 are in use)"); return false; }
    state=1; code.clear(); note="Asking your router to open the port...";
    router=nullptr;   // an earlier one may still be in use by its thread: left alone (a few hundred bytes)
    if(!getenv("HS_COOP_NO_UPNP")) { router=new Router(); std::thread(routerThread,router,hostLocal,hostPort).detach(); }
    else finishHosting();
    return true;
}
bool LobbyJoin(const char* text,char* error,size_t errorSize) {
    uint64_t steamLobby=0;
    if(text) CoopLogHide(text);
    if(text && readSteamCode(text,steamLobby)) {
        LobbyLeave();
        if(!SteamAvailable()) { snprintf(error,errorSize,"that is a Steam lobby code, and Steam is not available in this game"); return false; }
        if(!SteamJoinLobby(steamLobby)) { snprintf(error,errorSize,"%s",SteamError()); return false; }
        viaSteam=true; steamNetStarted=false; state=3; code.clear(); note="Joining through Steam...";
        lastCode=text; FILE* f=fopen((baseDir+"coop-lobby.txt").c_str(),"w"); if(f) { fputs(text,f); fclose(f); }
        CoopLog("Lobby: joining a Steam lobby"); return true;
    }
    CodeData c{};
    if(!text || !readCode(text,c)) { snprintf(error,errorSize,"that is not a valid lobby code (check for a typo)"); return false; }
    LobbyLeave();
    // Both of the host's addresses are tried: the internet one, and the local one for players on the same network.
    char first[40], second[40]; bool haveInternet=!isZero(c.internet);
    snprintf(first,sizeof(first),"%s:%u",dotted(haveInternet?c.internet:c.local).c_str(),c.port);
    snprintf(second,sizeof(second),"%s:%u",dotted(c.local).c_str(),c.port);
    if(!CoopStartNetwork(false,"0.0.0.0:0",first,haveInternet?second:nullptr,passwordOf(c.key).c_str())) { snprintf(error,errorSize,"could not start networking"); return false; }
    state=3; code.clear(); note="Connecting to the host..."; joinRequest=true;
    std::string shown=text; lastCode=shown;
    FILE* f=fopen((baseDir+"coop-lobby.txt").c_str(),"w"); if(f) { fputs(shown.c_str(),f); fclose(f); }
    CoopLog("Lobby: joining with a code (host %s%s)",first,haveInternet?" and its local address":"");
    return true;
}
void LobbyLeave() {
    if(!state) return;
    closeRouterPort(); CoopStopNetwork();
    if(viaSteam) SteamLeaveLobby();
    viaSteam=false; steamNetStarted=false;
    state=0; code.clear(); note.clear(); joinRequest=false;
    CoopLog("Lobby: left");
}
const char* LobbyCode() { return code.c_str(); }
int LobbyState() { return state; }
bool LobbyTakeJoinRequest() { bool r=joinRequest; joinRequest=false; return r; }
// Called only once the player is in the game: joining while the game is still starting up (Steam started it for the
// invite) changed the generation seed under the loading galaxy and the game closed.
void LobbyInviteTick() {
    // An invite accepted in Steam (or the game started for one) joins that lobby: no code to copy.
    if(!getenv("HS_COOP_NO_STEAM")) {
        uint64_t invited=SteamTakeInvite();
        if(invited && !(viaSteam && SteamLobbyId()==invited)) {
            char err[160]="";
            if(LobbyJoin(steamCode(invited).c_str(),err,sizeof(err))) note="Joining your friend through Steam...";
            else { snprintf(panelError,sizeof(panelError),"%s",err); panelErrorAt=GetTickCount(); CoopLog("Lobby: could not join the invite: %s",err); }
        }
    }
}
void LobbyTick() {
    if(state==1 && !viaSteam && router && router->done) finishHosting();
    if(!viaSteam) return;
    SteamTick();
    int st=SteamLobbyState();
    if(state==1 && st==2) {
        // The lobby exists: its id is the code, its data holds the session key.
        if(!CoopStartNetworkSteam(true,SteamSessionKey())) { note="Could not start networking."; return; }
        steamNetStarted=true; code=steamCode(SteamLobbyId()); CoopLogHide(code.c_str()); state=2; note="Send this code, or invite your partner in Steam (friends list: Invite to Game).";
        copyToClipboard(code); CoopLog("Lobby: Steam lobby ready; code %s",code.c_str());
    } else if(state==1 && st==-1) {
        // Steam would not make a lobby: the IP-address code is the fallback.
        std::string why=SteamError(); char err[160]="";
        viaSteam=false; SteamLeaveLobby();
        if(LobbyHostByIp(err,sizeof(err))) CoopLog("Lobby: Steam lobby failed (%s); hosting by IP address",why.c_str());
        else { note="Steam: "+why+". "+err; state=0; }
    } else if(state==3 && st==4 && !steamNetStarted && SteamSessionKey()[0]) {
        if(CoopStartNetworkSteam(false,SteamSessionKey())) { steamNetStarted=true; joinRequest=true; note="Connecting to the host through Steam..."; }
    } else if(state==3 && st==-1) {
        std::string why=SteamError(); LobbyLeave(); snprintf(panelError,sizeof(panelError),"%s",why.c_str()); panelErrorAt=GetTickCount();
    }
}

// ---- The panel in the Escape menu ----
static bool typing; static std::string typed;
static std::string globalText(CInstance* self,const char* name) {
    GmArgs a; a.AddStr(api,name); RValue v=Engine(api,"variable_global_get",self,a.Count(),a.Build());
    return GetRValueKind(v)==RValueKind::String && v.str && v.str->text ? v.str->text : "";
}
static void setGlobalText(CInstance* self,const char* name,const char* text) { GmArgs a; a.AddStr(api,name); a.AddStr(api,text); Engine(api,"variable_global_set",self,a.Count(),a.Build()); }
static bool pressed(CInstance* self,int key) { GmArgs a; a.AddReal(key); return Number(Engine(api,"keyboard_check_pressed",self,a.Count(),a.Build()))>0.5; }
static void text(CInstance* self,double x,double y,const char* s,double colour) {
    GmArgs c; c.AddReal(colour); Engine(api,"draw_set_colour",self,c.Count(),c.Build());
    GmArgs t; t.AddReal(x); t.AddReal(y); t.AddStr(api,s); Engine(api,"draw_text",self,t.Count(),t.Build());
}
void LobbyDraw(CInstance* self,bool menuOpen,bool connected,const char* partnerName,double h) {
    if(!self) return;
    if(!menuOpen) { typing=false; return; }
    const int F5=116, F6=117, ENTER=13;
    char error[160]="";
    if(typing) {
        typed=globalText(self,"keyboard_string"); if(typed.size()>40) typed.resize(40);
        // Ctrl+V pastes: a valid code joins at once; anything else is put in the box to be corrected.
        const int CTRL=17, KEY_V=86;
        GmArgs held; held.AddReal(CTRL);
        if(pressed(self,KEY_V) && Number(Engine(api,"keyboard_check",self,held.Count(),held.Build()))>0.5) {
            std::string clip=codeInClipboard();
            if(!clip.empty()) { typing=false; if(!LobbyJoin(clip.c_str(),error,sizeof(error))) { snprintf(panelError,sizeof(panelError),"%s",error); panelErrorAt=GetTickCount(); } }
            else {
                std::string raw=clipboardText(); typed.clear();
                for(char ch:raw) if(typed.size()<40 && (isalnum((unsigned char)ch) || ch=='-')) typed+=ch;
                setGlobalText(self,"keyboard_string",typed.c_str());
                if(typed.empty()) { snprintf(panelError,sizeof(panelError),"the clipboard has no lobby code"); panelErrorAt=GetTickCount(); }
            }
        }
        else if(pressed(self,ENTER)) { typing=false; if(!LobbyJoin(typed.c_str(),error,sizeof(error))) { snprintf(panelError,sizeof(panelError),"%s",error); panelErrorAt=GetTickCount(); } }
        else if(pressed(self,F6)) typing=false;
    } else if(pressed(self,F5)) {
        if(state==1||state==2) LobbyLeave();
        else if(!LobbyHost(error,sizeof(error))) { snprintf(panelError,sizeof(panelError),"%s",error); panelErrorAt=GetTickCount(); }
    } else if(pressed(self,F6)) {
        if(state==3) LobbyLeave();
        else if((state==1||state==2) && viaSteam) { if(!LobbyHostByIp(error,sizeof(error))) { snprintf(panelError,sizeof(panelError),"%s",error); panelErrorAt=GetTickCount(); } }
        else {
            // A code copied from a chat is taken straight from the clipboard (either kind of code, with whatever was
            // copied around it); otherwise it is typed, or pasted into the box with Ctrl+V.
            std::string clip=codeInClipboard();
            if(!clip.empty()) { if(!LobbyJoin(clip.c_str(),error,sizeof(error))) { snprintf(panelError,sizeof(panelError),"%s",error); panelErrorAt=GetTickCount(); } }
            else { typing=true; typed=lastCode; setGlobalText(self,"keyboard_string",typed.c_str()); }
        }
    }
    double x=40, y=h-250; const double white=0xffffff, yellow=0x66ddff, grey=0xc8c8c8, red=0x4040ff;
    GmArgs left; left.AddReal(0); Engine(api,"draw_set_halign",self,left.Count(),left.Build());
    {   // a dark backing, so the panel reads over any floor
        GmArgs a; a.AddReal(0.72); Engine(api,"draw_set_alpha",self,a.Count(),a.Build());
        GmArgs c; c.AddReal(0); Engine(api,"draw_set_colour",self,c.Count(),c.Build());
        GmArgs r; r.AddReal(x-14); r.AddReal(y-10); r.AddReal(x+760); r.AddReal(y+150); r.AddReal(0); Engine(api,"draw_rectangle",self,r.Count(),r.Build());
        GmArgs one; one.AddReal(1); Engine(api,"draw_set_alpha",self,one.Count(),one.Build());
    }
    text(self,x,y,"CO-OP",yellow); y+=26;
    char line[220];
    if(typing) {
        snprintf(line,sizeof(line),"Lobby code: %s_",typed.c_str()); text(self,x,y,line,white); y+=24;
        text(self,x,y,"Type the code or press Ctrl+V to paste it, then Enter.  F6 cancels.",grey); y+=24;
    } else if(state==1) { text(self,x,y,viaSteam?"Hosting: creating a Steam lobby...":"Hosting: getting your lobby code...",white); y+=24; text(self,x,y,"F5  stop hosting",grey); y+=24; }
    else if(state==2) {
        snprintf(line,sizeof(line),"Lobby code (copied):  %s",code.c_str()); text(self,x,y,line,white); y+=24;
        text(self,x,y,note.c_str(),grey); y+=24;
        if(connected) snprintf(line,sizeof(line),"%s has joined.",partnerName&&*partnerName?partnerName:"Your partner"); else snprintf(line,sizeof(line),"Waiting for your partner to enter the code...");
        text(self,x,y,line,connected?yellow:grey); y+=24;
        text(self,x,y,viaSteam?"F5  stop hosting      F6  switch to an IP-address code":"F5  stop hosting",grey); y+=24;
    } else if(state==3) {
        if(connected) snprintf(line,sizeof(line),"Joined %s.",partnerName&&*partnerName?partnerName:"your partner"); else snprintf(line,sizeof(line),"Connecting to the host...  (the host must be in the game with hosting on)");
        text(self,x,y,line,connected?yellow:white); y+=24;
        text(self,x,y,"F6  leave",grey); y+=24;
    } else {
        text(self,x,y,"F5  Host a game (you get a lobby code to give your partner)",white); y+=24;
        text(self,x,y,"F6  Join a game with a lobby code (copy the code first, or type it)",white); y+=24;
    }
    if(panelError[0] && GetTickCount()-panelErrorAt<6000) text(self,x,y,panelError,red);
    text(self,0,0,"",white);
}
