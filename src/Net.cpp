#include "Net.h"
#include "Console.h"
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <vector>

#pragma pack(push,1)
struct Header {
    uint32_t magic;
    uint8_t version, type;
    uint16_t size;           // payload bytes after the header
    uint64_t session;        // random per process
    uint32_t seq;            // per-datagram, increasing
};
struct FragmentHeader {
    uint32_t reliableSeq;    // per-fragment, consecutive
    uint32_t messageId;
    uint16_t index, count;
    uint8_t messageType;
};
#pragma pack(pop)

static const size_t MAC_SIZE=16, MAX_DATAGRAM=1400, FRAGMENT_PAYLOAD=1100, MAX_IN_FLIGHT=128;
// Resent after a quarter second (internet round trips of 50-200 ms would otherwise send everything twice); at most
// this many new fragments go out per frame, so a big transfer does not burst past the receiver's buffer.
static const DWORD RESEND_MS=250, PEER_QUIET_MS=5000, PEER_GONE_MS=10000;
static const size_t NEW_PER_PUMP=32;
static unsigned rejectedPassword, rejectedVersion; static char rejectedFrom[64];

static SOCKET sock=INVALID_SOCKET;
static bool isHost;
static sockaddr_in peer{};
static bool havePeer;
static BCRYPT_ALG_HANDLE hmacAlg;
static BCRYPT_HASH_HANDLE hmacProto;
static uint64_t session;
static uint32_t seq;
static uint64_t peerSession;
static uint32_t peerSeq;
static DWORD peerLastMs;
static char peerText[64];

// Reliable sender
struct Outgoing { uint32_t reliableSeq; std::vector<uint8_t> datagramPayload; DWORD lastSent; bool sent; };
static std::deque<Outgoing> outgoing;           // unacknowledged fragments, in order
static uint32_t nextReliableSeq=1, nextMessageId=1;
// Reliable receiver
static uint32_t expectedReliableSeq=1;
static std::map<uint32_t,std::vector<uint8_t>> earlyFragments;
static std::vector<uint8_t> assembling; static uint32_t assemblingId; static uint16_t assemblingNext;
static bool ackDue;

