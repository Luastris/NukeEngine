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

Material* Material::Clone() const
{
	Material* m = new Material();
	m->guid        = guid;
	m->matName     = matName;
	for (int i = 0; i < 4; ++i) m->color[i] = color[i];
	m->diffuseGuid = diffuseGuid;
	m->normalGuid  = normalGuid;
	m->specularGuid= specularGuid;
	m->shaderGuid  = shaderGuid;
	m->props       = props;
	m->Resolve();   // bind diff/norm/spec/shader pointers from ResDB
	return m;
}

void Material::Resolve()
{
	ResDB* db = ResDB::getSingleton();
	// Re-bind when the pointer is missing OR no longer matches the guid (e.g. a material instance
	// whose shaderGuid/texture was overridden after the clone). A stale guard here caused the
	// shader override to be ignored on world load (Player + PIE stop) — kept rendering "world".
	if (diffuseGuid.empty())                              diff = nullptr;
	else if (!diff   || diff->guid   != diffuseGuid)     diff   = db->GetTexture(diffuseGuid);
	if (normalGuid.empty())                              norm = nullptr;
	else if (!norm   || norm->guid   != normalGuid)     norm   = db->GetTexture(normalGuid);
	if (specularGuid.empty())                            spec = nullptr;
	else if (!spec   || spec->guid   != specularGuid)   spec   = db->GetTexture(specularGuid);
	if (shaderGuid.empty())                              shader = nullptr;
	else if (!shader || shader->guid != shaderGuid)     shader = db->GetShader(shaderGuid);
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
