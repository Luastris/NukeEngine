// Header-only boost.chrono BEFORE any include that pulls boost - the same mode as Time.cpp /
// Clock.cpp (mixing the lib flavour in one DLL links steady_clock::now twice).
#define BOOST_CHRONO_HEADER_ONLY
#include <boost/chrono.hpp>
#include "API/Model/FileIndex.h"
#include "API/Model/Jobs.h"
#include "API/Model/Events.h"
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/lock_guard.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <cstring>
#include <cstdlib>
#include <iostream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace bfs = boost::filesystem;

namespace nuke {

namespace {

const double kDebounceSec = 0.05;   // a save is a burst of events: one flush per burst
const double kPollSec     = 2.0;    // the fallback poll + the "did the missing root appear" check

std::string NormDir(std::string s)
{
	for (char& c : s) if (c == '\\') c = '/';
	while (s.size() > 1 && s.back() == '/') s.pop_back();
	return s;
}
std::string LowerExt(const std::string& rel)
{
	const size_t slash = rel.find_last_of('/');
	const size_t dot = rel.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
	std::string e = rel.substr(dot);
	for (char& c : e) c = (char)tolower((unsigned char)c);
	return e;
}
bool EntryLess(const FileIndex::Entry& a, const FileIndex::Entry& b) { return a.rel < b.rel; }

// One full walk of a root: every file and folder under it (folders too, so the tree views need no
// disk either). Missing root = an empty list.
bool ScanRoot(const std::string& root, std::vector<FileIndex::Entry>& out)
{
	out.clear();
	boost::system::error_code ec;
	bfs::path rp(root);
	if (!bfs::is_directory(rp, ec)) return false;
	const size_t base = root.size() + 1;
	for (bfs::recursive_directory_iterator it(rp, ec), end; it != end; it.increment(ec))
	{
		if (ec) break;
		std::string g = it->path().generic_string();
		if (g.size() <= base) continue;
		FileIndex::Entry e;
		e.rel = g.substr(base);
		e.isDir = bfs::is_directory(it->status());
		if (!e.isDir) { std::time_t t = bfs::last_write_time(it->path(), ec); e.mtime = ec ? 0 : (int64_t)t; ec.clear(); }
		out.push_back(std::move(e));
	}
	std::sort(out.begin(), out.end(), EntryLess);
	return true;
}
// One subtree (a folder that appeared), the same way.
void ScanSub(const std::string& root, const std::string& relDir, std::vector<FileIndex::Entry>& out)
{
	std::vector<FileIndex::Entry> all;
	ScanRoot(root + "/" + relDir, all);
	for (FileIndex::Entry& e : all) { e.rel = relDir + "/" + e.rel; out.push_back(std::move(e)); }
}

}  // namespace

// ---- Snapshot queries ----------------------------------------------------------------------------

void FileIndex::Snapshot::Children(const std::string& relDir, std::vector<const Entry*>& out) const
{
	const std::string pfx = relDir.empty() ? std::string() : relDir + "/";
	auto it = std::lower_bound(entries.begin(), entries.end(), pfx, [](const Entry& e, const std::string& p) { return e.rel < p; });
	for (; it != entries.end(); ++it)
	{
		if (it->rel.compare(0, pfx.size(), pfx) != 0) break;
		if (it->rel.find('/', pfx.size()) != std::string::npos) continue;   // deeper
		out.push_back(&*it);
	}
}

void FileIndex::Snapshot::Under(const std::string& relDir, std::vector<const Entry*>& out) const
{
	const std::string pfx = relDir.empty() ? std::string() : relDir + "/";
	auto it = pfx.empty() ? entries.begin()
	                      : std::lower_bound(entries.begin(), entries.end(), pfx, [](const Entry& e, const std::string& p) { return e.rel < p; });
	for (; it != entries.end(); ++it)
	{
		if (!pfx.empty() && it->rel.compare(0, pfx.size(), pfx) != 0) break;
		out.push_back(&*it);
	}
}

void FileIndex::Snapshot::WithExtension(const std::string& ext, std::vector<const Entry*>& out) const
{
	for (const Entry& e : entries)
		if (!e.isDir && LowerExt(e.rel) == ext) out.push_back(&e);
}

const FileIndex::Entry* FileIndex::Snapshot::Find(const std::string& rel) const
{
	auto it = std::lower_bound(entries.begin(), entries.end(), rel, [](const Entry& e, const std::string& r) { return e.rel < r; });
	return (it != entries.end() && it->rel == rel) ? &*it : nullptr;
}

// ---- the index -------------------------------------------------------------------------------------

struct FileIndex::Impl
{
	struct Root
	{
		Impl* owner = nullptr;
		std::string name, dir;
		std::shared_ptr<const Snapshot> snap;
		// watcher-thread state
		bool   backendOpen = false;
		bool   fullRescan = false;
		std::set<std::string> dirty;     // rel paths with pending notifications
		double lastEvent = 0.0;
		double lastPoll = 0.0;
#ifdef _WIN32
		HANDLE hDir = INVALID_HANDLE_VALUE;
		OVERLAPPED ov{};
		std::vector<unsigned char> buf;
#elif defined(__APPLE__)
		FSEventStreamRef stream = nullptr;
#elif defined(__linux__)
		std::map<int, std::string> wd;   // watch descriptor -> rel dir ("" = root)
#endif
	};
	boost::mutex mx;                              // roots + subscribers + snapshot pointers
	std::map<std::string, Root*> roots;           // by name
	struct Sub { long long id; std::string name; Handler fn; };
	std::vector<Sub> subs;
	long long nextSub = 1;
	// the watcher thread
	boost::thread thread;
	bool threadStarted = false, stop = false;
	bool pollOnly = false;                        // no OS backend: the 2 s poll for everything
	std::vector<std::string> cmdOpen, cmdClose;   // roots to (re)open / close on the thread
	std::vector<Root*> closing;                   // roots removed while the thread may still touch them
#ifdef _WIN32
	HANDLE wake = nullptr;
#elif defined(__linux__)
	int inotifyFd = -1, wakeFd = -1;
#endif

