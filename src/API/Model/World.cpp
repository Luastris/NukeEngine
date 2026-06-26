#include "API/Model/World.h"
#include "render/irender.h"
#include "API/Model/Camera.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Mesh.h"
#include "API/Model/UnknownComponent.h"
#include "API/Model/Prefab.h"
#include "interface/Modular.h"
#include "reflect/ReflectJson.h"
#include <boost/filesystem/fstream.hpp>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

namespace nuke {

World::World() : name("Default scene"), hierarchy(new bc::list<Atom*>()) {
	NukeReflectInit();   // ensure reflection schemas are registered (forces Reflect.gen.obj to link)
	std::cout << "[World]\t\t\t" << "This:" << this << ", Hierarchy is " << hierarchy << ", Hierarchy size: " << hierarchy->size() << std::endl;
}

// --- ray picking (editor viewport click-to-select) ---

static glm::quat ToGlmQ(const Quaternion& q) { return glm::quat((float)q.w, (float)q.x, (float)q.y, (float)q.z); }

static bool RayAABB(const glm::vec3& o, const glm::vec3& d, const glm::vec3& mn, const glm::vec3& mx, float& tHit)
{
	float tmin = -1e30f, tmax = 1e30f;
	for (int i = 0; i < 3; ++i)
	{
		if (std::fabs(d[i]) < 1e-8f) { if (o[i] < mn[i] || o[i] > mx[i]) return false; }
		else
		{
			float inv = 1.0f / d[i];
			float t1 = (mn[i] - o[i]) * inv, t2 = (mx[i] - o[i]) * inv;
			if (t1 > t2) std::swap(t1, t2);
			tmin = std::max(tmin, t1); tmax = std::min(tmax, t2);
			if (tmin > tmax) return false;
		}
	}
	tHit = (tmin >= 0.0f) ? tmin : tmax;
	return tHit >= 0.0f;
}

static void PickRec(bc::list<Atom*>& gos, const glm::vec3& ro, const glm::vec3& rd, float& bestDist, Atom*& best)
{
	for (auto go : gos)
	{
		if (auto* mr = go->GetComponent<MeshRenderer>())
		{
			Mesh* m = mr->mesh;
			if (m && m->vertexArray && m->numVerts > 0)
			{
				Transform& t = go->GetTransform();
				Vector3 p = t.globalPosition(); Quaternion q = t.globalRotation(); Vector3 s = t.globalScale();
				glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3((float)p.x, (float)p.y, (float)p.z))
				                * glm::mat4_cast(ToGlmQ(q))
				                * glm::scale(glm::mat4(1.0f), glm::vec3((float)s.x, (float)s.y, (float)s.z));
				glm::mat4 inv = glm::inverse(world);
				glm::vec3 lo = glm::vec3(inv * glm::vec4(ro, 1.0f));
				glm::vec3 ld = glm::vec3(inv * glm::vec4(rd, 0.0f));
				glm::vec3 mn(1e30f), mx(-1e30f);
				for (int i = 0; i < m->numVerts; ++i)
				{
					glm::vec3 v(m->vertexArray[i * 3], m->vertexArray[i * 3 + 1], m->vertexArray[i * 3 + 2]);
					mn = glm::min(mn, v); mx = glm::max(mx, v);
				}
				float tLocal;
				if (RayAABB(lo, ld, mn, mx, tLocal))
				{
					glm::vec3 worldHit = glm::vec3(world * glm::vec4(lo + tLocal * ld, 1.0f));
					float dist = glm::length(worldHit - ro);
					if (dist < bestDist) { bestDist = dist; best = go; }
				}
			}
		}
		if (go->children.size() > 0) PickRec(go->children, ro, rd, bestDist, best);
	}
}

Atom* World::Pick(const Vector3& origin, const Vector3& dir)
{
	glm::vec3 ro((float)origin.x, (float)origin.y, (float)origin.z);
	glm::vec3 rd = glm::normalize(glm::vec3((float)dir.x, (float)dir.y, (float)dir.z));
	float bestDist = 1e30f; Atom* best = nullptr;
	if (hierarchy) PickRec(*hierarchy, ro, rd, bestDist, best);
	return best;
}

Atom* World::Get(const char* name)
{
	for (auto go : GetHierarchy())
		if (go->GetName() == name)
			return go;
	return nullptr;
}

bc::list<Atom*>& World::GetHierarchy()
{
	return *hierarchy;
}

void World::Add(Atom* go)
{
	hierarchy->push_back(go);
}

void World::Start()
{

}

void World::Update()
{
	for (Atom* go : *hierarchy)
	{
		go->Update();
	}
}

// --- render pass (one render per camera) ---

static void CollectCameras(bc::list<Atom*>& gos, std::vector<Camera*>& out)
{
	for (auto go : gos)
	{
		if (auto* c = go->GetComponent<Camera>())
			out.push_back(c);
		if (go->children.size() > 0)
			CollectCameras(go->children, out);
	}
}

