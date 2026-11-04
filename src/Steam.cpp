// Steam lobbies and peer-to-peer packets: see Steam.h.
// The game ships a 2017 Steam library (SteamClient017): its flat C exports are used, found by name at run time, so the
// mod links against nothing and simply reports Steam as unavailable when an export is missing.
#include "Steam.h"
#include "Console.h"
#include <windows.h>
#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#pragma pack(push,8)
struct LobbyCreated { int result; uint64_t lobby; };                                   // callback 513
struct LobbyEnter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };   // callback 504
struct SessionRequest { uint64_t remote; };                                            // callback 1202
struct SessionFail { uint64_t remote; uint8_t error; };                                // callback 1203
struct JoinRequested { uint64_t lobby; uint64_t fromFriend; };                         // callback 333 (GameLobbyJoinRequested)
#pragma pack(pop)

// A callback object as Steam's library expects it (the SDK's CCallbackBase as Microsoft's compiler lays it out: the
// two Run overloads in reverse order, then the size). Checked with a standalone probe before use here.
struct Callback {
    virtual void __thiscall RunResult(void* data,bool ioFailure,uint64_t call) { if(result) result(data,ioFailure); }
    virtual void __thiscall Run(void* data) { if(plain) plain(data); }
    virtual int __thiscall Size() { return size; }
    uint8_t flags=0; int id=0;
    int size=0; void (*result)(void*,bool)=nullptr; void (*plain)(void*)=nullptr;
};

static HMODULE dll; static bool looked, usable;
static intptr_t matchmaking, networking, user, friends;
template<class T> static bool get(T& f,const char* name){ f=(T)GetProcAddress(dll,name); return f!=nullptr; }
static intptr_t (__cdecl* Client)();
static int (__cdecl* HUser)(); static int (__cdecl* HPipe)();
static void (__cdecl* RunCallbacks)();
static intptr_t (__cdecl* GetMatchmaking)(intptr_t,int,int,const char*);
static intptr_t (__cdecl* GetNetworking)(intptr_t,int,int,const char*);
static intptr_t (__cdecl* GetUser)(intptr_t,int,int,const char*);
static intptr_t (__cdecl* GetFriends)(intptr_t,int,int,const char*);
static bool (__cdecl* LoggedOn)(intptr_t);
static uint64_t (__cdecl* MySteamId)(intptr_t);
static uint64_t (__cdecl* CreateLobby)(intptr_t,int,int);
static uint64_t (__cdecl* JoinLobby)(intptr_t,uint64_t);
static void (__cdecl* LeaveLobby)(intptr_t,uint64_t);
static bool (__cdecl* SetLobbyData)(intptr_t,uint64_t,const char*,const char*);
static const char* (__cdecl* GetLobbyData)(intptr_t,uint64_t,const char*);
static int (__cdecl* NumMembers)(intptr_t,uint64_t);
static uint64_t (__cdecl* MemberAt)(intptr_t,uint64_t,int);
static uint64_t (__cdecl* LobbyOwner)(intptr_t,uint64_t);
static const char* (__cdecl* FriendName)(intptr_t,uint64_t);
static bool (__cdecl* SendPacket)(intptr_t,uint64_t,const void*,uint32_t,int,int);
static bool (__cdecl* PacketAvailable)(intptr_t,uint32_t*,int);
static bool (__cdecl* ReadPacket)(intptr_t,void*,uint32_t,uint32_t*,uint64_t*,int);
static bool (__cdecl* AcceptSession)(intptr_t,uint64_t);
static bool (__cdecl* CloseSession)(intptr_t,uint64_t);
static bool (__cdecl* AllowRelay)(intptr_t,bool);
static void (__cdecl* RegisterCallResult)(void*,uint64_t);
static void (__cdecl* UnregisterCallResult)(void*,uint64_t);
static void (__cdecl* RegisterCallback)(void*,int);
static void (__cdecl* UnregisterCallback)(void*);

