#pragma once
// Minimal little-endian message encoding for co-op messages.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

enum MessageType : uint8_t {
    MSG_MISSION_SHIP=1,   // reliable: a mission was accepted; generation variables of its ship
    MSG_GUARDS=2,         // unreliable: authoritative guard snapshot for one ship
    MSG_GUARD_HIT=3,      // reliable: a non-authoritative side hit a guard
    MSG_PLAYER_HIT=4,     // reliable: a guard hit the receiver's player (through its puppet)
    MSG_HELLO=5,          // reliable: build/protocol identity exchanged on connect
    MSG_MISSION_DONE=6,   // reliable: the sender completed a mission; the receiver gets a share of the pay
    MSG_INVITE=7,         // reliable: the sender asks the receiver to come to them
    MSG_RESCUE=8,         // reliable: the sender's pod reached the receiver drifting in space
    MSG_RESET=9,          // reliable: both players go to the given galaxy station (the host's home station)
    MSG_SHOT=10,          // unreliable: the sender fired (tracer direction)
    MSG_SOUND=11,         // unreliable: the sender's gun sound
    MSG_TEST=12,          // reliable: test-menu command for the receiver
    MSG_LOOK=13,          // reliable: the sender's character attachments (head, backpack, ...)
    MSG_OBJECTS=14,       // reliable: shared-ship objects that changed (doors, loot, barrels, windows, ...)
    MSG_EXPLOSION=15,     // reliable: an explosion from an unkeyed source aboard the shared ship
    MSG_GUARD_SOUND=16,   // unreliable: a host guard played a sound
    MSG_BODY=17,          // unreliable: the sender carries this guard; its ship-local position
    MSG_SHIP_MOVE=18,     // unreliable: where the sender's mission ship is (frame-relative)
    MSG_GALAXY_REQUEST=19,// reliable: the CLIENT asks for the HOST's galaxy (fingerprints differ)
    MSG_GALAXY_FILE=20,   // reliable: one file of the HOST's galaxy
    MSG_GUARD_REQUEST=21, // reliable: the follower lacks this guard (it joined the ship after generation)
    MSG_GUARD_SPAWN=22,   // reliable: the host's answer: the guard's position and variables
    MSG_DOORS=23,         // unreliable: doors the sender holds open for its own reasons
    MSG_ITEM_DROP=24,     // reliable: the sender's player dropped an item on the shared ship
    MSG_SENTRY_FIRE=25,   // unreliable: a host sentry fired
    MSG_SENTRY_STATE=26,  // unreliable, 10 Hz: the owner's sentry guns (ship name, then per sentry: key, state, direction index, angle, timers)
    MSG_KEYCARD=27,       // reliable: the sender got a keycard (ship name, level)
    MSG_GOTO_MISSION=28,  // reliable: both players go to this mission ship with their pods
    MSG_BULLET=29,        // reliable: a bullet the sender's player or host guard fired (variables, offset from the shooter)
    MSG_SHIP_GONE=32,     // reliable: a ship the sender created in the receiver's game is gone (culled/destroyed)
    MSG_GUARD_ALERT=33,   // reliable: a noise/alert reached the follower's copy of a guard (distraction, responding)
    MSG_MISSION_EVENT=34, // reliable: a mission counter script ran for a guard (kills, knockouts, witnesses, alarms)
    MSG_CRATE_MENU=35,    // unreliable: the sender's crate choice menu (active, found item, row labels, suggested row)
    MSG_CRATE_SUGGEST=36, // reliable: the row the sender suggests in the partner's crate menu
    MSG_CRATE_ITEM=37,    // reliable: the item in the sender's crate menu (object name + snapshot), for its details
    MSG_MODULE_GONE=38,   // reliable: a module of a ship the sender flies was destroyed (generation ship + position, cause)
    MSG_SHIP_RENAME=40,
    MSG_MISSION_FAILED=41, // reliable: the sender's mission failed (who, type, its ship)
    MSG_SLOWMO=43,
    MSG_GUARD_KIT=44,     // reliable: the sender's game gave a guard a gun (ship name, guard key, rarity, ammo, selected slot)        // unreliable: a guard's slow-mo request (SetTimeScale args) was about the receiver's player
    MSG_KEY_LEVEL=45,     // unreliable, 2 Hz: the sender's key level on the shared ship (ship name, level): keys are party-wide
    MSG_LOOT_LIST=46,     // reliable: the ship owner's floor loot (ship name, keys): the copy drops loot that is not on it
    MSG_EFFECT=47,        // unreliable: an effect script the sender's game ran (script index, arguments; ship and instances by key)
    MSG_GUARDS_TAKE_OVER=56, // reliable: the sender's game ran the shared ship's guards but rebuilt the ship (practice restarted); the receiver runs them now (ship name)
    MSG_OBJECTS_RESYNC=55, // reliable: the sender's copy of the shared ship is new (e.g. practice restarted); the guards' game sends its full state again (ship name)
    MSG_SCOOP_SOUND=54,   // reliable: the sender was caught by a pod after the partner's rescue; a sound of that scoop-up, played by the rescuer too
    MSG_SENTRY_ACTION=53, // reliable: the sender's player switched a sentry off or subverted it (ship name, key, Off, Hacked); the guards' game applies it
    MSG_REINFORCEMENT=52, // reliable: the guards' game brought in a reinforcement (ship name, CreateMercenaryOnShip arguments; the ship as '$')
    MSG_ALARM_REQUEST=48, // reliable: the sender's game wants the ship's alarm raised (ship name); the guards' authority decides
    MSG_REPORT_REQUEST=50, // reliable: the sender saved a bug report (its id); the receiver saves one too and sends its log
    MSG_REPORT_LOG=51,    // reliable: the end of the sender's session log for a bug report (id length, id, log text)
    MSG_SHIP_ARRIVED=49,  // reliable: the sender's ship docked at a station and is consumed there (ship name, station name)
    MSG_TARGET_DELIVERED=42, // reliable: a person (guard key) is now in the sender's pod (capture/rescue delivery)   // reliable: the sender's new ship got its final name during generation (name before, after)
    MSG_SHIP_PIECE=39,    // reliable: a ship the sender flies split; a new piece's name and one of its modules
    MSG_TELEPORT_FX=31,   // reliable: the sender's gadget played a teleport effect (glitch, swap); arguments mapped
    MSG_GUARD_ANIM=30,    // unreliable: a host guard played a one-shot torso animation (melee swing, punch)
};

struct Writer {
    std::vector<uint8_t> bytes;
    template<class T> void put(T v){ size_t n=bytes.size(); bytes.resize(n+sizeof(T)); memcpy(bytes.data()+n,&v,sizeof(T)); }
    void str(const std::string& s){ put<uint16_t>((uint16_t)s.size()); bytes.insert(bytes.end(),s.begin(),s.end()); }
};
struct Reader {
    const uint8_t* p; size_t left; bool ok=true;
    Reader(const uint8_t* d,size_t n):p(d),left(n){}
    template<class T> T get(){ T v{}; if(left<sizeof(T)){ ok=false; left=0; return v; } memcpy(&v,p,sizeof(T)); p+=sizeof(T); left-=sizeof(T); return v; }
    std::string str(){ uint16_t n=get<uint16_t>(); if(left<n){ ok=false; left=0; return {}; } std::string s((const char*)p,n); p+=n; left-=n; return s; }
};
