#pragma once
#ifndef NUKEE_CRASHREPORT_H
#define NUKEE_CRASHREPORT_H
#include "NukeAPI.h"
#include <string>

namespace nuke {

// Process-wide crash reporter. Install() hooks fatal failures (Windows: unhandled SEH
// exceptions + abort; posix: fatal signals) and writes a bundle into
// <writable>/config/crash/<host>-<boot time>-<pid>/:
//   crash.dmp   Windows minidump — open in VS/WinDbg against the build's PDBs
//   crash.txt   what/where: exception code + faulting module+offset; posix: raw backtrace
//   log.txt     tail of the engine log ring (best effort; not written from posix handlers)
//   info.json   host, boot time, platform
// and points the marker file config/crash/pending at the bundle. The editor shows the
// "last session crashed" viewer when the marker exists; ClearPending() acknowledges it.
class NUKEENGINE_API CrashReport
{
public:
	static void        Install(const char* host);   // idempotent; call FIRST in main
	// For hosts with their OWN crash filters (the editor's symbolized trace): write the bundle
	// from inside that filter instead of replacing it. Windows: pass the EXCEPTION_POINTERS
	// (null for non-SEH failures); posix: pass the signal number — only async-signal-safe work
	// happens there. `what` labels non-exception failures. Requires a prior Install().
	static void        WriteBundle(void* sehPointers, int sig, const char* what);
	static std::string PendingBundle();             // absolute bundle dir, "" when none
	static void        ClearPending();              // acknowledge (removes the marker only)
	static void        PrintBacktrace();            // symbolized stack of the CALLING thread -> stderr
};

}  // namespace nuke

#endif // !NUKEE_CRASHREPORT_H
