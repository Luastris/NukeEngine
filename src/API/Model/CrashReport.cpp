#include "API/Model/CrashReport.h"
#include "API/Model/Log.h"
#include "config.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <windows.h>
  #include <dbghelp.h>
  #include <csignal>
  #pragma comment(lib, "dbghelp.lib")
#else
  #include <csignal>
  #include <execinfo.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace bfs = boost::filesystem;
using namespace nuke;

namespace {

// Everything a handler needs is PRECOMPUTED at Install: a crashed process (and especially a
// posix signal handler) must not format strings or touch the heap to find out where to write.
bool        g_installed = false;
std::string g_bundleDir;      // <writable>/config/crash/<host>-<time>-<pid>
std::string g_crashTxt;       //   bundleDir/crash.txt
std::string g_infoJson;       //   bundleDir/info.json
std::string g_logTxt;         //   bundleDir/log.txt
std::string g_marker;         // <writable>/config/crash/pending  (contents = bundleDir)
std::string g_infoContent;    // pre-baked info.json body
#ifdef _WIN32
std::wstring g_bundleDirW, g_dumpW;
#endif

// Write the log-ring tail with plain stdio (the bundle dir already exists). Best effort:
// the ring lock/allocations may be poisoned in a crashed process — callers guard the call.
void WriteLogTail()
{
	std::FILE* f = std::fopen(g_logTxt.c_str(), "wb");
	if (!f) return;
	std::vector<LogEntry> ring = Log::Snapshot();
	const size_t from = ring.size() > 400 ? ring.size() - 400 : 0;
	for (size_t i = from; i < ring.size(); ++i)
	{
		const LogEntry& e = ring[i];
		std::fprintf(f, "%s [%s] %s\n", e.level == LOG_ERROR ? "E" : e.level == LOG_WARN ? "W" : "I",
		             e.tag.c_str(), e.text.c_str());
	}
	std::fclose(f);
}

void WriteInfoAndMarker()
{
	if (std::FILE* f = std::fopen(g_infoJson.c_str(), "wb"))
		{ std::fwrite(g_infoContent.data(), 1, g_infoContent.size(), f); std::fclose(f); }
	if (std::FILE* f = std::fopen(g_marker.c_str(), "wb"))
		{ std::fwrite(g_bundleDir.data(), 1, g_bundleDir.size(), f); std::fclose(f); }
}

#ifdef _WIN32

// Symbolized stack into the report: dbghelp walk over the given context (the FAULTING one
// for SEH, the current thread for abort/assert — the handler runs on the aborting thread).
// Best effort: dbghelp may be unusable in a crashed process; the minidump (written first)
// stays the authoritative record. POD locals only — the caller wraps this in __try.
void WriteStackTrace(std::FILE* f, CONTEXT* ctx, HANDLE walkThread = nullptr)
{
#if defined(_M_X64)
	HANDLE proc = GetCurrentProcess(), thread = walkThread ? walkThread : GetCurrentThread();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(proc, nullptr, TRUE);
	CONTEXT local;
	if (!ctx) { RtlCaptureContext(&local); ctx = &local; }
	CONTEXT walk = *ctx;   // StackWalk64 mutates the context
	STACKFRAME64 sf{};
	sf.AddrPC.Offset = walk.Rip; sf.AddrFrame.Offset = walk.Rbp; sf.AddrStack.Offset = walk.Rsp;
	sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
	std::fprintf(f, "stack:\n");
	for (int i = 0; i < 64; ++i)
	{
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &sf, &walk, nullptr,
		                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
		if (!sf.AddrPC.Offset) break;
		char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
		SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
		sym->SizeOfStruct = sizeof(SYMBOL_INFO); sym->MaxNameLen = 255;
		DWORD64 disp = 0;
		const char* name = SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym) ? sym->Name : "?";
		char modName[MAX_PATH]; modName[0] = '?'; modName[1] = 0;
		HMODULE mod = nullptr;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                       (LPCSTR)sf.AddrPC.Offset, &mod) && mod)
		{
			GetModuleFileNameA(mod, modName, MAX_PATH);
			const char* slash = std::strrchr(modName, '\\');
			if (slash) std::memmove(modName, slash + 1, std::strlen(slash + 1) + 1);
		}
		IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(line);
		DWORD ld = 0;
		if (SymGetLineFromAddr64(proc, sf.AddrPC.Offset, &ld, &line))
			std::fprintf(f, "#%02d %s!%s+0x%llX  (%s:%lu)\n", i, modName, name,
			             (unsigned long long)disp, line.FileName, (unsigned long)line.LineNumber);
		else
			std::fprintf(f, "#%02d %s!%s+0x%llX\n", i, modName, name, (unsigned long long)disp);
	}
