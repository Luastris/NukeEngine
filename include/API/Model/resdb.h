#ifndef RESDB_H
#define RESDB_H
#include "NukeAPI.h"
#include "Mesh.h"
#include "Texture.h"
#include "Material.h"
#include "Shader.h"
#include "Atom.h"
#include <boost/container/list.hpp>
#include <memory>
#include <map>
#include <string>

namespace nuke {

#ifdef WIN32
#define uint unsigned int
#endif

namespace bc = boost::container;

// The asset database. ONE shared instance lives in the engine DLL (getSingleton is
// defined out-of-line, not inline) so the editor, plugins and the runtime all see the
// same assets. Meshes are addressable by a stable GUID (built-ins use "builtin:<name>").
class NUKEENGINE_API ResDB
{
    ResDB();
    ~ResDB() {}
public:
    bc::list<Mesh*> meshes;
    bc::list<Texture*> textures;
    bc::list<Material*> materials;
    bc::list<Shader*> shaders;
    bc::list<Atom*> prefabs;

    std::map<std::string, Mesh*>     meshByGuid;   // GUID -> mesh asset
    std::map<std::string, Material*> matByGuid;    // GUID -> material asset
    std::map<std::string, Texture*>  texByGuid;    // GUID -> texture asset

    static ResDB* getSingleton();              // single instance (engine DLL)

    Mesh* GetMesh(const std::string& guid);    // nullptr if unknown
    void  RegisterMesh(Mesh* m);               // add to meshes + index by m->guid

    Material* GetMaterial(const std::string& guid);   // nullptr if unknown
    void      RegisterMaterial(Material* m);          // add to materials + index by m->guid

    Texture* GetTexture(const std::string& guid);     // nullptr if unknown
    void     RegisterTexture(Texture* t);             // add to textures + index by t->guid

    // Scan a content folder (recursively) and load every native asset (.numesh) into the DB,
    // indexed by the GUID stored in the file. Skips GUIDs already registered. Call at startup
    // so meshGuid references in saved worlds resolve.
    void  LoadContentDir(const std::string& dir);

    static std::string NewGuid();              // fresh opaque asset id (for imports)

	std::shared_ptr<uint> loadTexture(const std::string& name);
};
}  // namespace nuke

#endif // RESDB_H
