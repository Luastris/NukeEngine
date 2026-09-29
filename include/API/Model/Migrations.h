#pragma once
#ifndef NUKEE_MIGRATIONS_H
#define NUKEE_MIGRATIONS_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace nuke {

// Versioned data upgrades, one registry for everything that persists:
//  - DOCUMENT formats ("world", "prefab", "save", "material", ...): a "version" stamp in the JSON
//    (missing = 1, the format of 2026-09-29); steps from -> from+1 rewrite the parsed document
//    before it is deserialized. A document NEWER than this engine knows is refused, not guessed.
//  - COMPONENT data: a type declares its data version; SaveAtom stamps "v" on the component,
//    LoadAtom runs the steps on {type, props, ...} - worlds, cells, prefabs, parked atoms, saves.
//  - BINARY assets (.numesh / .nutex / .nuanim) upgrade inside their loaders already; the batch
//    re-saves them at the current version.
//  - SAVEGAMES: a "save" header {engine, format, game}; the game declares its own save-data
//    version (SetGameVersion) and upgrades older saves through C++ steps on kind "save" or the
//    script hosts' text upgrader (Lua `saveUpgrade(from, to, json)` / C# `SaveUpgrade`).
//  - The BATCH (UpgradeContent): walks a content tree, upgrades every migratable file in place,
//    re-saves stale binaries, reports what changed and what needs a manual reimport.
class NUKEENGINE_API Migrations
{
	NUKE_CLASS_NOCREATE(Migrations, Object)
public:
	// ---- script surface ----
	[[nuke::func]] static void        SetGameVersion(int version);   // the game's save-data version (declare at startup; 0 = unversioned)
	[[nuke::func]] static int         GameVersion();
	// Batch-upgrade a content tree; apply=false only reports. Returns the report text (also LastReport).
	[[nuke::func]] static std::string UpgradeContent(const std::string& dir, bool apply);
	[[nuke::func]] static std::string LastReportText();
	// Back up the project (its content root + project file) into `backupDir`: the files the last report
	// would rewrite, or the whole content tree. Writes nubackup.json there. Returns "" or the error.
	[[nuke::func]] static std::string Backup(const std::string& backupDir, bool wholeProject);
	// Put a backup's files back where they came from (nubackup.json says where). "" or the error.
	[[nuke::func]] static std::string Restore(const std::string& backupDir);
	// The default backup folder for this project: beside the project folder, "<name>-backup-<engine>-<date>".
	[[nuke::func]] static std::string DefaultBackupDir();

	// ---- native: document formats ----
	using Step = std::function<void(nlohmann::json& doc)>;
	static void DeclareFormat(const std::string& kind, int current);   // the version this engine writes
	static int  FormatVersion(const std::string& kind);                // 1 when undeclared
	static void Register(const std::string& kind, int from, const std::string& description, Step step);
	// Bring `doc` to the current format. False = newer than known (untouched, logged).
	static bool Upgrade(const std::string& kind, nlohmann::json& doc, std::vector<std::string>* log);
	static bool Pending(const std::string& kind, const nlohmann::json& doc);   // an upgrade (or a refusal) would happen
	static int  StampOf(const nlohmann::json& doc);                            // "version" (missing = 1)

	// ---- native: component data ----
	static void DeclareComponentVersion(const std::string& type, int current);
	static int  ComponentVersion(const std::string& type);   // 0 = undeclared (no stamp written)
	static void RegisterComponent(const std::string& type, int from, const std::string& description, Step step);   // step gets {type, props, ...}
	static bool UpgradeComponent(nlohmann::json& component, std::vector<std::string>* log);   // true = changed
	static bool UpgradeAtomTree(nlohmann::json& atom, std::vector<std::string>* log);        // every component, children too

	// ---- native: savegames ----
	static nlohmann::json SaveHeader();   // {"engine", "format", "game"}
	// A script host's upgrader over the save's JSON text: returns 1 handled, 0 no handler defined
	// by the game, -1 the handler failed. The first host that handles it wins.
	using TextUpgrader = std::function<int(int from, int to, std::string& jsonText)>;
	static void AddSaveTextUpgrader(TextUpgrader fn);
	// World format steps, then the game's (kind "save" steps or the text upgrader). False = refuse.
	static bool UpgradeSave(nlohmann::json& doc, std::vector<std::string>* log);

	// ---- native: the batch ----
	struct NUKEENGINE_API Report
	{
		struct Item { std::string path, action, note; };
		std::vector<Item> items;
		int upgraded = 0, resaved = 0, reimport = 0, newer = 0, failed = 0;
		bool applied = false;
		std::string Text() const;
	};
	static Report        UpgradeProjectContent(const std::string& dir, bool apply);
	static const Report& LastReport();
	// Backups with explicit roots (the editor passes its project file); the reflected pair above
	// resolves them from the running project. A backup holds nubackup.json {engine, date, project,
	// whole, files[]} + the files under their content-relative paths (+ the project file).
	struct NUKEENGINE_API BackupInfo { std::string dir, engine, date, project; bool whole = false; int files = 0; };
	static bool BackupProject(const std::string& contentDir, const std::string& projectFile, const std::string& backupDir, bool whole, const Report* touched, std::string* error);
	static bool RestoreProject(const std::string& backupDir, const std::string& contentDir, const std::string& projectFile, std::string* error);
	static bool ReadBackupInfo(const std::string& backupDir, BackupInfo& out);
	static std::vector<BackupInfo> FindBackups(const std::string& projectDir);   // "<name>-backup-*" beside the project folder, newest first
	// A binary format for the batch: magic + u32 version header; a file below `current` is loaded and
	// re-saved through `resave`; below `reimportBelow` the report asks for a reimport (0 = never).
	static void DeclareBinary(const std::string& ext, const char* magic8, int current, int reimportBelow,
	                          std::function<bool(const std::string& path)> resave);
	// The JSON formats the batch knows, by extension ("kind" is the DeclareFormat / Register key).
	static void DeclareJsonExt(const std::string& ext, const std::string& kind);
};

}  // namespace nuke

#endif // !NUKEE_MIGRATIONS_H
