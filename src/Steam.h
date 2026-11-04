#pragma once
// Steam lobbies and Steam's peer-to-peer packets as the connection between the two players: no IP addresses, no router
// settings (Steam relays when a direct path cannot be made). Uses the game's own steam_api.dll, which the game has
// already started; nothing here starts or stops Steam.
//
// The host creates a lobby (two members at most); its id is the lobby code. The partner joins the lobby by id. The
// session key (the shared secret that authenticates every packet, as with coop.ini's password) is random and kept as
// lobby data, which only members can read. Packets then travel with ISteamNetworking between the two accounts.
#include <cstdint>

// The game's Steam library is loaded and the player is logged on.
bool SteamAvailable();
// 0 nothing, 1 creating a lobby, 2 hosting one, 3 joining one, 4 joined one, -1 the last attempt failed (SteamError).
int SteamLobbyState();
const char* SteamError();
bool SteamHostLobby();
bool SteamJoinLobby(uint64_t lobby);
void SteamLeaveLobby();
uint64_t SteamLobbyId();
// The session's shared secret ("" until known: the joiner reads it from the lobby once inside).
const char* SteamSessionKey();
// The other member's name ("" until they are in the lobby).
const char* SteamPartnerName();
// Steam invites. Every frame: returns once the id of a lobby this player was asked to join by Steam itself - an
// accepted "Invite to game", "Join Game" on a friend, or the game started by Steam for one of those (+connect_lobby on
// the command line) - else 0.
uint64_t SteamTakeInvite();
// (Developers) the same as an invite accepted in Steam.
void SteamSimulateInvite(uint64_t lobby);
// Every frame while a lobby is in use.
void SteamTick();
// Packets to and from the other member. Receive returns the size, or -1 when nothing waits.
bool SteamSend(const void* data, uint32_t size);
int SteamReceive(void* buffer, uint32_t capacity);