static void RenderMeshes(bc::list<Atom*>& gos, iRender* r)
{
	for (auto go : gos)
	{
		if (auto* mr = go->GetComponent<MeshRenderer>())
		{
			if (mr->enabled && mr->mesh)
			{
				Transform& t = go->GetTransform();
				Vector3    p = t.globalPosition();
				Quaternion q = t.globalRotation();
				Vector3    s = t.globalScale();
				float pos[3]   = { (float)p.x, (float)p.y, (float)p.z };
				float quat[4]  = { (float)q.x, (float)q.y, (float)q.z, (float)q.w };
				float scale[3] = { (float)s.x, (float)s.y, (float)s.z };
				r->renderObject(mr->mesh, mr->mat, pos, quat, scale);
			}
		}
		if (go->children.size() > 0)
			RenderMeshes(go->children, r);
	}
}

void World::Render(iRender* r)
{
	if (!r) return;

	std::vector<Camera*> cams;
	CollectCameras(*hierarchy, cams);
	std::sort(cams.begin(), cams.end(), [](Camera* a, Camera* b) { return a->depth < b->depth; });

	const bool editor = AppInstance::GetSingleton()->isEditor();
	for (Camera* cam : cams)
	{
		if (!cam->transform) continue;
		// In the editor the world is drawn ONLY into off-screen RTs (the viewport panel + the
		// selected-camera preview). A world camera with target 0 would paint the backbuffer
		// full-window — i.e. make the editor look like the Player. Skip those here; they render
		// normally in the Player (isEditor() == false).
		if (editor && cam->renderTarget == 0) continue;
		NukeCameraDesc d;
		d.target = cam->renderTarget;
		d.vpW = 0; d.vpH = 0; // renderer uses the target's full size
		d.clear[0] = cam->clearColor[0]; d.clear[1] = cam->clearColor[1];
		d.clear[2] = cam->clearColor[2]; d.clear[3] = cam->clearColor[3];
		Vector3 cp = cam->transform->globalPosition();
		Vector3 cf = cam->transform->direction();
		Vector3 cu = cam->transform->up();
		d.camPos[0] = (float)cp.x; d.camPos[1] = (float)cp.y; d.camPos[2] = (float)cp.z;
		d.camFwd[0] = (float)cf.x; d.camFwd[1] = (float)cf.y; d.camFwd[2] = (float)cf.z;
		d.camUp[0]  = (float)cu.x; d.camUp[1]  = (float)cu.y; d.camUp[2]  = (float)cu.z;
		d.fov   = (float)cam->fov * 0.01745329252f; // degrees -> radians
		d.nearZ = cam->_near;
		d.farZ  = cam->_far;

		r->beginCamera(d);
		RenderMeshes(*hierarchy, r);
		r->endCamera();
	}
}

// --- scene serialization (.nuworld JSON via reflection) ---

static void SaveAtom(Atom* go, json& j)
{
	j["name"] = go->GetName();
	Transform& t = go->GetTransform();
	if (TypeInfo* tti = t.GetType())
		SaveObject(*tti, &t, j["transform"]);
	for (Component* c : go->components)
	{
		if (UnknownComponent* uc = dynamic_cast<UnknownComponent*>(c))
		{
			// Plugin type not loaded — write the preserved type + props back verbatim.
			json cj;
			cj["type"]  = uc->typeName;
			cj["props"] = uc->rawProps.empty() ? json::object()
			                                   : json::parse(uc->rawProps, nullptr, false);
			if (!uc->requiredPlugin.empty()) cj["plugin"] = uc->requiredPlugin;
			j["components"].push_back(cj);
			continue;
		}
		TypeInfo* ti = c->GetType();
		if (!ti) continue;                       // unreflected component — skip
		json cj;
		cj["type"] = ti->name;
		SaveObject(*ti, c, cj["props"]);
		const char* pl = PluginForType(ti->name);   // tag which plugin a component requires
		if (pl && pl[0]) cj["plugin"] = pl;
		j["components"].push_back(cj);
	}
	for (Atom* ch : go->children)
	{
		json chj;
		SaveAtom(ch, chj);
		j["children"].push_back(chj);
	}
}

static Atom* LoadAtom(const json& j)
{
	Atom* go = new Atom(j.value("name", std::string("Atom")).c_str());
	if (j.contains("transform"))
	{
		Transform& t = go->GetTransform();
		if (TypeInfo* tti = t.GetType())
			LoadObject(*tti, &t, j["transform"]);
	}
	if (j.contains("components"))
		for (const json& cj : j["components"])
		{
			std::string type = cj.value("type", std::string());
			TypeInfo* ti = Registry_Find(type);
			if (ti && ti->create && IsTypeActive(type))
			{
				Component* c = (Component*)ti->create();
				if (cj.contains("props")) LoadObject(*ti, c, cj["props"]);
				go->AddComponent(c);             // Init() wires transform/owner
			}
			else
			{
				// Type inactive (its plugin isn't loaded) — keep it inert + preserve its data,
				// and remember which plugin it needs so the editor can show that.
				UnknownComponent* uc = new UnknownComponent();
				uc->typeName = type;
				if (cj.contains("props")) uc->rawProps = cj["props"].dump();
				uc->requiredPlugin = cj.value("plugin", std::string(PluginForType(type)));
				go->AddComponent(uc);
			}
		}
	if (j.contains("children"))
		for (const json& chj : j["children"])
			LoadAtom(chj)->SetParent(go);
	return go;
}

