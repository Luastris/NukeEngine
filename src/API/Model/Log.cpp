#include "API/Model/Log.h"
#include <boost/thread/mutex.hpp>
#include <cctype>
#include <cstring>
#include <deque>
#include <iostream>
#include <streambuf>

namespace nuke {

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
	// Consecutive duplicates collapse (a per-frame error would flood the ring in seconds).
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

// Find a "path.ext:123" fragment — the entry's SOURCE (a Lua runtime error carries its
// script and line; compiler-style engine messages carry the .cpp). Hand-rolled, no <regex>.
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
	// Trim trailing \r and surrounding whitespace.
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
	if (ContainsCI(s, "error") || ContainsCI(s, "failed") || ContainsCI(s, "exception")
	 || ContainsCI(s, "panic") || ContainsCI(s, "corrupt") || ContainsCI(s, "refused"))
		lv = LOG_ERROR;
	else if (ContainsCI(s, "warn") || ContainsCI(s, "deprecated") || ContainsCI(s, "skipped")
	      || ContainsCI(s, "missing") || ContainsCI(s, "not found") || ContainsCI(s, "stale"))
		lv = (lv == LOG_ERROR) ? lv : LOG_WARN;

	std::string file; int line = 0;
	FindSource(s, file, line);
	Log::Write(lv, tag, s, file, line);
}

// A tee streambuf: every char continues to the ORIGINAL buffer (the real console/IDE
// output keeps working), completed lines are ingested into the ring.
class TeeBuf : public std::streambuf
{
public:
	TeeBuf(std::streambuf* orig, bool errStream) : orig_(orig), err_(errStream) {}
protected:
	int overflow(int c) override
	{
		if (c == EOF) return orig_ ? orig_->pubsync() : 0;
		if (orig_) orig_->sputc((char)c);
		if (c == '\n') { IngestLine(line_, err_); line_.clear(); }
		else if (line_.size() < 4096) line_ += (char)c;
		return c;
	}
	std::streamsize xsputn(const char* p, std::streamsize n) override
	{
		for (std::streamsize i = 0; i < n; ++i) overflow((unsigned char)p[i]);
		return n;
	}
	int sync() override { return orig_ ? orig_->pubsync() : 0; }
private:
	std::streambuf* orig_;
	bool            err_;
	std::string     line_;
};

void Log::CaptureStd()
{
	static bool done = false;
	if (done) return;
	done = true;
	static TeeBuf coutTee(std::cout.rdbuf(), false);
	static TeeBuf cerrTee(std::cerr.rdbuf(), true);
	std::cout.rdbuf(&coutTee);
	std::cerr.rdbuf(&cerrTee);
	std::cout << "[Log]\t\tconsole capture active (cout+cerr -> Console panel)" << std::endl;
}

}  // namespace nuke
