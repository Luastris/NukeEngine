#ifndef RESDB_H
#define RESDB_H
#include "Mesh.h"
#include "Texture.h"
#include "Material.h"
#include "Shader.h"
#include "Atom.h"
#include <boost/container/list.hpp>
#include <memory>

namespace nuke {

#ifdef WIN32
#define uint unsigned int
#endif

namespace bc = boost::container;

class ResDB
{
    ResDB() {}
    ~ResDB() {}
public:
    bc::list<Mesh*> meshes;
    bc::list<Texture*> textures;
    bc::list<Material*> materials;
    bc::list<Shader*> shaders;
    bc::list<Atom*> prefabs;

    static ResDB* getSingleton(){
        static ResDB instance;
        return &instance;
    }

	std::shared_ptr<uint> loadTexture(const std::string& name);
};
}  // namespace nuke

#endif // RESDB_H