// --- live plugin (un)load: swap an atom's components between real <-> inert placeholder ---
static void DowngradeAtom(Atom* a, const std::string& dll)
{
	for (auto it = a->components.begin(); it != a->components.end(); ++it)
	{
		Component* c = *it;
		if (dynamic_cast<UnknownComponent*>(c)) continue;
		TypeInfo* ti = c->GetType();
		if (!ti) continue;
		if (std::string(PluginForType(ti->name)) != dll) continue;
		json props; SaveObject(*ti, c, props);
		UnknownComponent* uc = new UnknownComponent();
		uc->typeName       = ti->name;
		uc->rawProps       = props.dump();
		uc->requiredPlugin = dll;
		uc->atom           = a;
		uc->transform      = &a->GetTransform();
		*it = uc;            // replace in place (preserves order)
		delete c;
	}
	for (Atom* ch : a->children) DowngradeAtom(ch, dll);
}

static void UpgradeAtom(Atom* a, const std::string& dll)
{
	std::vector<UnknownComponent*> todo;
	for (Component* c : a->components)
	{
		UnknownComponent* uc = dynamic_cast<UnknownComponent*>(c);
		if (!uc || uc->requiredPlugin != dll) continue;
		TypeInfo* ti = Registry_Find(uc->typeName);
		if (ti && ti->create) todo.push_back(uc);
	}
	for (UnknownComponent* uc : todo)
	{
		TypeInfo* ti = Registry_Find(uc->typeName);
		json props = uc->rawProps.empty() ? json::object()
		                                  : json::parse(uc->rawProps, nullptr, false);
		a->components.remove(uc);
		Component* c = (Component*)ti->create();
		LoadObject(*ti, c, props);
		a->AddComponent(c);   // Init wires transform/owner + side effects (e.g. Lua compile)
		delete uc;
	}
	for (Atom* ch : a->children) UpgradeAtom(ch, dll);
}

// --- prefabs: a saved Atom subtree, reusing the world's atom (de)serialization ---
bool SavePrefab(Atom* root, const std::string& path)
{
	if (!root) return false;
	json j;
	SaveAtom(root, j);
	boost::filesystem::path p(path);
	boost::filesystem::ofstream f(p);
	if (!f) return false;
	f << j.dump(2);
	return (bool)f;
}

Atom* LoadPrefab(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream f(p);
	if (!f) return nullptr;
	json j = json::parse(f, nullptr, false);
	if (j.is_discarded()) return nullptr;
	return LoadAtom(j);
}

void World::ConvertPluginToUnknown(const std::string& moduleFile)
{
	for (Atom* a : GetHierarchy()) DowngradeAtom(a, moduleFile);
}
void World::RestorePluginComponents(const std::string& moduleFile)
{
	for (Atom* a : GetHierarchy()) UpgradeAtom(a, moduleFile);
}

std::string World::SaveToString()
{
	json j;
	j["type"] = "World";
	j["version"] = 1;
	j["atoms"] = json::array();
	for (Atom* go : *hierarchy)
	{
		if (go->GetName() == "Editor Camera") continue;   // editor infra, not part of the scene
		json gj;
		SaveAtom(go, gj);
		j["atoms"].push_back(gj);
	}
	return j.dump(2);
}

void World::LoadFromString(const std::string& data)
{
	json j = json::parse(data, nullptr, false);
	if (j.is_discarded()) { std::cout << "[World]\t\t\t" << "LoadFromString: bad JSON" << std::endl; return; }
	for (auto it = hierarchy->begin(); it != hierarchy->end(); )   // keep editor camera, drop the rest
	{
		if ((*it)->GetName() == "Editor Camera") ++it;
		else it = hierarchy->erase(it);
	}
	if (j.contains("atoms"))
		for (const json& gj : j["atoms"])
			Add(LoadAtom(gj));
}

void World::SaveToFile(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ofstream f(p);
	if (f) f << SaveToString();
	std::cout << "[World]\t\t\t" << "Saved to " << path << std::endl;
}

void World::LoadFromFile(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream f(p);
	if (!f) { std::cout << "[World]\t\t\t" << "LoadFromFile: cannot open " << path << std::endl; return; }
	std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	LoadFromString(data);
	std::cout << "[World]\t\t\t" << "Loaded " << path << std::endl;
}
}  // namespace nuke