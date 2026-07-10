#ifndef ASSIMPORTER_H
#define ASSIMPORTER_H
#include "NukeAPI.h"
#include <iostream>
#include <assimp/Importer.hpp>
#include <assimp/mesh.h>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <API/Model/resdb.h>
#include <API/Model/MeshRenderer.h>
#include <boost/function.hpp>
#include <vector>

namespace nuke {

using namespace std;

class NUKEENGINE_API AssImporter
{
	AssImporter();
	~AssImporter();
	Atom* ImportObject(aiNode* node, const aiScene* scene);

public:
	static AssImporter* getSingleton() {
		static AssImporter instance;
		return &instance;
	}
	
	void Import(const char* path);

	// Full conversion: read an external file (OBJ/FBX/glTF/...) via assimp and write each mesh
	// as a native .numesh asset into destDir, each with a fresh GUID, registered in ResDB.
	// Returns the number of meshes converted. Nothing references the source file afterwards.
	int ImportToContent(const char* srcPath, const char* destDir);

	// Import a standalone image (png/jpg/tga/bmp/...) -> a native .nutex (BC-compressed, mipped),
	// registered in ResDB. Returns the new texture GUID ("" on failure).
	std::string ImportImage(const char* srcPath, const char* destDir);

	// Import an audio file (ogg/wav/mp3/flac) = collision-safe COPY into content (no custom
	// format — the audio service decodes the file; components reference it by relative path).
	bool ImportAudio(const char* srcPath, const char* destDir);

	// Dispatch by file extension: image -> ImportImage, audio -> ImportAudio, otherwise ->
	// ImportToContent. Returns true on any success. Browser Import button + Explorer drag&drop.
	bool ImportAny(const char* srcPath, const char* destDir);

	// ASYNC import on the core job system (2.4): the heavy work (assimp parsing, BC
	// compression, file writes) runs on a WORKER; every ResDB mutation the import
	// produces is deferred and applied on the MAIN thread (ResDB is not thread-safe),
	// then onDone(ok) fires there too. Imports are serialized among themselves.
	void ImportAnyAsync(const std::string& srcPath, const std::string& destDir,
	                    boost::function<void(bool)> onDone = boost::function<void(bool)>());

	// INTERNAL: apply a ResDB mutation now (sync import) or queue it for the main
	// thread (async import worker) — the sink is THREAD-LOCAL, so a synchronous import
	// on the game thread never crosses wires with a worker import. Used by the free
	// helper functions in assimporter.cpp too.
	static void Reg(const boost::function<void()>& f);
};
}  // namespace nuke

#endif // ASSIMPORTER_H
