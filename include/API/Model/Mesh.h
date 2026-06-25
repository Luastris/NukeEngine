#pragma once
#ifndef NUKEE_MESH_H
#define NUKEE_MESH_H
#include "Transform.h"
#include "Material.h"
#include <assimp/mesh.h>
#include <boost/container/list.hpp>
#include <memory>
#include "../../../NukeEngine.h"

namespace nuke {

namespace bc = boost::container;
//class Mesh;
//template class bc::list<Mesh*>;

class Mesh
{
public:
    char name[256];
    float *vertexArray;
    float *normalArray;
    float *uvArray;

    int numVerts;

	bc::list<Mesh*>  children;


	Mesh();

	void ImportAIMesh(aiMesh* mesh);

	// Primitive factories (procedural geometry, no asset needed).
	static Mesh* CreateCube();
};
}  // namespace nuke

#endif // !NUKEE_MESH_H
