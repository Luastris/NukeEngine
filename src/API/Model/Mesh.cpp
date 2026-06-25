#include "API/Model/Mesh.h"

namespace nuke {

Mesh::Mesh() {
	children.clear();
}

void Mesh::ImportAIMesh(aiMesh* mesh) {
	numVerts = mesh->mNumFaces * 3;

	vertexArray = new float[mesh->mNumFaces * 3 * 3];
	normalArray = new float[mesh->mNumFaces * 3 * 3];
	uvArray = new float[mesh->mNumFaces * 3 * 2];

	for (unsigned int i = 0; i < mesh->mNumFaces; i++)
	{
		const aiFace& face = mesh->mFaces[i];

		for (int j = 0; j < 3; j++)
		{
			//cout << face.mIndices[j] << endl;
			aiVector3D uv = mesh->mTextureCoords[0][face.mIndices[j]];
			memcpy(uvArray, &uv, sizeof(float) * 2);
			uvArray += 2;

			aiVector3D normal = mesh->mNormals[face.mIndices[j]];
			memcpy(normalArray, &normal, sizeof(float) * 3);
			normalArray += 3;

			aiVector3D pos = mesh->mVertices[face.mIndices[j]];
			memcpy(vertexArray, &pos, sizeof(float) * 3);
			vertexArray += 3;
		}
	}

	uvArray -= mesh->mNumFaces * 3 * 2;
	normalArray -= mesh->mNumFaces * 3 * 3;
	vertexArray -= mesh->mNumFaces * 3 * 3;
	const char* __name = mesh->mName.C_Str();
	strcpy(name, __name);
	//name = __name;
}

Mesh* Mesh::CreateCube() {
	Mesh* m = new Mesh();
	const float h = 0.5f;
	const float C[8][3] = {
		{-h,-h,-h},{ h,-h,-h},{ h, h,-h},{-h, h,-h},
		{-h,-h, h},{ h,-h, h},{ h, h, h},{-h, h, h}
	};
	const int   F[6][4] = { {4,5,6,7},{1,0,3,2},{5,1,2,6},{0,4,7,3},{7,6,2,3},{0,1,5,4} };
	const float N[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
	const int   tri[6]  = { 0,1,2, 0,2,3 };

	m->numVerts   = 36;
	m->vertexArray = new float[36 * 3];
	m->normalArray = new float[36 * 3];
	m->uvArray     = new float[36 * 2];
	int vi = 0;
	for (int f = 0; f < 6; ++f)
		for (int t = 0; t < 6; ++t)
		{
			const float* p = C[F[f][tri[t]]];
			m->vertexArray[vi * 3 + 0] = p[0]; m->vertexArray[vi * 3 + 1] = p[1]; m->vertexArray[vi * 3 + 2] = p[2];
			m->normalArray[vi * 3 + 0] = N[f][0]; m->normalArray[vi * 3 + 1] = N[f][1]; m->normalArray[vi * 3 + 2] = N[f][2];
			m->uvArray[vi * 2 + 0] = 0.0f; m->uvArray[vi * 2 + 1] = 0.0f;
			++vi;
		}
	strcpy(m->name, "Cube");
	return m;
}
}  // namespace nuke