#pragma once
#ifndef NUKEE_IMPORTERS_H
#define NUKEE_IMPORTERS_H
#include "NukeAPI.h"
#include <functional>
#include <string>
#include <vector>

namespace nuke {

// A plugin-registered ASSET IMPORTER: converts an external file format the core can't read
// (EPS, PSD layers, a studio's custom mesh format, ...) into native engine assets. The core
// handles images (stb), audio (copy) and 3D via assimp; a plugin fills the gaps or OVERRIDES a
// built-in for an extension it handles better. Registered from NUKEModule::OnLoad, like a
// scripting backend or a file-type descriptor (see AssetCreators.h).
//
// AssImporter::ImportAny consults these BEFORE its built-in dispatch, so a matching importer wins.
struct AssetImporter
{
	std::string              label;   // human name, e.g. "EPS Vector" (logs / future import UI)
	std::vector<std::string> exts;    // handled extensions, lowercase incl. dot: { ".eps", ".ai" }

	// Convert `srcPath` into native asset(s) written under `destDir` and registered in ResDB;
	// return true on success. THREADING: this runs on the import WORKER (async browser import) or
	// the calling thread (sync). Do the file IO / decoding here, but for every ResDB mutation call
	// nuke::AssImporter::Reg(fn) — the engine routes it to the MAIN thread (ResDB is not
	// thread-safe). Keep it free of ImGui / editor types (the engine has no editor dependency).
	std::function<bool(const std::string& srcPath, const std::string& destDir)> import;
};

// Register an importer. Deduped by label (re-enabling a plugin re-registers harmlessly).
NUKEENGINE_API void RegisterImporter(const AssetImporter& imp);
// All registered importers (for the import file-dialog filter / diagnostics).
NUKEENGINE_API const std::vector<AssetImporter>& AssetImporters();
// The importer that handles `ext` (".eps", case-insensitive), or null. First match wins.
NUKEENGINE_API const AssetImporter* ImporterForExt(const std::string& ext);

// Route a ResDB mutation (RegisterTexture / RegisterMesh / SetAssetPath / ...) to the MAIN thread. An
// importer's callback runs on a worker; wrap EVERY ResDB write in this. Lets a plugin importer depend on
// this one light header instead of the heavy assimp importer header. (Wraps AssImporter::Reg.)
NUKEENGINE_API void ImporterDefer(const std::function<void()>& fn);

}  // namespace nuke

#endif // !NUKEE_IMPORTERS_H
