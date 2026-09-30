#pragma once
#ifndef NUKEE_IMPORTER_H
#define NUKEE_IMPORTER_H
#include "NukeAPI.h"
#include <boost/function.hpp>
#include <string>
#include <vector>

namespace nuke {

// The asset import facade. Images (stb) and audio (a copy) are engine-side; scene files
// (FBX / glTF / OBJ / ...) go to the "exchange" SERVICE (NukeExchange), plugin importers
// (interface/Importers.h) win for the extensions they register. ImportAnyAsync runs the whole
// thing on a worker with a status-bar entry; anything that touches ResDB inside an import
// goes through Reg() so it lands on the game thread.
class NUKEENGINE_API Importer
{
public:
	static Importer* getSingleton();

	// One scene file -> native assets + prefab in destDir. Meshes written (or clips for an
	// animation-only file); 0 = failed or no exchange service (logged).
	int ImportModel(const char* srcPath, const char* destDir);
	// One image -> a BC-compressed .nutex (animated GIF: frame stack). Returns the GUID, "" = failed.
	std::string ImportImage(const char* srcPath, const char* destDir);
	// One audio file -> a collision-safe copy (referenced by content path).
	bool ImportAudio(const char* srcPath, const char* destDir);
	// Dispatch by extension: plugin importers, images, audio, else the exchange service.
	bool ImportAny(const char* srcPath, const char* destDir);
	// Worker import with progress; `onDone(ok)` on the game thread after the deferred writes.
	void ImportAnyAsync(const std::string& srcPath, const std::string& destDir, boost::function<void(bool)> onDone);

	// Deferred ResDB writes: queued to the game thread inside a worker import, run at once otherwise.
	static void Reg(const boost::function<void()>& f);
	// Live progress (worker imports only; no-ops on the game thread): the current unit's label
	// + inner fraction, one unit finished, the unit total once the source was counted.
	static void Stage(const std::string& label, float sub = 0.0f);
	static void UnitDone();
	static void SetTotal(int total);

	// The exchange service's reach: extensions with the dot (".fbx" ...), empty without a provider.
	static bool ModelImportAvailable();
	static std::vector<std::string> ModelExtensions();
	static std::vector<std::string> ExportExtensions();
	// Image / audio extensions the engine imports itself (with the dot).
	static const std::vector<std::string>& ImageExtensions();
	static const std::vector<std::string>& AudioExtensions();
};

}  // namespace nuke

#endif // !NUKEE_IMPORTER_H
