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
};
}  // namespace nuke

#endif // ASSIMPORTER_H
