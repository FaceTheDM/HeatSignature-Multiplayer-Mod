#pragma once
// Nonblocking UDP transport with HMAC-SHA256 authentication. The password never travels on the wire.
// Carries: the 30 Hz player state, unreliable snapshots, and reliable ordered messages (fragmented, acknowledged).
#include <winsock2.h>
#include <cstdint>
#include <cstddef>

#pragma pack(push,1)
struct PlayerState {
    double fore, starboard, imageAngle, foreSpeed, starboardSpeed, measuredSpeed;
    int32_t parentId, parentObject;
    uint8_t inGameplay;
    char name[40];
    char parentName[48];     // stable identity; instance ids are process-local
    double timeScaleWanted;  // the local game's own desired time scale (shared time policy)
    double parentX, parentY; // parent's position relative to the shared frame, to tell same-named ships apart
    uint8_t down;            // unconscious or dead
    uint32_t galaxy;         // galaxy fingerprint (0 = not known yet)
    uint32_t build;          // plugin build id; both players should run the same build
    // Presence (anywhere in the galaxy), positions relative to the shared frame:
    double worldX, worldY;   // the player (or their pod while piloting it)
    double podX, podY;       // the player's own pod
    double podAngle;         // its heading (MyImageAngle)
    uint8_t mode;            // 0 aboard a ship/station, 1 piloting their pod, 2 drifting in space
    float danger;            // seconds left: oxygen while drifting, bleed-out while bleeding; <0 = not in danger
    int32_t station;         // galaxy station (oStation) id when aboard a station ship, else 0; ids match across games
    int32_t homeStation;     // the player's home galaxy station (global HomeStation)
    char skin[40], head[40]; // appearance: spine skin and head attachment set
    uint8_t female;
    float seenAsThreat;      // the player's own SeenAsThreat (guards ignore players who are not a threat)
    char state[24];          // the player's state name (Free, Controlling, ...)
    double podVx, podVy;     // pod velocity (units per second) for extrapolation
    // Galaxy stations by name: instance ids of galaxy objects are not guaranteed to match between games.
    char stationName[48], homeStationName[48];
    // The pod while docked: the ship it is docked to, its ship-local position and heading, and the airlock used.
    uint8_t podDocked;
    char podShipName[48];
    double podFore, podStarboard, podRelAngle, airlockFore, airlockStarboard;
    // What the partner sees in your hands and how your torso moves.
    char torsoAnim[32], oneShotAnim[32];
    uint16_t oneShotSeq;
    char weaponSlot[24], weaponName[40];
    char missionShip[48];    // the ship of the sender's active mission ("" when they have none)
    float slipstream;        // seconds of slipstream left on the sender's pod (0 = not slipstreaming)
    uint8_t paused;          // the sender has the game's pause menu open (both games then stand still)
    uint8_t riding;
    char practice[32];       // the practice scenario the sender is in ("" when not practising)
    char practiceShip[48];   // and its practice ship's name (names are drawn differently in each game)          // the sender rides in the receiver's pod (caught drifting): its own pod is not its own to fly
};
#pragma pack(pop)

#ifndef HS_COOP_PACKET_VERSION
#define HS_COOP_PACKET_VERSION 19   // 19: automatic updates removed (type 5 is no longer used)
#endif
enum { PACKET_MAGIC=0x31435348, PACKET_VERSION=HS_COOP_PACKET_VERSION };
enum { PACKET_STATE=1, PACKET_RELIABLE=2, PACKET_ACK=3, PACKET_UNRELIABLE=4 };   // 5 was automatic updates (removed)

struct NetConfig {
    bool host;
    char bind[64];
    char peer[64];
    char password[128];
    bool sharedTime;   // time=shared (default) or time=independent
    bool debug;        // debug=1: F10 test menu, developer console file, partner test commands, traces (developers only)
    bool verbose;      // log=verbose: detailed per-event logging
    int devPort;       // dev_port=N (developer builds with debug=1 only): localhost developer channel
};

bool NetLoadConfig(const char* path, NetConfig& out, char* error, size_t errorSize);
bool NetOpen(const NetConfig& config);
// The same protocol over another carrier than the UDP socket (Steam's peer-to-peer packets): `send` delivers one
// datagram to the partner, `receive` returns the size of the next one or -1. Uses config.host and config.password.
bool NetOpenTransport(const NetConfig& config, bool (*send)(const void*,uint32_t), int (*receive)(void*,uint32_t), const char* name);
// Stops networking (the socket, the key, everything known about the partner); NetOpen may be called again afterwards.
void NetClose();
// CLIENT: a second address of the same host (its address on the local network), tried alongside `peer` until one of
// them answers.
void NetSetAltPeer(const char* address);
bool NetIsHost();
void NetSendState(const PlayerState& state);
// Reliable, ordered delivery (fragmented); resent until acknowledged.
void NetSendReliable(uint8_t type, const void* data, size_t size);
// Single datagram, may be lost; at most ~1200 bytes.
void NetSendUnreliable(uint8_t type, const void* data, size_t size);
struct NetHandlers {
    void (*state)(const PlayerState&);
    void (*message)(uint8_t type, const uint8_t* data, size_t size, bool reliable);
    void (*sessionReset)();   // peer restarted; reliable state was reset
};
// Call every frame: drains the socket, acknowledges and retransmits.
void NetPump(const NetHandlers& handlers);
const char* NetPeerText();
bool NetPeerAlive(unsigned timeoutMs);
// Why the partner's packets are being refused ("" if they are not).
const char* NetRejectedText();
