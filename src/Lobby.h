#pragma once
// Lobby codes: joining a partner without editing coop.ini.
//
// One player hosts from the Escape menu and gets a code; the other enters the code in the same menu. The code carries
// the host's addresses (the internet address its router reports, and its address on the local network), the port and a
// random session key (the shared secret that authenticates every packet). No server is involved: the two games talk
// directly, as with coop.ini. For players on different networks the host's router must let the connection in; the mod
// asks the router to open the port itself (UPnP), which many home routers allow. Where it does not, the code still
// works on the same network or over a VPN, or after forwarding the port by hand (the panel says which).
#include "ModInterface.h"
#include <cstddef>

void LobbyInit(const HS_ModApi* api, const char* baseDir);
// Console (developers) and menu actions. On failure `error` says why.
// Hosts through a Steam lobby when the game runs under Steam, else by IP address.
bool LobbyHost(char* error, size_t errorSize);
// Hosts by IP address (the fallback when Steam does not connect the two players).
bool LobbyHostByIp(char* error, size_t errorSize);
bool LobbyJoin(const char* code, char* error, size_t errorSize);
void LobbyLeave();
// "" until the host's code is ready.
const char* LobbyCode();
// 0 not used, 1 hosting (preparing the code), 2 hosting (code ready), 3 joined with a code.
int LobbyState();
// Every frame: finishes hosting once the router has answered. Keys and drawing of the Escape-menu panel.
void LobbyTick();
// Every frame while the player is in the game: an invite accepted in Steam (or the game started by Steam for one) joins
// that lobby without a code.
void LobbyInviteTick();
void LobbyDraw(CInstance* self, bool menuOpen, bool connected, const char* partnerName, double guiHeight);
// True once after a successful join with a code: the joiner is then brought to the host's station.
bool LobbyTakeJoinRequest();
