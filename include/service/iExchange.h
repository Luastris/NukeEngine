#pragma once
#ifndef NUKE_IEXCHANGE_H
#define NUKE_IEXCHANGE_H

namespace nuke {

// The model import / export service ("exchange"): the provider (NukeExchange) reads scene files
// (FBX / glTF / OBJ / ...) into the engine's native assets and writes native assets back out.
// The engine's Importer facade routes model files here; without a provider model import is
// unavailable (images, audio and plugin importers still work) and the editor says so.
// POD seam: plain C strings, size-query buffers; the provider talks to the engine through its
// exported C++ API (Mesh::Build, Texture::BuildFromRGBA, Importer::Reg / Stage ...).
class iExchange
{
public:
	static constexpr const char* kServiceName = "exchange";
	virtual ~iExchange() {}

	// Import one scene file into `destDir` (native assets + a prefab). Returns the number of
	// meshes written, or the number of clips for an animation-only file; 0 = failed. Runs on
	// the import worker: ResDB writes go through Importer::Reg, progress through Importer::Stage.
	virtual int ImportModel(const char* srcPath, const char* destDir) = 0;

	// The file extensions this provider imports / exports, ";"-joined with the dot
	// (".fbx;.gltf;..."). Size-query: returns the length, fills up to `cap`.
	virtual int ImportExtensions(char* buf, int cap) = 0;
	virtual int ExportExtensions(char* buf, int cap) = 0;

	// Export a native asset: kind "mesh" (id = mesh GUID), "prefab" (id = content-relative or
	// absolute .nuprefab path) or "clip" (id = clip GUID); the format follows dstPath's extension.
	virtual bool Export(const char* kind, const char* id, const char* dstPath) = 0;
};

}  // namespace nuke

#endif // !NUKE_IEXCHANGE_H
