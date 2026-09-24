#pragma once
#ifndef NUKEE_FILEINDEX_H
#define NUKEE_FILEINDEX_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace nuke {

// The engine's file index: every folder a host cares about (project content, shaders, sources,
// mods, saves) is scanned ONCE, then kept current by the OS's own change notifications
// (ReadDirectoryChangesW / inotify / FSEvents; a 2 s mtime poll only where none of them exists).
// Consumers never walk the disk: they take an immutable Snapshot (a shared_ptr - it stays valid
// and unchanged for as long as they hold it, whatever the watcher does meanwhile) and query it,
// or subscribe to the changes, which arrive coalesced on the MAIN thread (Jobs::RunOnMain) and as
// the "fs.changed" engine event {"root","path","dir","kind"}. Editor and Player share it.
class NUKEENGINE_API FileIndex
{
public:
	struct Entry
	{
		std::string rel;       // root-relative, forward slashes, no leading slash ("Worlds/a.nuworld")
		bool        isDir = false;
		int64_t     mtime = 0; // last write, seconds since the epoch (files; 0 for folders)
	};
	// One root's contents at one moment. Entries are sorted by `rel` (byte order); the lookups
	// use that order. Immutable: a change makes a NEW snapshot with a higher version.
	struct NUKEENGINE_API Snapshot
	{
		std::string        root;      // absolute folder, forward slashes, no trailing slash
		uint64_t           version = 0;
		bool               present = false;   // the root folder exists on disk
		std::vector<Entry> entries;
		// Direct children of a folder ("" = the root): files and folders, in `entries` order.
		void Children(const std::string& relDir, std::vector<const Entry*>& out) const;
		// The whole subtree under a folder ("" = everything).
		void Under(const std::string& relDir, std::vector<const Entry*>& out) const;
		// Every FILE with that extension (lowercase, with the dot: ".nuworld"), any depth.
		void WithExtension(const std::string& ext, std::vector<const Entry*>& out) const;
		const Entry* Find(const std::string& rel) const;   // exact rel, or nullptr
		std::string  Abs(const Entry& e) const { return root + "/" + e.rel; }
		std::string  Abs(const std::string& rel) const { return rel.empty() ? root : root + "/" + rel; }
	};
	enum class ChangeKind { Added = 0, Removed = 1, Modified = 2 };
	struct Change
	{
		std::string rootName;   // the name Watch() registered
		std::string rel;        // as Entry::rel
		bool        isDir = false;
		ChangeKind  kind = ChangeKind::Modified;
	};
	using Handler = std::function<void(const Change&)>;

	static FileIndex& Get();

	// Register a folder under a name: scanned synchronously (a snapshot exists on return), watched
	// from then on. The same name with another folder replaces the old root; a missing folder is
	// watched for its creation (empty snapshot meanwhile).
	void Watch(const std::string& name, const std::string& absDir);
	void Unwatch(const std::string& name);
	bool IsWatched(const std::string& name) const;
	std::string RootDir(const std::string& name) const;   // "" when not watched
	// The name whose root contains this absolute path ("" = none) + the root-relative path out.
	std::string RootOf(const std::string& absPath, std::string* relOut = nullptr) const;

	std::shared_ptr<const Snapshot> Get(const std::string& name) const;   // nullptr when not watched
	uint64_t Version(const std::string& name) const;                     // 0 when not watched

	// Change subscription: `name` "" = every root. Handlers run on the main thread.
	long long Subscribe(const std::string& name, Handler fn);
	void      Unsubscribe(long long id);

	// A full rescan of a root (the watcher does it itself after a notification overflow).
	void Rescan(const std::string& name);
	// Stop the watcher thread. Hosts call it before Jobs::Shutdown (the handlers run through Jobs).
	void Shutdown();

private:
	FileIndex();
	~FileIndex();
	struct Impl;
	Impl* m;
};

}  // namespace nuke

#endif // NUKEE_FILEINDEX_H
