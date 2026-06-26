#include "API/Model/Material.h"
#include "API/Model/resdb.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>

namespace nuke {

namespace bfs = boost::filesystem;
using json = nlohmann::json;

Material::Material() {}

void Material::ImportAiMaterial(aiMaterial* m) {
	aiString nm;
	if (m->Get(AI_MATKEY_NAME, nm) == AI_SUCCESS) matName = nm.C_Str();

	aiColor3D col(1.f, 1.f, 1.f);
	if (m->Get(AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS) { color[0] = col.r; color[1] = col.g; color[2] = col.b; }
	float opacity = 1.f;
	if (m->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) color[3] = opacity;

	aiMat = m;   // textures are converted + assigned (as .nutex GUIDs) by the importer
}

void Material::Resolve()
{
	ResDB* db = ResDB::getSingleton();
	if (!diff && !diffuseGuid.empty())  diff = db->GetTexture(diffuseGuid);
	if (!norm && !normalGuid.empty())   norm = db->GetTexture(normalGuid);
	if (!spec && !specularGuid.empty()) spec = db->GetTexture(specularGuid);
	if (!shader && !shaderGuid.empty()) shader = db->GetShader(shaderGuid);
}

bool Material::SaveToFile(const std::string& path) const
{
	json j;
	j["guid"]     = guid;
	j["name"]     = matName;
	j["shader"]   = shaderGuid;
	j["color"]    = { color[0], color[1], color[2], color[3] };
	j["diffuse"]  = diffuseGuid;
	j["normal"]   = normalGuid;
	j["specular"] = specularGuid;
	boost::filesystem::path p(path);
	boost::filesystem::ofstream f(p);
	if (!f) return false;
	f << j.dump(2);
	return (bool)f;
}

Material* Material::LoadFromFile(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream f(p);
	if (!f) return nullptr;
	json j = json::parse(f, nullptr, false);
	if (j.is_discarded()) return nullptr;

	Material* m = new Material();
	m->guid         = j.value("guid", std::string());
	m->matName      = j.value("name", std::string());
	m->shaderGuid   = j.value("shader", std::string("world"));
	m->diffuseGuid  = j.value("diffuse", std::string());
	m->normalGuid   = j.value("normal", std::string());
	m->specularGuid = j.value("specular", std::string());
	if (j.contains("color") && j["color"].is_array() && j["color"].size() == 4)
		for (int i = 0; i < 4; ++i) m->color[i] = j["color"][i].get<float>();
	return m;
}
}  // namespace nuke