	static double Now()
	{
		return boost::chrono::duration_cast<boost::chrono::duration<double>>(boost::chrono::steady_clock::now().time_since_epoch()).count();
	}
	void Wake();
	void Start();
	void Loop();
	void OpenBackend(Root& r);
	void CloseBackend(Root& r);
	void Pump(double timeoutSec);   // one wait for OS notifications, dirty paths out
	void Flush(Root& r);            // dirty / rescan -> a new snapshot + the changes
	void Publish(Root& r, std::shared_ptr<const Snapshot> snap, std::vector<Change> changes);
	void Diff(const Root& r, const std::vector<Entry>& oldE, const std::vector<Entry>& newE, std::vector<Change>& out);
#ifdef _WIN32
	void Issue(Root& r);
	void Collect(Root& r);
#elif defined(__APPLE__)
	static void MacCallback(ConstFSEventStreamRef, void*, size_t, void*, const FSEventStreamEventFlags*, const FSEventStreamEventId*);
#elif defined(__linux__)
	void AddWatches(Root& r, const std::string& relDir);
#endif
};

FileIndex& FileIndex::Get() { static FileIndex g; return g; }
FileIndex::FileIndex() : m(new Impl())
{
	m->pollOnly = std::getenv("NUKE_FILEINDEX_POLL") != nullptr;
#ifdef _WIN32
	m->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#elif defined(__linux__)
	m->wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (!m->pollOnly) m->inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (m->inotifyFd < 0) m->pollOnly = true;
#elif !defined(__APPLE__)
	m->pollOnly = true;
#endif
}
FileIndex::~FileIndex() { Shutdown(); delete m; }

void FileIndex::Impl::Wake()
{
#ifdef _WIN32
	if (wake) SetEvent(wake);
#elif defined(__linux__)
	if (wakeFd >= 0) { uint64_t one = 1; (void)!write(wakeFd, &one, sizeof one); }
#endif
}

void FileIndex::Impl::Start()
{
	if (threadStarted) return;
	threadStarted = true;
	thread = boost::thread([this] { Loop(); });
}

// ---- public API ------------------------------------------------------------------------------------

void FileIndex::Watch(const std::string& name, const std::string& absDir)
{
	const std::string dir = NormDir(bfs::absolute(bfs::path(absDir)).generic_string());
	Impl::Root* r = nullptr;
	{
		boost::lock_guard<boost::mutex> l(m->mx);
		auto it = m->roots.find(name);
		if (it != m->roots.end())
		{
			if (it->second->dir == dir) return;   // already watched
			m->cmdClose.push_back(name);            // the thread closes the old backend
			m->closing.push_back(it->second);
			m->roots.erase(it);
		}
	}
	// The first scan on the caller's thread: a snapshot exists when Watch returns.
	auto snap = std::make_shared<Snapshot>();
	snap->root = dir; snap->version = 1;
	snap->present = ScanRoot(dir, snap->entries);
	r = new Impl::Root();
	r->owner = m; r->name = name; r->dir = dir; r->snap = snap;
	{
		boost::lock_guard<boost::mutex> l(m->mx);
		m->roots[name] = r;
		m->cmdOpen.push_back(name);
		m->Start();
	}
	m->Wake();
	std::cout << "[FileIndex]\twatching '" << name << "' = " << dir << " (" << snap->entries.size() << " entries" << (m->pollOnly ? ", poll" : "") << ")" << std::endl;
}

void FileIndex::Unwatch(const std::string& name)
{
	boost::lock_guard<boost::mutex> l(m->mx);
	auto it = m->roots.find(name);
	if (it == m->roots.end()) return;
	m->cmdClose.push_back(name);
	m->closing.push_back(it->second);
	m->roots.erase(it);
	m->Wake();
}

bool FileIndex::IsWatched(const std::string& name) const
{
	boost::lock_guard<boost::mutex> l(m->mx);
	return m->roots.count(name) != 0;
}

std::string FileIndex::RootDir(const std::string& name) const
{
	boost::lock_guard<boost::mutex> l(m->mx);
	auto it = m->roots.find(name);
	return it == m->roots.end() ? std::string() : it->second->dir;
}

std::string FileIndex::RootOf(const std::string& absPath, std::string* relOut) const
{
	const std::string p = NormDir(bfs::absolute(bfs::path(absPath)).generic_string());
	boost::lock_guard<boost::mutex> l(m->mx);
	std::string best; size_t bestLen = 0;
	for (auto& kv : m->roots)
	{
		const std::string& d = kv.second->dir;
		if (p.size() < d.size() || p.compare(0, d.size(), d) != 0) continue;
		if (p.size() > d.size() && p[d.size()] != '/') continue;
		if (d.size() >= bestLen) { best = kv.first; bestLen = d.size(); if (relOut) *relOut = p.size() > d.size() ? p.substr(d.size() + 1) : std::string(); }
	}
	return best;
}

std::shared_ptr<const FileIndex::Snapshot> FileIndex::Get(const std::string& name) const
{
	boost::lock_guard<boost::mutex> l(m->mx);
	auto it = m->roots.find(name);
	return it == m->roots.end() ? nullptr : it->second->snap;
}

uint64_t FileIndex::Version(const std::string& name) const
{
	boost::lock_guard<boost::mutex> l(m->mx);
	auto it = m->roots.find(name);
	return it == m->roots.end() ? 0 : it->second->snap->version;
}

long long FileIndex::Subscribe(const std::string& name, Handler fn)
{
	boost::lock_guard<boost::mutex> l(m->mx);
	const long long id = m->nextSub++;
	m->subs.push_back({ id, name, std::move(fn) });
	return id;
}

void FileIndex::Unsubscribe(long long id)
{
	boost::lock_guard<boost::mutex> l(m->mx);
	m->subs.erase(std::remove_if(m->subs.begin(), m->subs.end(), [&](const Impl::Sub& s) { return s.id == id; }), m->subs.end());
}

void FileIndex::Rescan(const std::string& name)
{
	{
		boost::lock_guard<boost::mutex> l(m->mx);
		auto it = m->roots.find(name);
		if (it == m->roots.end()) return;
		it->second->fullRescan = true;
	}
	m->Wake();
}

void FileIndex::Shutdown()
{
	if (!m->threadStarted) return;
	{
		boost::lock_guard<boost::mutex> l(m->mx);
		m->stop = true;
	}
	m->Wake();
	if (m->thread.joinable()) m->thread.join();
	m->threadStarted = false;
	boost::lock_guard<boost::mutex> l(m->mx);
	for (auto& kv : m->roots) { m->CloseBackend(*kv.second); delete kv.second; }
	m->roots.clear();
	for (Impl::Root* r : m->closing) { m->CloseBackend(*r); delete r; }
	m->closing.clear();
#ifdef _WIN32
	if (m->wake) { CloseHandle(m->wake); m->wake = nullptr; }
#elif defined(__linux__)
	if (m->inotifyFd >= 0) { close(m->inotifyFd); m->inotifyFd = -1; }
	if (m->wakeFd >= 0) { close(m->wakeFd); m->wakeFd = -1; }
#endif
}

// ---- the watcher thread ------------------------------------------------------------------------

void FileIndex::Impl::Loop()
{
	for (;;)
	{
		// commands: roots to open / close (their handles belong to this thread)
		std::vector<std::string> open, closeNames;
		std::vector<Root*> gone;
		bool quit = false;
		{
			boost::lock_guard<boost::mutex> l(mx);
			open.swap(cmdOpen); closeNames.swap(cmdClose); gone.swap(closing); quit = stop;
		}
		for (Root* r : gone) { CloseBackend(*r); delete r; }
		if (quit) return;
		for (const std::string& n : open)
		{
			boost::lock_guard<boost::mutex> l(mx);
			auto it = roots.find(n);
			if (it != roots.end() && !it->second->backendOpen) OpenBackend(*it->second);
		}
		// the wait: 50 ms while a burst is settling, 2 s otherwise (the poll / missing-root check)
		double timeout = kPollSec;
		{
			boost::lock_guard<boost::mutex> l(mx);
			for (auto& kv : roots) if (!kv.second->dirty.empty() || kv.second->fullRescan) timeout = kDebounceSec;
		}
		Pump(timeout);
		// flush the roots whose burst settled, poll the ones without a backend
		std::vector<Root*> due;
		{
			boost::lock_guard<boost::mutex> l(mx);
			const double now = Now();
			for (auto& kv : roots)
			{
				Root& r = *kv.second;
				const bool polled = pollOnly || !r.backendOpen;
				if (polled && now - r.lastPoll >= kPollSec) { r.lastPoll = now; r.fullRescan = true; }
				if (r.fullRescan || (!r.dirty.empty() && now - r.lastEvent >= kDebounceSec)) due.push_back(&r);
			}
		}
		for (Root* r : due) Flush(*r);
	}
}

void FileIndex::Impl::Diff(const Root& r, const std::vector<Entry>& oldE, const std::vector<Entry>& newE, std::vector<Change>& out)
{
	size_t i = 0, j = 0;
	while (i < oldE.size() || j < newE.size())
	{
		if (j >= newE.size() || (i < oldE.size() && oldE[i].rel < newE[j].rel))
		{ out.push_back({ r.name, oldE[i].rel, oldE[i].isDir, ChangeKind::Removed }); ++i; }
		else if (i >= oldE.size() || newE[j].rel < oldE[i].rel)
		{ out.push_back({ r.name, newE[j].rel, newE[j].isDir, ChangeKind::Added }); ++j; }
		else
		{
			if (oldE[i].isDir != newE[j].isDir) { out.push_back({ r.name, oldE[i].rel, oldE[i].isDir, ChangeKind::Removed }); out.push_back({ r.name, newE[j].rel, newE[j].isDir, ChangeKind::Added }); }
			else if (!newE[j].isDir && oldE[i].mtime != newE[j].mtime) out.push_back({ r.name, newE[j].rel, false, ChangeKind::Modified });
			++i; ++j;
		}
	}
}

void FileIndex::Impl::Flush(Root& r)
{
	std::set<std::string> dirty; bool full = false;
	std::shared_ptr<const Snapshot> cur;
	{
		boost::lock_guard<boost::mutex> l(mx);
		dirty.swap(r.dirty); full = r.fullRescan; r.fullRescan = false; cur = r.snap;
	}
	std::vector<Change> changes;
	auto next = std::make_shared<Snapshot>();
	next->root = cur->root; next->version = cur->version + 1;
	if (full)
	{
		next->present = ScanRoot(r.dir, next->entries);
		Diff(r, cur->entries, next->entries, changes);
	}
	else
	{
		next->present = cur->present;
		next->entries = cur->entries;
		boost::system::error_code ec;
		for (const std::string& rel : dirty)
		{
			const bfs::path p(r.dir + "/" + rel);
			const bool exists = bfs::exists(p, ec); ec.clear();
			auto it = std::lower_bound(next->entries.begin(), next->entries.end(), rel, [](const Entry& e, const std::string& s) { return e.rel < s; });
			const bool known = it != next->entries.end() && it->rel == rel;
			if (!exists)
			{
				if (!known) continue;
				if (it->isDir)
				{   // the subtree goes with it
					const std::string pfx = rel + "/";
					auto end = it + 1;
					while (end != next->entries.end() && end->rel.compare(0, pfx.size(), pfx) == 0) ++end;
					for (auto k = it; k != end; ++k) changes.push_back({ r.name, k->rel, k->isDir, ChangeKind::Removed });
					next->entries.erase(it, end);
				}
				else { changes.push_back({ r.name, rel, false, ChangeKind::Removed }); next->entries.erase(it); }
				continue;
			}
			const bool isDir = bfs::is_directory(p, ec); ec.clear();
			if (isDir)
			{
				if (known && it->isDir) continue;   // a folder's own timestamp: its children report themselves
				if (known) { changes.push_back({ r.name, rel, false, ChangeKind::Removed }); next->entries.erase(it); }
				std::vector<Entry> sub;
				Entry d; d.rel = rel; d.isDir = true; sub.push_back(d);
				ScanSub(r.dir, rel, sub);
				for (Entry& e : sub)
				{
					auto at = std::lower_bound(next->entries.begin(), next->entries.end(), e.rel, [](const Entry& a, const std::string& s) { return a.rel < s; });
					if (at != next->entries.end() && at->rel == e.rel) continue;
					changes.push_back({ r.name, e.rel, e.isDir, ChangeKind::Added });
					next->entries.insert(at, std::move(e));
				}
#ifdef __linux__
				AddWatches(r, rel);
#endif
				continue;
			}
			std::time_t t = bfs::last_write_time(p, ec); const int64_t mt = ec ? 0 : (int64_t)t; ec.clear();
			if (known)
			{
				if (it->isDir) { changes.push_back({ r.name, rel, true, ChangeKind::Removed }); it->isDir = false; it->mtime = mt; changes.push_back({ r.name, rel, false, ChangeKind::Added }); continue; }
				if (it->mtime == mt) continue;
				it->mtime = mt; changes.push_back({ r.name, rel, false, ChangeKind::Modified });
			}
			else
			{
				Entry e; e.rel = rel; e.mtime = mt;
				next->entries.insert(it, std::move(e));
				changes.push_back({ r.name, rel, false, ChangeKind::Added });
			}
		}
	}
	if (changes.empty()) return;   // nothing observable changed (a same-mtime rewrite, a poll that saw the same tree)
	Publish(r, next, std::move(changes));
}

void FileIndex::Impl::Publish(Root& r, std::shared_ptr<const Snapshot> snap, std::vector<Change> changes)
{
	std::vector<Sub> targets;
	{
		boost::lock_guard<boost::mutex> l(mx);
		r.snap = snap;
		for (const Sub& s : subs) if (s.name.empty() || s.name == r.name) targets.push_back(s);
	}
	if (changes.empty()) return;
	{
		int add = 0, mod = 0, rem = 0;
		for (const Change& c : changes) (c.kind == ChangeKind::Added ? add : c.kind == ChangeKind::Removed ? rem : mod)++;
		std::cout << "[FileIndex]	'" << r.name << "' v" << snap->version << ": +" << add << " ~" << mod << " -" << rem
		          << " (" << changes.front().rel << (changes.size() > 1 ? ", ..." : "") << ")" << std::endl;
	}
	// the engine event (scripts, modules) + the native handlers, both on the main thread
	for (const Change& c : changes)
	{
		nlohmann::json j; j["root"] = c.rootName; j["path"] = c.rel; j["dir"] = c.isDir;
		j["kind"] = c.kind == ChangeKind::Added ? "added" : c.kind == ChangeKind::Removed ? "removed" : "modified";
		Events::EmitEngine("fs.changed", j.dump());
	}
	if (targets.empty()) return;
	auto list = std::make_shared<std::vector<Change>>(std::move(changes));
	auto subsCopy = std::make_shared<std::vector<Sub>>(std::move(targets));
	Jobs::RunOnMain([list, subsCopy]()
	{
		for (const Change& c : *list)
			for (const Sub& s : *subsCopy)
				if (s.fn) s.fn(c);
	});
}

// ---- backends ------------------------------------------------------------------------------------

#ifdef _WIN32

void FileIndex::Impl::Issue(Root& r)
{
	if (r.hDir == INVALID_HANDLE_VALUE) return;
	ResetEvent(r.ov.hEvent);
	const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE
	                   | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
	if (!ReadDirectoryChangesW(r.hDir, r.buf.data(), (DWORD)r.buf.size(), TRUE, filter, nullptr, &r.ov, nullptr))
	{
		std::cout << "[FileIndex]\t'" << r.name << "': ReadDirectoryChangesW failed (" << GetLastError() << "), polling" << std::endl;
		CloseBackend(r);
	}
}

void FileIndex::Impl::Collect(Root& r)
{
	DWORD bytes = 0;
	if (!GetOverlappedResult(r.hDir, &r.ov, &bytes, FALSE))
	{
		const DWORD err = GetLastError();
		if (err == ERROR_IO_INCOMPLETE) return;
		if (err == ERROR_NOTIFY_ENUM_DIR) bytes = 0;   // the buffer overflowed: everything may have changed
		else { std::cout << "[FileIndex]\t'" << r.name << "': notification error " << err << ", polling" << std::endl; CloseBackend(r); return; }
	}
	{
		boost::lock_guard<boost::mutex> l(mx);
		r.lastEvent = Now();
		if (bytes == 0) r.fullRescan = true;
		else
		{
			size_t off = 0;
			for (;;)
			{
				const FILE_NOTIFY_INFORMATION* fi = (const FILE_NOTIFY_INFORMATION*)(r.buf.data() + off);
				const int wlen = (int)(fi->FileNameLength / sizeof(WCHAR));
				const int need = WideCharToMultiByte(CP_UTF8, 0, fi->FileName, wlen, nullptr, 0, nullptr, nullptr);
				std::string rel((size_t)std::max(need, 0), '\0');
				if (need > 0) WideCharToMultiByte(CP_UTF8, 0, fi->FileName, wlen, &rel[0], need, nullptr, nullptr);
				for (char& c : rel) if (c == '\\') c = '/';
				if (!rel.empty()) r.dirty.insert(rel);
				if (fi->NextEntryOffset == 0) break;
				off += fi->NextEntryOffset;
			}
		}
	}
	Issue(r);
}

void FileIndex::Impl::OpenBackend(Root& r)
{
	if (pollOnly) return;
	if (r.hDir != INVALID_HANDLE_VALUE) return;
	std::wstring w; w.resize(r.dir.size() * 2 + 2);
	const int n = MultiByteToWideChar(CP_UTF8, 0, r.dir.c_str(), (int)r.dir.size(), &w[0], (int)w.size());
	w.resize((size_t)std::max(n, 0));
	HANDLE h = CreateFileW(w.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                       OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;   // not there (yet): the 2 s check retries
	r.hDir = h;
	memset(&r.ov, 0, sizeof r.ov);
	r.ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	r.buf.resize(256 * 1024);
	r.backendOpen = true;
	Issue(r);
}

void FileIndex::Impl::CloseBackend(Root& r)
{
	if (r.hDir != INVALID_HANDLE_VALUE)
	{
		CancelIoEx(r.hDir, &r.ov);
		DWORD bytes = 0; GetOverlappedResult(r.hDir, &r.ov, &bytes, TRUE);
		CloseHandle(r.hDir); r.hDir = INVALID_HANDLE_VALUE;
	}
	if (r.ov.hEvent) { CloseHandle(r.ov.hEvent); r.ov.hEvent = nullptr; }
	r.backendOpen = false;
}

void FileIndex::Impl::Pump(double timeoutSec)
{
	std::vector<HANDLE> hs; std::vector<Root*> owners;
	hs.push_back(wake); owners.push_back(nullptr);
	{
		boost::lock_guard<boost::mutex> l(mx);
		for (auto& kv : roots)
			if (kv.second->backendOpen)
			{
				// a root that vanished under its handle: close it, the check reopens it when it returns
				boost::system::error_code ec;
				if (!bfs::is_directory(bfs::path(kv.second->dir), ec)) { CloseBackend(*kv.second); kv.second->fullRescan = true; continue; }
				hs.push_back(kv.second->ov.hEvent); owners.push_back(kv.second);
			}
			else if (Now() - kv.second->lastPoll >= kPollSec) OpenBackend(*kv.second);   // a root that appeared
	}
	const DWORD ms = (DWORD)(timeoutSec * 1000.0);
	const DWORD rc = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, ms);
	if (rc >= WAIT_OBJECT_0 + 1 && rc < WAIT_OBJECT_0 + hs.size())
	{
		Root* r = owners[rc - WAIT_OBJECT_0];
		Collect(*r);
		// drain the other signalled ones without waiting
		for (size_t i = 1; i < hs.size(); ++i)
			if (owners[i] != r && WaitForSingleObject(hs[i], 0) == WAIT_OBJECT_0) Collect(*owners[i]);
	}
}

#elif defined(__APPLE__)

void FileIndex::Impl::MacCallback(ConstFSEventStreamRef, void* info, size_t n, void* paths, const FSEventStreamEventFlags* flags, const FSEventStreamEventId*)
{
	Root* r = (Root*)info;
	Impl* self = r->owner;
	const char** ps = (const char**)paths;
	boost::lock_guard<boost::mutex> l(self->mx);
	r->lastEvent = Now();
	for (size_t i = 0; i < n; ++i)
	{
		if (flags[i] & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagRootChanged)) { r->fullRescan = true; continue; }
		const std::string p = NormDir(ps[i]);
		if (p.size() > r->dir.size() && p.compare(0, r->dir.size(), r->dir) == 0 && p[r->dir.size()] == '/') r->dirty.insert(p.substr(r->dir.size() + 1));
		else r->fullRescan = true;
	}
}