static int state; static std::string error, key, partnerName;
static uint64_t lobby, partner, me, pendingCall;
static Callback onCreated, onEntered, onSessionRequest, onSessionFail;
static bool callbacksRegistered;
static const int CHANNEL=0;
// Developer test with ONE Steam account (two copies of the game logged in as the same user): the "partner" is this
// same account. Steam delivers a packet sent to one's own account back to the account's processes, which exercises the
// lobby, the key exchange and the packet path; a real session is two different accounts.
static bool selfTest() { static int v=-1; if(v<0) v=getenv("HS_COOP_STEAM_SELF")?1:0; return v==1; }

static bool load() {
    if(looked) return usable;
    // Asked before the game has started Steam (early in its start-up): not final, asked again later.
    static DWORD lastTry; DWORD t=GetTickCount(); if(lastTry && t-lastTry<2000) return false; lastTry=t?t:1;
    dll=GetModuleHandleA("steam_api.dll"); if(!dll) return false;
    { intptr_t (__cdecl* c)()=nullptr; if(get(c,"SteamClient") && !c()) return false; }
    looked=true;
    bool ok=get(Client,"SteamClient") && get(HUser,"SteamAPI_GetHSteamUser") && get(HPipe,"SteamAPI_GetHSteamPipe") && get(RunCallbacks,"SteamAPI_RunCallbacks")
        && get(GetMatchmaking,"SteamAPI_ISteamClient_GetISteamMatchmaking") && get(GetNetworking,"SteamAPI_ISteamClient_GetISteamNetworking")
        && get(GetUser,"SteamAPI_ISteamClient_GetISteamUser") && get(GetFriends,"SteamAPI_ISteamClient_GetISteamFriends")
        && get(LoggedOn,"SteamAPI_ISteamUser_BLoggedOn") && get(MySteamId,"SteamAPI_ISteamUser_GetSteamID")
        && get(CreateLobby,"SteamAPI_ISteamMatchmaking_CreateLobby") && get(JoinLobby,"SteamAPI_ISteamMatchmaking_JoinLobby") && get(LeaveLobby,"SteamAPI_ISteamMatchmaking_LeaveLobby")
        && get(SetLobbyData,"SteamAPI_ISteamMatchmaking_SetLobbyData") && get(GetLobbyData,"SteamAPI_ISteamMatchmaking_GetLobbyData")
        && get(NumMembers,"SteamAPI_ISteamMatchmaking_GetNumLobbyMembers") && get(MemberAt,"SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex") && get(LobbyOwner,"SteamAPI_ISteamMatchmaking_GetLobbyOwner")
        && get(FriendName,"SteamAPI_ISteamFriends_GetFriendPersonaName")
        && get(SendPacket,"SteamAPI_ISteamNetworking_SendP2PPacket") && get(PacketAvailable,"SteamAPI_ISteamNetworking_IsP2PPacketAvailable") && get(ReadPacket,"SteamAPI_ISteamNetworking_ReadP2PPacket")
        && get(AcceptSession,"SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser") && get(CloseSession,"SteamAPI_ISteamNetworking_CloseP2PSessionWithUser") && get(AllowRelay,"SteamAPI_ISteamNetworking_AllowP2PPacketRelay")
        && get(RegisterCallResult,"SteamAPI_RegisterCallResult") && get(UnregisterCallResult,"SteamAPI_UnregisterCallResult")
        && get(RegisterCallback,"SteamAPI_RegisterCallback") && get(UnregisterCallback,"SteamAPI_UnregisterCallback");
    if(!ok) { CoopLog("Steam: this steam_api.dll lacks a function the lobby needs; Steam lobbies are off"); return false; }
    intptr_t client=Client(); if(!client) { CoopLog("Steam: not started by the game; Steam lobbies are off"); return false; }
    int hu=HUser(), hp=HPipe();
    matchmaking=GetMatchmaking(client,hu,hp,"SteamMatchMaking009"); networking=GetNetworking(client,hu,hp,"SteamNetworking005");
    user=GetUser(client,hu,hp,"SteamUser019"); friends=GetFriends(client,hu,hp,"SteamFriends015");
    if(!matchmaking || !networking || !user) { CoopLog("Steam: an interface is missing (matchmaking %d, networking %d, user %d); Steam lobbies are off",matchmaking!=0,networking!=0,user!=0); return false; }
    usable=true;
    return true;
}
bool SteamAvailable() { return load() && LoggedOn(user); }

