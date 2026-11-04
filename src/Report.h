#pragma once
// Session logs and bug reports, so a problem found while playing with a friend can be looked at afterwards.
//
// Every co-op log line is also written to coop-logs\session-<date>-<time>.log in the game folder; the last
// KEEP_SESSIONS sessions are kept (the loader's ModLoader.log is overwritten on every launch, which lost a friend's crash
// log). Lines are flushed as they are written, so a log survives the game stopping on a fatal error.
//
// A bug report (co-op menu, F8 then 8) saves into coop-reports\<date>-<time>\: this game's session log, a screenshot
// and the current state, and asks the partner's game for its session log, which arrives as partner-log.txt. The
// partner's game saves the same report on its side.
#include "ModInterface.h"
#include <cstddef>
#include <cstdint>

void ReportInit(const HS_ModApi* api, const char* baseDir);
void ReportLogLine(const char* line);
// The session recorder: one "REC" line a second in the session log only (both players' places and states, the shared
// ship and its alarm), so a play session can be stepped through afterwards (with the private developer tools).
void ReportRecord(const char* line);
// Starts a bug report (the menu, or the console's "report"). `note` is shown in the report's state file.
void ReportSave(const char* note);
// Every frame (needs an instance for the screenshot).
void ReportTick(CInstance* self);
void ReportOnMessage(uint8_t type, const uint8_t* data, size_t size);
// The folder of the last report, "" if none (for the on-screen notice).
const char* ReportLastFolder();
