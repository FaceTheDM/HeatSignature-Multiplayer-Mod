#pragma once
// Development console: reads coop-command.txt (one command per line) on the game thread.
//   capture | play | quit
//   get ID VAR [INDEX]          set ID VAR VALUE [INDEX]
//   call SCRIPT SELF ARGS...    eng BUILTIN SELF ARGS...
//   list OBJECT                 (instance ids of an object index)
// ID/SELF: number, '@' = local player, '!' = remote puppet, '$' = player's Parent.
// ARGS: number, @/!/$ as above, or "text" for a string.
#include "ModInterface.h"

struct CoopContext {
    const HS_ModApi* api;
    int player;
    int puppet;
    // Dev-only scripted walk of the local player (console `walk RADIUS SECONDS`), so tests move smoothly instead of teleporting.
    double walkRadius, walkSeconds, walkStartMs, walkFore, walkStarboard;
    // Dev-only scripted walk to a point (console `goto FORE STARBOARD [SPEED]`): moved per frame, never teleported (guards
    // that see a player teleport panic instead of fighting).
    bool gotoActive; double gotoFore, gotoStarboard, gotoSpeed; DWORD gotoLastMs; double gotoBest; DWORD gotoBestMs;
};
void ConsoleSubscribeTraces(const HS_ModApi* api, const char* path);
void ConsoleRun(CoopContext& ctx, CInstance* hookSelf, const char* path);
RValue Engine(const HS_ModApi* api, const char* name, CInstance* self, int argc, RValue** argv);
double Number(const RValue& v);
const RValue* GlobalVar(int id);
enum { GLOBAL_PLAYER=5 };
// Log lines go to ModLoader.log and coop-logs, which players attach to bug reports. Unless debug=1 is set, every line
// is masked first: lobby codes (see CoopLogHide) and IP addresses (loopback stays, private networks keep their first two
// parts, anything else becomes "<public-ip>").
void CoopLog(const char* fmt, ...);
void CoopLogHide(const char* secret);   // a lobby code this game made or was given: never written in clear
// log=verbose in coop.ini: detailed per-event logging (default: state changes and errors only).
bool CoopVerbose();
