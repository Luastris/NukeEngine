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

	// Read an external model file via assimp and write each mesh as a native .numesh asset into
	// destDir with a fresh GUID, registered in ResDB. Returns the number of meshes converted.
	int ImportToContent(const char* srcPath, const char* destDir);

	// Import a standalone image into a native .nutex (BC-compressed, mipped), registered in ResDB.
	// Returns the new texture GUID ("" on failure).
	std::string ImportImage(const char* srcPath, const char* destDir);

	// Import an audio file as a collision-safe COPY into content; there is no custom format, so
	// components reference it by relative path and the audio service decodes it.
	bool ImportAudio(const char* srcPath, const char* destDir);

	// Dispatch by file extension: image -> ImportImage, audio -> ImportAudio, else ImportToContent.
	// True on any success.
	bool ImportAny(const char* srcPath, const char* destDir);

	// Async import on the job system: the heavy work runs on a WORKER, every ResDB mutation is
	// deferred to the MAIN thread (ResDB is not thread-safe), and onDone(ok) fires there too.
	// Imports are serialized among themselves.
	void ImportAnyAsync(const std::string& srcPath, const std::string& destDir,
	                    boost::function<void(bool)> onDone = boost::function<void(bool)>());

	// Apply a ResDB mutation now (sync import) or queue it for the main thread (async worker).
	// The sink is THREAD-LOCAL, so a synchronous import never crosses wires with a worker import.
	static void Reg(const boost::function<void()>& f);
};
}  // namespace nuke

#endif // ASSIMPORTER_H