#else
	(void)ctx;
	std::fprintf(f, "(no stack walker for this architecture — see crash.dmp)\n");
#endif
}

void WriteStackTraceGuarded(std::FILE* f, CONTEXT* ctx)
{
	__try { WriteStackTrace(f, ctx); }
	__except (EXCEPTION_EXECUTE_HANDLER) { std::fprintf(f, "(stack walk failed — see crash.dmp)\n"); }
}

void WriteWindowsBundle(EXCEPTION_POINTERS* ep, const char* what)
{
	static volatile LONG once = 0;   // one bundle per process, whoever reports first
	if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
	CreateDirectoryW(g_bundleDirW.c_str(), nullptr);   // parents exist since Install

	// Minidump FIRST — raw API only, before anything that could re-fault.
	HANDLE hFile = CreateFileW(g_dumpW.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                           FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), ep, FALSE };
		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
		                  (MINIDUMP_TYPE)(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
		                  ep ? &mei : nullptr, nullptr, nullptr);
		CloseHandle(hFile);
	}

	if (std::FILE* f = std::fopen(g_crashTxt.c_str(), "wb"))
	{
		if (ep)
		{
			const DWORD code = ep->ExceptionRecord->ExceptionCode;
			void* addr = ep->ExceptionRecord->ExceptionAddress;
			std::fprintf(f, "unhandled exception 0x%08lX at %p\n", (unsigned long)code, addr);
			// Name the faulting module + offset — identifies the culprit without symbols.
			HMODULE mod = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
			                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &mod) && mod)
			{
				char name[MAX_PATH] = {};
				GetModuleFileNameA(mod, name, MAX_PATH);
				std::fprintf(f, "module: %s + 0x%llX\n", name,
				             (unsigned long long)((char*)addr - (char*)mod));
			}
		}
		else
			std::fprintf(f, "%s\n", what);
		WriteStackTraceGuarded(f, ep ? ep->ContextRecord : nullptr);
		std::fclose(f);
	}

	__try { WriteLogTail(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
	WriteInfoAndMarker();
}

LONG WINAPI SehFilter(EXCEPTION_POINTERS* ep)
{
	WriteWindowsBundle(ep, "unhandled exception");
	return EXCEPTION_EXECUTE_HANDLER;   // die quietly; the bundle is the report
}

void AbortHandler(int)
{
	WriteWindowsBundle(nullptr, "abort() / failed assert");
	_exit(3);
}

#else  // posix ------------------------------------------------------------------------------

// Bundle body shared by our handler and host-owned handlers (WriteBundle): syscalls +
// backtrace only — no heap, no locale, no stdio buffers.
void WritePosixBundle(int sig)
{
	static volatile sig_atomic_t once = 0;   // one bundle per process
	if (once++) return;
	mkdir(g_bundleDir.c_str(), 0755);   // parents exist since Install
	int fd = open(g_crashTxt.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd >= 0)
	{
		char head[64];
		int n = snprintf(head, sizeof(head), "fatal signal %d\nbacktrace:\n", sig);
		if (n > 0) write(fd, head, (size_t)n);
		void* frames[64];
		int cnt = backtrace(frames, 64);
		backtrace_symbols_fd(frames, cnt, fd);
		close(fd);
	}
	int mi = open(g_marker.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (mi >= 0) { write(mi, g_bundleDir.c_str(), g_bundleDir.size()); close(mi); }
	int fi = open(g_infoJson.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fi >= 0) { write(fi, g_infoContent.c_str(), g_infoContent.size()); close(fi); }
}

void SignalHandler(int sig)
{
	WritePosixBundle(sig);
	// Restore the default action and re-raise: normal termination status + core if enabled.
	signal(sig, SIG_DFL);
	raise(sig);
}

#endif

}  // namespace

void CrashReport::Install(const char* host)
{
	if (g_installed) return;
	g_installed = true;

	char stamp[32];
	std::time_t now = std::time(nullptr);
	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &now);
	const unsigned long pid = (unsigned long)GetCurrentProcessId();
	const char* plat = "windows";
#else
	localtime_r(&now, &tmv);
	const unsigned long pid = (unsigned long)getpid();
#ifdef __APPLE__
	const char* plat = "macos";
#else
	const char* plat = "linux";
#endif
#endif
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);

	boost::system::error_code ec;
	const bfs::path root = Config::writableDir() / "config" / "crash";
	bfs::create_directories(root, ec);   // bundle dir itself is created BY the handler

	char leaf[160];
	std::snprintf(leaf, sizeof(leaf), "%s-%s-%lu", host ? host : "host", stamp, pid);
	const bfs::path bundle = root / leaf;
	g_bundleDir = bundle.string();
	g_crashTxt  = (bundle / "crash.txt").string();
	g_infoJson  = (bundle / "info.json").string();
	g_logTxt    = (bundle / "log.txt").string();
	g_marker    = (root / "pending").string();
	g_infoContent = std::string("{ \"host\": \"") + (host ? host : "host") +
	                "\", \"time\": \"" + stamp + "\", \"platform\": \"" + plat + "\" }\n";

