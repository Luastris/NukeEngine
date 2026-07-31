#pragma once
#ifndef NUKEE_IMPORTERS_H
#define NUKEE_IMPORTERS_H
#include "NukeAPI.h"
#include <functional>
#include <string>
#include <vector>

namespace nuke {

// A plugin-registered asset importer: converts an external file format into native engine assets.
// Registered from NUKEModule::OnLoad. AssImporter::ImportAny consults these BEFORE its built-in
// dispatch, so a matching importer wins.
struct AssetImporter
{
	std::string              label;   // human name, e.g. "EPS Vector" (logs / future import UI)
	std::vector<std::string> exts;    // handled extensions, lowercase incl. dot: { ".eps", ".ai" }

	// Convert `srcPath` into native asset(s) under `destDir`, registered in ResDB; true on success.
	// THREADING: runs on the import worker. Do file IO / decoding here, but route EVERY ResDB
	// mutation through ImporterDefer — ResDB is not thread-safe.
	std::function<bool(const std::string& srcPath, const std::string& destDir)> import;
};

// Register an importer. Deduped by label (re-enabling a plugin re-registers harmlessly).
NUKEENGINE_API void RegisterImporter(const AssetImporter& imp);
// All registered importers (for the import file-dialog filter / diagnostics).
NUKEENGINE_API const std::vector<AssetImporter>& AssetImporters();
// The importer that handles `ext` (".eps", case-insensitive), or null. First match wins.
NUKEENGINE_API const AssetImporter* ImporterForExt(const std::string& ext);

// Route a ResDB mutation to the MAIN thread. Importer callbacks run on a worker, so wrap EVERY
// ResDB write in this.
NUKEENGINE_API void ImporterDefer(const std::function<void()>& fn);

}  // namespace nuke

#endif // !NUKEE_IMPORTERS_H
