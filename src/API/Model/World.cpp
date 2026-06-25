#include "API/Model/World.h"
#include "render/irender.h"
#include "API/Model/Camera.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Mesh.h"
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

	for (Camera* cam : cams)
	{
		if (!cam->transform) continue;
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
}  // namespace nuke