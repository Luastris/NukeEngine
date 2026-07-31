#pragma once
#ifndef NUKEE_LOG_H
#define NUKEE_LOG_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nuke {

// Engine-wide logging. Two inputs feed one ring: the typed API below, and CaptureStd() which tees
// std::cout/std::cerr through it, parsing "[tag]" prefixes, severity and "path.ext:line" sources.
// Thread-safe; the ring keeps the most recent kMaxEntries.

enum LogLevel { LOG_INFO = 0, LOG_WARN = 1, LOG_ERROR = 2 };

struct LogEntry
{
	LogLevel    level = LOG_INFO;
	std::string tag;          // subsystem ("Package", "NukeScript", ...); "" when none
	std::string text;         // the message (tag prefix stripped)
	std::string file;         // source location ("" = none): a script, engine .cpp, shader...
	int         line  = 0;
	int         count = 1;    // consecutive identical messages collapse into one entry
	uint64_t    id    = 0;    // monotonic (stable ImGui ids across collapses)
};

class NUKEENGINE_API Log
{
	NUKE_CLASS_NOCREATE(Log, Object)   // scripts log to the Console: Log.Info/Warn/Error(tag, text)
public:
	static constexpr size_t kMaxEntries = 4096;

	static void Write(LogLevel level, const std::string& tag, const std::string& text,
	                  const std::string& file = std::string(), int line = 0);
	[[nuke::func]] static void Info (const std::string& tag, const std::string& text) { Write(LOG_INFO,  tag, text); }
	[[nuke::func]] static void Warn (const std::string& tag, const std::string& text) { Write(LOG_WARN,  tag, text); }
	[[nuke::func]] static void Error(const std::string& tag, const std::string& text) { Write(LOG_ERROR, tag, text); }

	// Route std::cout / std::cerr through the ring. Idempotent; call before modules load so
	// their boot logs are captured too.
	static void CaptureStd();

	// Enable/disable echoing to the OS console (that write is slow). Off still feeds the ring
	// when CaptureStd is active; otherwise the streams go to a null sink. Idempotent.
	static void SetConsoleEcho(bool on);

	static uint64_t Version();                   // bumps on every append (cheap change check)
	static std::vector<LogEntry> Snapshot();     // copy of the ring, oldest first
	static void Clear();
	static void Counts(int& info, int& warn, int& error);   // totals currently in the ring
};

}  // namespace nuke

// Source-carrying convenience macros for engine/host C++ code.
#define NUKE_LOG(tag, text)  ::nuke::Log::Write(::nuke::LOG_INFO,  (tag), (text), __FILE__, __LINE__)
#define NUKE_WARN(tag, text) ::nuke::Log::Write(::nuke::LOG_WARN,  (tag), (text), __FILE__, __LINE__)
#define NUKE_ERR(tag, text)  ::nuke::Log::Write(::nuke::LOG_ERROR, (tag), (text), __FILE__, __LINE__)

#endif // !NUKEE_LOG_H
