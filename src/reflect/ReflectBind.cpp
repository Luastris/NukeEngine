#include "reflect/ReflectBind.h"
#include "API/Model/Atom.h"
#include "API/Model/Component.h"
#include "API/Model/World.h"
#include "API/Model/resdb.h"       // object handles: created assets register into ResDB
#include "API/Model/Material.h"
#include "API/Model/Texture.h"
#include "API/Model/Mesh.h"
#include "API/Model/AnimClip.h"
#include "API/Model/Shader.h"
#include "API/Model/MeshRenderer.h"   // asset-field write hook re-resolves mesh/material
#include "interface/AppInstance.h"
#include "render/irender.h"           // invalidateTexture after script-set pixels
#include <boost/filesystem.hpp>
#include <cmath>
#include <cstring>
#include <map>

namespace nuke {

const Field* Reflect_FindField(const TypeInfo* type, const std::string& name)
{
	while (type)
	{
		for (const Field& f : type->fields)
			if (f.name == name) return &f;
		type = type->base.empty() ? nullptr : Registry_Find(type->base);
	}
	return nullptr;
}

const Method* Reflect_FindMethod(const TypeInfo* type, const std::string& name)
{
	while (type)
	{
		for (const Method& m : type->methods)
			if (m.name == name) return &m;
		type = type->base.empty() ? nullptr : Registry_Find(type->base);
	}
	return nullptr;
}

bool Reflect_Invoke(void* obj, const Method& m, const ReflectValue* args, std::size_t n, ReflectValue& ret)
{
	if (!m.invoke) return false;
	if (!obj && !m.isStatic) return false;   // instance methods need an instance; statics take null
	ret = ReflectValue();
	return m.invoke(obj, args, n, ret);
}

ReflectValue Reflect_GetField(void* obj, const Field& f)
{
	ReflectValue out;
	out.type = f.type;
	void* p = f.addr ? f.addr(obj) : nullptr;
	if (!p) { out.type = FT::Unknown; return out; }
	switch (f.type)
	{
		case FT::Bool:   out.b   = *(bool*)p; break;
		case FT::Int:    out.num = *(int*)p; break;
		case FT::Float:  out.num = *(float*)p; break;
		case FT::Double: out.num = *(double*)p; break;
		case FT::String: out.str = *(std::string*)p; break;
		case FT::Vec2:  { Vector2& v = *(Vector2*)p; out.v[0] = v.x; out.v[1] = v.y; break; }
		case FT::Vec3:  { Vector3& v = *(Vector3*)p; out.v[0] = v.x; out.v[1] = v.y; out.v[2] = v.z; break; }
		case FT::Vec4:  { Vector4& v = *(Vector4*)p; out.v[0] = v.x; out.v[1] = v.y; out.v[2] = v.z; out.v[3] = v.w; break; }
		case FT::Quat:  { Quaternion& q = *(Quaternion*)p; out.v[0] = q.x; out.v[1] = q.y; out.v[2] = q.z; out.v[3] = q.w; break; }
		case FT::Color: { Color& c = *(Color*)p; out.v[0] = c.r; out.v[1] = c.g; out.v[2] = c.b; out.v[3] = c.a; break; }
		default: out.type = FT::Unknown; break;
	}
	return out;
}

bool Reflect_SetField(void* obj, const Field& f, const ReflectValue& v)
{
	void* p = f.addr ? f.addr(obj) : nullptr;
	if (!p) return false;
	switch (f.type)
	{
		case FT::Bool:   *(bool*)p        = v.b; return true;
		case FT::Int:    *(int*)p         = (int)v.num; return true;
		case FT::Float:  *(float*)p       = (float)v.num; return true;
		case FT::Double: *(double*)p      = v.num; return true;
		case FT::String: *(std::string*)p = v.str; return true;
		case FT::Vec2:  { Vector2& d = *(Vector2*)p; d.x = v.v[0]; d.y = v.v[1]; return true; }
		case FT::Vec3:  { Vector3& d = *(Vector3*)p; d.x = v.v[0]; d.y = v.v[1]; d.z = v.v[2]; return true; }
		case FT::Vec4:  { Vector4& d = *(Vector4*)p; d.x = v.v[0]; d.y = v.v[1]; d.z = v.v[2]; d.w = v.v[3]; return true; }
		case FT::Quat:  { Quaternion& q = *(Quaternion*)p; q.x = v.v[0]; q.y = v.v[1]; q.z = v.v[2]; q.w = v.v[3]; return true; }
		case FT::Color: { Color& c = *(Color*)p; c.r = v.v[0]; c.g = v.v[1]; c.b = v.v[2]; c.a = v.v[3]; return true; }
		default: return false;
	}
}

std::vector<std::string> Reflect_ComponentTypes()
{
	std::vector<std::string> out;
	for (TypeInfo* t : Registry_All())
		if (t && t->create && t->base == "Component") out.push_back(t->name);
	return out;
}

Component* Reflect_FindComponent(Atom* atom, const std::string& typeName)
{
	if (!atom || typeName.empty()) return nullptr;
	for (Component* c : atom->components)
		if (c && c->GetType() && c->GetType()->name == typeName) return c;
	return nullptr;
}

Component* Reflect_AddComponent(Atom* atom, const std::string& typeName)
{
	if (!atom) return nullptr;
	TypeInfo* ti = Registry_Find(typeName);
	if (!ti || !ti->create || ti->base != "Component") return nullptr;   // same guard as the editor menu
	Component* c = (Component*)ti->create();
	atom->AddComponent(c);   // Init: sets atom/transform + registers in atom->components
	return c;
}

Component* Reflect_ResolveComponent(unsigned long atomId, unsigned long componentId)
{
	World* w = AppInstance::GetSingleton()->currentScene;
	if (!w) return nullptr;
	Atom* a = w->GetById((long)atomId);
	if (!a) return nullptr;
	for (Component* c : a->components)
		if (c && c->id.id == componentId) return c;
	return nullptr;
}

// ---- reflected OBJECTS (task #67) --------------------------------------------------------------
// The engine-owned handle table: scripts hold ONLY ids. Handles never dangle into freed
// memory by construction: created assets register into ResDB (which owns them forever,
// like imported ones), wrapped instances belong to their owners — a wrapped material dies
// only with its MeshRenderer, whose (atom, component) pair the scripts also track.

namespace {
struct ObjHandle
{
	void*       obj = nullptr;
	TypeInfo*   ti  = nullptr;
	std::string guid;   // "" for non-asset instances
};
std::map<unsigned long, ObjHandle>& ObjTable()
{
	static std::map<unsigned long, ObjHandle> s;
	return s;
}
std::map<void*, unsigned long>& ObjIndex()   // instance -> existing handle (wrap dedup)
{
	static std::map<void*, unsigned long> s;
	return s;
}
unsigned long NextObjId()
{
	static unsigned long id = 1;
	return id++;
}
unsigned long StoreHandle(void* obj, TypeInfo* ti, const std::string& guid)
{
	if (!obj || !ti) return 0;
	auto it = ObjIndex().find(obj);
	if (it != ObjIndex().end()) return it->second;
	unsigned long id = NextObjId();
	ObjTable()[id] = { obj, ti, guid };
	ObjIndex()[obj] = id;
	return id;
}
}  // namespace

bool Reflect_IsAssetType(const std::string& typeName)
{
	// The ResDB-backed asset classes: guid-identified, findable by name/guid, materialized
	// on Create. Keep in sync with Reflect_CreateObject / Reflect_ObjectFromGuid below.
	return typeName == "Material" || typeName == "Texture" || typeName == "Mesh"
	    || typeName == "AnimClip" || typeName == "Shader";
}

unsigned long Reflect_CreateObject(const std::string& typeName)
{
	TypeInfo* ti = Registry_Find(typeName);
	if (!ti || !ti->create) return 0;
	void* obj = ti->create();
	if (!obj) return 0;

	// Asset types register into ResDB under a fresh guid — a script-made material/texture
	// behaves exactly like an imported one (components resolve it by guid, it persists).
	std::string guid;
	ResDB* db = ResDB::getSingleton();
	if      (typeName == "Material") { Material* m = (Material*)obj; m->guid = ResDB::NewGuid(); m->matName = "Script Material"; db->RegisterMaterial(m); guid = m->guid; }
	else if (typeName == "Texture")  { Texture*  t = (Texture*)obj;  t->guid = ResDB::NewGuid(); db->RegisterTexture(t);  guid = t->guid; }
	else if (typeName == "Mesh")     { Mesh*     m = (Mesh*)obj;     m->guid = ResDB::NewGuid(); db->RegisterMesh(m);     guid = m->guid; }
	else if (typeName == "AnimClip") { AnimClip* c = (AnimClip*)obj; c->guid = ResDB::NewGuid(); db->RegisterClip(c);     guid = c->guid; }
	else if (typeName == "Shader")   { Shader*   s = (Shader*)obj;   s->guid = ResDB::NewGuid(); db->RegisterShader(s);   guid = s->guid; }
	return StoreHandle(obj, ti, guid);
}

unsigned long Reflect_WrapObject(void* obj, const std::string& typeName)
{
	if (!obj) return 0;
	TypeInfo* ti = Registry_Find(typeName);
	if (!ti) return 0;
	// Assets carry their guid on the instance — pick it up so ObjectGuid works for wraps too.
	std::string guid;
	if      (typeName == "Material") guid = ((Material*)obj)->guid;
	else if (typeName == "Texture")  guid = ((Texture*)obj)->guid;
	else if (typeName == "Mesh")     guid = ((Mesh*)obj)->guid;
	else if (typeName == "AnimClip") guid = ((AnimClip*)obj)->guid;
	else if (typeName == "Shader")   guid = ((Shader*)obj)->guid;
	return StoreHandle(obj, ti, guid);
}

// ---- ObjectRef channel (FT::ObjectRef in [[nuke::func]] signatures) ----------------------------

unsigned long Reflect_WrapObjectPtr(void* obj, const char* typeName)
{
	return (obj && typeName) ? Reflect_WrapObject(obj, typeName) : 0;
}

void* Reflect_ObjectPtr(unsigned long id, const char* typeName)
{
	auto it = ObjTable().find(id);
	if (it == ObjTable().end() || !typeName || !*typeName) return nullptr;
	// IS-A check: the handle's registered type must be `typeName` or derive from it.
	for (TypeInfo* t = it->second.ti; t; t = t->base.empty() ? nullptr : Registry_Find(t->base))
		if (t->name == typeName) return it->second.obj;
	return nullptr;
}

void Reflect_DropObject(void* obj)
{
	if (!obj) return;
	auto idx = ObjIndex().find(obj);
	if (idx == ObjIndex().end()) return;
	ObjTable().erase(idx->second);   // every script-held handle id goes stale-safe dead
	ObjIndex().erase(idx);
}

unsigned long Reflect_ObjectFromGuid(const std::string& guid)
{
	if (guid.empty()) return 0;
	ResDB* db = ResDB::getSingleton();
	if (Material* m = db->GetMaterial(guid)) return Reflect_WrapObject(m, "Material");
	if (Texture*  t = db->GetTexture(guid))  return Reflect_WrapObject(t, "Texture");
	if (Mesh*     m = db->GetMesh(guid))     return Reflect_WrapObject(m, "Mesh");
	if (AnimClip* c = db->GetClip(guid))     return Reflect_WrapObject(c, "AnimClip");
	if (Shader*   s = db->GetShader(guid))   return Reflect_WrapObject(s, "Shader");
	return 0;
}

std::string Reflect_ObjectGuid(unsigned long id)
{
	auto it = ObjTable().find(id);
	return it != ObjTable().end() ? it->second.guid : std::string();
}

const char* Reflect_ObjectType(unsigned long id)
{
	auto it = ObjTable().find(id);
	return it != ObjTable().end() ? it->second.ti->name.c_str() : "";
}

ReflectValue Reflect_ObjectGet(unsigned long id, const std::string& field)
{
	auto it = ObjTable().find(id);
	if (it == ObjTable().end()) return ReflectValue();
	const Field* f = Reflect_FindField(it->second.ti, field);
	return f ? Reflect_GetField(it->second.obj, *f) : ReflectValue();
}

bool Reflect_ObjectSet(unsigned long id, const std::string& field, const ReflectValue& v)
{
	auto it = ObjTable().find(id);
	if (it == ObjTable().end()) return false;
	const Field* f = Reflect_FindField(it->second.ti, field);
	if (!f || !Reflect_SetField(it->second.obj, *f, v)) return false;
	// An asset REFERENCE changed (shader/texture guids): re-resolve the runtime pointers
	// right away — a script-assigned shader/map takes effect this frame, not on reload.
	if (!f->asset.empty() && it->second.ti->name == "Material")
		((Material*)it->second.obj)->Resolve();
	return true;
}

bool Reflect_ObjectInvoke(unsigned long id, const std::string& method,
                          const ReflectValue* args, std::size_t n, ReflectValue& ret)
{
	auto it = ObjTable().find(id);
	if (it == ObjTable().end()) return false;
	const Method* m = Reflect_FindMethod(it->second.ti, method);
	return m && Reflect_Invoke(it->second.obj, *m, args, n, ret);
}

unsigned long Reflect_FindAsset(const std::string& typeName, const std::string& name)
{
	if (name.empty()) return 0;
	ResDB* db = ResDB::getSingleton();
	auto ci = [](const std::string& a, const std::string& b) {
		if (a.size() != b.size()) return false;
		for (std::size_t i = 0; i < a.size(); ++i)
			if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
		return true;
	};
	const std::string& t = typeName;
	if (t.empty() || t == "Shader")
	{
		if (Shader* s = db->GetShader(name)) return Reflect_WrapObject(s, "Shader");
		for (Shader* s : db->shaders) if (s && ci(s->name, name)) return Reflect_WrapObject(s, "Shader");
	}
	if (t.empty() || t == "Material")
		for (Material* m : db->materials) if (m && ci(m->matName, name)) return Reflect_WrapObject(m, "Material");
	if (t.empty() || t == "Mesh")
	{
		if (Mesh* m = db->GetMesh(name)) return Reflect_WrapObject(m, "Mesh");   // "builtin:cube"
		for (Mesh* m : db->meshes) if (m && ci(m->name, name)) return Reflect_WrapObject(m, "Mesh");
	}
	if (t.empty() || t == "AnimClip")
		for (AnimClip* c : db->clips) if (c && ci(c->name, name)) return Reflect_WrapObject(c, "AnimClip");
	if (t.empty() || t == "Texture")
		for (Texture* x : db->textures) if (x && ci(x->name, name)) return Reflect_WrapObject(x, "Texture");
	// Fall back to the FILE STEM of any tracked asset path.
	for (auto& kv : db->pathByGuid)
		if (ci(boost::filesystem::path(kv.second).stem().string(), name))
			if (unsigned long id = Reflect_ObjectFromGuid(kv.first)) return id;
	return 0;
}

void* Reflect_SubObject(Component* c, const std::string& path, TypeInfo** ti)
{
	if (c && path == "material")
		if (MeshRenderer* mr = dynamic_cast<MeshRenderer*>(c))
		{
			if (ti) *ti = Registry_Find("Material");
			return mr->mat;   // may be null (no material resolved yet) — callers degrade
		}
	return nullptr;
}

unsigned long Reflect_ComponentObject(unsigned long atomId, unsigned long compId, const std::string& path)
{
	Component* c = Reflect_ResolveComponent(atomId, compId);
	TypeInfo* ti = nullptr;
	void* obj = Reflect_SubObject(c, path, &ti);
	return (obj && ti) ? Reflect_WrapObject(obj, ti->name) : 0;
}

void Reflect_ComponentFieldChanged(Component* c, const Field& f)
{
	if (!c || f.asset.empty()) return;
	// Asset-ref writes take effect NOW: re-resolve the runtime pointers behind the guid.
	if (MeshRenderer* mr = dynamic_cast<MeshRenderer*>(c))
	{
		if (f.name == "meshGuid") mr->mesh = ResDB::getSingleton()->GetMesh(mr->meshGuid);
		if (f.name == "matGuid")
			if (Material* asset = ResDB::getSingleton()->GetMaterial(mr->matGuid))
			{
				if (mr->mat) { Reflect_DropObject(mr->mat); delete mr->mat; }   // kill wrapped handles first
				mr->mat = asset->Clone();   // instance semantics, same as world load
			}
	}
}

bool Reflect_SetTexturePixels(unsigned long id, int w, int h, const void* rgba, std::size_t len)
{
	if (std::string(Reflect_ObjectType(id)) != "Texture") return false;
	if (!rgba || w <= 0 || h <= 0 || len != (std::size_t)w * h * 4) return false;
	auto it = ObjTable().find(id);
	if (it == ObjTable().end()) return false;
	Texture* t = (Texture*)it->second.obj;
	t->width = w; t->height = h;
	t->format = Texture::FMT_RGBA8;
	t->mipCount = 1;
	const unsigned char* p = (const unsigned char*)rgba;
	t->pixels.assign(p, p + len);
	if (iRender* r = AppInstance::GetSingleton()->render) r->invalidateTexture(t);   // live refresh
	return true;
}

bool Reflect_SetMeshGeometry(unsigned long id, int numVerts,
                             const float* verts, const float* normals, const float* uvs)
{
	if (std::string(Reflect_ObjectType(id)) != "Mesh") return false;
	if (!verts || numVerts <= 0 || numVerts % 3 != 0) return false;   // unindexed triangle list
	auto it = ObjTable().find(id);
	if (it == ObjTable().end()) return false;
	Mesh* m = (Mesh*)it->second.obj;

	float* v = new float[(std::size_t)numVerts * 3];
	float* n = new float[(std::size_t)numVerts * 3];
	float* u = new float[(std::size_t)numVerts * 2];
	memcpy(v, verts, (std::size_t)numVerts * 3 * sizeof(float));
	if (normals)
		memcpy(n, normals, (std::size_t)numVerts * 3 * sizeof(float));
	else
		for (int t = 0; t < numVerts; t += 3)   // flat per-triangle normals
		{
			const float* a = v + (std::size_t)t * 3;
			const float* b = a + 3;
			const float* c = a + 6;
			float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			float nx = e1[1] * e2[2] - e1[2] * e2[1];
			float ny = e1[2] * e2[0] - e1[0] * e2[2];
			float nz = e1[0] * e2[1] - e1[1] * e2[0];
			float len = sqrtf(nx * nx + ny * ny + nz * nz);
			if (len > 1e-20f) { nx /= len; ny /= len; nz /= len; }
			for (int k = 0; k < 3; ++k)
			{
				n[(std::size_t)(t + k) * 3 + 0] = nx;
				n[(std::size_t)(t + k) * 3 + 1] = ny;
				n[(std::size_t)(t + k) * 3 + 2] = nz;
			}
		}
	if (uvs) memcpy(u, uvs, (std::size_t)numVerts * 2 * sizeof(float));
	else     memset(u, 0, (std::size_t)numVerts * 2 * sizeof(float));

	delete[] m->vertexArray;
	delete[] m->normalArray;
	delete[] m->uvArray;
	m->vertexArray = v;
	m->normalArray = n;
	m->uvArray     = u;
	m->numVerts    = numVerts;
	m->boundsValid = false;
	m->version++;   // renderer re-uploads its cached GPU buffers on mismatch
	return true;
}

}  // namespace nuke
