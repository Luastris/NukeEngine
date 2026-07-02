#include "API/Model/Resource.h"
#include "API/Model/resdb.h"

namespace nuke {

static std::string PathOf(const std::string& guid)
{
	auto& db = *ResDB::getSingleton();
	auto it = db.pathByGuid.find(guid);
	return it != db.pathByGuid.end() ? it->second : std::string();
}

static Resource Describe(const std::string& guid)
{
	Resource r;
	r.guid = guid;
	auto& db = *ResDB::getSingleton();
	if (Mesh* m = db.GetMesh(guid))              { r.type = "mesh";     r.name = m->name; }
	else if (Material* mt = db.GetMaterial(guid)) { r.type = "material"; r.name = mt->matName; }
	else if (db.GetTexture(guid))                 { r.type = "texture"; }   // textures carry no display name
	else if (Shader* s = db.GetShader(guid))      { r.type = "shader";   r.name = s->name; }
	else { r.guid.clear(); return r; }            // unknown -> invalid descriptor
	r.path = PathOf(guid);
	return r;
}

Resource Resource::Find(const std::string& guid) { return Describe(guid); }

Resource Resource::FindByPath(const std::string& path)
{
	auto& db = *ResDB::getSingleton();
	auto it = db.guidByPath.find(path);
	return it != db.guidByPath.end() ? Describe(it->second) : Resource();
}

std::vector<Resource> Resource::All(const std::string& type)
{
	auto& db = *ResDB::getSingleton();
	std::vector<Resource> out;
	auto want = [&](const char* t) { return type.empty() || type == t; };
	if (want("mesh"))     for (auto& kv : db.meshByGuid)   out.push_back(Describe(kv.first));
	if (want("material")) for (auto& kv : db.matByGuid)    out.push_back(Describe(kv.first));
	if (want("texture"))  for (auto& kv : db.texByGuid)    out.push_back(Describe(kv.first));
	if (want("shader"))   for (auto& kv : db.shaderByGuid) out.push_back(Describe(kv.first));
	return out;
}

Mesh*     Resource::GetMesh(const std::string& guid)     { return ResDB::getSingleton()->GetMesh(guid); }
Material* Resource::GetMaterial(const std::string& guid) { return ResDB::getSingleton()->GetMaterial(guid); }
Texture*  Resource::GetTexture(const std::string& guid)  { return ResDB::getSingleton()->GetTexture(guid); }
Shader*   Resource::GetShader(const std::string& guid)   { return ResDB::getSingleton()->GetShader(guid); }

}  // namespace nuke
