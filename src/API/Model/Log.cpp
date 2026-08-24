#include "API/Model/Log.h"
#include <atomic>
#include <boost/chrono.hpp>
#include <boost/thread/mutex.hpp>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <streambuf>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace nuke {

static std::atomic<bool> g_consoleEcho{true};   // echo to the OS console; read on the hot path
static bool              g_captured = false;   // CaptureStd installed the tee (editor)

// Seconds since the process was created: the kernel's creation stamp on Windows (counts the
// loader + static init too), the first call's steady clock elsewhere.
double Log::Uptime()
{
#ifdef _WIN32
	static const long long t0 = []() -> long long
	{
		FILETIME c, e, k, u;
		if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
			return ((long long)c.dwHighDateTime << 32) | c.dwLowDateTime;
		FILETIME now; GetSystemTimeAsFileTime(&now);
		return ((long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
	}();
	FILETIME now; GetSystemTimeAsFileTime(&now);
	const long long t = ((long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
	return (double)(t - t0) * 1e-7;   // 100ns ticks
#else
	static const boost::chrono::steady_clock::time_point t0 = boost::chrono::steady_clock::now();
	return boost::chrono::duration<double>(boost::chrono::steady_clock::now() - t0).count();
#endif
}

// ---- the ring ---------------------------------------------------------------------------------

static boost::mutex          gLogLock;
static std::deque<LogEntry>  gRing;
static uint64_t              gVersion = 0;
static uint64_t              gNextId  = 1;
static int                   gCount[3] = { 0, 0, 0 };

void Log::Write(LogLevel level, const std::string& tag, const std::string& text,
                const std::string& file, int line)
{
	boost::mutex::scoped_lock l(gLogLock);
	// Consecutive duplicates collapse into a count instead of new entries.
	if (!gRing.empty())
	{
		LogEntry& last = gRing.back();
		if (last.level == level && last.tag == tag && last.text == text && last.file == file && last.line == line)
		{
			++last.count;
			++gVersion;
			return;
		}
	}
	LogEntry e;
	e.level = level; e.tag = tag; e.text = text; e.file = file; e.line = line;
	e.id = gNextId++;
	e.time = Uptime();
	gRing.push_back(std::move(e));
	++gCount[level];
	if (gRing.size() > kMaxEntries)
	{
		--gCount[gRing.front().level];
		gRing.pop_front();
	}
	++gVersion;
}

uint64_t Log::Version() { boost::mutex::scoped_lock l(gLogLock); return gVersion; }

std::vector<LogEntry> Log::Snapshot()
{
	boost::mutex::scoped_lock l(gLogLock);
	return std::vector<LogEntry>(gRing.begin(), gRing.end());
}

void Log::Clear()
{
	boost::mutex::scoped_lock l(gLogLock);
	gRing.clear();
	gCount[0] = gCount[1] = gCount[2] = 0;
	++gVersion;
}

void Log::Counts(int& info, int& warn, int& error)
{
	boost::mutex::scoped_lock l(gLogLock);
	info = gCount[0]; warn = gCount[1]; error = gCount[2];
}

// ---- std::cout / std::cerr capture ------------------------------------------------------------

// Strip ANSI escape sequences (the terminal color codes some libraries emit).
static std::string StripAnsi(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '\x1b')
		{
			size_t j = i + 1;
			if (j < s.size() && s[j] == '[')
			{
				++j;
				while (j < s.size() && !isalpha((unsigned char)s[j])) ++j;
				i = j;   // skip the final letter too
				continue;
			}
		}
		out += s[i];
	}
	return out;
}

static bool ContainsCI(const std::string& hay, const char* needle)
{
	const size_t n = strlen(needle);
	if (hay.size() < n) return false;
	for (size_t i = 0; i + n <= hay.size(); ++i)
	{
		size_t j = 0;
		while (j < n && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) ++j;
		if (j == n) return true;
	}
	return false;
}

// ContainsCI minus ZERO COUNTS: "0 Error(s)" / "no errors" is a success report, not an error
// — every match must NOT be preceded by a zero/none counter for the word to signal.
static bool SignalsCI(const std::string& hay, const char* needle)
{
	const size_t n = strlen(needle);
	if (hay.size() < n) return false;
	for (size_t i = 0; i + n <= hay.size(); ++i)
	{
		size_t j = 0;
		while (j < n && tolower((unsigned char)hay[i + j]) == tolower((unsigned char)needle[j])) ++j;
		if (j != n) continue;
		// Look left past whitespace for "0" (a bare zero count) or "no".
		size_t p = i;
		while (p > 0 && (hay[p - 1] == ' ' || hay[p - 1] == '\t')) --p;
		const bool zero = p >= 1 && hay[p - 1] == '0' && (p < 2 || !isdigit((unsigned char)hay[p - 2]));
		const bool none = p >= 2 && tolower((unsigned char)hay[p - 2]) == 'n'
		               && tolower((unsigned char)hay[p - 1]) == 'o'
		               && (p < 3 || !isalpha((unsigned char)hay[p - 3]));
		if (!zero && !none) return true;
	}
	return false;
}

// Find a "path.ext:123" fragment in `s` and return it as file + line.
static void FindSource(const std::string& s, std::string& file, int& line)
{
	static const char* exts[] = { ".lua:", ".cpp:", ".h:", ".hpp:", ".hlsl:", ".cs:", ".nuworld:", ".json:" };
	for (const char* ext : exts)
	{
		const size_t extLen = strlen(ext);
		for (size_t pos = 0; (pos = s.find(ext, pos)) != std::string::npos; pos += extLen)
		{
			size_t digits = pos + extLen;
			if (digits >= s.size() || !isdigit((unsigned char)s[digits])) continue;
			size_t end = digits;
			while (end < s.size() && isdigit((unsigned char)s[end])) ++end;
			// Backtrack to the path start: stop at whitespace, quotes, brackets.
			size_t start = pos;
			while (start > 0)
			{
				char c = s[start - 1];
				if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' || c == '[' || c == '<') break;
				--start;
			}
			if (start == pos) continue;   // no path characters at all
			file = s.substr(start, pos + extLen - 1 - start);   // includes the extension, excludes ':'
			line = atoi(s.substr(digits, end - digits).c_str());
			return;
		}
	}
}

// One captured LINE -> a ring entry: strip ANSI, peel the "[tag]" prefix, guess severity.
static void IngestLine(const std::string& raw, bool fromErr)
{
	std::string s = StripAnsi(raw);
	while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
	size_t b = 0;
	while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
	s.erase(0, b);
	if (s.empty()) return;

	std::string tag;
	if (s[0] == '[')
	{
		size_t close = s.find(']');
		if (close != std::string::npos && close > 1 && close < 32)
		{
			tag = s.substr(1, close - 1);
			size_t after = close + 1;
			while (after < s.size() && (s[after] == ' ' || s[after] == '\t')) ++after;
			s.erase(0, after);
		}
	}
	if (s.empty() && tag.empty()) return;

	LogLevel lv = fromErr ? LOG_ERROR : LOG_INFO;
	if (SignalsCI(s, "error") || ContainsCI(s, "failed") || ContainsCI(s, "exception")
	 || ContainsCI(s, "panic") || ContainsCI(s, "corrupt") || ContainsCI(s, "refused"))
		lv = LOG_ERROR;
	else if (SignalsCI(s, "warn") || ContainsCI(s, "deprecated") || ContainsCI(s, "skipped")
	      || ContainsCI(s, "missing") || ContainsCI(s, "not found") || ContainsCI(s, "stale"))
		lv = (lv == LOG_ERROR) ? lv : LOG_WARN;

	std::string file; int line = 0;
	FindSource(s, file, line);
	Log::Write(lv, tag, s, file, line);
}

// Tee streambuf: echoes to the OS stream and ingests completed lines into the ring. The echo
// uses C stdio (locked per call) — writing through the captured filebuf races concurrent
// direct-stdout writers (printf, a hosted CLR) and corrupts the FILE state.
class TeeBuf : public std::streambuf
{
public:
	TeeBuf(std::streambuf* orig, bool errStream) : orig_(orig), err_(errStream) {}
protected:
	int overflow(int c) override
	{
		if (c == EOF) return Echoing() ? std::fflush(Os()) : 0;
		const bool echo = Echoing();
		std::string& line = Line();
		if (echo) { if (line.empty() && c != '\n') Stamp(); std::fputc(c, Os()); }
		if (c == '\n') { IngestLine(line, err_); line.clear(); }
		else if (line.size() < 4096) line += (char)c;
		return c;
	}
	std::streamsize xsputn(const char* p, std::streamsize n) override
	{
		// Console write in line-sized pieces (each line start gets its uptime stamp), then the
		// same scan feeds the ring line by line.
		const bool echo = Echoing();
		std::string& line = Line();
		std::streamsize from = 0;
		for (std::streamsize i = 0; i < n; ++i)
		{
			const char ch = p[i];
			if (echo && line.empty() && ch != '\n') Stamp();
			if (ch == '\n')
			{
				if (echo) { std::fwrite(p + from, 1, (size_t)(i + 1 - from), Os()); from = i + 1; }
				IngestLine(line, err_); line.clear();
			}
			else if (line.size() < 4096) line += ch;
		}
		if (echo && from < n) std::fwrite(p + from, 1, (size_t)(n - from), Os());
		return n;
	}
	int sync() override { return Echoing() ? std::fflush(Os()) : 0; }
private:
	FILE* Os() const { return err_ ? stderr : stdout; }
	bool  Echoing() const { return orig_ && g_consoleEcho.load(std::memory_order_relaxed); }
	// The line accumulator is PER THREAD: cout is written from the render thread (backend log
	// callbacks) and the game thread at once, and a shared buffer raced — StripAnsi walked a
	// string another thread was appending to. Lines interleave per thread, never corrupt.
	std::string& Line()
	{
		static thread_local std::string lines[2];
		return lines[err_ ? 1 : 0];
	}
	// "[  12.345] " — seconds since process start, at the start of every console line.
	void Stamp()
	{
		char b[24];
		const int n = std::snprintf(b, sizeof(b), "[%8.3f] ", Log::Uptime());
		if (n > 0) std::fwrite(b, 1, (size_t)n, Os());
	}
	std::streambuf* orig_;   // kept: capture stays reversible / non-null marks "echo possible"
	bool            err_;
};

void Log::CaptureStd()
{
	static bool done = false;
	if (done) return;
	done = true;
	g_captured = true;
	static TeeBuf coutTee(std::cout.rdbuf(), false);
	static TeeBuf cerrTee(std::cerr.rdbuf(), true);
	std::cout.rdbuf(&coutTee);
	std::cerr.rdbuf(&cerrTee);
	std::cout << "[Log]\t\tconsole capture active (cout+cerr -> Console panel)" << std::endl;
}

// Discard streambuf for hosts without capture: drops bytes when echo is off.
namespace {
struct NullBuf : std::streambuf
{
	int overflow(int c) override { return c; }                                   // pretend written
	std::streamsize xsputn(const char*, std::streamsize n) override { return n; }
};
NullBuf         g_null;
std::streambuf* g_savedCout = nullptr;   // originals, to restore when echo turns back on
std::streambuf* g_savedCerr = nullptr;
}  // namespace

void Log::SetConsoleEcho(bool on)
{
	g_consoleEcho.store(on, std::memory_order_relaxed);
	if (g_captured)
		return;   // the tee already gates the OS write on g_consoleEcho
	// No tee: swap cout/cerr to a null sink while off, restore on.
	if (!on)
	{
		if (!g_savedCout) { g_savedCout = std::cout.rdbuf(); g_savedCerr = std::cerr.rdbuf(); }
		std::cout.rdbuf(&g_null);
		std::cerr.rdbuf(&g_null);
	}
	else if (g_savedCout)
	{
		std::cout.rdbuf(g_savedCout);
		std::cerr.rdbuf(g_savedCerr);
		g_savedCout = g_savedCerr = nullptr;
	}
}

}  // namespace nuke
