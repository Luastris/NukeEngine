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
	if (m->Get(AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS) { color.r = col.r; color.g = col.g; color.b = col.b; }
	float opacity = 1.f;
	if (m->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) color.a = opacity;

	// PBR scalar factors (glTF / PBR materials).
	float mf = 0.f, rf = 1.f, sf = 1.f;
	if (m->Get(AI_MATKEY_METALLIC_FACTOR, mf)  == AI_SUCCESS) metallic  = mf;
	if (m->Get(AI_MATKEY_ROUGHNESS_FACTOR, rf) == AI_SUCCESS) roughness = rf;
#ifdef AI_MATKEY_SPECULAR_FACTOR
	if (m->Get(AI_MATKEY_SPECULAR_FACTOR, sf)  == AI_SUCCESS) specular  = sf;   // KHR_materials_specular
#endif
	aiColor3D ec(0.f, 0.f, 0.f);
	if (m->Get(AI_MATKEY_COLOR_EMISSIVE, ec) == AI_SUCCESS)
	{
		emissive.r = ec.r; emissive.g = ec.g; emissive.b = ec.b;
		if (ec.r > 0.f || ec.g > 0.f || ec.b > 0.f) emissiveIntensity = 1.0f;
	}

	aiMat = m;   // textures are converted + assigned (as .nutex GUIDs) by the importer
}

Material* Material::Clone() const
{
	Material* m = new Material();
	m->guid        = guid;
	m->matName     = matName;
	m->color = color;
	m->diffuseGuid = diffuseGuid;
	m->normalGuid  = normalGuid;
	m->specularGuid= specularGuid;
	m->metalRoughGuid = metalRoughGuid;
	m->occlusionGuid  = occlusionGuid;
	m->emissiveGuid   = emissiveGuid;
	m->metallic    = metallic;
	m->roughness   = roughness;
	m->specular    = specular;
	m->emissive = emissive;
	m->emissiveIntensity = emissiveIntensity;
	m->castShadows = castShadows;
	m->blendMode   = blendMode;
	m->shaderGuid  = shaderGuid;
	m->props       = props;
	m->Resolve();   // bind diff/norm/spec/mr/ao/em/shader pointers from ResDB
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
	if (metalRoughGuid.empty())                          mr = nullptr;
	else if (!mr     || mr->guid     != metalRoughGuid) mr     = db->GetTexture(metalRoughGuid);
	if (occlusionGuid.empty())                           ao = nullptr;
	else if (!ao     || ao->guid     != occlusionGuid)  ao     = db->GetTexture(occlusionGuid);
	if (emissiveGuid.empty())                            em = nullptr;
	else if (!em     || em->guid     != emissiveGuid)   em     = db->GetTexture(emissiveGuid);
	if (shaderGuid.empty())                              shader = nullptr;
	else if (!shader || shader->guid != shaderGuid)     shader = db->GetShader(shaderGuid);
}

bool Material::SaveToFile(const std::string& path) const
{
	json j;
	j["guid"]     = guid;
	j["name"]     = matName;
	j["shader"]   = shaderGuid;
	j["color"]    = { color.r, color.g, color.b, color.a };
	j["diffuse"]  = diffuseGuid;
	j["normal"]   = normalGuid;
	j["specular"] = specularGuid;
	j["metalRough"] = metalRoughGuid;
	j["occlusion"]  = occlusionGuid;
	j["emissiveMap"]= emissiveGuid;
	j["metallic"]   = metallic;
	j["roughness"]  = roughness;
	j["specularFactor"] = specular;
	j["emissive"]   = { emissive.r, emissive.g, emissive.b };
	j["emissiveIntensity"] = emissiveIntensity;
	j["castShadows"] = castShadows;
	j["blendMode"]   = blendMode;
	boost::filesystem::path p(path);
	boost::filesystem::ofstream f(p);
	if (!f) return false;
	f << j.dump(2);
	return (bool)f;
}

Material* Material::LoadFromFile(const std::string& path)
{
	boost::filesystem::ifstream f{boost::filesystem::path(path)};   // brace-init: vexing parse
	if (!f) return nullptr;
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return LoadFromString(text);
}

Material* Material::LoadFromString(const std::string& text)
{
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded()) return nullptr;

	Material* m = new Material();
	m->guid         = j.value("guid", std::string());
	m->matName      = j.value("name", std::string());
	m->shaderGuid   = j.value("shader", std::string("world"));
	m->diffuseGuid  = j.value("diffuse", std::string());
	m->normalGuid   = j.value("normal", std::string());
	m->specularGuid = j.value("specular", std::string());
	m->metalRoughGuid = j.value("metalRough", std::string());
	m->occlusionGuid  = j.value("occlusion", std::string());
	m->emissiveGuid   = j.value("emissiveMap", std::string());
	m->metallic     = j.value("metallic", 0.0f);
	m->roughness    = j.value("roughness", 0.6f);
	m->specular     = j.value("specularFactor", 1.0f);
	m->emissiveIntensity = j.value("emissiveIntensity", 0.0f);
	m->castShadows = j.value("castShadows", true);
	m->blendMode   = j.value("blendMode", 0);
	if (j.contains("color") && j["color"].is_array() && j["color"].size() == 4)
	{
		m->color.r = j["color"][0]; m->color.g = j["color"][1];
		m->color.b = j["color"][2]; m->color.a = j["color"][3];
	}
	if (j.contains("emissive") && j["emissive"].is_array() && j["emissive"].size() == 3)
	{
		m->emissive.r = j["emissive"][0]; m->emissive.g = j["emissive"][1]; m->emissive.b = j["emissive"][2];
	}
	return m;
}
}  // namespace nuke