static bool parseAddress(const char* text, sockaddr_in& out) {
    char host[64]; strncpy(host,text,sizeof(host)-1); host[sizeof(host)-1]=0;
    char* colon=strrchr(host,':'); if(!colon) return false;
    *colon=0; int port=atoi(colon+1); if(port<0||port>65535) return false;
    memset(&out,0,sizeof(out)); out.sin_family=AF_INET; out.sin_port=htons((u_short)port);
    if(!*host || !strcmp(host,"*")) { out.sin_addr.s_addr=INADDR_ANY; return true; }
    if(inet_pton(AF_INET,host,&out.sin_addr)==1) return true;
    addrinfo hints{},*res=nullptr; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_DGRAM;
    if(getaddrinfo(host,nullptr,&hints,&res)!=0||!res) return false;
    out.sin_addr=((sockaddr_in*)res->ai_addr)->sin_addr; freeaddrinfo(res); return true;
}
static void trim(char* s) {
    char* e=s+strlen(s); while(e>s && (e[-1]=='\r'||e[-1]=='\n'||e[-1]==' '||e[-1]=='\t')) *--e=0;
    char* b=s; while(*b==' '||*b=='\t') b++; if(b!=s) memmove(s,b,strlen(b)+1);
}
bool NetLoadConfig(const char* path, NetConfig& c, char* error, size_t errorSize) {
    memset(&c,0,sizeof(c)); c.sharedTime=true;
    FILE* f=fopen(path,"r");
    if(!f) { snprintf(error,errorSize,"no %s; networking disabled",path); return false; }
    char line[256];
    while(fgets(line,sizeof(line),f)) {
        if(line[0]=='#'||line[0]==';') continue;
        char* eq=strchr(line,'='); if(!eq) continue; *eq=0;
        char* key=line; char* value=eq+1; trim(key); trim(value);
        for(char* p=value;*p;p++) if((*p==';'||*p=='#') && p>value && (p[-1]==' '||p[-1]=='\t')) { *p=0; trim(value); break; }
        if(!_stricmp(key,"role")) c.host=!_stricmp(value,"HOST");
        else if(!_stricmp(key,"bind")) strncpy(c.bind,value,sizeof(c.bind)-1);
        else if(!_stricmp(key,"peer")) strncpy(c.peer,value,sizeof(c.peer)-1);
        else if(!_stricmp(key,"password")) strncpy(c.password,value,sizeof(c.password)-1);
        else if(!_stricmp(key,"time")) c.sharedTime=_stricmp(value,"independent")!=0;
        else if(!_stricmp(key,"debug")) c.debug=atoi(value)!=0;
        else if(!_stricmp(key,"log")) c.verbose=!_stricmp(value,"verbose");
        else if(!_stricmp(key,"dev_port")) c.devPort=atoi(value);
    }
    fclose(f);
    if(strlen(c.password)<8) { snprintf(error,errorSize,"password must be at least 8 characters; networking disabled"); return false; }
    if(!c.host && !*c.peer) { snprintf(error,errorSize,"CLIENT requires peer=HOST_IP:PORT; networking disabled"); return false; }
    if(!*c.bind) strcpy(c.bind,c.host?"0.0.0.0:27015":"0.0.0.0:0");
    return true;
}
static void mac(const uint8_t* data, size_t size, uint8_t out[32]) {
    BCRYPT_HASH_HANDLE h=nullptr;
    BCryptDuplicateHash(hmacProto,&h,nullptr,0,0);
    BCryptHashData(h,(PUCHAR)data,(ULONG)size,0);
    BCryptFinishHash(h,out,32,0);
    BCryptDestroyHash(h);
}
bool NetOpen(const NetConfig& c) {
    WSADATA wsa; if(WSAStartup(MAKEWORD(2,2),&wsa)) { CoopLog("Net: WSAStartup failed"); return false; }
    // The password is only the HMAC key; each packet is authenticated by duplicating this keyed prototype.
    if(BCryptOpenAlgorithmProvider(&hmacAlg,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG)<0 ||
       BCryptCreateHash(hmacAlg,&hmacProto,nullptr,0,(PUCHAR)c.password,(ULONG)strlen(c.password),0)<0) { CoopLog("Net: HMAC unavailable"); return false; }
    BCryptGenRandom(nullptr,(PUCHAR)&session,sizeof(session),BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    sockaddr_in local{};
    if(!parseAddress(c.bind,local)) { CoopLog("Net: bad bind address %s",c.bind); return false; }
    isHost=c.host;
    if(!isHost) { if(!parseAddress(c.peer,peer)) { CoopLog("Net: bad peer address %s",c.peer); return false; } havePeer=true; }
    sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(sock==INVALID_SOCKET) { CoopLog("Net: socket failed %d",WSAGetLastError()); return false; }
    if(bind(sock,(sockaddr*)&local,sizeof(local))) { CoopLog("Net: bind %s failed %d",c.bind,WSAGetLastError()); closesocket(sock); sock=INVALID_SOCKET; return false; }
    u_long nonblocking=1; ioctlsocket(sock,FIONBIO,&nonblocking);
    int buffer=4<<20; setsockopt(sock,SOL_SOCKET,SO_RCVBUF,(const char*)&buffer,sizeof(buffer)); setsockopt(sock,SOL_SOCKET,SO_SNDBUF,(const char*)&buffer,sizeof(buffer));
    // Ignore ICMP port-unreachable resets so a peer that is not up yet does not break recvfrom.
    BOOL no=FALSE; DWORD bytes=0; WSAIoctl(sock,_WSAIOW(IOC_VENDOR,12),&no,sizeof(no),nullptr,0,&bytes,nullptr,nullptr);
    CoopLog("Net: %s bound %s%s%s",isHost?"HOST":"CLIENT",c.bind,isHost?"":" peer=",isHost?"":c.peer);
    return true;
}
bool NetIsHost() { return isHost; }
static unsigned ownEchoes, fromPartner, sentCount;
static bool (*customSend)(const void*,uint32_t); static int (*customReceive)(void*,uint32_t); static bool useCustom; static char customName[24];
bool NetOpenTransport(const NetConfig& c,bool (*send)(const void*,uint32_t),int (*receive)(void*,uint32_t),const char* name) {
    if(BCryptOpenAlgorithmProvider(&hmacAlg,BCRYPT_SHA256_ALGORITHM,nullptr,BCRYPT_ALG_HANDLE_HMAC_FLAG)<0 ||
       BCryptCreateHash(hmacAlg,&hmacProto,nullptr,0,(PUCHAR)c.password,(ULONG)strlen(c.password),0)<0) { CoopLog("Net: HMAC unavailable"); return false; }
    BCryptGenRandom(nullptr,(PUCHAR)&session,sizeof(session),BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    isHost=c.host; customSend=send; customReceive=receive; useCustom=true; havePeer=true;
    snprintf(customName,sizeof(customName),"%s",name);
    CoopLog("Net: %s through %s",isHost?"HOST":"CLIENT",name);
    return true;
}
static sockaddr_in altPeer; static bool haveAlt;
void NetSetAltPeer(const char* address) { haveAlt=address && *address && parseAddress(address,altPeer); }
static void resetReliable();
void NetClose() {
    if(sock!=INVALID_SOCKET) { closesocket(sock); sock=INVALID_SOCKET; }
    useCustom=false; customSend=nullptr; customReceive=nullptr;
    if(hmacProto) { BCryptDestroyHash(hmacProto); hmacProto=nullptr; }
    if(hmacAlg) { BCryptCloseAlgorithmProvider(hmacAlg,0); hmacAlg=nullptr; }
    havePeer=false; haveAlt=false; peerSession=0; peerSeq=0; peerLastMs=0; seq=0; rejectedPassword=rejectedVersion=0; peerText[0]=0;
    resetReliable();
}

static void sendDatagram(uint8_t type, const void* payload, size_t size) {
    if(size+sizeof(Header)+MAC_SIZE>MAX_DATAGRAM) { static int warned; if(warned++<5) CoopLog("Net: message type %d of %u bytes is too large for one datagram; not sent",(int)type,(unsigned)size); return; }
    if((sock==INVALID_SOCKET && !useCustom) || !havePeer) return;
    uint8_t buf[MAX_DATAGRAM];
    Header h{}; h.magic=PACKET_MAGIC; h.version=PACKET_VERSION; h.type=type; h.size=(uint16_t)size; h.session=session; h.seq=++seq;
    memcpy(buf,&h,sizeof(h)); if(size) memcpy(buf+sizeof(h),payload,size);
    uint8_t full[32]; mac(buf,sizeof(h)+size,full); memcpy(buf+sizeof(h)+size,full,MAC_SIZE);
    if(useCustom) {
        customSend(buf,(uint32_t)(sizeof(h)+size+MAC_SIZE)); sentCount++;
        // Developer aid for the one-account Steam test: how many packets came from the partner and how many were this
        // game's own, handed back by Steam.
        static DWORD last; DWORD now=GetTickCount();
        if(CoopVerbose() && now-last>5000) { last=now; CoopLog("Net: through %s: sent %u, from the partner %u, own packets handed back %u",customName,sentCount,fromPartner,ownEchoes); sentCount=fromPartner=ownEchoes=0; }
        return;
    }
    sendto(sock,(const char*)buf,(int)(sizeof(h)+size+MAC_SIZE),0,(sockaddr*)&peer,sizeof(peer));
    // Until the host has answered from one of its addresses, the other one is tried too.
    if(haveAlt && !isHost && !peerSession) sendto(sock,(const char*)buf,(int)(sizeof(h)+size+MAC_SIZE),0,(sockaddr*)&altPeer,sizeof(altPeer));
}
static_assert(sizeof(PlayerState)+64<1400,"PlayerState must fit one datagram");
void NetSendState(const PlayerState& state) { sendDatagram(PACKET_STATE,&state,sizeof(state)); }
void NetSendUnreliable(uint8_t type, const void* data, size_t size) {
    std::vector<uint8_t> p(1+size); p[0]=type; if(size) memcpy(p.data()+1,data,size);
    sendDatagram(PACKET_UNRELIABLE,p.data(),p.size());
}
void NetSendReliable(uint8_t type, const void* data, size_t size) {
    // Nothing is kept for a partner who is not connected: what matters is sent again when they connect (session start).
    if(!NetPeerAlive(PEER_QUIET_MS)) return;
    uint16_t count=(uint16_t)((size+FRAGMENT_PAYLOAD-1)/FRAGMENT_PAYLOAD); if(!count) count=1;
    uint32_t id=nextMessageId++;
    for(uint16_t i=0;i<count;i++) {
        size_t off=i*FRAGMENT_PAYLOAD, len=size-off<FRAGMENT_PAYLOAD?size-off:FRAGMENT_PAYLOAD; if(size==0) len=0;
        FragmentHeader f{nextReliableSeq++,id,i,count,type};
        Outgoing o; o.reliableSeq=f.reliableSeq; o.lastSent=0; o.sent=false;
        o.datagramPayload.resize(sizeof(f)+len); memcpy(o.datagramPayload.data(),&f,sizeof(f)); if(len) memcpy(o.datagramPayload.data()+sizeof(f),(const uint8_t*)data+off,len);
        outgoing.push_back(std::move(o));
    }
}
static void resetReliable() {
    outgoing.clear(); nextReliableSeq=1; nextMessageId=1;
    expectedReliableSeq=1; earlyFragments.clear(); assembling.clear(); assemblingNext=0; assemblingId=0;
}
static void deliverFragment(const std::vector<uint8_t>& payload, const NetHandlers& h) {
    FragmentHeader f; memcpy(&f,payload.data(),sizeof(f));
    const uint8_t* body=payload.data()+sizeof(f); size_t len=payload.size()-sizeof(f);
    if(f.index==0){ assembling.clear(); assemblingId=f.messageId; assemblingNext=0; }
    if(f.messageId!=assemblingId || f.index!=assemblingNext) { CoopLog("Net: reliable fragment out of order; dropped message %u",f.messageId); assemblingNext=0xffff; return; }
    assembling.insert(assembling.end(),body,body+len); assemblingNext++;
    if(assemblingNext==f.count && h.message) h.message(f.messageType,assembling.data(),assembling.size(),true);
}
void NetPump(const NetHandlers& h) {
    if(sock==INVALID_SOCKET && !useCustom) return;
    uint8_t buf[2048]; sockaddr_in from{}; int fromLen=sizeof(from);
    while(true) {
        int n=useCustom?customReceive(buf,sizeof(buf)):recvfrom(sock,(char*)buf,sizeof(buf),0,(sockaddr*)&from,&fromLen);
        if(n<0) break;
        if(n<(int)(sizeof(Header)+MAC_SIZE)) continue;
        Header hd; memcpy(&hd,buf,sizeof(hd));
        if(hd.magic!=PACKET_MAGIC) continue;
        if(hd.version!=PACKET_VERSION) { rejectedVersion++; snprintf(rejectedFrom,sizeof(rejectedFrom),"%s",inet_ntoa(from.sin_addr)); continue; }
        if(sizeof(Header)+hd.size+MAC_SIZE!=(size_t)n) continue;
        uint8_t full[32]; mac(buf,sizeof(Header)+hd.size,full);
        uint8_t diff=0; for(size_t i=0;i<MAC_SIZE;i++) diff|=full[i]^buf[sizeof(Header)+hd.size+i];   // constant-time compare
        if(diff) { rejectedPassword++; snprintf(rejectedFrom,sizeof(rejectedFrom),"%s",inet_ntoa(from.sin_addr)); continue; }
        if(hd.session==session) { ownEchoes++; continue; }   // this game's own packet come back (a carrier that loops back)
        fromPartner++;
        DWORD now=GetTickCount();
        bool sameSession=hd.session==peerSession;
        // Accept a new session only once the current one has gone quiet, which limits replayed-session takeover.
        if(!sameSession && peerSession && now-peerLastMs<5000) continue;
        if(sameSession && hd.seq<=peerSeq) continue;
        if(!sameSession) {
            CoopLog("Net: peer session %08llx from %s:%u",(unsigned long long)hd.session,inet_ntoa(from.sin_addr),ntohs(from.sin_port));
            // A new partner session (first contact or a restart): both sides start the reliable stream fresh and send
            // again what the partner needs (mission ships, object states, ...).
            resetReliable(); peerSession=hd.session; peerLastMs=now; if(h.sessionReset) h.sessionReset();
        }
        peerSession=hd.session; peerSeq=hd.seq; peerLastMs=now;
        if(useCustom) {}
        else if(isHost) { peer=from; havePeer=true; }
        else if(haveAlt) { if(from.sin_addr.s_addr==altPeer.sin_addr.s_addr && from.sin_port==altPeer.sin_port) peer=altPeer; haveAlt=false; }
        if(useCustom) snprintf(peerText,sizeof(peerText),"%s",customName); else
        snprintf(peerText,sizeof(peerText),"%s:%u",inet_ntoa(from.sin_addr),ntohs(from.sin_port));
        const uint8_t* body=buf+sizeof(Header);
        switch(hd.type) {
        case PACKET_STATE:
            if(hd.size==sizeof(PlayerState) && h.state){ PlayerState s; memcpy(&s,body,sizeof(s)); s.name[sizeof(s.name)-1]=0; s.parentName[sizeof(s.parentName)-1]=0; h.state(s); }
            break;
        case PACKET_UNRELIABLE:
            if(hd.size>=1 && h.message) h.message(body[0],body+1,hd.size-1,false);
            break;
        case PACKET_ACK:
            if(hd.size==4){ uint32_t acked; memcpy(&acked,body,4); while(!outgoing.empty() && outgoing.front().reliableSeq<=acked) outgoing.pop_front(); }
            break;
        case PACKET_RELIABLE: {
            if(hd.size<sizeof(FragmentHeader)) break;
            FragmentHeader f; memcpy(&f,body,sizeof(f));
            ackDue=true;
            if(f.reliableSeq<expectedReliableSeq) break;   // duplicate
            earlyFragments[f.reliableSeq].assign(body,body+hd.size);
            for(auto it=earlyFragments.find(expectedReliableSeq); it!=earlyFragments.end(); it=earlyFragments.find(expectedReliableSeq)) {
                std::vector<uint8_t> payload=std::move(it->second); earlyFragments.erase(it); expectedReliableSeq++;
                deliverFragment(payload,h);
            }
            break; }
        }
    }
    DWORD now=GetTickCount();
    if(ackDue){ uint32_t acked=expectedReliableSeq-1; sendDatagram(PACKET_ACK,&acked,4); ackDue=false; }
    // A partner gone quiet is not flooded with resends; after a while their queue is dropped (they start a new session).
    if(!NetPeerAlive(PEER_QUIET_MS)) { if(!outgoing.empty() && (!peerSession || now-peerLastMs>PEER_GONE_MS)) outgoing.clear(); return; }
    size_t inFlight=0, fresh=0;
    for(Outgoing& o:outgoing) {
        if(++inFlight>MAX_IN_FLIGHT) break;
        if(!o.sent) { if(fresh++>=NEW_PER_PUMP) break; }
        else if(now-o.lastSent<RESEND_MS) continue;
        sendDatagram(PACKET_RELIABLE,o.datagramPayload.data(),o.datagramPayload.size()); o.sent=true; o.lastSent=now;
    }
}
const char* NetPeerText() { return peerText; }
const char* NetRejectedText() {
    static char text[160];
    if(rejectedPassword) snprintf(text,sizeof(text),"packets from %s rejected: wrong password (both players need the same password)",rejectedFrom);
    else if(rejectedVersion) snprintf(text,sizeof(text),"packets from %s rejected: different HS_Coop version (both need the same HS_Coop zip)",rejectedFrom);
    else text[0]=0;
    return text;
}
bool NetPeerAlive(unsigned timeoutMs) { return peerSession && GetTickCount()-peerLastMs<timeoutMs; }