void FileIndex::Impl::OpenBackend(Root& r)
{
	if (pollOnly || r.stream) return;
	boost::system::error_code ec;
	if (!bfs::is_directory(bfs::path(r.dir), ec)) return;
	CFStringRef s = CFStringCreateWithCString(nullptr, r.dir.c_str(), kCFStringEncodingUTF8);
	CFArrayRef arr = CFArrayCreate(nullptr, (const void**)&s, 1, &kCFTypeArrayCallBacks);
	FSEventStreamContext ctx{ 0, &r, nullptr, nullptr, nullptr };
	r.stream = FSEventStreamCreate(nullptr, &MacCallback, &ctx, arr, kFSEventStreamEventIdSinceNow, kDebounceSec,
	                               kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
	CFRelease(arr); CFRelease(s);
	if (!r.stream) return;
	FSEventStreamScheduleWithRunLoop(r.stream, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
	if (!FSEventStreamStart(r.stream)) { FSEventStreamInvalidate(r.stream); FSEventStreamRelease(r.stream); r.stream = nullptr; return; }
	r.backendOpen = true;
}

void FileIndex::Impl::CloseBackend(Root& r)
{
	if (r.stream) { FSEventStreamStop(r.stream); FSEventStreamInvalidate(r.stream); FSEventStreamRelease(r.stream); r.stream = nullptr; }
	r.backendOpen = false;
}

void FileIndex::Impl::Pump(double timeoutSec)
{
	{
		boost::lock_guard<boost::mutex> l(mx);
		for (auto& kv : roots)
			if (!kv.second->backendOpen && Now() - kv.second->lastPoll >= kPollSec) OpenBackend(*kv.second);
	}
	CFRunLoopRunInMode(kCFRunLoopDefaultMode, timeoutSec, false);   // the run loop's timeout is the wake (50 ms / 2 s)
}

#elif defined(__linux__)

void FileIndex::Impl::AddWatches(Root& r, const std::string& relDir)
{
	if (inotifyFd < 0) return;
	const uint32_t mask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;
	std::vector<std::string> dirs; dirs.push_back(relDir);
	if (r.snap)
	{
		std::vector<const Entry*> under; r.snap->Under(relDir, under);
		for (const Entry* e : under) if (e->isDir) dirs.push_back(e->rel);
	}
	for (const std::string& d : dirs)
	{
		const std::string abs = d.empty() ? r.dir : r.dir + "/" + d;
		const int wd = inotify_add_watch(inotifyFd, abs.c_str(), mask);
		if (wd >= 0) r.wd[wd] = d;
	}
}

void FileIndex::Impl::OpenBackend(Root& r)
{
	if (pollOnly || inotifyFd < 0) return;
	boost::system::error_code ec;
	if (!bfs::is_directory(bfs::path(r.dir), ec)) return;
	AddWatches(r, std::string());
	r.backendOpen = !r.wd.empty();
}

void FileIndex::Impl::CloseBackend(Root& r)
{
	if (inotifyFd >= 0) for (auto& kv : r.wd) inotify_rm_watch(inotifyFd, kv.first);
	r.wd.clear();
	r.backendOpen = false;
}

void FileIndex::Impl::Pump(double timeoutSec)
{
	{
		boost::lock_guard<boost::mutex> l(mx);
		for (auto& kv : roots)
			if (!kv.second->backendOpen && Now() - kv.second->lastPoll >= kPollSec) OpenBackend(*kv.second);
	}
	pollfd fds[2]; int n = 0;
	if (wakeFd >= 0) { fds[n].fd = wakeFd; fds[n].events = POLLIN; ++n; }
	if (inotifyFd >= 0) { fds[n].fd = inotifyFd; fds[n].events = POLLIN; ++n; }
	if (n == 0) { boost::this_thread::sleep(boost::posix_time::milliseconds((int)(timeoutSec * 1000.0))); return; }
	if (poll(fds, (nfds_t)n, (int)(timeoutSec * 1000.0)) <= 0) return;
	if (wakeFd >= 0 && (fds[0].revents & POLLIN)) { uint64_t v; (void)!read(wakeFd, &v, sizeof v); }
	if (inotifyFd < 0 || !(fds[n - 1].revents & POLLIN)) return;
	alignas(inotify_event) unsigned char buf[64 * 1024];
	for (;;)
	{
		const ssize_t len = read(inotifyFd, buf, sizeof buf);
		if (len <= 0) break;
		boost::lock_guard<boost::mutex> l(mx);
		for (ssize_t off = 0; off < len;)
		{
			const inotify_event* ev = (const inotify_event*)(buf + off);
			off += (ssize_t)(sizeof(inotify_event) + ev->len);
			for (auto& kv : roots)
			{
				Root& r = *kv.second;
				auto w = r.wd.find(ev->wd);
				if (w == r.wd.end()) continue;
				r.lastEvent = Now();
				if (ev->mask & (IN_Q_OVERFLOW | IN_DELETE_SELF | IN_MOVE_SELF)) { r.fullRescan = true; if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) { inotify_rm_watch(inotifyFd, ev->wd); r.wd.erase(w); } break; }
				if (ev->len == 0) break;
				const std::string rel = w->second.empty() ? std::string(ev->name) : w->second + "/" + ev->name;
				r.dirty.insert(rel);
				break;
			}
		}
	}
}

#else

void FileIndex::Impl::OpenBackend(Root&) {}
void FileIndex::Impl::CloseBackend(Root& r) { r.backendOpen = false; }
void FileIndex::Impl::Pump(double timeoutSec) { boost::this_thread::sleep(boost::posix_time::milliseconds((int)(timeoutSec * 1000.0))); }

#endif

}  // namespace nuke
