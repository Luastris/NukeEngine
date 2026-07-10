#pragma once
#ifndef NUKEE_PACKAGE_H
#define NUKEE_PACKAGE_H
#include "NukeAPI.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nuke {

// Release packaging (roadmap 3.2): the NUPAK container + the layered content resolver.
//
// ONE container format for both artifacts:
//   * game.nupak — the packed PROJECT (manifest + content), an immutable release
//     artifact, max compression (zstd by default);
//   * <name>.numod — a MOD overlay (editable in the editor, store by default), mounted
//     ABOVE the project pak so same-path entries override and new entries add.
//
// Entry paths are PROJECT-relative, '/'-separated ("game.nuproj", "content/Worlds/x.nuworld"),
// matched case-insensitively (Windows content habits).
//
// The RESOLVER is a layer stack: the RAW project/overlay directory on top (dev + modder
// edits), then mounted paks by priority. Packed content is served as BYTES ONLY (Read) —
// pak entries are NEVER written to disk: laying the decompressed project out beside the
// pak would void its whole point. ResolveRead() serves the raw layer alone (loaders take
// the file path in dev, the memory variants in packed runtime).
class NUKEENGINE_API Package
{
public:
	// Entry compression method (per entry; a pak may mix them).
	enum Method { M_Store = 0, M_Zlib = 1, M_Zstd = 2 };

	// ---- writer ------------------------------------------------------------------------
	// Pack `files` (projectRelativePath -> source disk file) into `outPak`. `method` +
	// `level` apply to every entry (level: zlib 1..9, zstd 1..22; ignored for store);
	// entries that don't shrink are stored raw automatically. `progress` (optional) gets
	// (done, total). False on any IO error (partial output is deleted).
	static bool Create(const std::vector<std::pair<std::string, std::string>>& files,
	                   const std::string& outPak, int method, int level,
	                   const std::function<void(int, int)>& progress = nullptr);

	// ---- standalone pak handle (editor tooling: mod diff, pak inspection) ---------------
	struct Entry
	{
		std::string path;         // project-relative, '/'-separated (original case)
		uint64_t    offset = 0;   // payload position inside the pak
		uint64_t    rawSize = 0;
		uint64_t    packSize = 0;
		uint32_t    crc = 0;      // crc32 of the RAW bytes
		uint8_t     method = 0;   // Method
	};
	class NUKEENGINE_API File
	{
	public:
		bool Open(const std::string& pakPath);           // parse the TOC (keeps no OS handle)
		const std::vector<Entry>& Entries() const { return entries; }
		const Entry* Find(const std::string& rel) const; // case-insensitive
		bool Read(const std::string& rel, std::string& out) const;   // decompress one entry
		const std::string& Path() const { return path; }
	private:
		std::string path;
		std::vector<Entry> entries;
	};

	// ---- mounted layer stack (runtime resolver) -----------------------------------------
	// Higher priority wins. False when the pak can't be parsed.
	static bool Mount(const std::string& pakPath, int priority);
	static void UnmountAll();
	static int  MountedCount();
	static std::vector<std::string> MountedPaks();   // pak paths, priority order (top first)

	// ---- mods with dependencies (mods-on-mods) -------------------------------------------
	// Every .numod may carry a "mod.json" manifest: {"name": "...", "requires": ["OtherMod"]}.
	// `requires` = the mods that were MOUNTED in the authoring session (a compatibility
	// patch depends on the mods it patches; a mod reusing another mod's system depends on
	// it) — they must load BELOW the dependent mod.
	struct ModInfo
	{
		std::string pakPath;                 // mounted file
		std::string name;                    // manifest name, else the file stem
		std::vector<std::string> requires_;  // dependency names (load below this mod)
	};
	// Mount every mod enabled in <gameRoot>/config/mods.json above the base pak: tolerant
	// path resolution (as written -> mods/<entry> -> mods/<filename>), dependency-aware
	// order (a mod mounts AFTER everything it requires; the config order is kept among
	// independent mods; a mod whose dependency is missing/disabled is SKIPPED with a log).
	// Returns how many mods mounted. Shared by the Player boot and the editor session.
	static int MountMods(const std::string& gameRoot);
	// Metadata of the mounted mods (mount order, bottom-up). The base pak is not a mod.
	static const std::vector<ModInfo>& Mods();

	// The base (lowest) layer: the raw project directory (dev / extracted-pak editing).
	// "" = no raw layer (pure packed runtime).
	static void SetRawRoot(const std::string& projectDir);
	static const std::string& RawRoot();

	// Resolve project-relative -> content. Read() returns bytes from the TOP layer;
	// ResolveRead() returns a disk path from the RAW layer only ("" for pak-only entries
	// — packed content never touches the disk).
	static bool        Read(const std::string& rel, std::string& out);
	// EVERY layer's copy of `rel`, BOTTOM-UP (base pak, then mods by ascending priority,
	// the raw overlay last). Returns the count. Feeds World::MergeWorldLayers — two mods
	// touching the same world must MERGE, not have the top copy replace the other.
	static int         ReadAll(const std::string& rel, std::vector<std::string>& out);
	// Same, tagged with each copy's source pak path ("" = the raw overlay) — the world
	// merge needs to know WHICH mod a copy came from to pick that mod's diff baseline.
	static int         ReadAllInfo(const std::string& rel,
	                               std::vector<std::pair<std::string, std::string>>& out);   // (data, pakPath)
	// Bytes from the top MOUNTED layer only (raw overlay skipped) — what the game stack
	// provides UNDER the modder's edits; Package Mod diffs the work tree against this.
	static bool        ReadMounted(const std::string& rel, std::string& out);
	static bool        Exists(const std::string& rel);
	static std::string ResolveRead(const std::string& rel);

	// Union of every layer's entries under `prefix` (project-relative), deduped by path
	// (top layer's casing wins), sorted. Raw-layer scan skips the usual dev noise.
	static std::vector<std::string> List(const std::string& prefix);

	// crc32 helper (zlib) — shared by the writer, the extractor and the editor's mod diff.
	static uint32_t Crc32(const void* data, size_t size, uint32_t seed = 0);
};

}  // namespace nuke

#endif // !NUKEE_PACKAGE_H