#ifdef _WIN32
	g_bundleDirW = bundle.wstring();
	g_dumpW      = (bundle / "crash.dmp").wstring();
	SetUnhandledExceptionFilter(SehFilter);
	signal(SIGABRT, AbortHandler);   // abort()/asserts bypass SEH
#else
	struct sigaction sa{};
	sa.sa_handler = SignalHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND;   // second fault inside the handler terminates, no loop
	for (int sig : { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS })
		sigaction(sig, &sa, nullptr);
#endif
}

void CrashReport::WriteBundle(void* sehPointers, int sig, const char* what)
{
	if (!g_installed) return;
#ifdef _WIN32
	(void)sig;
	WriteWindowsBundle((EXCEPTION_POINTERS*)sehPointers, what ? what : "host-reported failure");
#else
	(void)sehPointers; (void)what;
	WritePosixBundle(sig);
#endif
}

std::string CrashReport::PendingBundle()
{
	const bfs::path marker = Config::writableDir() / "config" / "crash" / "pending";
	boost::system::error_code ec;
	if (!bfs::exists(marker, ec)) return std::string();
	bfs::ifstream f(marker);
	std::string dir;
	std::getline(f, dir);
	if (dir.empty() || !bfs::exists(bfs::path(dir), ec)) return std::string();
	return dir;
}

void CrashReport::ClearPending()
{
	boost::system::error_code ec;
	bfs::remove(Config::writableDir() / "config" / "crash" / "pending", ec);
}

void CrashReport::PrintBacktrace()
{
#ifdef _WIN32
	WriteStackTrace(stderr, nullptr);
	std::fflush(stderr);
#endif
}

void CrashReport::PrintThreadBacktrace(void* threadHandle, void* ctx)
{
#ifdef _WIN32
	WriteStackTrace(stderr, (CONTEXT*)ctx, (HANDLE)threadHandle);
	std::fflush(stderr);
#endif
}
