#pragma once
#ifndef NUKEE_MESH_H
#define NUKEE_MESH_H
#include "NukeAPI.h"
#include "Transform.h"
#include "Material.h"
#include <assimp/mesh.h>
#include <boost/container/list.hpp>
#include <memory>
#include <string>
#include "../../../NukeEngine.h"

namespace nuke {

namespace bc = boost::container;
//class Mesh;
//template class bc::list<Mesh*>;

class NUKEENGINE_API Mesh
{
public:
    char name[256];
    std::string guid;   // asset id ("builtin:cube" for primitives, generated for imports)
    float *vertexArray;
    float *normalArray;
    float *uvArray;

    int numVerts;

	// Local-space bounds (for frustum culling). Lazily computed from vertexArray on first use.
	float aabbMin[3] = { 0, 0, 0 };
	float aabbMax[3] = { 0, 0, 0 };
	bool  boundsValid = false;
	void  EnsureBounds();

	bc::list<Mesh*>  children;


	Mesh();

	void ImportAIMesh(aiMesh* mesh);

	// Primitive factories (procedural geometry). Registered in ResDB under "builtin:<name>".
	static Mesh* CreateCube();
	static Mesh* CreatePlane();
	static Mesh* CreateSphere();

	// Native asset format (.numesh): binary header + interleaved-free vertex/normal/uv arrays.
	// Import converts external files (OBJ/FBX/...) into these so nothing references the source
	// at runtime. The GUID is stored inside the file; ResDB indexes by it.
	bool         SaveToFile(const std::string& path) const;
	static Mesh* LoadFromFile(const std::string& path);
};
}  // namespace nuke

#endif // !NUKEE_MESH_H
