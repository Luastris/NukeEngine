#ifndef RESDB_H
#define RESDB_H
#include "NukeAPI.h"
#include "Mesh.h"
#include "Texture.h"
#include "Material.h"
#include "Shader.h"
#include "AnimClip.h"
#include "AnimSM.h"
#include "BlendSpace.h"
#include "Sequence.h"
#include "Ragdoll.h"
#include "Skeleton.h"
#include "BoneMap.h"
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

// The asset database, addressed by stable GUID (built-ins use "builtin:<name>"). ONE shared
// instance lives in the engine DLL — getSingleton is defined OUT-OF-LINE, never inline, so the
// editor, plugins and the runtime all see the same assets.
class NUKEENGINE_API ResDB
{
    ResDB();
    ~ResDB() {}
public:
    bc::list<Mesh*> meshes;
    bc::list<Texture*> textures;
    bc::list<Material*> materials;
    bc::list<Shader*> shaders;
    bc::list<AnimClip*> clips;                     // animation clips (.nuanim)
    bc::list<Skeleton*> skeletons;                 // skeleton assets (.nuskel)
    bc::list<BoneMap*>  boneMaps;                  // retarget maps (.nubonemap)
    bc::list<AnimSM*>     animSMs;                 // state machines (.nusm)
    bc::list<BlendSpace*> blendSpaces;             // blend spaces (.nublend)
    bc::list<Sequence*>   sequences;               // sequences (.nuseq)
    bc::list<RagdollDef*> ragdolls;                // ragdoll rigs (.nurag)
    bc::list<Atom*> prefabs;

    std::map<std::string, Mesh*>     meshByGuid;   // GUID -> mesh asset
    std::map<std::string, Material*> matByGuid;    // GUID -> material asset
    std::map<std::string, Texture*>  texByGuid;    // GUID -> texture asset
    std::map<std::string, Shader*>   shaderByGuid; // GUID -> shader asset
    std::map<std::string, AnimClip*> clipByGuid;   // GUID -> animation clip
    std::map<std::string, Skeleton*> skelByGuid;   // GUID -> skeleton
    std::map<std::string, BoneMap*>  boneMapByGuid;// GUID -> retarget map
    std::map<std::string, AnimSM*>     smByGuid;   // GUID -> state machine
    std::map<std::string, BlendSpace*> blendByGuid;// GUID -> blend space
    std::map<std::string, Sequence*>   seqByGuid;  // GUID -> sequence
    std::map<std::string, RagdollDef*> ragByGuid;  // GUID -> ragdoll rig
    std::map<std::string, std::string> pathByGuid; // GUID -> source file path (for "locate"/DnD)
    std::map<std::string, std::string> guidByPath; // source file path -> GUID
    std::map<std::string, long long>   assetMtime; // asset path -> last-seen mtime (hot-reload)

    static ResDB* getSingleton();              // single instance (engine DLL)

    Mesh* GetMesh(const std::string& guid);    // nullptr if unknown
    void  RegisterMesh(Mesh* m);               // add to meshes + index by m->guid

    Material* GetMaterial(const std::string& guid);   // nullptr if unknown
    void      RegisterMaterial(Material* m);          // add to materials + index by m->guid

    Texture* GetTexture(const std::string& guid);     // nullptr if unknown
    void     RegisterTexture(Texture* t);             // add to textures + index by t->guid

    Shader*  GetShader(const std::string& guid);      // nullptr if unknown
    void     RegisterShader(Shader* s);               // add to shaders + index by s->guid

    AnimClip* GetClip(const std::string& guid);       // nullptr if unknown
    AnimClip* GetClipByName(const std::string& name); // first clip with this name (Animator states)
    void      RegisterClip(AnimClip* c);              // add to clips + index by c->guid
    Skeleton* GetSkeleton(const std::string& guid);   // nullptr if unknown
    void      RegisterSkeleton(Skeleton* s);          // add to skeletons + index by s->guid

    BoneMap* GetBoneMap(const std::string& guid);     // nullptr if unknown
    void     RegisterBoneMap(BoneMap* b);             // add to boneMaps + index by b->guid

    AnimSM*     GetAnimSM(const std::string& guid);   // nullptr if unknown
    void        RegisterAnimSM(AnimSM* m);            // add to animSMs + index by m->guid
    BlendSpace* GetBlendSpace(const std::string& guid);   // nullptr if unknown
    void        RegisterBlendSpace(BlendSpace* b);    // add to blendSpaces + index by b->guid
    Sequence*   GetSequence(const std::string& guid); // nullptr if unknown
    void        RegisterSequence(Sequence* s);        // add to sequences + index by s->guid
    RagdollDef* GetRagdoll(const std::string& guid);  // nullptr if unknown
    void        RegisterRagdoll(RagdollDef* r);       // add to ragdolls + index by r->guid
    // Scan a dir recursively for "<name>.vs.hlsl" + "<name>.ps.hlsl" pairs -> Shader assets.
    void     LoadShadersDir(const std::string& dir);
    // Build a renderer pipeline for each loaded shader (sets Shader::rendererHandle); call once
    // after render init.
    void     BuildShaderPipelines(iRender* r);
    // Incremental variant: build up to `maxCount` missing pipelines, return how many are still
    // missing (0 = done).
    int      BuildShaderPipelinesStep(iRender* r, int maxCount);
    void     HotReloadShaders(iRender* r);
    // Create an iRender render target for every loaded RenderTexture (sets Texture::rtId). After init.
    void     CreateRenderTextures(iRender* r);
    // Hot-reload edited .numat/.nutex into their LIVE ResDB objects + re-Resolve / re-upload (no restart).
    void     HotReloadAssets(iRender* r);

    // Scan a content folder recursively and load every native asset into the DB, indexed by the
    // GUID stored in the file (already-registered GUIDs are skipped). Call at startup so asset
    // references in saved worlds resolve.
    void  LoadContentDir(const std::string& dir);
    // Same, over the PACKAGE layer stack: raw overlay files load from disk, pak entries from
    // MEMORY (packed content never touches the disk).
    void  LoadContentPackaged();
    // Load ONE content file by disk path (the shared per-extension dispatch of the scans).
    void  LoadContentFile(const std::string& path);
    // Load ONE packed entry from bytes (project-relative path decides the type).
    void  LoadContentEntry(const std::string& rel, const std::string& bytes);
    // Content shaders from the Package layers (pairs + .post.hlsl, built from pak bytes).
    void  LoadShadersPackaged();

    static std::string NewGuid();              // fresh opaque asset id (for imports)

    // Asset source paths (for the inspector's "locate original" + browser drag&drop -> guid).
    void        SetAssetPath(const std::string& guid, const std::string& path);
    void        MoveAssetPath(const std::string& oldPath, const std::string& newPath);  // on file rename/move
    std::string PathForGuid(const std::string& guid) const;   // "" if unknown
    std::string GuidForPath(const std::string& path) const;   // "" if unknown (EXACT key match)
    // Path-FORM-insensitive lookup for a CONTENT-RELATIVE reference ("Tilesets/atlas.nutex"):
    // registered keys are absolute with native slashes, so exact GuidForPath never matches a
    // relative form. Use this wherever assets are referenced by content path.
    std::string GuidForContentPath(const std::string& contentRel) const;

    // Live cleanup when a resource is deleted: drop it from the DB, and reset any LOADED material
    // referencing the guid back to defaults + re-Resolve (no project reload needed).
    void RemoveByGuid(const std::string& guid);
    void UnlinkGuid(const std::string& guid);

	std::shared_ptr<uint> loadTexture(const std::string& name);
};
}  // namespace nuke

#endif // RESDB_H
