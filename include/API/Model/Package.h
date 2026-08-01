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

// Release packaging: the NUPAK container + the layered content resolver. One format for both
// game.nupak (packed project) and <name>.numod (mod overlay mounted above it).
// Entry paths are PROJECT-relative, '/'-separated, matched case-insensitively.
// The resolver is a layer stack: raw project directory on top, then mounted paks by priority.
// Pak entries are served as bytes only and are NEVER written to disk; ResolveRead() serves the
// raw layer alone.
class NUKEENGINE_API Package
{
public:
	// Entry compression method (per entry; a pak may mix them).
	enum Method { M_Store = 0, M_Zlib = 1, M_Zstd = 2 };

	// ---- writer ----
	// Pack `files` (projectRelativePath -> source disk file) into `outPak`. `level`: zlib 1..9,
	// zstd 1..22, ignored for store; entries that don't shrink are stored raw. `progress` gets
	// (done, total). False on any IO error (partial output is deleted).
	static bool Create(const std::vector<std::pair<std::string, std::string>>& files,
	                   const std::string& outPak, int method, int level,
	                   const std::function<void(int, int)>& progress = nullptr);

	// ---- standalone pak handle (editor tooling) ----
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

	// ---- mounted layer stack (runtime resolver) ----
	// Higher priority wins. False when the pak can't be parsed.
	static bool Mount(const std::string& pakPath, int priority);
	static bool Unmount(const std::string& pakPath);   // drop ONE layer (path-equivalent match)
	static void UnmountAll();
	static int  MountedCount();
	static std::vector<std::string> MountedPaks();   // pak paths, priority order (top first)

	// ---- pak manifest (base game / DLC identity + platform tags) ----
	// Every .nupak may carry a reserved "pak.json" entry:
	//   {"kind": "base"|"dlc", "name": "...", "base": "<base pak name>", "platforms": ["win64"]}
	// A DLC mounts only onto the base whose name matches its "base". No pak.json = a legacy base.
	struct PakInfo
	{
		std::string kind;                     // "" (legacy base) / "base" / "dlc" / "part"
		std::string name;                     // display name (the game's or the DLC's)
		std::string base;                     // DLC: the base pak name it extends
		std::vector<std::string> platforms;   // empty = any platform
		std::vector<std::string> parts;       // split-part pak filenames, mounted with this pak
		std::string partOf;                   // non-empty = this pak IS a part of that one
	};
	static bool ReadPakInfo(const std::string& pakPath, PakInfo& out);   // false = no/bad pak.json
	// The platform tag this binary was built for ("win64"), matched against pak.json
	// "platforms" and mod.json "platform".
	static const char* CurrentPlatform();
	// Mount every DLC pak in <gameRoot>/content/dlc, filename order, priorities 1..K (above the
	// base at 0, below the mods at 1000+). Returns how many mounted.
	// Mount the split parts a pak declares in its own pak.json, at the SAME priority as it.
	// Returns how many mounted. No parts = 0.
	static int MountPakParts(const std::string& mainPak, int priority);
	static int MountDlcs(const std::string& gameRoot, const std::string& baseName);
	// Names of the DLCs mounted by the last MountDlcs (cleared by UnmountAll). A mod records
	// these as its "dlc" requirement, and the loader skips a mod whose DLC is absent.
	static const std::vector<std::string>& MountedDlcs();

	// ---- mods with dependencies (mods-on-mods) ----
	// Every .numod may carry a "mod.json" manifest:
	//   {"name": "...", "requires": ["OtherMod"], "platform": "any"|"win64",
	//    "parts": ["X.textures.numod", ...]}
	// or, for a split part pak: {"name": "...", "part_of": "<main mod name>"}.
	// `requires` = the mods MOUNTED in the authoring session; they must load BELOW this mod.
	struct ModInfo
	{
		std::string pakPath;                 // mounted file
		std::string name;                    // manifest name, else the file stem
		std::vector<std::string> requires_;  // dependency names (load below this mod)
		std::vector<std::string> dlc;        // DLC names the mod was authored against
		std::string platform;                // "" / "any" = cross-platform, else a platform tag
		std::vector<std::string> parts;      // split-part pak filenames (mounted with this mod)
		std::string partOf;                  // non-empty = this pak IS a part (skip as a mod)
	};
	// Mount every mod enabled in <gameRoot>/config/mods.json above the base pak, dependency-aware
	// (a mod mounts AFTER everything it requires; config order kept among independent mods; a mod
	// with a missing/disabled dependency is skipped). Returns how many mounted.
	static int MountMods(const std::string& gameRoot);
	// Same dependency-aware mounting with an explicit entry list instead of config/mods.json.
	static int MountModList(const std::string& gameRoot, const std::vector<std::string>& entries);
	// Metadata of the mounted mods (mount order, bottom-up). The base pak is not a mod.
	static const std::vector<ModInfo>& Mods();

	// The base (lowest) layer: the raw project directory (dev / extracted-pak editing).
	// "" = no raw layer (pure packed runtime).
	static void SetRawRoot(const std::string& projectDir);
	static const std::string& RawRoot();

	// Resolve project-relative -> content. Read() returns bytes from the TOP layer;
	// ResolveRead() returns a disk path from the RAW layer only ("" for pak-only entries).
	static bool        Read(const std::string& rel, std::string& out);
	// EVERY layer's copy of `rel`, BOTTOM-UP (base pak, mods by ascending priority, raw overlay
	// last). Returns the count; feeds World::MergeWorldLayers.
	static int         ReadAll(const std::string& rel, std::vector<std::string>& out);
	// Same, tagged with each copy's source pak path ("" = the raw overlay) — the world merge
	// picks a mod's diff baseline from it.
	static int         ReadAllInfo(const std::string& rel,
	                               std::vector<std::pair<std::string, std::string>>& out);   // (data, pakPath)
	// Bytes from the top MOUNTED layer only (raw overlay skipped); Package Mod diffs against this.
	static bool        ReadMounted(const std::string& rel, std::string& out);
	static bool        Exists(const std::string& rel);
	static std::string ResolveRead(const std::string& rel);

	// Union of every layer's entries under `prefix` (project-relative), deduped by path
	// (top layer's casing wins), sorted. Raw-layer scan skips the usual dev noise.
	static std::vector<std::string> List(const std::string& prefix);

	// crc32 helper (zlib).
	static uint32_t Crc32(const void* data, size_t size, uint32_t seed = 0);
};

}  // namespace nuke

#endif // !NUKEE_PACKAGE_H
