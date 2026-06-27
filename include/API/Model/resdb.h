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

class iRender;   // fwd (BuildShaderPipelines / HotReloadShaders take it)

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
    std::map<std::string, Shader*>   shaderByGuid; // GUID -> shader asset
    std::map<std::string, std::string> pathByGuid; // GUID -> source file path (for "locate"/DnD)
    std::map<std::string, std::string> guidByPath; // source file path -> GUID

    static ResDB* getSingleton();              // single instance (engine DLL)

    Mesh* GetMesh(const std::string& guid);    // nullptr if unknown
    void  RegisterMesh(Mesh* m);               // add to meshes + index by m->guid

    Material* GetMaterial(const std::string& guid);   // nullptr if unknown
    void      RegisterMaterial(Material* m);          // add to materials + index by m->guid

    Texture* GetTexture(const std::string& guid);     // nullptr if unknown
    void     RegisterTexture(Texture* t);             // add to textures + index by t->guid

    Shader*  GetShader(const std::string& guid);      // nullptr if unknown
    void     RegisterShader(Shader* s);               // add to shaders + index by s->guid
    // Scan a dir (recursively) for "<name>.vs.hlsl" + "<name>.ps.hlsl" pairs -> Shader assets.
    // Used for both roots: the engine's built-in `shaders/` and the project content folder.
    void     LoadShadersDir(const std::string& dir);
    // Build a renderer pipeline for each loaded shader (sets Shader::rendererHandle). Call once
    // after render init. HotReloadShaders re-reads changed shader files + rebuilds their pipeline.
    void     BuildShaderPipelines(iRender* r);
    void     HotReloadShaders(iRender* r);

    // Scan a content folder (recursively) and load every native asset (.numesh) into the DB,
    // indexed by the GUID stored in the file. Skips GUIDs already registered. Call at startup
    // so meshGuid references in saved worlds resolve.
    void  LoadContentDir(const std::string& dir);

    static std::string NewGuid();              // fresh opaque asset id (for imports)

    // Asset source paths (for the inspector's "locate original" + browser drag&drop -> guid).
    void        SetAssetPath(const std::string& guid, const std::string& path);
    void        MoveAssetPath(const std::string& oldPath, const std::string& newPath);  // on file rename/move
    std::string PathForGuid(const std::string& guid) const;   // "" if unknown
    std::string GuidForPath(const std::string& path) const;   // "" if unknown

    // Live cleanup when a resource is deleted: drop it from the DB so it vanishes from pickers; and
    // reset any LOADED material that references a guid back to defaults + re-Resolve (so the running
    // session updates without a project reload).
    void RemoveByGuid(const std::string& guid);
    void UnlinkGuid(const std::string& guid);

	std::shared_ptr<uint> loadTexture(const std::string& name);
};
}  // namespace nuke

#endif // RESDB_H