// ---- Callbacks (run by Steam's dispatch, on the game's thread) ----
static void fail(const char* why) { error=why; state=-1; CoopLog("Steam: %s",why); }
static void created(void* data,bool ioFailure) {
    const LobbyCreated* c=(const LobbyCreated*)data; pendingCall=0;
    if(state!=1) return;
    if(ioFailure || c->result!=1 || !c->lobby) { char b[96]; snprintf(b,sizeof(b),"Steam did not create the lobby (result %d)",ioFailure?-1:c->result); return fail(b); }
    lobby=c->lobby;
    uint8_t raw[6]; BCryptGenRandom(nullptr,raw,sizeof(raw),BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    char hex[40]; snprintf(hex,sizeof(hex),"hs-steam-%02x%02x%02x%02x%02x%02x",raw[0],raw[1],raw[2],raw[3],raw[4],raw[5]);
    key=hex; SetLobbyData(matchmaking,lobby,"hs_key",key.c_str()); SetLobbyData(matchmaking,lobby,"hs_coop","1");
    if(selfTest()) partner=me;
    state=2; CoopLog("Steam: lobby created; waiting for the partner");
}
static void entered(void* data,bool ioFailure) {
    const LobbyEnter* e=(const LobbyEnter*)data; pendingCall=0;
    if(state!=3) return;
    if(ioFailure || e->response!=1) { char b[120]; snprintf(b,sizeof(b),"Steam would not let you into the lobby (%s)",ioFailure?"no answer":e->response==2?"it no longer exists":e->response==4?"it is full":"refused"); return fail(b); }
    lobby=e->lobby; partner=LobbyOwner(matchmaking,lobby);
    state=4; CoopLog("Steam: in the lobby");
}
static bool isMember(uint64_t id) {
    if(!lobby) return false;
    int n=NumMembers(matchmaking,lobby); for(int i=0;i<n;i++) if(MemberAt(matchmaking,lobby,i)==id) return true;
    return false;
}
// Steam asks before the first packet from someone is delivered: only the other member of our lobby is accepted.
static void sessionRequest(void* data) { const SessionRequest* r=(const SessionRequest*)data; if(r->remote==partner || isMember(r->remote)) AcceptSession(networking,r->remote); }
static void sessionFail(void* data) { const SessionFail* f=(const SessionFail*)data; static DWORD last; if(GetTickCount()-last>5000) { last=GetTickCount(); CoopLog("Steam: no connection to the partner yet (error %d); retrying",(int)f->error); } }
// An invite accepted in Steam while the game runs.
static Callback onJoinRequested; static uint64_t invitedLobby; static bool inviteReady;
static void joinRequested(void* data) { const JoinRequested* j=(const JoinRequested*)data; invitedLobby=j->lobby; CoopLog("Steam: invite accepted; joining that lobby"); }
void SteamSimulateInvite(uint64_t id) { invitedLobby=id; }
uint64_t SteamTakeInvite() {
    if(!inviteReady) {
        if(!SteamAvailable()) return 0;
        inviteReady=true;
        onJoinRequested.size=sizeof(JoinRequested); onJoinRequested.plain=joinRequested; RegisterCallback(&onJoinRequested,333);
        // Started by Steam for an invite: "+connect_lobby <id>" on the command line.
        const char* cl=GetCommandLineA(); const char* at=cl?strstr(cl,"+connect_lobby"):nullptr;
        if(at) { uint64_t id=_strtoui64(at+14,nullptr,10); if(id) { invitedLobby=id; CoopLog("Steam: started for an invite; joining that lobby"); } }
    }
    // The game pumps Steam itself; this does not depend on it (ten times a second is plenty for an invite).
    static DWORD lastPump; DWORD now=GetTickCount(); if(!state && now-lastPump>100) { lastPump=now; RunCallbacks(); }
    uint64_t id=invitedLobby; invitedLobby=0; return id;
}
static void registerCallbacks() {
    if(callbacksRegistered) return;
    onCreated.size=sizeof(LobbyCreated); onCreated.id=513; onCreated.result=created;
    onEntered.size=sizeof(LobbyEnter); onEntered.id=504; onEntered.result=entered;
    onSessionRequest.size=sizeof(SessionRequest); onSessionRequest.plain=sessionRequest; RegisterCallback(&onSessionRequest,1202);
    onSessionFail.size=sizeof(SessionFail); onSessionFail.plain=sessionFail; RegisterCallback(&onSessionFail,1203);
    callbacksRegistered=true;
}

// ---- Lobbies ----
int SteamLobbyState() { return state; }
const char* SteamError() { return error.c_str(); }
uint64_t SteamLobbyId() { return lobby; }
const char* SteamSessionKey() { return key.c_str(); }
const char* SteamPartnerName() { return partnerName.c_str(); }
static void reset() {
    if(pendingCall) { UnregisterCallResult(state==1?(void*)&onCreated:(void*)&onEntered,pendingCall); pendingCall=0; }
    if(partner && networking) CloseSession(networking,partner);
    if(lobby && matchmaking) LeaveLobby(matchmaking,lobby);
    lobby=partner=0; key.clear(); partnerName.clear(); state=0;
}
bool SteamHostLobby() {
    if(!SteamAvailable()) { error="Steam is not available"; return false; }
    reset(); registerCallbacks(); me=MySteamId(user); AllowRelay(networking,true);
    const int PUBLIC=2;   // joinable by anyone who has its id; the session key in the lobby still gates every packet
    pendingCall=CreateLobby(matchmaking,PUBLIC,2);
    if(!pendingCall) { error="Steam refused to create a lobby"; return false; }
    RegisterCallResult(&onCreated,pendingCall); state=1; error.clear();
    return true;
}
bool SteamJoinLobby(uint64_t id) {
    if(!SteamAvailable()) { error="Steam is not available"; return false; }
    reset(); registerCallbacks(); me=MySteamId(user); AllowRelay(networking,true);
    pendingCall=JoinLobby(matchmaking,id);
    if(!pendingCall) { error="Steam refused the join request"; return false; }
    RegisterCallResult(&onEntered,pendingCall); state=3; error.clear();
    return true;
}
void SteamLeaveLobby() { if(usable && state) { reset(); CoopLog("Steam: left the lobby"); } state=0; }
void SteamTick() {
    if(!usable || !state || state==-1) return;
    // The game pumps Steam itself; pumping here too costs nothing and does not depend on how often it does.
    RunCallbacks();
    static DWORD lastPoll; DWORD now=GetTickCount(); if(now-lastPoll<300) return; lastPoll=now;
    if(state==2 || state==4) {
        // Who else is in the lobby (the host learns of the partner this way), and the key for the joiner.
        int n=NumMembers(matchmaking,lobby); uint64_t other=0;
        for(int i=0;i<n;i++) { uint64_t m=MemberAt(matchmaking,lobby,i); if(m && m!=me) other=m; }
        if(state==2 && other!=partner && !selfTest()) {
            if(partner) CloseSession(networking,partner);
            partner=other; partnerName=other&&friends&&FriendName?FriendName(friends,other):"";
            CoopLog("Steam: %s",other?"the partner is in the lobby":"the partner left the lobby");
        }
        if(state==4 && key.empty()) { const char* k=GetLobbyData(matchmaking,lobby,"hs_key"); if(k && *k) { key=k; CoopLog("Steam: session key received"); } }
        if(state==4 && partnerName.empty() && partner && friends && FriendName) partnerName=FriendName(friends,partner);
    }
}

// ---- Packets ----
bool SteamSend(const void* data,uint32_t size) {
    if(!usable || !partner) return false;
    // Unreliable packets carry up to 1200 bytes; anything larger goes as one reliable Steam message.
    return SendPacket(networking,partner,data,size,size<=1200?0:2,CHANNEL);
}
int SteamReceive(void* buffer,uint32_t capacity) {
    if(!usable || (state!=2 && state!=4)) return -1;
    uint32_t size=0;
    while(PacketAvailable(networking,&size,CHANNEL)) {
        uint32_t read=0; uint64_t from=0;
        if(!ReadPacket(networking,buffer,capacity,&read,&from,CHANNEL)) return -1;
        if(partner && from==partner) return (int)read;   // anyone else's packets are dropped
    }
    return -1;
}
