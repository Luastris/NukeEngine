// Header-only boost.chrono must come BEFORE any boost include: the lib flavor double-defines
// steady_clock::now inside the engine DLL.
#define BOOST_CHRONO_HEADER_ONLY
#include <boost/chrono.hpp>
#include "API/Model/World.h"
#include <memory>
#include <functional>
#include "render/irender.h"
#include "API/Model/Camera.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Material.h"
#include "API/Model/Light.h"
#include "API/Model/Environment.h"
#include "API/Model/PostProcess.h"
#include "API/Model/Shader.h"
#include "API/Model/ReflectionProbe.h"
#include "API/Model/Time.h"
#include "API/Model/Events.h"
#include "API/Model/Profiler.h"
#include "API/Model/Game.h"
#include "API/Model/StatusBar.h"
#include "API/Model/Sprite.h"
#include "API/Model/Canvas.h"
#include "API/Model/RectAnchor.h"
#include "API/Model/Decal.h"
#include "API/Model/Collider.h"
#include "API/Model/Jobs.h"
#include "API/Model/Rigidbody.h"
#include "API/Model/CharacterController.h"
#include "API/Model/DebugDraw.h"
#include "API/Model/InstancedMesh.h"
#include "API/Model/Wind.h"
#include "interface/WorldHooks.h"
#include "API/Model/BendVolumes.h"
#include "interface/Services.h"
#include "service/iPhysics.h"
#include "service/iAudio.h"
#include "API/Model/Audio.h"
#include "API/Model/AudioListener.h"
#include <cmath>
#include <map>
#include <set>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include "API/Model/Mesh.h"
#include "API/Model/Texture.h"
#include "API/Model/resdb.h"
#include "API/Model/UnknownComponent.h"
#include "API/Model/Prefab.h"
#include "interface/AppInstance.h"
#include "input/Input.h"
#include "interface/Modular.h"
#include "reflect/ReflectJson.h"
#include <boost/filesystem/fstream.hpp>
#include <iostream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

namespace nuke {

World::World() : name("Default scene"), hierarchy(new bc::list<Atom*>()) {
	NukeReflectInit();   // forces Reflect.gen.obj to link
	std::cout << "[World]\t\t\t" << "This:" << this << ", Hierarchy is " << hierarchy << ", Hierarchy size: " << hierarchy->size() << std::endl;
}

// --- ray picking ---

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

// Ray vs an oriented rectangle (centre qc, unit half-axes rgt/up, half extents halfW/halfH).
static bool RayQuad(const glm::vec3& ro, const glm::vec3& rd, const glm::vec3& qc,
                    const glm::vec3& rgt, const glm::vec3& up, float halfW, float halfH, float& outT)
{
	glm::vec3 n = glm::normalize(glm::cross(rgt, up));
	float denom = glm::dot(n, rd);
	if (fabsf(denom) <= 1e-6f) return false;
	float tHit = glm::dot(qc - ro, n) / denom;   // rd is normalized -> tHit is distance
	if (tHit <= 0.0f) return false;
	glm::vec3 d = (ro + tHit * rd) - qc;
	if (fabsf(glm::dot(d, rgt)) > halfW || fabsf(glm::dot(d, up)) > halfH) return false;
	outT = tHit;
	return true;
}

static void PickRec(bc::list<Atom*>& gos, const glm::vec3& ro, const glm::vec3& rd, float& bestDist, Atom*& best,
                    Canvas* ctx = nullptr)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;   // not rendered -> clicks pass through
		// A canvas re-parents the coordinate space for its subtree (same rule as rendering).
		Canvas* here = atom->GetComponent<Canvas>();
		Canvas* cur  = here ? here : ctx;
		if (here && here->transform)
		{
			const float es = (here->mode == CanvasMode::WorldSpace) ? 1.0f : here->PxToWorld();
			Transform* t = here->transform;
			Vector3 p = t->globalPosition(), R = t->right(), U = t->up();
			glm::vec3 c((float)p.x, (float)p.y, (float)p.z);
			glm::vec3 rgt = glm::normalize(glm::vec3((float)R.x, (float)R.y, (float)R.z));
			glm::vec3 up  = glm::normalize(glm::vec3((float)U.x, (float)U.y, (float)U.z));
			float tHit;
			if (RayQuad(ro, rd, c, rgt, up, here->width * 0.5f * es, here->height * 0.5f * es, tHit) && tHit < bestDist)
			{ bestDist = tHit; best = atom; }
		}
		if (auto* mr = atom->GetComponent<MeshRenderer>())
		{
			Mesh* m = mr->mesh;
			if (m && m->vertexArray && m->numVerts > 0)
			{
				Transform& t = atom->GetTransform();
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
					if (dist < bestDist) { bestDist = dist; best = atom; }
				}
			}
		}
		// Sprites have no mesh: pick their textured quad, and canvas children pick where they render.
		if (auto* sp = atom->GetComponent<Sprite>())
		{
			if (sp->enabled)
			{
				Transform& t = atom->GetTransform();
				Vector3 p = t.globalPosition(), s = t.globalScale();
				float halfW = sp->width  * 0.5f * (float)s.x;
				float halfH = sp->height * 0.5f * (float)s.y;
				glm::vec3 center((float)p.x, (float)p.y, (float)p.z), rgt, up;
				bool onCanvas = false;
				if (cur && cur != here && cur->transform)   // a child of a canvas (not the canvas atom itself)
				{
					Transform* ct = cur->transform;
					Vector3 cp = ct->globalPosition(), R = ct->right(), U = ct->up();
					rgt = glm::normalize(glm::vec3((float)R.x, (float)R.y, (float)R.z));
					up  = glm::normalize(glm::vec3((float)U.x, (float)U.y, (float)U.z));
					glm::vec3 c0((float)cp.x, (float)cp.y, (float)cp.z);
					if (cur->mode != CanvasMode::WorldSpace)
					{
						// Matches DrawSprites' editor path: children keep true world offsets on the plane.
						const float dx = (float)(p.x - cp.x), dy = (float)(p.y - cp.y);
						center = c0 + rgt * dx + up * dy;
					}
					else
					{
						// World canvas: position projected onto the plane (Z pinned).
						glm::vec3 dv = center - c0;
						center = c0 + rgt * glm::dot(dv, rgt) + up * glm::dot(dv, up);
					}
					onCanvas = true;
				}
				else if (sp->mode == SpriteMode::Billboard)
				{
					// Face the ray (≈ the camera): build a screen-facing basis perpendicular to rd.
					glm::vec3 n = -rd;
					glm::vec3 uref = (fabsf(glm::dot(glm::vec3(0, 1, 0), rd)) > 0.99f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
					rgt = glm::normalize(glm::cross(uref, n));
					up  = glm::normalize(glm::cross(n, rgt));
				}
				else
				{
					Vector3 R = t.right(), U = t.up();
					rgt = glm::normalize(glm::vec3((float)R.x, (float)R.y, (float)R.z));
					up  = glm::normalize(glm::vec3((float)U.x, (float)U.y, (float)U.z));
				}
				// Quad centre = atom origin shifted by the pivot; canvas children draw centre-on-origin.
				float px = onCanvas ? 0.0f : 1.0f - 2.0f * sp->pivotX;
				float py = onCanvas ? 0.0f : 1.0f - 2.0f * sp->pivotY;
				glm::vec3 qc = center + rgt * (halfW * px) + up * (halfH * py);
				float tHit;
				if (RayQuad(ro, rd, qc, rgt, up, halfW, halfH, tHit) && tHit < bestDist)
				{ bestDist = tHit; best = atom; }
			}
		}
		if (atom->children.size() > 0) PickRec(atom->children, ro, rd, bestDist, best, cur);
	}
}

Atom* World::PickDist(const Vector3& origin, const Vector3& dir, float& outDist)
{
	glm::vec3 ro((float)origin.x, (float)origin.y, (float)origin.z);
	glm::vec3 rd = glm::normalize(glm::vec3((float)dir.x, (float)dir.y, (float)dir.z));
	float bestDist = 1e30f; Atom* best = nullptr;
	if (hierarchy) PickRec(*hierarchy, ro, rd, bestDist, best);
	outDist = bestDist;
	return best;
}
Atom* World::Pick(const Vector3& origin, const Vector3& dir) { float d; return PickDist(origin, dir, d); }

World::~World()
{
	Reflect_DropObject(this);   // invalidate script handles to this world
}

static Atom* FindByName(Atom* node, const std::string& name)
{
	if (!node) return nullptr;
	if (node->name == name) return node;
	for (Atom* c : node->children)
		if (Atom* r = FindByName(c, name)) return r;
	return nullptr;
}

Atom* World::Get(const std::string& name)
{
	// Whole-tree lookup, roots first (a root match always wins over a nested one).
	for (auto atom : GetHierarchy())
		if (atom->GetName() == name)
			return atom;
	for (auto atom : GetHierarchy())
		if (Atom* r = FindByName(atom, name))
			return r;
	return nullptr;
}

static Atom* FindById(Atom* node, long id)
{
	if (!node) return nullptr;
	if (node->id.id == id) return node;
	for (Atom* c : node->children)
		if (Atom* r = FindById(c, id)) return r;
	return nullptr;
}

Atom* World::GetById(long id)
{
	for (Atom* atom : GetHierarchy())
		if (Atom* r = FindById(atom, id)) return r;
	return nullptr;
}

bc::list<Atom*>& World::GetHierarchy()
{
	return *hierarchy;
}

void World::Add(Atom* atom)
{
	hierarchy->push_back(atom);
}

Atom* World::CreateAtom(const std::string& name)
{
	boost::recursive_mutex::scoped_lock lock(gameLock);   // scripts create mid-frame
	Atom* a = new Atom();
	a->name = name.empty() ? "Atom" : name;
	Add(a);
	return a;
}

void World::QueueDestroy(long atomId)
{
	if (!atomId) return;
	boost::recursive_mutex::scoped_lock lock(gameLock);
	destroyQueue.push_back(atomId);
}

void World::Start()
{

}

void World::LockGame()   { gameLock.lock(); }
void World::UnlockGame() { gameLock.unlock(); }

void World::Update()
{
	Profiler::Scope profScope("update");
	Jobs::PumpMain();   // deliver background-job results on the game thread
	static int slowLog = 0;
	auto t0 = boost::chrono::steady_clock::now();
	boost::recursive_mutex::scoped_lock lock(gameLock);
	{
		double ms = boost::chrono::duration_cast<boost::chrono::duration<double, boost::milli>>(
			boost::chrono::steady_clock::now() - t0).count();
		if (ms > 50.0 && slowLog < 20)
			{ std::cout << "[World]			Update lock wait: " << ms << " ms" << std::endl; ++slowLog; }
	}
	AppInstance* app = AppInstance::GetSingleton();
	app->worldTickActive = true;   // scripts' Game.LoadWorld queues instead of reloading mid-loop
	Input::Update(Time::getSingleton()->delta);   // evaluate input once per tick, BEFORE scripts query it
	Time::getSingleton()->Advance(Time::getSingleton()->gameDelta);
	Wind::Advance(Time::getSingleton()->gameDelta);
	Events::Pump([this](const std::string& n, const std::string& p)
	{
		std::function<void(bc::list<Atom*>&)> deliver = [&](bc::list<Atom*>& gos)
		{
			for (Atom* atom : gos)
			{
				if (!atom || !atom->enabled) continue;
				for (Component* c : atom->components)
					if (c && c->enabled) c->OnEvent(n, p);
				deliver(atom->children);
			}
		};
		deliver(*hierarchy);
	});
	for (Atom* atom : *hierarchy)
	{
		atom->Update();
	}
	app->worldTickActive = false;
	// Flush deferred destruction after the traversal, still under the game lock. Swap first:
	// RemoveAtomById may queue more destroys, and those must wait one frame.
	if (!destroyQueue.empty())
	{
		std::vector<long> doomed;
		doomed.swap(destroyQueue);
		for (long id : doomed)
		{
			if (!GetById(id))
				std::cout << "[World]\t\t\t" << "destroy flush: atom " << id << " not found" << std::endl;
			RemoveAtomById(id);
		}
	}
	// Script-queued world switch, applied at the frame boundary (traversal done, lock held).
	if (!app->pendingWorldLoad.empty())
	{
		std::string path;
		path.swap(app->pendingWorldLoad);
		std::cout << "[World]\t\t\t" << "Game.LoadWorld -> '" << path << "'" << std::endl;
		if (!app->OpenWorld(path))
			std::cout << "[World]\t\t\t" << "Game.LoadWorld: world '" << path << "' not found" << std::endl;
	}
	// Savegame load: a full runtime snapshot (atoms + calendar + schedule + script/tilemap state).
	if (!app->pendingSaveLoad.empty())
	{
		std::string path;
		path.swap(app->pendingSaveLoad);
		std::cout << "[World]\t\t\t" << "Game.LoadGame -> '" << path << "'" << std::endl;
		app->FlushWorldActivation();   // a still-growing world completes before the save replaces it
		suppressPersistOnce = true;    // the save is the full state; persistent atoms would duplicate
		LoadFromFile(path);
	}
	app->ApplyAsyncWorldLoad();      // staged async world swaps in here (traversal done, lock held)
	app->ContinueWorldActivation();  // budgeted slice of a still-growing world
}

// --- fixed-step physics driver (no-op without an iPhysics provider) ---

// Lazily creates each Collider's physics body and drives kinematic bodies from the Transform.
// Fills `bodyMap` (bodyId -> Collider) for contact-event dispatch after the step.
static void SyncBodies(bc::list<Atom*>& gos, iPhysics* p, std::map<uint64_t, Collider*>& bodyMap, float dt,
                       bool off = false)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		const bool kill = off || !atom->enabled;   // recurse to destroy nested bodies, don't skip
		if (Collider* col = atom->GetComponent<Collider>())
		{
			// A CharacterController owns its atom's collision; a Collider body would fight it.
			// Either case (or a disabled collider) must also destroy an already-created body.
			if (atom->GetComponent<CharacterController>() || !col->enabled || kill)
			{
				if (col->bodyId)
				{
					p->destroyBody(col->bodyId);
					col->bodyId = 0;
					col->hasSync = false;
				}
				if (!atom->children.empty())
					SyncBodies(atom->children, p, bodyMap, dt, kill);
				continue;
			}

			Rigidbody* rb = atom->GetComponent<Rigidbody>();
			Transform& t = atom->GetTransform();

			// Motion type changed live: recreate. The backend silently ignores moveKinematic
			// on a body that was created static.
			const int desiredMotion = rb ? (rb->isKinematic ? 2 : 1) : 0;
			if (col->bodyId && col->bodyMotion != desiredMotion)
			{
				p->destroyBody(col->bodyId);
				col->bodyId = 0;
				col->hasSync = false;
			}
			// Scale edited (or a parent scaled): the shape is scale-baked, so it must be rebuilt
			// at the new size. Relative epsilon — a jitter of the last float bit is not an edit.
			if (col->bodyId)
			{
				const Vector3 sNow = t.globalScale();
				const double se = 1e-4;
				if (std::fabs(std::fabs(sNow.x) - col->bodyScale[0]) > se * std::max(1.0, std::fabs(sNow.x)) ||
				    std::fabs(std::fabs(sNow.y) - col->bodyScale[1]) > se * std::max(1.0, std::fabs(sNow.y)) ||
				    std::fabs(std::fabs(sNow.z) - col->bodyScale[2]) > se * std::max(1.0, std::fabs(sNow.z)))
				{
					p->destroyBody(col->bodyId);
					col->bodyId = 0;
					col->hasSync = false;
				}
			}

			if (col->enabled && !col->bodyId)
			{
				// Bake the world scale into the shape (physics engines don't scale bodies).
				Vector3 pos = t.globalPosition();
				Quaternion rot = t.globalRotation();
				Vector3 scl = t.globalScale();
				NukeBodyDesc d;
				d.shape = col->shape;
				d.halfExtents[0] = (float)(col->halfExtents.x * fabs(scl.x));
				d.halfExtents[1] = (float)(col->halfExtents.y * fabs(scl.y));
				d.halfExtents[2] = (float)(col->halfExtents.z * fabs(scl.z));
				const float rScale = (float)std::max(fabs(scl.x), std::max(fabs(scl.y), fabs(scl.z)));
				d.radius     = col->radius * rScale;
				d.halfHeight = col->halfHeight * (float)fabs(scl.y);
				d.friction    = col->friction;
				d.restitution = col->restitution;
				d.isTrigger   = col->isTrigger;
				d.convex      = col->convex;
				d.motion = rb ? (rb->isKinematic ? 2 : 1) : 0;
				if (rb)
				{
					d.mass           = rb->mass;
					d.useGravity     = rb->useGravity;
					d.linearDamping  = rb->linearDamping;
					d.angularDamping = rb->angularDamping;
				}
				d.pos[0] = (float)pos.x; d.pos[1] = (float)pos.y; d.pos[2] = (float)pos.z;
				d.quat[0] = (float)rot.x; d.quat[1] = (float)rot.y; d.quat[2] = (float)rot.z; d.quat[3] = (float)rot.w;

				// Mesh shape: triangle soup from the sibling MeshRenderer, scale-baked.
				std::vector<float> scaledVerts;
				if (col->shape == Collider::S_Mesh)
				{
					MeshRenderer* mr = atom->GetComponent<MeshRenderer>();
					Mesh* mesh = mr ? mr->mesh : nullptr;
					if (!mesh || !mesh->vertexArray || mesh->numVerts < 3)
					{
						std::cout << "[World]\t\t\tmesh collider on '" << atom->name
						          << "' has no sibling MeshRenderer mesh - skipped" << std::endl;
						continue;
					}
					// The physics seam takes an unindexed soup: expand v4 indices while baking
					// the world scale (soup meshes copy through 1:1, as before).
					// LOD0 ONLY: the appended LOD ranges are coincident simplified shells.
					const int soupVerts = mesh->numIndices > 0 ? mesh->Lod0IndexCount() : mesh->numVerts;
					scaledVerts.resize((size_t)soupVerts * 3);
					for (int i = 0; i < soupVerts; ++i)
					{
						const uint32_t src = mesh->numIndices > 0 ? mesh->indexArray[i] : (uint32_t)i;
						scaledVerts[(size_t)i * 3 + 0] = mesh->vertexArray[src * 3 + 0] * (float)scl.x;
						scaledVerts[(size_t)i * 3 + 1] = mesh->vertexArray[src * 3 + 1] * (float)scl.y;
						scaledVerts[(size_t)i * 3 + 2] = mesh->vertexArray[src * 3 + 2] * (float)scl.z;
					}
					d.meshVerts     = scaledVerts.data();
					d.meshVertCount = soupVerts;
				}
				col->bodyId = p->createBody(d);
				col->bodyMotion = desiredMotion;
				col->bodyScale[0] = (float)fabs(scl.x);   // what the shape was baked with
				col->bodyScale[1] = (float)fabs(scl.y);
				col->bodyScale[2] = (float)fabs(scl.z);
				col->lastSyncPos = pos; col->lastSyncRot = rot; col->hasSync = true;
			}
			else if (col->bodyId)
			{
				Vector3 pos = t.globalPosition();
				Quaternion rot = t.globalRotation();
				float fp[3] = { (float)pos.x, (float)pos.y, (float)pos.z };
				float fq[4] = { (float)rot.x, (float)rot.y, (float)rot.z, (float)rot.w };
				if (desiredMotion == 2)
				{
					// Kinematic tracking servo: render-cadence writes vs fixed-cadence steps beat,
					// so drive a smoothed feedforward velocity plus a proportional pull to the
					// newest write rather than the raw per-step delta.
					const double dx = pos.x - col->lastSyncPos.x, dy = pos.y - col->lastSyncPos.y, dz = pos.z - col->lastSyncPos.z;
					const double qd = fabs(Quaternion::Dot(rot, col->lastSyncRot));
					const bool moved = !col->hasSync || (dx * dx + dy * dy + dz * dz) > 1e-10 || qd < 1.0 - 1e-7;
					if (moved && col->quietSteps > 30)
					{
						// Parked then moved = teleport, not a glide: snap instead of gliding.
						p->setBodyPose(col->bodyId, fp, fq);
						col->kinVelEma = Vector3();
						col->kinAngVelEma = Vector3();
						col->quietSteps = 0;
						col->lastSyncPos = pos; col->lastSyncRot = rot; col->hasSync = true;
					}
					else
					{
						if (moved)
						{
							// Instantaneous velocity = delta since the last write over elapsed time.
							const double span = dt * (col->quietSteps + 1);
							const Vector3 instV(dx / span, dy / span, dz / span);
							Quaternion dq = (rot * col->lastSyncRot.conjugate()).Normalized();
							if (dq.w < 0) { dq.x = -dq.x; dq.y = -dq.y; dq.z = -dq.z; dq.w = -dq.w; }   // shortest arc
							const double sinHalf = sqrt(std::max(0.0, 1.0 - dq.w * dq.w));
							const double ang = 2.0 * atan2(sinHalf, dq.w);
							Vector3 instW;
							if (sinHalf > 1e-9)
								instW = Vector3(dq.x / sinHalf, dq.y / sinHalf, dq.z / sinHalf) * (ang / span);
							const bool fromRest = col->kinVelEma.abs() < 1e-9 && col->kinAngVelEma.abs() < 1e-9;
							col->kinVelEma    = fromRest ? instV : col->kinVelEma * 0.75 + instV * 0.25;
							col->kinAngVelEma = fromRest ? instW : col->kinAngVelEma * 0.75 + instW * 0.25;
							col->quietSteps = 0;
							col->lastSyncPos = pos; col->lastSyncRot = rot; col->hasSync = true;
						}
						else
						{
							// Short gaps are the beat; a long gap means writes stopped, so bleed off.
							++col->quietSteps;
							if (col->quietSteps > 3)
							{
								col->kinVelEma    *= 0.6;
								col->kinAngVelEma *= 0.6;
							}
						}

						float bp[3], bq[4];
						if (p->getBodyPose(col->bodyId, bp, bq))
						{
							// v = feedforward + Kp * error, capped so a huge error closes fast but calmly.
							const double Kp = 5.0;   // 1/s: ~0.2 s to absorb tracking error
							Vector3 v = col->kinVelEma
							          + Vector3(col->lastSyncPos.x - bp[0], col->lastSyncPos.y - bp[1], col->lastSyncPos.z - bp[2]) * Kp;
							Quaternion qb(bq[0], bq[1], bq[2], bq[3]);
							Quaternion qe = (col->lastSyncRot * qb.conjugate()).Normalized();
							if (qe.w < 0) { qe.x = -qe.x; qe.y = -qe.y; qe.z = -qe.z; qe.w = -qe.w; }
							const double esin = sqrt(std::max(0.0, 1.0 - qe.w * qe.w));
							const double eang = 2.0 * atan2(esin, qe.w);
							Vector3 w = col->kinAngVelEma;
							if (esin > 1e-9)
								w += Vector3(qe.x / esin, qe.y / esin, qe.z / esin) * (eang * Kp);
							const double vMax = std::max(col->kinVelEma.abs() * 3.0, 2.0);
							const double wMax = std::max(col->kinAngVelEma.abs() * 3.0, 3.0);
							const double vLen = v.abs(), wLen = w.abs();
							if (vLen > vMax) v *= vMax / vLen;
							if (wLen > wMax) w *= wMax / wLen;

							// Compose the next pose from the velocity command; the backend derives
							// the velocities that carry riders.
							const Vector3 np(bp[0] + v.x * dt, bp[1] + v.y * dt, bp[2] + v.z * dt);
							Quaternion nq = qb;
							const double wA = w.abs();
							if (wA > 1e-9)
								nq = (Quaternion::FromAxisAngle(Vector3(w.x / wA, w.y / wA, w.z / wA),
								                                wA * dt * (180.0 / 3.14159265358979323846)) * qb).Normalized();
							float tp[3] = { (float)np.x, (float)np.y, (float)np.z };
							float tq[4] = { (float)nq.x, (float)nq.y, (float)nq.z, (float)nq.w };
							p->moveKinematic(col->bodyId, tp, tq, dt);
						}
						else
							p->moveKinematic(col->bodyId, fp, fq, dt);
					}
				}
				else
				{
					// Static/dynamic: push only external transform changes. PullDynamicPoses
					// refreshes lastSync, so the driver's own write-backs don't re-trigger here.
					const double dx = pos.x - col->lastSyncPos.x, dy = pos.y - col->lastSyncPos.y, dz = pos.z - col->lastSyncPos.z;
					const double qd = fabs(Quaternion::Dot(rot, col->lastSyncRot));
					const bool moved = !col->hasSync || (dx * dx + dy * dy + dz * dz) > 1e-10 || qd < 1.0 - 1e-7;
					if (moved)
					{
						p->setBodyPose(col->bodyId, fp, fq);   // static move / dynamic teleport
						col->lastSyncPos = pos; col->lastSyncRot = rot; col->hasSync = true;
					}
				}
			}
			if (col->bodyId)
				bodyMap[col->bodyId] = col;
		}
		else if (kill)
		{
			// No collider here, but the subtree is off: keep walking to kill nested bodies.
			if (!atom->children.empty())
				SyncBodies(atom->children, p, bodyMap, dt, true);
			continue;
		}
		if (!atom->children.empty())
			SyncBodies(atom->children, p, bodyMap, dt, kill);
	}
}

// --- character controllers (virtual capsules) ---

// Capsule feet position relative to the atom position, in scaled local units.
static Vector3 CharFeetLocal(CharacterController* cc, const Vector3& scl)
{
	Vector3 f(cc->capsuleOffset.x * scl.x, cc->capsuleOffset.y * scl.y, cc->capsuleOffset.z * scl.z);
	if (cc->pivot == CharacterController::P_Center)
		f.y -= std::max(0.2, cc->height * fabs(scl.y)) * 0.5;   // same height clamp everywhere
	return f;
}

// Lazily creates backend characters, composes each one's desired velocity for the step and
// pushes it to the seam. Runs under the game lock, before step().
static void SyncCharacters(bc::list<Atom*>& gos, iPhysics* p, const float gravity[3], float dt,
                           bool off = false)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		const bool kill = off || !atom->enabled;   // acts down the subtree
		if (CharacterController* cc = atom->GetComponent<CharacterController>())
		{
			Transform& t = atom->GetTransform();
			// Disabled component or atom: destroy the backend character, else it keeps steering.
			if (!cc->enabled || kill)
			{
				if (cc->charId) { p->destroyCharacter(cc->charId); cc->charId = 0; }
				if (!atom->children.empty()) SyncCharacters(atom->children, p, gravity, dt, kill);
				continue;
			}
			// Bake the world scale into the capsule; a live size change recreates in place.
			const Vector3 scl = t.globalScale();
			const float rScale = (float)std::max(fabs(scl.x), fabs(scl.z));
			const float r  = std::max(0.05f, cc->radius * rScale);
			const float H  = std::max(0.2f, (float)(cc->height * fabs(scl.y)));   // same clamp as the gizmo
			const float hh = std::max(0.01f, H * 0.5f - r);
			if (cc->charId && (fabs(cc->bakedRadius - r) > 1e-5f || fabs(cc->bakedHalf - hh) > 1e-5f))
			{
				p->destroyCharacter(cc->charId);
				cc->charId = 0;
			}
			if (!cc->charId)
			{
				const Vector3 pos = t.globalPosition();
				const Vector3 fl = CharFeetLocal(cc, scl);
				NukeCharacterDesc d;
				d.radius = r; d.halfHeight = hh;
				d.maxSlopeDeg  = cc->maxSlope;
				d.stepHeight   = cc->stepHeight;
				d.stickDistance = cc->stickDistance;
				d.mass         = cc->pushMass;
				d.maxStrength  = cc->maxPushForce;
				d.pos[0] = (float)(pos.x + fl.x); d.pos[1] = (float)(pos.y + fl.y); d.pos[2] = (float)(pos.z + fl.z);
				cc->charId = p->createCharacter(d);
				cc->bakedRadius = r; cc->bakedHalf = hh;
				cc->liveSlope = cc->maxSlope; cc->liveStep = cc->stepHeight; cc->liveStick = cc->stickDistance;
			}
			if (!cc->charId) continue;
			// Live walk-parameter edits: no recreate needed.
			if (cc->liveSlope != cc->maxSlope || cc->liveStep != cc->stepHeight || cc->liveStick != cc->stickDistance)
			{
				p->setCharacterParams(cc->charId, cc->maxSlope, cc->stepHeight, cc->stickDistance);
				cc->liveSlope = cc->maxSlope; cc->liveStep = cc->stepHeight; cc->liveStick = cc->stickDistance;
			}
			if (cc->pendingTeleport)
			{
				// Teleport takes an atom position: convert to the capsule's feet.
				const Vector3 fl = CharFeetLocal(cc, scl);
				float tp[3] = { (float)(cc->teleportPos.x + fl.x),
				                (float)(cc->teleportPos.y + fl.y),
				                (float)(cc->teleportPos.z + fl.z) };
				p->setCharacterPosition(cc->charId, tp);
				cc->pendingTeleport = false;
				cc->verticalVel = 0;
			}

			Vector3 v;
			if (!cc->autoGravity && cc->rawSet)
			{
				// Raw mode: caller's velocity verbatim; we only resolve collisions.
				v = cc->rawVelocity;
			}
			else
			{
				// Managed mode: horizontal from SetMove; vertical integrated here.
				const bool grounded = cc->groundState == NUKE_GROUND_ON;
				if (grounded && cc->verticalVel <= 0)
					cc->verticalVel = 0;                     // landed
				if (grounded && cc->pendingJump > 0)
				{
					cc->verticalVel = cc->pendingJump;       // lift off
					cc->pendingJump = 0;
				}
				if (!grounded || cc->verticalVel > 0)
					cc->verticalVel += gravity[1] * cc->gravityScale * dt;
				cc->pendingJump = 0;                         // an unconsumed jump expires
				v = Vector3(cc->moveInput.x, cc->verticalVel, cc->moveInput.z);
				if (grounded && cc->inheritPlatform)
					v += cc->groundVel;
			}
			float fv[3] = { (float)v.x, (float)v.y, (float)v.z };
			p->setCharacterVelocity(cc->charId, fv);
		}
		if (!atom->children.empty())
			SyncCharacters(atom->children, p, gravity, dt, kill);
	}
}

// Writes stepped character positions back into the Transforms and snapshots the ground state
// for the query API. Runs under the game lock, after step().
static void PullCharacters(bc::list<Atom*>& gos, iPhysics* p, const std::map<uint64_t, Collider*>& bodyMap)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		CharacterController* cc = atom->GetComponent<CharacterController>();
		if (cc && cc->charId)
		{
			float pos[3], n[3], gv[3];
			int state; uint64_t gb;
			if (p->getCharacterState(cc->charId, pos, state, n, gv, gb))
			{
				Transform& t = atom->GetTransform();
				// The character owns position only; rotation stays gameplay's. The backend
				// reports the capsule's feet, so map back through the pivot.
				const Vector3 fl = CharFeetLocal(cc, t.globalScale());
				t.SetGlobal(Vector3(pos[0] - fl.x, pos[1] - fl.y, pos[2] - fl.z),
				            t.globalRotation(), t.globalScale());
				cc->groundState  = state;
				cc->groundNormal = Vector3(n[0], n[1], n[2]);
				cc->groundVel    = Vector3(gv[0], gv[1], gv[2]);
				cc->groundBody   = gb;
				auto it = gb ? bodyMap.find(gb) : bodyMap.end();
				cc->groundAtomId = (it != bodyMap.end() && it->second->atom) ? (long long)it->second->atom->id.id : 0;
				float av[3];
				p->getCharacterVelocity(cc->charId, av);
				cc->actualVel = Vector3(av[0], av[1], av[2]);
			}
		}
		if (!atom->children.empty())
			PullCharacters(atom->children, p, bodyMap);
	}
}

// Routes drained contact transitions to both atoms' components: trigger pairs go to
// OnTriggerEnter/Exit, the rest to OnCollisionEnter/Exit.
static void DispatchContacts(iPhysics* p, const std::map<uint64_t, Collider*>& bodyMap)
{
	NukeContactEvent ev[128];
	int n;
	while ((n = p->fetchContacts(ev, 128)) > 0)
	{
		for (int i = 0; i < n; ++i)
		{
			auto ia = bodyMap.find(ev[i].bodyA);
			auto ib = bodyMap.find(ev[i].bodyB);
			if (ia == bodyMap.end() || ib == bodyMap.end()) continue;   // body died mid-step
			Collider* ca = ia->second; Collider* cb = ib->second;
			Atom* aa = ca->atom; Atom* ab = cb->atom;
			if (!aa || !ab) continue;
			const bool trigger = ca->isTrigger || cb->isTrigger;
			const bool enter   = ev[i].phase == 0;
			auto notify = [&](Atom* self, Atom* other)
			{
				for (Component* c : self->components)
				{
					if (!c || !c->enabled) continue;
					if (trigger) { if (enter) c->OnTriggerEnter(other);   else c->OnTriggerExit(other); }
					else         { if (enter) c->OnCollisionEnter(other); else c->OnCollisionExit(other); }
				}
			};
			notify(aa, ab);
			notify(ab, aa);
		}
		if (n < 128) break;
	}
}

// Write simulated poses back into the Transforms of DYNAMIC bodies.
static void PullDynamicPoses(bc::list<Atom*>& gos, iPhysics* p)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		Collider* col = atom->GetComponent<Collider>();
		Rigidbody* rb = atom->GetComponent<Rigidbody>();
		if (col && rb && !rb->isKinematic && col->bodyId)
		{
			float fp[3], fq[4];
			if (p->getBodyPose(col->bodyId, fp, fq))
			{
				Transform& t = atom->GetTransform();
				t.SetGlobal(Vector3(fp[0], fp[1], fp[2]),
				            Quaternion(fq[0], fq[1], fq[2], fq[3]),
				            t.globalScale());
				// Cache what the transform will RETURN, not the raw values written: the write
				// round-trips quat->euler->quat, so raw values would read back as an external
				// move and make SyncBodies pose-snap the body every step.
				col->lastSyncPos = t.globalPosition();
				col->lastSyncRot = t.globalRotation();
				col->hasSync = true;
			}
		}
		if (!atom->children.empty())
			PullDynamicPoses(atom->children, p);
	}
}

void World::FixedUpdate()
{
	Profiler::Scope profScope("fixed");
	// ONE fixed step; the cadence lives in AppInstance::FixedThread, so this must not know
	// about frames or accumulate time. World phases hold the game lock, the solve does not.
	const float dt = settings.fixedDt > 0.0001f ? settings.fixedDt : 1.0f / 60.0f;
	iPhysics* p = GetService<iPhysics>();
	std::map<uint64_t, Collider*> bodyMap;   // this step's live bodies, for contact dispatch

	if (p)
	{
		boost::recursive_mutex::scoped_lock lock(gameLock);
		p->init();   // idempotent
		p->setGravity(settings.gravity);
		SyncBodies(*hierarchy, p, bodyMap, dt);
		SyncCharacters(*hierarchy, p, settings.gravity, dt);   // desired velocities for this step
	}

	if (p)
		p->step(dt);   // unlocked: the backend solves while the frame proceeds

	{
		boost::recursive_mutex::scoped_lock lock(gameLock);
		AppInstance* app = AppInstance::GetSingleton();
		app->worldTickActive = true;   // Game.LoadWorld from fixed hooks queues (applied by Update)
		if (p)
		{
			PullDynamicPoses(*hierarchy, p);
			PullCharacters(*hierarchy, p, bodyMap);   // positions + ground snapshots
			DispatchContacts(p, bodyMap);
		}
		for (Atom* atom : *hierarchy)
			atom->FixedUpdate();               // scripts' fixedUpdate runs here, under the lock
		app->worldTickActive = false;
	}
}

// --- render pass (one render per camera) ---

static void CollectCameras(bc::list<Atom*>& gos, std::vector<Camera*>& out)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;   // disabled atom = whole subtree off
		if (auto* c = atom->GetComponent<Camera>())
			if (c->enabled) out.push_back(c);
		if (atom->children.size() > 0)
			CollectCameras(atom->children, out);
	}
}

// The game's view camera: the first enabled camera flagged mainCamera, else the first enabled
// camera in hierarchy order. Editor cameras never qualify. Null when the world has none.
Camera* World::GetMainCamera()
{
	std::vector<Camera*> cams;
	CollectCameras(*hierarchy, cams);
	Camera* first = nullptr;
	for (Camera* c : cams)
	{
		if (c->editorCamera) continue;
		if (c->mainCamera) return c;
		if (!first) first = c;
	}
	return first;
}

static void CollectLights(bc::list<Atom*>& gos, std::vector<Light*>& out)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (auto* l = atom->GetComponent<Light>())
			if (l->enabled) out.push_back(l);
		if (atom->children.size() > 0)
			CollectLights(atom->children, out);
	}
}

// First enabled AudioListener's transform, depth-first. Null when the world has none.
static Transform* FindAudioListener(bc::list<Atom*>& gos)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (auto* l = atom->GetComponent<AudioListener>())
			if (l->enabled && l->transform) return l->transform;
		if (!atom->children.empty())
			if (Transform* t = FindAudioListener(atom->children)) return t;
	}
	return nullptr;
}

// Post-process components; one may sit beside each Camera, matched by shared transform.
static void CollectPostProcess(bc::list<Atom*>& gos, std::vector<PostProcess*>& out)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (auto* p = atom->GetComponent<PostProcess>())
			if (p->enabled) out.push_back(p);
		if (atom->children.size() > 0)
			CollectPostProcess(atom->children, out);
	}
}

static ReflectionProbe* FindReflectionProbe(bc::list<Atom*>& gos)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (auto* p = atom->GetComponent<ReflectionProbe>()) if (p->enabled) return p;
		if (atom->children.size() > 0) if (ReflectionProbe* p = FindReflectionProbe(atom->children)) return p;
	}
	return nullptr;
}

static Environment* FindEnvironment(bc::list<Atom*>& gos)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (auto* e = atom->GetComponent<Environment>()) if (e->enabled) return e;
		if (atom->children.size() > 0) if (Environment* e = FindEnvironment(atom->children)) return e;
	}
	return nullptr;
}

// One queued draw (gathered before drawing so transparent objects can be sorted back-to-front).
// Sectioned v4 meshes carry the EFFECTIVE per-slot materials (fallbacks already applied);
// matCount <= 1 = the classic single-material path.
static const int kMaxDrawSlots = 16;
struct DrawItem { Mesh* mesh; Material* mat; float pos[3], quat[4], scale[3]; Vector3 wpos; int blend; bool inReflections;
                  float prevPos[3], prevQuat[4], prevScale[3]; bool hasPrev;   // prev transform for TAA velocity
                  Material* mats[kMaxDrawSlots]; int matCount = 0;             // per-slot materials (v4 sections)
                  bool anyOpaque = true, anyBlend = false; };                  // pass membership across slots

// Render-layer filter: bit i of `mask` = render atoms with Atom::layer == i.
static inline bool LayerVisible(Atom* atom, unsigned int mask) { return (mask >> (atom->layer & 31)) & 1u; }

static void CollectMeshes(bc::list<Atom*>& gos, std::vector<DrawItem>& out, unsigned int mask = 0xFFFFFFFFu)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (LayerVisible(atom, mask))
		if (auto* mr = atom->GetComponent<MeshRenderer>())
			if (mr->enabled && mr->mesh)
			{
				Transform& t = atom->GetTransform();
				Vector3    p = t.globalPosition();
				Quaternion q = t.globalRotation();
				Vector3    s = t.globalScale();
				DrawItem it;
				it.mesh = mr->mesh; it.mat = mr->mat;
				it.pos[0]=(float)p.x; it.pos[1]=(float)p.y; it.pos[2]=(float)p.z;
				it.quat[0]=(float)q.x; it.quat[1]=(float)q.y; it.quat[2]=(float)q.z; it.quat[3]=(float)q.w;
				it.scale[0]=(float)s.x; it.scale[1]=(float)s.y; it.scale[2]=(float)s.z;
				it.wpos = p;
				it.blend = mr->mat ? mr->mat->blendMode : 0;   // 0 = opaque, 1/2 = transparent/additive
				// Sectioned mesh / per-slot materials: snapshot the EFFECTIVE slot list.
				it.matCount = 0; it.anyOpaque = it.blend == 0; it.anyBlend = it.blend != 0;
				if (!mr->mats.empty() || mr->mesh->numSlots > 1)
				{
					int slots = (int)mr->mats.size() > mr->mesh->numSlots ? (int)mr->mats.size() : mr->mesh->numSlots;
					if (slots > kMaxDrawSlots) slots = kMaxDrawSlots;
					it.anyOpaque = false; it.anyBlend = false;
					for (int sl = 0; sl < slots; ++sl)
					{
						Material* m = mr->MaterialForSlot(sl);
						it.mats[sl] = m;
						const int bm = m ? m->blendMode : 0;
						it.anyOpaque = it.anyOpaque || bm == 0;
						it.anyBlend  = it.anyBlend  || bm != 0;
					}
					it.matCount = slots;
					if (slots > 0 && !it.mats[0]) it.mats[0] = mr->mat;   // slot-0 fallback for the renderer
					// Single-slot meshes with a matGuids[0] override must reach the LEGACY
					// draw paths (matCount <= 1 falls back to it.mat) with the EFFECTIVE material.
					if (slots > 0 && it.mats[0]) { it.mat = it.mats[0]; it.blend = it.mats[0]->blendMode; }
				}
				it.inReflections = mr->inReflections;
				it.hasPrev = mr->hasPrev;   // false on the first frame -> zero motion
				for (int k = 0; k < 3; ++k) it.prevPos[k] = mr->prevPos[k];
				for (int k = 0; k < 4; ++k) it.prevQuat[k] = mr->prevQuat[k];
				for (int k = 0; k < 3; ++k) it.prevScale[k] = mr->prevScale[k];
				out.push_back(it);
			}
		if (atom->children.size() > 0)
			CollectMeshes(atom->children, out, mask);
	}
}

// Frustum cull: true when all 8 corners of the mesh AABB fall outside the same clip plane.
// D3D clip convention: x/y in [-w,w], z in [0,w].
static bool FrustumCull(const DrawItem& it, const float vp[16])
{
	if (!it.mesh) return false;
	it.mesh->EnsureBounds();
	if (!it.mesh->boundsValid) return false;   // unknown bounds -> never cull
	const float* mn = it.mesh->aabbMin; const float* mx = it.mesh->aabbMax;
	glm::quat Q(it.quat[3], it.quat[0], it.quat[1], it.quat[2]);
	glm::vec3 P(it.pos[0], it.pos[1], it.pos[2]), S(it.scale[0], it.scale[1], it.scale[2]);
	int oL = 0, oR = 0, oB = 0, oT = 0, oN = 0, oF = 0;
	for (int c = 0; c < 8; ++c)
	{
		glm::vec3 lc((c & 1) ? mx[0] : mn[0], (c & 2) ? mx[1] : mn[1], (c & 4) ? mx[2] : mn[2]);
		glm::vec3 w = P + Q * (lc * S);                         // local -> world
		float x  = w.x*vp[0] + w.y*vp[4] + w.z*vp[8]  + vp[12]; // world -> clip (row-vector * row-major VP)
		float y  = w.x*vp[1] + w.y*vp[5] + w.z*vp[9]  + vp[13];
		float z  = w.x*vp[2] + w.y*vp[6] + w.z*vp[10] + vp[14];
		float ww = w.x*vp[3] + w.y*vp[7] + w.z*vp[11] + vp[15];
		if (x < -ww) ++oL; if (x > ww) ++oR;
		if (y < -ww) ++oB; if (y > ww) ++oT;
		if (z < 0.0f) ++oN; if (z > ww) ++oF;
	}
	return oL == 8 || oR == 8 || oB == 8 || oT == 8 || oN == 8 || oF == 8;
}

// Combined view*proj for the current camera; the renderer's matrices are row-major.
static void CameraVP(iRender* r, float vp[16])
{
	float view[16], proj[16];
	r->getViewProj(view, proj);
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
		{ float s = 0; for (int k = 0; k < 4; ++k) s += view[i*4+k] * proj[k*4+j]; vp[i*4+j] = s; }
}

// --- GPU instancing: InstancedMesh components draw as chunked instanced ranges ---

static void CollectInstancedMeshes(bc::list<Atom*>& gos, std::vector<InstancedMesh*>& out, unsigned int mask = 0xFFFFFFFFu)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (LayerVisible(atom, mask))
			if (auto* im = atom->GetComponent<InstancedMesh>())
				if (im->enabled) out.push_back(im);
		if (atom->children.size() > 0)
			CollectInstancedMeshes(atom->children, out, mask);
	}
}

// Same 8-corner clip test as FrustumCull, over a chunk's precomputed world bounds.
static bool CullAABB(const float mn[3], const float mx[3], const float vp[16])
{
	int oL = 0, oR = 0, oB = 0, oT = 0, oN = 0, oF = 0;
	for (int c = 0; c < 8; ++c)
	{
		float wx = (c & 1) ? mx[0] : mn[0], wy = (c & 2) ? mx[1] : mn[1], wz = (c & 4) ? mx[2] : mn[2];
		float x  = wx*vp[0] + wy*vp[4] + wz*vp[8]  + vp[12];
		float y  = wx*vp[1] + wy*vp[5] + wz*vp[9]  + vp[13];
		float z  = wx*vp[2] + wy*vp[6] + wz*vp[10] + vp[14];
		float ww = wx*vp[3] + wy*vp[7] + wz*vp[11] + vp[15];
		if (x < -ww) ++oL; if (x > ww) ++oR;
		if (y < -ww) ++oB; if (y > ww) ++oT;
		if (z < 0.0f) ++oN; if (z > ww) ++oF;
	}
	return oL == 8 || oR == 8 || oB == 8 || oT == 8 || oN == 8 || oF == 8;
}

// Camera pass: every visible chunk of every InstancedMesh becomes one instanced draw call.
static void DrawInstancedMeshes(std::vector<InstancedMesh*>& ims, iRender* r, bool cull)
{
	if (ims.empty()) return;
	float vp[16]; if (cull) CameraVP(r, vp);
	for (InstancedMesh* im : ims)
	{
		if (!im->EnsureRenderReady(r)) continue;
		for (const InstancedMesh::Chunk& c : im->chunks)
			if (!(cull && CullAABB(c.mn, c.mx, vp)))
				r->renderObjectInstanced(im->mesh, im->mat, im->gpuBuf, c.first, c.count);
	}
}

// G-buffer/velocity prepass: opaque instanced sets only.
static void DrawInstancedGBuffer(std::vector<InstancedMesh*>& ims, iRender* r, bool cull)
{
	if (ims.empty()) return;
	float vp[16]; if (cull) CameraVP(r, vp);
	for (InstancedMesh* im : ims)
	{
		if (im->mat && im->mat->blendMode != 0) continue;
		if (!im->EnsureRenderReady(r)) continue;
		for (const InstancedMesh::Chunk& c : im->chunks)
			if (!(cull && CullAABB(c.mn, c.mx, vp)))
				r->renderGBufferInstanced(im->mesh, im->mat, im->gpuBuf, c.first, c.count);
	}
}

// Shadow depth pass: all chunks; the light frustum is not the camera's, so no camera culling.
static void DrawInstancedShadows(std::vector<InstancedMesh*>& ims, iRender* r)
{
	for (InstancedMesh* im : ims)
	{
		if (!im->castShadows) continue;
		if (im->mat && !im->mat->castShadows) continue;
		if (!im->EnsureRenderReady(r)) continue;
		for (const InstancedMesh::Chunk& c : im->chunks)
			r->renderShadowInstanced(im->mesh, im->gpuBuf, c.first, c.count, im->mat);
	}
}

// Snapshots each MeshRenderer's global transform as "previous" for next frame's TAA motion
// vectors. Must run once per frame, after all cameras have rendered.
static void UpdatePrevTransforms(bc::list<Atom*>& gos)
{
	for (auto atom : gos)
	{
		if (auto* mr = atom->GetComponent<MeshRenderer>())
		{
			Transform& t = atom->GetTransform();
			Vector3 p = t.globalPosition(); Quaternion q = t.globalRotation(); Vector3 s = t.globalScale();
			mr->prevPos[0] = (float)p.x; mr->prevPos[1] = (float)p.y; mr->prevPos[2] = (float)p.z;
			mr->prevQuat[0] = (float)q.x; mr->prevQuat[1] = (float)q.y; mr->prevQuat[2] = (float)q.z; mr->prevQuat[3] = (float)q.w;
			mr->prevScale[0] = (float)s.x; mr->prevScale[1] = (float)s.y; mr->prevScale[2] = (float)s.z;
			mr->hasPrev = true;
		}
		UpdatePrevTransforms(atom->children);
	}
}

// SSR G-buffer prepass: opaque geometry only. Must use the same cull setting as the colour
// pass so the depths line up.
static void DrawGBuffer(std::vector<DrawItem>& items, iRender* r, bool cull)
{
	float vp[16]; if (cull) CameraVP(r, vp);
	for (auto& it : items)
		if (it.anyOpaque && !(cull && FrustumCull(it, vp)))
		{
			if (it.matCount > 1)
				r->renderGBufferObjectMulti(it.mesh, it.mats, it.matCount, it.pos, it.quat, it.scale,
				                            it.hasPrev ? it.prevPos : nullptr, it.hasPrev ? it.prevQuat : nullptr, it.hasPrev ? it.prevScale : nullptr, 0);
			else if (it.blend == 0)
				r->renderGBufferObject(it.mesh, it.mat, it.pos, it.quat, it.scale,
				                       it.hasPrev ? it.prevPos : nullptr, it.hasPrev ? it.prevQuat : nullptr, it.hasPrev ? it.prevScale : nullptr);
		}
}

// Draws a gathered scene for one camera: opaque first, then transparent/additive sorted
// back-to-front by camera distance. Frustum-culled when `cull`.
static void DrawCollected(std::vector<DrawItem>& items, const Vector3& camPos, iRender* r, bool cull)
{
	float vp[16]; if (cull) CameraVP(r, vp);
	auto culled = [&](const DrawItem& it) { return cull && FrustumCull(it, vp); };

	for (auto& it : items)
		if (it.anyOpaque && !culled(it))
		{
			if (it.matCount > 1) r->renderObjectMulti(it.mesh, it.mats, it.matCount, it.pos, it.quat, it.scale, 0);
			else if (it.blend == 0) r->renderObject(it.mesh, it.mat, it.pos, it.quat, it.scale);
		}

	std::vector<DrawItem*> tr;
	for (auto& it : items) if (it.anyBlend && !culled(it)) tr.push_back(&it);
	if (!tr.empty())
	{
		auto dist2 = [&](const DrawItem* it) {
			double dx = it->wpos.x - camPos.x, dy = it->wpos.y - camPos.y, dz = it->wpos.z - camPos.z;
			return dx*dx + dy*dy + dz*dz;
		};
		std::sort(tr.begin(), tr.end(), [&](const DrawItem* a, const DrawItem* b) { return dist2(a) > dist2(b); });
		for (auto* it : tr)
		{
			if (it->matCount > 1) r->renderObjectMulti(it->mesh, it->mats, it->matCount, it->pos, it->quat, it->scale, 1);
			else r->renderObject(it->mesh, it->mat, it->pos, it->quat, it->scale);
		}
	}
}

// --- Sprites: unlit textured quads. World sprites draw after the meshes, back-to-front;
// screen sprites draw in their canvas's reference-pixel space via drawSpriteScreen. ---

// Texture px -> world units for standalone world sprites; matches the default canvas Pixels Per Unit.
static const float kSpritePxToWorld = 1.0f / 100.0f;

// Builds the nine-slice 3x3 sub-rects (centre-origin offsets/sizes in OUTPUT units) and the
// matching UV grid lines. Returns false when there is nothing to slice.
struct NinePatch { float rx[3], rw[3], ry[3], rh[3]; float u[4], v[4]; };
static bool BuildNinePatch(Texture* tex, int sl, int sr, int st, int sb,
                           float u0, float v0, float u1, float v1,
                           float outW, float outH, float pxToOut, NinePatch& np)
{
	if (!tex || tex->width <= 0 || tex->height <= 0) return false;
	if (sl <= 0 && sr <= 0 && st <= 0 && sb <= 0) return false;
	if (outW <= 0.0f || outH <= 0.0f) return false;
	const float cellW = fabsf(u1 - u0) * tex->width, cellH = fabsf(v1 - v0) * tex->height;
	if (cellW < 1.0f || cellH < 1.0f) return false;

	float bl = sl * pxToOut, br = sr * pxToOut, bt = st * pxToOut, bb = sb * pxToOut;
	if (bl + br > outW && bl + br > 0.0f) { float s = outW / (bl + br); bl *= s; br *= s; }
	if (bt + bb > outH && bt + bb > 0.0f) { float s = outH / (bt + bb); bt *= s; bb *= s; }

	np.rw[0] = bl; np.rw[2] = br; np.rw[1] = outW - bl - br; if (np.rw[1] < 0.0f) np.rw[1] = 0.0f;
	np.rx[0] = -outW * 0.5f + bl * 0.5f;
	np.rx[1] = -outW * 0.5f + bl + np.rw[1] * 0.5f;
	np.rx[2] =  outW * 0.5f - br * 0.5f;
	// rows bottom -> top (index 0 = bottom strip)
	np.rh[0] = bb; np.rh[2] = bt; np.rh[1] = outH - bb - bt; if (np.rh[1] < 0.0f) np.rh[1] = 0.0f;
	np.ry[0] = -outH * 0.5f + bb * 0.5f;
	np.ry[1] = -outH * 0.5f + bb + np.rh[1] * 0.5f;
	np.ry[2] =  outH * 0.5f - bt * 0.5f;

	const float fl = sl / cellW, fr = sr / cellW, ft = st / cellH, fb = sb / cellH;
	np.u[0] = u0; np.u[1] = u0 + (u1 - u0) * fl; np.u[2] = u1 - (u1 - u0) * fr; np.u[3] = u1;
	np.v[0] = v0; np.v[1] = v0 + (v1 - v0) * ft; np.v[2] = v1 - (v1 - v0) * fb; np.v[3] = v1;   // v grows downward
	return true;
}

// Emits one screen sprite: a single rect, or the 3x3 nine-slice grid when enabled.
static void EmitScreenSprite(iRender* r, Sprite* sp, const float rect[4], const float refSize[2],
                             const float uvr[4], const float tn[4], int queue, int scaleMode)
{
	NinePatch np;
	const int sl = sp->flipX ? sp->tex->sliceRight  : sp->tex->sliceLeft;
	const int sr = sp->flipX ? sp->tex->sliceLeft   : sp->tex->sliceRight;
	const int st = sp->flipY ? sp->tex->sliceBottom : sp->tex->sliceTop;
	const int sb = sp->flipY ? sp->tex->sliceTop    : sp->tex->sliceBottom;
	if (sp->tex->nineSlice && BuildNinePatch(sp->tex, sl, sr, st, sb, uvr[0], uvr[1], uvr[2], uvr[3],
	                                    rect[2], rect[3], 1.0f /* 1 texture px = 1 reference px */, np))
	{
		for (int c = 0; c < 3; ++c)
			for (int rr = 0; rr < 3; ++rr)
			{
				if (np.rw[c] <= 0.0f || np.rh[rr] <= 0.0f) continue;
				float r2[4]  = { rect[0] + np.rx[c], rect[1] + np.ry[rr], np.rw[c], np.rh[rr] };
				float uv2[4] = { np.u[c], np.v[2 - rr], np.u[c + 1], np.v[3 - rr] };
				r->drawSpriteScreenEx(sp->tex, r2, refSize, uv2, tn, queue, scaleMode);
			}
		return;
	}
	r->drawSpriteScreenEx(sp->tex, rect, refSize, uvr, tn, queue, scaleMode);
}

// Emits one world-quad sprite (centre + unit axes + half extents), single quad or nine-slice.
// pxToOut converts the texture's slice pixels into the quad's units.
static void EmitWorldSprite(iRender* r, Sprite* sp, const Vector3& center, const Vector3& Rn, const Vector3& Un,
                            float halfW, float halfH, float pxToOut, const float uvr[4], const float tn[4])
{
	NinePatch np;
	const int sl = sp->flipX ? sp->tex->sliceRight  : sp->tex->sliceLeft;
	const int sr = sp->flipX ? sp->tex->sliceLeft   : sp->tex->sliceRight;
	const int st = sp->flipY ? sp->tex->sliceBottom : sp->tex->sliceTop;
	const int sb = sp->flipY ? sp->tex->sliceTop    : sp->tex->sliceBottom;
	if (sp->tex->nineSlice && BuildNinePatch(sp->tex, sl, sr, st, sb, uvr[0], uvr[1], uvr[2], uvr[3],
	                                    halfW * 2.0f, halfH * 2.0f, pxToOut, np))
	{
		for (int c = 0; c < 3; ++c)
			for (int rr = 0; rr < 3; ++rr)
			{
				if (np.rw[c] <= 0.0f || np.rh[rr] <= 0.0f) continue;
				float c3[3]  = { (float)(center.x + Rn.x * np.rx[c] + Un.x * np.ry[rr]),
				                 (float)(center.y + Rn.y * np.rx[c] + Un.y * np.ry[rr]),
				                 (float)(center.z + Rn.z * np.rx[c] + Un.z * np.ry[rr]) };
				float rv2[3]  = { (float)Rn.x * np.rw[c] * 0.5f, (float)Rn.y * np.rw[c] * 0.5f, (float)Rn.z * np.rw[c] * 0.5f };
				float upv2[3] = { (float)Un.x * np.rh[rr] * 0.5f, (float)Un.y * np.rh[rr] * 0.5f, (float)Un.z * np.rh[rr] * 0.5f };
				float uv2[4]  = { np.u[c], np.v[2 - rr], np.u[c + 1], np.v[3 - rr] };
				r->drawSprite(sp->tex, c3, rv2, upv2, uv2, tn);
			}
		return;
	}
	float rv[3]  = { (float)Rn.x * halfW, (float)Rn.y * halfW, (float)Rn.z * halfW };
	float upv[3] = { (float)Un.x * halfH, (float)Un.y * halfH, (float)Un.z * halfH };
	float cc[3]  = { (float)center.x, (float)center.y, (float)center.z };
	r->drawSprite(sp->tex, cc, rv, upv, uvr, tn);
}

struct ScreenSpr { Sprite* sp; Canvas* cv; };
static void GatherSprites(bc::list<Atom*>& gos, Canvas* ctx, std::vector<ScreenSpr>& world, std::vector<ScreenSpr>& screen,
                          unsigned int mask)
{
	for (Atom* atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		Canvas* here = atom->GetComponent<Canvas>();
		Canvas* cur  = here ? here : ctx;   // a canvas re-parents the coordinate space for its subtree
		if (LayerVisible(atom, mask))
			if (Sprite* sp = atom->GetComponent<Sprite>())
				if (sp->enabled)
				{
					if (cur && cur->mode != CanvasMode::WorldSpace) screen.push_back({ sp, cur });
					else                                            world.push_back({ sp, cur });
				}
		GatherSprites(atom->children, cur, world, screen, mask);
	}
}
static void DrawSprites(bc::list<Atom*>& hierarchy, const NukeCameraDesc& d, const Vector3& camPos, iRender* r,
                        Camera* cam, unsigned int mask)
{
	std::vector<ScreenSpr> sprites;
	std::vector<ScreenSpr> screen;
	GatherSprites(hierarchy, nullptr, sprites, screen, mask);

	// Screen-space canvas sprites: rect in canvas reference pixels, centre-origin, Z ignored.
	// The editor camera is the exception: it draws them as world-plane rectangles so they can
	// be seen, picked and edited.
	if (!screen.empty())
	{
		const bool editorCam = cam && cam->editorCamera;
		std::stable_sort(screen.begin(), screen.end(), [](const ScreenSpr& a, const ScreenSpr& b) { return a.cv->sortOrder < b.cv->sortOrder; });
		for (ScreenSpr& s : screen)
		{
			Sprite* sp = s.sp; Canvas* cv = s.cv;
			if (!sp->transform || sp->textureGuid.empty()) continue;
			if (!sp->tex || sp->tex->guid != sp->textureGuid) sp->tex = ResDB::getSingleton()->GetTexture(sp->textureGuid);
			if (!sp->tex) continue;
			Vector3 spos = sp->transform->globalPosition();
			Vector3 cpos = cv->transform ? cv->transform->globalPosition() : Vector3();
			Vector3 sc   = sp->transform->globalScale();
			float u0 = sp->u0, v0 = sp->v0, u1 = sp->u1, v1 = sp->v1;
			if (sp->flipX) std::swap(u0, u1);
			if (sp->flipY) std::swap(v0, v1);
			float uvr[4] = { u0, v0, u1, v1 };
			float tn[4]  = { sp->tint.r, sp->tint.g, sp->tint.b, sp->tint.a };

			// Children keep world-authored units: convert to reference pixels via pixelsPerUnit.
			const float ppu = cv->pixelsPerUnit > 0.01f ? cv->pixelsPerUnit : 100.0f;
			if (editorCam)
			{
				// World-plane preview: children sit on the plane at their true world offsets, 1:1.
				if (!cv->transform) continue;
				Vector3 R = cv->transform->right(), U = cv->transform->up();
				const float dx = (float)(spos.x - cpos.x), dy = (float)(spos.y - cpos.y);
				const float hw = sp->width  * 0.5f * (float)sc.x;
				const float hh = sp->height * 0.5f * (float)sc.y;
				Vector3 c(cpos.x + R.x * dx + U.x * dy,
				          cpos.y + R.y * dx + U.y * dy,
				          cpos.z + R.z * dx + U.z * dy);
				// nine-slice px on the plane: 1 texture px = 1 reference px = 1/ppu world units
				EmitWorldSprite(r, sp, c, R, U, hw, hh, 1.0f / ppu, uvr, tn);
				continue;
			}
			// A bound canvas draws only for its camera; compare by owning atom.
			if (cv->targetCamera && (!cam || cv->targetCamera != cam->atom)) continue;
			float rect[4]    = { (float)(spos.x - cpos.x) * ppu, (float)(spos.y - cpos.y) * ppu,
			                     sp->width * (float)sc.x * ppu, sp->height * (float)sc.y * ppu };
			float refSize[2] = { cv->width, cv->height };
			EmitScreenSprite(r, sp, rect, refSize, uvr, tn,
			                 cv->renderQueue == CanvasQueue::AfterPost ? 1 : 0, (int)cv->scaling);
		}
	}
	if (sprites.empty()) return;

	// Camera basis for billboards (re-orthogonalized, same convention as the renderer's look-at).
	Vector3 camF(d.camFwd[0], d.camFwd[1], d.camFwd[2]);
	Vector3 camU0(d.camUp[0], d.camUp[1], d.camUp[2]);
	double rx = camU0.y*camF.z - camU0.z*camF.y, ry = camU0.z*camF.x - camU0.x*camF.z, rz = camU0.x*camF.y - camU0.y*camF.x;
	double rl = std::sqrt(rx*rx + ry*ry + rz*rz); if (rl < 1e-9) rl = 1.0;
	Vector3 camR(rx/rl, ry/rl, rz/rl);
	Vector3 camU(camF.y*camR.z - camF.z*camR.y, camF.z*camR.x - camF.x*camR.z, camF.x*camR.y - camF.y*camR.x);

	auto d2 = [&](const Vector3& p) { double dx=p.x-camPos.x, dy=p.y-camPos.y, dz=p.z-camPos.z; return dx*dx+dy*dy+dz*dz; };
	std::sort(sprites.begin(), sprites.end(), [&](const ScreenSpr& a, const ScreenSpr& b) {
		Vector3 pa = a.sp->transform ? a.sp->transform->globalPosition() : Vector3();
		Vector3 pb = b.sp->transform ? b.sp->transform->globalPosition() : Vector3();
		return d2(pa) > d2(pb);   // farthest first
	});

	for (ScreenSpr& ws : sprites)
	{
		Sprite* sp = ws.sp;
		if (!sp->transform || sp->textureGuid.empty()) continue;
		if (!sp->tex || sp->tex->guid != sp->textureGuid) sp->tex = ResDB::getSingleton()->GetTexture(sp->textureGuid);
		if (!sp->tex) continue;

		Transform* t = sp->transform;
		Vector3 pos = t->globalPosition(), sc = t->globalScale();
		float halfW = sp->width  * 0.5f * (float)sc.x;
		float halfH = sp->height * 0.5f * (float)sc.y;
		Vector3 rU = (sp->mode == SpriteMode::Billboard) ? camR : t->right();
		Vector3 uU = (sp->mode == SpriteMode::Billboard) ? camU : t->up();
		// Children of a world-space canvas are pinned to its surface: position projected onto
		// the plane, quad lying in it.
		if (ws.cv && ws.cv->transform)
		{
			Transform* ct = ws.cv->transform;
			Vector3 cp = ct->globalPosition(), R = ct->right(), U = ct->up();
			Vector3 dv(pos.x - cp.x, pos.y - cp.y, pos.z - cp.z);
			double du = dv.x*R.x + dv.y*R.y + dv.z*R.z;
			double dvU = dv.x*U.x + dv.y*U.y + dv.z*U.z;
			pos = Vector3(cp.x + R.x*du + U.x*dvU, cp.y + R.y*du + U.y*dvU, cp.z + R.z*du + U.z*dvU);
			rU = R; uU = U;   // in-plane orientation; billboard doesn't apply on a canvas
		}
		// The atom origin sits at the pivot; shift the quad centre so the pivot lands on the atom.
		float px = 1.0f - 2.0f*sp->pivotX, py = 1.0f - 2.0f*sp->pivotY;
		Vector3 c(pos.x + rU.x*halfW*px + uU.x*halfH*py,
		          pos.y + rU.y*halfW*px + uU.y*halfH*py,
		          pos.z + rU.z*halfW*px + uU.z*halfH*py);
		float u0 = sp->u0, v0 = sp->v0, u1 = sp->u1, v1 = sp->v1;
		if (sp->flipX) std::swap(u0, u1);
		if (sp->flipY) std::swap(v0, v1);
		float uvr[4] = { u0, v0, u1, v1 };
		float tn[4]  = { sp->tint.r, sp->tint.g, sp->tint.b, sp->tint.a };
		EmitWorldSprite(r, sp, c, rU, uU, halfW, halfH, kSpritePxToWorld, uvr, tn);
	}
}

// Applies RectAnchor components before rendering (edit mode included): each enabled side pins the
// matching edge, two opposite sides stretch the element. Distances are in canvas units: reference
// px on a screen canvas, world units on a world canvas.
static void ApplyCanvasLayouts(bc::list<Atom*>& gos, Canvas* ctx)
{
	for (Atom* atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		Canvas* here = atom->GetComponent<Canvas>();
		Canvas* cur  = here ? here : ctx;
		if (cur && cur != here && cur->transform)
			if (RectAnchor* ra = atom->GetComponent<RectAnchor>())
				if (ra->enabled && (ra->left || ra->right || ra->top || ra->bottom))
				{
					const bool  world = (cur->mode == CanvasMode::WorldSpace);
					const float ppu = world ? 1.0f : (cur->pixelsPerUnit > 0.01f ? cur->pixelsPerUnit : 100.0f);
					const float hw = cur->width * 0.5f, hh = cur->height * 0.5f;

					Transform* ct = cur->transform;
					Transform& t  = atom->GetTransform();
					Vector3 cp = ct->globalPosition();
					Vector3 gpos = t.globalPosition();
					Vector3 gsc  = t.globalScale();
					Vector3 R = ct->right(), U = ct->up();

					// Current centre in canvas units (centre-origin).
					float cx, cy;
					if (world)
					{
						Vector3 d(gpos.x - cp.x, gpos.y - cp.y, gpos.z - cp.z);
						cx = (float)(d.x*R.x + d.y*R.y + d.z*R.z);
						cy = (float)(d.x*U.x + d.y*U.y + d.z*U.z);
					}
					else { cx = (float)(gpos.x - cp.x) * ppu; cy = (float)(gpos.y - cp.y) * ppu; }

					// Element size in canvas units; anchors on a sprite-less atom pin the point.
					Sprite* sp = atom->GetComponent<Sprite>();
					float w = sp ? sp->width  * (float)gsc.x * ppu : 0.0f;
					float h = sp ? sp->height * (float)gsc.y * ppu : 0.0f;

					if (ra->left && ra->right)        // both edges pinned -> stretch with the canvas
					{
						w  = cur->width - ra->distLeft - ra->distRight; if (w < 1.0f) w = 1.0f;
						cx = -hw + ra->distLeft + w * 0.5f;
						if (sp) sp->width = w / (ppu * (float)(gsc.x != 0.0 ? gsc.x : 1.0));
					}
					else if (ra->left)  cx = -hw + ra->distLeft  + w * 0.5f;
					else if (ra->right) cx =  hw - ra->distRight - w * 0.5f;

					if (ra->bottom && ra->top)
					{
						h  = cur->height - ra->distBottom - ra->distTop; if (h < 1.0f) h = 1.0f;
						cy = -hh + ra->distBottom + h * 0.5f;
						if (sp) sp->height = h / (ppu * (float)(gsc.y != 0.0 ? gsc.y : 1.0));
					}
					else if (ra->bottom) cy = -hh + ra->distBottom + h * 0.5f;
					else if (ra->top)    cy =  hh - ra->distTop    - h * 0.5f;

					Vector3 gp;
					if (world)
						gp = Vector3(cp.x + R.x*cx + U.x*cy, cp.y + R.y*cx + U.y*cy, cp.z + R.z*cx + U.z*cy);
					else
						gp = Vector3(cp.x + cx / ppu, cp.y + cy / ppu, cp.z);   // transform units; Z pinned
					t.SetGlobal(gp, t.globalRotation(), gsc);
				}
		ApplyCanvasLayouts(atom->children, cur);
	}
}

// Editor-only: draws every canvas rectangle as debug lines. Unselected canvases get a thin
// depth-tested frame, the selected one a bright on-top highlight.
static void DrawCanvasGizmos(bc::list<Atom*>& gos, iRender* r, Atom* sel)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		if (Canvas* cv = atom->GetComponent<Canvas>())
			if (cv->transform)
			{
				const float es = (cv->mode == CanvasMode::WorldSpace) ? 1.0f : cv->PxToWorld();
				Transform* t = cv->transform;
				Vector3 p = t->globalPosition(), R = t->right(), U = t->up();
				float hw = cv->width * 0.5f * es, hh = cv->height * 0.5f * es;
				auto corner = [&](float sx, float sy, float* o) {
					o[0] = (float)(p.x + sx * hw * R.x + sy * hh * U.x);
					o[1] = (float)(p.y + sx * hw * R.y + sy * hh * U.y);
					o[2] = (float)(p.z + sx * hw * R.z + sy * hh * U.z);
				};
				float c00[3], c10[3], c11[3], c01[3];
				corner(-1, -1, c00); corner(1, -1, c10); corner(1, 1, c11); corner(-1, 1, c01);
				if (atom == sel)
				{
					const float col[4] = { 0.30f, 0.70f, 1.0f, 1.0f };   // selection highlight, on top
					r->drawDebugLine(c00, c10, col); r->drawDebugLine(c10, c11, col);
					r->drawDebugLine(c11, c01, col); r->drawDebugLine(c01, c00, col);
				}
				else
				{
					const float col[4] = { 0.5f, 0.5f, 0.5f, 1.0f };     // thin gray, occluded by the scene
					r->drawDebugLineDepth(c00, c10, col); r->drawDebugLineDepth(c10, c11, col);
					r->drawDebugLineDepth(c11, c01, col); r->drawDebugLineDepth(c01, c00, col);
				}
			}
		DrawCanvasGizmos(atom->children, r, sel);
	}
}

// --- Decals: screen-space, composited onto the opaque colour from the depth prepass ---
static void CollectDecals(bc::list<Atom*>& gos, std::vector<Decal*>& out, unsigned int mask = 0xFFFFFFFFu)
{
	for (Atom* atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (LayerVisible(atom, mask))
		if (Decal* d = atom->GetComponent<Decal>()) if (d->enabled) out.push_back(d);
		CollectDecals(atom->children, out, mask);
	}
}
static void DrawDecals(std::vector<Decal*>& decals, iRender* r)
{
	for (Decal* dc : decals)
	{
		if (!dc->transform || dc->textureGuid.empty()) continue;
		if (!dc->tex || dc->tex->guid != dc->textureGuid) dc->tex = ResDB::getSingleton()->GetTexture(dc->textureGuid);
		if (!dc->tex) continue;
		Transform* t = dc->transform;
		Vector3 p = t->globalPosition(); Quaternion q = t->globalRotation(); Vector3 s = t->globalScale();
		float pos[3]   = { (float)p.x, (float)p.y, (float)p.z };
		float quat[4]  = { (float)q.x, (float)q.y, (float)q.z, (float)q.w };
		float scale[3] = { (float)s.x, (float)s.y, (float)s.z };
		float tn[4]    = { dc->tint.r, dc->tint.g, dc->tint.b, dc->tint.a };
		r->drawDecal(dc->tex, pos, quat, scale, tn, dc->intensity, dc->angleFade, (int)dc->mode);
	}
}
// Editor-only: draws the selected atom's decal box wireframe plus its projection axis.
static void DrawDecalGizmos(bc::list<Atom*>& gos, iRender* r, Atom* sel)
{
	for (Atom* atom : gos)
	{
		if (!atom) continue;
		if (atom == sel)
		if (Decal* dc = atom->GetComponent<Decal>())
			if (dc->transform)
			{
				Transform* t = dc->transform;
				Vector3 p = t->globalPosition(), R = t->right(), U = t->up(), F = t->direction(), s = t->globalScale();
				float hx = 0.5f*(float)s.x, hy = 0.5f*(float)s.y, hz = 0.5f*(float)s.z;
				float c[8][3]; int idx = 0;
				for (float sz : {-1.f, 1.f}) for (float sy : {-1.f, 1.f}) for (float sx : {-1.f, 1.f})
				{
					c[idx][0] = (float)(p.x + sx*hx*R.x + sy*hy*U.x + sz*hz*F.x);
					c[idx][1] = (float)(p.y + sx*hx*R.y + sy*hy*U.y + sz*hz*F.y);
					c[idx][2] = (float)(p.z + sx*hx*R.z + sy*hy*U.z + sz*hz*F.z);
					++idx;
				}
				const float col[4] = { 1.0f, 0.5f, 0.2f, 1.0f };   // orange box
				auto ln = [&](int a, int b) { r->drawDebugLine(c[a], c[b], col); };
				ln(0,1); ln(2,3); ln(4,5); ln(6,7);   // along X (bit0)
				ln(0,2); ln(1,3); ln(4,6); ln(5,7);   // along Y (bit1)
				ln(0,4); ln(1,5); ln(2,6); ln(3,7);   // along Z (bit2)
				float ctr[3] = { (float)p.x, (float)p.y, (float)p.z };
				float tip[3] = { (float)(p.x + F.x*hz), (float)(p.y + F.y*hz), (float)(p.z + F.z*hz) };
				const float acol[4] = { 1.0f, 0.9f, 0.3f, 1.0f };   // projection axis (+Z)
				r->drawDebugLine(ctr, tip, acol);
			}
		DrawDecalGizmos(atom->children, r, sel);
	}
}

// Fires Component::OnRender for every enabled component at `phase`; the seam module components
// use to draw. The camera's view/proj is already bound.
static void DrawComponentHooks(bc::list<Atom*>& gos, iRender* r, RenderPhase phase, unsigned int mask = 0xFFFFFFFFu)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (LayerVisible(atom, mask))
			for (Component* c : atom->components)
			{
				if (!c || !c->enabled) continue;
				// Timed per COMPONENT TYPE: a module drawing itself into a render phase is the
				// one place the engine hands the frame to foreign code, so the profiler must be
				// able to name which component ate it ("rnd.hook.<Type>").
				// Phase names are interned per type: Profiler::Scope keeps the pointer, so a
				// temporary string would dangle. Render is single-threaded, hence the plain map.
				static std::map<const TypeInfo*, std::string> hookPhase;
				const TypeInfo* ti = c->GetType();
				auto hit = hookPhase.find(ti);
				if (hit == hookPhase.end())
					hit = hookPhase.emplace(ti, "rnd.hook." + (ti ? ti->name : std::string("?"))).first;
				Profiler::Scope ps(hit->second.c_str());
				c->OnRender(r, phase);
			}
		DrawComponentHooks(atom->children, r, phase, mask);
	}
}

// Depth-only traversal for the shadow pass: every enabled mesh whose material casts shadows
// (null material = casts by default).
static void RenderShadowMeshes(bc::list<Atom*>& gos, iRender* r)
{
	for (auto atom : gos)
	{
		if (!atom || !atom->enabled) continue;
		if (auto* mr = atom->GetComponent<MeshRenderer>())
		{
			// Multi-slot: casts when ANY effective slot material casts.
			bool casts = !mr->mat || mr->mat->castShadows;
			if (mr->mesh && mr->mesh->numSlots > 1)
				for (int sl = 0; sl < mr->mesh->numSlots && !casts; ++sl)
					if (Material* m = mr->MaterialForSlot(sl)) casts = m->castShadows;
			if (mr->enabled && mr->mesh && casts)
			{
				Transform& t = atom->GetTransform();
				Vector3    p = t.globalPosition();
				Quaternion q = t.globalRotation();
				Vector3    s = t.globalScale();
				float pos[3]   = { (float)p.x, (float)p.y, (float)p.z };
				float quat[4]  = { (float)q.x, (float)q.y, (float)q.z, (float)q.w };
				float scale[3] = { (float)s.x, (float)s.y, (float)s.z };
				if (!mr->mats.empty() || mr->mesh->numSlots > 1)
				{
					Material* slotMats[kMaxDrawSlots]; int slots = mr->mesh->numSlots;
					if ((int)mr->mats.size() > slots) slots = (int)mr->mats.size();
					if (slots > kMaxDrawSlots) slots = kMaxDrawSlots;
					for (int sl = 0; sl < slots; ++sl) slotMats[sl] = mr->MaterialForSlot(sl);
					r->renderShadowObjectMulti(mr->mesh, slotMats, slots, pos, quat, scale);
				}
				else
					r->renderShadowObject(mr->mesh, pos, quat, scale, mr->mat);
			}
		}
		if (atom->children.size() > 0)
			RenderShadowMeshes(atom->children, r);
	}
}

// --- editor gizmos: wire shapes for the selected atom ---
static void EmitSelectionGizmos(Atom* a)
{
	Transform& t = a->GetTransform();
	Vector3 pos = t.globalPosition();
	Quaternion rot = t.globalRotation();
	Vector3 scl = t.globalScale();

	// Skeleton overlay (SkinnedMeshRenderer): parent->child bone lines + joint dots, orange.
	// Uses the LAST applied pose; before any ApplyPose the bind pose is shown.
	if (SkinnedMeshRenderer* smr = a->GetComponent<SkinnedMeshRenderer>())
		if (Skeleton* sk = smr->EnsureSkeleton())
		{
			const size_t nb = sk->bones.size();
			const bool posed = smr->Globals().size() == nb * 16;
			glm::mat4 atomW = glm::translate(glm::mat4(1.0f), glm::vec3((float)pos.x, (float)pos.y, (float)pos.z))
			                * glm::mat4_cast(glm::quat((float)rot.w, (float)rot.x, (float)rot.y, (float)rot.z))
			                * glm::scale(glm::mat4(1.0f), glm::vec3((float)scl.x, (float)scl.y, (float)scl.z));
			std::vector<glm::vec3> joints(nb);
			std::vector<glm::mat4> bind;   // filled only when unposed
			if (!posed) bind.resize(nb);
			for (size_t i = 0; i < nb; ++i)
			{
				glm::mat4 g;
				if (posed) g = glm::make_mat4(smr->Globals().data() + i * 16);
				else
				{
					const MeshBone& b = sk->bones[i];
					glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(b.localPos[0], b.localPos[1], b.localPos[2]))
					                * glm::mat4_cast(glm::quat(b.localRot[3], b.localRot[0], b.localRot[1], b.localRot[2]))
					                * glm::scale(glm::mat4(1.0f), glm::vec3(b.localScale[0], b.localScale[1], b.localScale[2]));
					g = bind[i] = (b.parent >= 0) ? bind[b.parent] * local : local;
				}
				joints[i] = glm::vec3(atomW * glm::vec4(glm::vec3(g[3]), 1.0f));
			}
			const Color bc(1.0, 0.6, 0.15, 1.0);
			for (size_t i = 0; i < nb; ++i)
			{
				const int par = sk->bones[i].parent;
				if (par >= 0)
					DebugDraw::Line(Vector3(joints[par].x, joints[par].y, joints[par].z),
					                Vector3(joints[i].x, joints[i].y, joints[i].z), bc);
				DebugDraw::WireSphere(Vector3(joints[i].x, joints[i].y, joints[i].z), 0.015, bc);
			}
			// Sockets: small cyan markers at their world poses.
			for (const SkeletonSocket& sock : sk->sockets)
			{
				Vector3 sp; Quaternion sr;
				if (smr->SocketWorld(sock.name, sp, sr))
					DebugDraw::WireSphere(sp, 0.025, Color(0.2, 0.9, 1.0, 1.0));
			}
		}

	if (Light* l = a->GetComponent<Light>())
	{
		const Color c(1.0, 0.9, 0.2, 1.0);   // lights: yellow
		Vector3 fwd = t.direction();
		if (l->type == 0)        // directional: an arrow along the travel direction
			DebugDraw::Arrow(pos, Vector3(pos.x + fwd.x * 3, pos.y + fwd.y * 3, pos.z + fwd.z * 3), c);
		else if (l->type == 1)   // point: range sphere
			DebugDraw::WireSphere(pos, l->range, c);
		else                     // spot: world.ps lights around -dir, so the cone opens along -forward
			DebugDraw::WireCone(pos, Vector3(-fwd.x, -fwd.y, -fwd.z), l->spotAngle, l->range, c);
	}

	if (Collider* col = a->GetComponent<Collider>())
	{
		// colliders: green; triggers: cyan
		const Color c = col->isTrigger ? Color(0.2, 0.9, 1.0, 1.0) : Color(0.3, 1.0, 0.3, 1.0);
		const double rScale = std::max(fabs(scl.x), std::max(fabs(scl.y), fabs(scl.z)));
		switch (col->shape)
		{
			case Collider::S_Sphere:
				DebugDraw::WireSphere(pos, col->radius * rScale, c);
				break;
			case Collider::S_Capsule:
				DebugDraw::WireCapsule(pos, col->radius * rScale, col->halfHeight * fabs(scl.y), rot, c);
				break;
			case Collider::S_Mesh:
			{
				MeshRenderer* mr = a->GetComponent<MeshRenderer>();
				if (mr && mr->mesh)
				{
					mr->mesh->EnsureBounds();
					float* mn = mr->mesh->aabbMin; float* mx = mr->mesh->aabbMax;
					Vector3 lc((mn[0] + mx[0]) * 0.5, (mn[1] + mx[1]) * 0.5, (mn[2] + mx[2]) * 0.5);
					Vector3 lh((mx[0] - mn[0]) * 0.5 * fabs(scl.x),
					           (mx[1] - mn[1]) * 0.5 * fabs(scl.y),
					           (mx[2] - mn[2]) * 0.5 * fabs(scl.z));
					Vector3 wc = rot.Rotate(Vector3(lc.x * scl.x, lc.y * scl.y, lc.z * scl.z));
					DebugDraw::WireBox(Vector3(pos.x + wc.x, pos.y + wc.y, pos.z + wc.z), lh, rot, c);
				}
				break;
			}
			default:
				DebugDraw::WireBox(pos, Vector3(col->halfExtents.x * fabs(scl.x),
				                                col->halfExtents.y * fabs(scl.y),
				                                col->halfExtents.z * fabs(scl.z)), rot, c);
				break;
		}
	}

	if (CharacterController* cc = a->GetComponent<CharacterController>())
	{
		const Color c(1.0, 0.6, 0.15, 1.0);   // characters: orange capsule
		// Must apply the same clamps as the driver so the gizmo shows what will simulate.
		const double rScale = std::max(fabs(scl.x), fabs(scl.z));
		const double r = std::max(0.05, (double)cc->radius * rScale);
		const double H = std::max(0.2, (double)cc->height * fabs(scl.y));
		const double half = std::max(0.01, H * 0.5 - r);   // cylinder half
		Vector3 center(pos.x + cc->capsuleOffset.x * scl.x,
		               pos.y + cc->capsuleOffset.y * scl.y,
		               pos.z + cc->capsuleOffset.z * scl.z);
		if (cc->pivot == CharacterController::P_Feet) center.y += H * 0.5;
		DebugDraw::WireCapsule(center, r, half, Quaternion::Identity(), c);
	}

	if (Camera* cam = a->GetComponent<Camera>())
	{
		if (a->GetName() != "Editor Camera")   // the editor camera doesn't gizmo itself
		{
			const Color c(1.0, 1.0, 1.0, 1.0);   // camera frustum: white
			iRender* r = AppInstance::GetSingleton()->render;
			const double aspect = (r && r->height > 0) ? (double)r->width / r->height : 16.0 / 9.0;
			const double vFov = cam->fov * 3.14159265358979 / 180.0;
			Vector3 fwd = t.direction();
			Vector3 up = t.up();
			Vector3 right = t.right();
			auto plane = [&](double dist, Vector3* out)
			{
				const double h = tan(vFov * 0.5) * dist, w = h * aspect;
				Vector3 cptr(pos.x + fwd.x * dist, pos.y + fwd.y * dist, pos.z + fwd.z * dist);
				for (int i = 0; i < 4; ++i)
				{
					const double sx = (i == 0 || i == 3) ? -1 : 1, sy = (i < 2) ? 1 : -1;
					out[i] = Vector3(cptr.x + right.x * w * sx + up.x * h * sy,
					                 cptr.y + right.y * w * sx + up.y * h * sy,
					                 cptr.z + right.z * w * sx + up.z * h * sy);
				}
			};
			// far plane clamped for readability
			Vector3 n[4], f[4];
			plane(cam->_near, n);
			plane(std::min((double)cam->_far, 25.0), f);
			for (int i = 0; i < 4; ++i)
			{
				DebugDraw::Line(n[i], n[(i + 1) % 4], c);
				DebugDraw::Line(f[i], f[(i + 1) % 4], c);
				DebugDraw::Line(n[i], f[i], c);
			}
		}
	}

	if (ReflectionProbe* rp = a->GetComponent<ReflectionProbe>())
	{
		const Color c(0.8, 0.4, 1.0, 1.0);   // probes: purple
		if (rp->boxProjection)
			DebugDraw::WireBox(pos, ScaleExtents(Vector3(rp->boxSize.x * 0.5, rp->boxSize.y * 0.5,
			                                            rp->boxSize.z * 0.5), scl), rot, c);
		else
			DebugDraw::WireSphere(pos, 1.0, c);   // infinite probe: a small marker sphere
	}

	if (WindZone* wz = a->GetComponent<WindZone>())
	{
		const Color c(0.35, 0.9, 0.75, 1.0);   // wind zones: teal bounds + direction/radial arrows
		if (wz->shape == 0)
			DebugDraw::WireSphere(pos, wz->radius, c);
		else
			DebugDraw::WireBox(pos, wz->halfExtents, rot, c);
		const double reach = wz->shape == 0 ? wz->radius : std::max({ fabs(wz->halfExtents.x), fabs(wz->halfExtents.y), fabs(wz->halfExtents.z) });
		if (wz->mode == 0)   // directional: arrow along forward through the volume
		{
			Vector3 f = t.direction();
			DebugDraw::Arrow(Vector3(pos.x - f.x * reach * 0.8, pos.y - f.y * reach * 0.8, pos.z - f.z * reach * 0.8),
			                 Vector3(pos.x + f.x * reach * 0.8, pos.y + f.y * reach * 0.8, pos.z + f.z * reach * 0.8), c);
		}
		else                 // radial: outward arrows on the cardinal axes (suction shows reversed)
		{
			const double s = wz->strength >= 0 ? 1.0 : -1.0;
			const Vector3 axes[6] = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };
			for (const Vector3& ax : axes)
			{
				Vector3 inner(pos.x + ax.x * reach * 0.25, pos.y + ax.y * reach * 0.25, pos.z + ax.z * reach * 0.25);
				Vector3 outer(pos.x + ax.x * reach * 0.85, pos.y + ax.y * reach * 0.85, pos.z + ax.z * reach * 0.85);
				if (s > 0) DebugDraw::Arrow(inner, outer, c); else DebugDraw::Arrow(outer, inner, c);
			}
		}
	}
}

void World::Render(iRender* r)
{
	if (!r) return;
	Profiler::Scope profScope("render");   // CPU side of the render pass

	// Live profiler line, ~2x/s, real world only.
	if (!auxiliary)
	{
		static double lastPush = 0.0;
		if (Time::getSingleton()->elapsed - lastPush > 0.5)
		{
			lastPush = Time::getSingleton()->elapsed;
			char buf[256];
			// GPU passes are reported by the renderer a few frames late; 0 = the backend has no
			// duration queries, and then only the CPU phases are shown.
			const double gShadow = Profiler::Ms("gpu.shadow"), gScene = Profiler::Ms("gpu.scene");
			const double gPost = Profiler::Ms("gpu.post"), gTone = Profiler::Ms("gpu.tonemap");
			const double gUI = Profiler::Ms("gpu.ui");
			if (gShadow + gScene + gPost + gTone + gUI > 0.0)
				snprintf(buf, sizeof(buf),
				         "upd %.2f | fix %.2f | rnd %.2f ms   GPU: shadow %.2f | scene %.2f | post %.2f | tone %.2f | ui %.2f ms",
				         Profiler::Ms("update"), Profiler::Ms("fixed"), Profiler::Ms("render"),
				         gShadow, gScene, gPost, gTone, gUI);
			else
				snprintf(buf, sizeof(buf), "upd %.2f | fix %.2f | rnd %.2f ms",
				         Profiler::Ms("update"), Profiler::Ms("fixed"), Profiler::Ms("render"));
			StatusBar::Set("profiler", buf);
			// A frame this slow is a bug, not a workload: print WHERE it went, at most twice a
			// second, so the breakdown is in the log without anyone hunting for a profiler.
			if (Profiler::Ms("render") > 33.0)
			{
				std::string line;
				std::string phases = Profiler::Phases();
				size_t st = 0;
				while (st < phases.size())
				{
					size_t nl = phases.find('\n', st);
					if (nl == std::string::npos) nl = phases.size();
					const std::string ph = phases.substr(st, nl - st);
					st = nl + 1;
					if (ph.rfind("rnd.", 0) != 0 && ph.rfind("gpu.", 0) != 0) continue;
					if (ph == "rnd.cameras") continue;   // its own sub-phases carry the detail
					const double ms = Profiler::Ms(ph);
					if (ms < 0.05) continue;
					char one[64];
					snprintf(one, sizeof(one), "%s %.2f  ", ph.c_str(), ms);
					line += one;
				}
				std::cout << "[Profiler]		render " << Profiler::Ms("render") << " ms — " << line << std::endl;
			}
		}
	}

	std::unique_ptr<Profiler::Scope> prePhase(new Profiler::Scope("rnd.pre"));
	// Layout must run before anything gathers transforms this frame.
	ApplyCanvasLayouts(*hierarchy, nullptr);

	// Editor gizmos for the selection; lines live for one frame.
	{
		AppInstance* app = AppInstance::GetSingleton();
		if (app->isEditor() && app->playState == 0 && app->selectedInHieararchy)
			EmitSelectionGizmos(app->selectedInHieararchy);
	}

	// Advance animated textures by real frame time. Only the current world may do this, or an
	// auxiliary world rendered in the same frame double-steps the global animation clock.
	if (this == AppInstance::GetSingleton()->currentWorld)
	{
		double dtMs = Time::getSingleton()->delta * 1000.0;
		for (Texture* t : ResDB::getSingleton()->textures)
		{
			if (!t || t->frameCount <= 1) continue;
			t->animTimeMs += dtMs;
			for (int guard = 0; guard < t->frameCount; ++guard)
			{
				int d = (t->curFrame < (int)t->frameDelaysMs.size() && t->frameDelaysMs[t->curFrame] > 0)
				        ? t->frameDelaysMs[t->curFrame] : 100;
				if (t->animTimeMs < d) break;
				t->animTimeMs -= d;
				t->curFrame = (t->curFrame + 1) % t->frameCount;
			}
		}
	}

	std::vector<Camera*> cams;
	CollectCameras(*hierarchy, cams);
	std::sort(cams.begin(), cams.end(), [](Camera* a, Camera* b) { return a->depth < b->depth; });

	// Gather scene lights once for this frame; the renderer keeps them for every camera pass.
	std::vector<Light*> lights;
	CollectLights(*hierarchy, lights);

	// Audio pumps from Render, not Update: Update only runs in play mode, but the editor's
	// preview bus and the analysis must work in edit mode too.
	if (this == AppInstance::GetSingleton()->currentWorld)
		if (iAudio* au = GetService<iAudio>())
		{
			au->init();   // idempotent; the first call opens the device
			Transform* ears = FindAudioListener(*hierarchy);
			if (!ears)   // no listener -> the game's main camera, else any camera
			{
				Camera* mc = GetMainCamera();
				if (!mc && !cams.empty()) mc = cams[0];
				if (mc && mc->transform) ears = mc->transform;
			}
			if (ears)
			{
				Vector3 p = ears->globalPosition(), f = ears->direction(), u = ears->up();
				float lp[3] = { (float)p.x, (float)p.y, (float)p.z };
				float lf[3] = { (float)f.x, (float)f.y, (float)f.z };
				float lu[3] = { (float)u.x, (float)u.y, (float)u.z };
				au->setListener(lp, lf, lu);
			}
			au->setGamePaused(AppInstance::GetSingleton()->playState == 2);
			au->update((float)Time::getSingleton()->delta);
			Audio::Refresh();   // cache this frame's analysis
		}
		else Audio::Refresh();   // no provider -> calm zeros

	// Post-process components; each applies only to the Camera on its own atom.
	std::vector<PostProcess*> pps;
	CollectPostProcess(*hierarchy, pps);

	// Time of day drives the first directional light from the Environment's hour.
	float todElev = 0.0f;   // sun elevation -1..1, also used by the sky build
	Environment* todEnv = FindEnvironment(*hierarchy);
	const bool todOn = (todEnv && todEnv->useTimeOfDay);
	if (todOn)
	{
		if (todEnv->daySpeed > 0.0f)
		{
			todEnv->hour += (float)(todEnv->daySpeed * Time::getSingleton()->delta);
			todEnv->hour = std::fmod(todEnv->hour, 24.0f); if (todEnv->hour < 0) todEnv->hour += 24.0f;
		}
		float ang = (todEnv->hour - 6.0f) / 12.0f * 3.14159265f;   // 0 at 06:00, PI at 18:00
		todElev = std::sin(ang);
		float sx = -std::cos(ang), sy = todElev, sz = 0.25f;        // direction TOWARD the sun
		float sl = std::sqrt(sx*sx + sy*sy + sz*sz); if (sl > 1e-6f) { sx/=sl; sy/=sl; sz/=sl; }
		// Daylight ramps up quickly once the sun clears the horizon.
		float day = (todElev + 0.1f) / 0.3f; day = day < 0 ? 0.0f : (day > 1 ? 1.0f : day);
		for (Light* L : lights)
			if (L->type == 0 && L->transform)   // the first directional light = the sun
			{
				glm::quat q = glm::rotation(glm::vec3(0, 0, 1), glm::vec3(-sx, -sy, -sz));   // forward = travel dir
				L->transform->rotation = Quaternion(q.x, q.y, q.z, q.w);
				L->color = Color(1.0, 0.55 + 0.45 * day, 0.25 + 0.75 * day, 1.0);
				L->intensity = day * 4.0f;
				break;
			}
	}

	std::vector<NukeLight> gpuLights;
	for (Light* L : lights)
	{
		if (!L->transform) continue;
		NukeLight n; n.type = L->type;
		Vector3 p = L->transform->globalPosition();
		Vector3 d = L->transform->direction();
		n.pos[0] = (float)p.x; n.pos[1] = (float)p.y; n.pos[2] = (float)p.z;
		n.dir[0] = (float)d.x; n.dir[1] = (float)d.y; n.dir[2] = (float)d.z;
		n.color[0] = (float)L->color.r; n.color[1] = (float)L->color.g; n.color[2] = (float)L->color.b;
		n.intensity = L->intensity; n.range = L->range;
		n.castShadows = L->castShadows ? 1 : 0;
		float outer = L->spotAngle * 0.01745329252f;
		float inner = outer * (1.0f - (L->spotBlend < 0 ? 0 : (L->spotBlend > 1 ? 1 : L->spotBlend)));
		n.spotOuter = std::cos(outer); n.spotInner = std::cos(inner);
		gpuLights.push_back(n);
	}
	// Module-submitted dynamic lights: one-frame submissions appended after the scene's Lights.
	{
		std::vector<NukeLight>& fl = FrameLights::Frame();
		for (const NukeLight& n : fl) gpuLights.push_back(n);
		fl.clear();
	}
	// Must precede setLights: the directional ortho extent uses shadowDistance.
	// AUXILIARY worlds (asset previews) inherit the live world's global shadow settings —
	// their default 2048 must not fight the main world's res every frame (map rebuild thrash).
	if (!auxiliary)
		r->setShadowSettings(settings.shadowRes, settings.shadowDistance, settings.shadowDepthBias,
		                     settings.shadowNormalBias, settings.shadowSoftness);
	r->setLights(gpuLights.empty() ? nullptr : gpuLights.data(), (int)gpuLights.size());

	// Environment (sky + ambient): first Environment component, default sky if none.
	{
		NukeSky sky;
		if (Environment* env = FindEnvironment(*hierarchy))
		{
			sky.mode = env->mode;
			sky.top[0]=(float)env->skyTop.r; sky.top[1]=(float)env->skyTop.g; sky.top[2]=(float)env->skyTop.b;
			sky.horizon[0]=(float)env->skyHorizon.r; sky.horizon[1]=(float)env->skyHorizon.g; sky.horizon[2]=(float)env->skyHorizon.b;
			sky.ground[0]=(float)env->skyGround.r; sky.ground[1]=(float)env->skyGround.g; sky.ground[2]=(float)env->skyGround.b;
			sky.skyIntensity = env->skyIntensity;
			sky.ambient[0]=(float)env->ambient.r; sky.ambient[1]=(float)env->ambient.g; sky.ambient[2]=(float)env->ambient.b;
			sky.ambientIntensity = env->ambientIntensity;
			sky.exposure = env->exposure; sky.whitePoint = env->whitePoint;   // SDR tonemap
			if (env->useTimeOfDay)   // sky colours overridden by the time of day
			{
				auto sat = [](float v){ return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
				auto lrp = [](float a, float b, float t){ return a + (b - a) * t; };
				float el = todElev, day = sat((el + 0.1f) / 0.3f);
				float glow = sat(1.0f - std::fabs(el) * 4.0f) * sat(el * 3.0f + 0.3f);   // dawn/dusk orange band
				sky.top[0]=lrp(0.02f,0.30f,day); sky.top[1]=lrp(0.02f,0.50f,day); sky.top[2]=lrp(0.06f,0.90f,day);
				float h0=lrp(0.04f,0.70f,day), h1=lrp(0.04f,0.80f,day), h2=lrp(0.08f,0.95f,day);
				sky.horizon[0]=lrp(h0,0.95f,glow); sky.horizon[1]=lrp(h1,0.55f,glow); sky.horizon[2]=lrp(h2,0.30f,glow);
				sky.ground[0]=lrp(0.01f,0.20f,day); sky.ground[1]=lrp(0.01f,0.20f,day); sky.ground[2]=lrp(0.02f,0.22f,day);
				sky.skyIntensity = lrp(0.15f, 1.0f, day);
				sky.ambientIntensity = env->ambientIntensity * (0.15f + 0.85f * day);   // dark at night
			}
			if (env->stars)
			{
				float night = 0.4f;
				if (env->useTimeOfDay) { float d = (todElev + 0.1f) / 0.3f; d = d < 0 ? 0 : (d > 1 ? 1 : d); night = 1.0f - d; }
				sky.stars = night < 0 ? 0 : night;
				if (!env->starsTexGuid.empty()) sky.starsTex = ResDB::getSingleton()->GetTexture(env->starsTexGuid);
			}
			if (env->moon && !env->moonTexGuid.empty())   // textured moon opposite the sun
			{
				sky.moonTex  = ResDB::getSingleton()->GetTexture(env->moonTexGuid);
				sky.moonSize = env->moonSize * 0.01745329252f;   // deg -> radians
				float mx = 0, my = 1, mz = 0;
				for (Light* L : lights)
					if (L->type == 0 && L->transform) { Vector3 d = L->transform->direction(); mx=(float)d.x; my=(float)d.y; mz=(float)d.z; break; }
				float ml = std::sqrt(mx*mx + my*my + mz*mz); if (ml > 1e-6f) { mx/=ml; my/=ml; mz/=ml; }
				sky.moonDir[0] = mx; sky.moonDir[1] = my; sky.moonDir[2] = mz;   // sun travel dir
				sky.moonPhase = env->moonPhase;
				float vis = 0.8f;
				if (env->useTimeOfDay) { float d = (todElev + 0.1f) / 0.3f; d = d < 0 ? 0 : (d > 1 ? 1 : d); vis = 1.0f - d; }
				sky.moonAmount = vis < 0 ? 0 : vis;
			}
			if (env->sunDisk)
				for (Light* L : lights)
					if (L->type == 0 && L->transform)   // first directional light = the sky's sun
					{
						Vector3 d = L->transform->direction();
						sky.sunDir[0]=(float)d.x; sky.sunDir[1]=(float)d.y; sky.sunDir[2]=(float)d.z;
						sky.sunColor[0]=(float)L->color.r; sky.sunColor[1]=(float)L->color.g; sky.sunColor[2]=(float)L->color.b;
						sky.sunIntensity = L->intensity;
						break;
					}
		}
		r->setSky(sky);
	}

	// Scene view position, shared by the pusher cap below and the RT nearest-instance pick.
	Vector3 sceneCamP(0, 0, 0);

	// Wind: push the current animated global to the renderer for vertex-bend consumers.
	// Zones/turbulence stay per-point on the CPU (Wind::Sample).
	{
		// Characters and awake dynamic bodies become foliage "pushers". Must be collected
		// before setWind, which writes them into the per-frame BendCB. Cap 8, nearest first.
		struct Push { float x, y, z, r; };
		std::vector<Push> pushers;
		Vector3 camP(0, 0, 0); bool camMain = false, camAny = false;
		std::function<void(bc::list<Atom*>&)> collect = [&](bc::list<Atom*>& gos)
		{
			for (Atom* a : gos)
			{
				if (!a) continue;
				if (Camera* cam = a->GetComponent<Camera>())
				{
					if ((!camAny || (cam->mainCamera && !camMain)) && a->GetName() != "Editor Camera")
					{ camP = a->GetTransform().globalPosition(); camAny = true; camMain = cam->mainCamera; }
				}
				if (CharacterController* cc = a->GetComponent<CharacterController>())
				{
					Vector3 p = a->GetTransform().globalPosition();
					pushers.push_back({ (float)p.x, (float)p.y, (float)p.z, std::max(0.8f, (float)cc->radius * 3.0f) });
				}
				else if (Rigidbody* rb = a->GetComponent<Rigidbody>())
				{
					if (rb->enabled && !rb->isKinematic)
					{
						Vector3 p = a->GetTransform().globalPosition();
						float rad = 1.0f;
						if (Collider* col = a->GetComponent<Collider>())
							rad = std::max(0.6f, (float)std::max({ fabs(col->halfExtents.x), fabs(col->halfExtents.y), fabs(col->halfExtents.z), (double)col->radius }) * 2.0f);
						pushers.push_back({ (float)p.x, (float)p.y, (float)p.z, rad });
					}
				}
				if (!a->children.empty()) collect(a->children);
			}
		};
		collect(*hierarchy);
		// Edit mode renders through the Editor Camera, so anchor the camera-relative caps to
		// it; otherwise "nearest" drifts to a parked game camera.
		{
			AppInstance* app = AppInstance::GetSingleton();
			if (app && app->isEditor() && app->playState == 0)
				for (Atom* a : *hierarchy)
					if (a && a->GetName() == "Editor Camera") { camP = a->GetTransform().globalPosition(); break; }
		}
		sceneCamP = camP;
		if (pushers.size() > 8)
		{
			std::sort(pushers.begin(), pushers.end(), [&](const Push& a, const Push& b)
			{
				float ax = a.x - (float)camP.x, ay = a.y - (float)camP.y, az = a.z - (float)camP.z;
				float bx = b.x - (float)camP.x, by = b.y - (float)camP.y, bz = b.z - (float)camP.z;
				return ax * ax + ay * ay + az * az < bx * bx + by * by + bz * bz;
			});
			pushers.resize(8);
		}
		static std::vector<float> flat; flat.clear();
		for (const Push& p : pushers) flat.insert(flat.end(), { p.x, p.y, p.z, p.r });
		r->setBendPushers(flat.empty() ? nullptr : flat.data(), (int)pushers.size());

		// Bend volumes: live wind zones plus module submissions from the last frame (1-frame
		// latency). Capped at 16 nearest the camera, same policy as the pushers.
		{
			std::vector<BendVolume> vols;
			Wind::CollectZones(vols);
			for (const BendVolume& v : BendVolumes::Consume()) vols.push_back(v);
			if (vols.size() > 16)
			{
				std::sort(vols.begin(), vols.end(), [&](const BendVolume& a, const BendVolume& b)
				{
					float ax = a.pos[0] - (float)camP.x, ay = a.pos[1] - (float)camP.y, az = a.pos[2] - (float)camP.z;
					float bx = b.pos[0] - (float)camP.x, by = b.pos[1] - (float)camP.y, bz = b.pos[2] - (float)camP.z;
					return ax * ax + ay * ay + az * az < bx * bx + by * by + bz * bz;
				});
				vols.resize(16);
			}
			static std::vector<float> vflat; vflat.clear();
			for (size_t i = 0; i < vols.size(); ++i)
			{
				const BendVolume& v = vols[i];
				const float seed = (float)((i * 37) % 13) * 0.7f;
				vflat.insert(vflat.end(), { v.pos[0], v.pos[1], v.pos[2], v.radius,
				                            v.dir[0], v.dir[1], v.dir[2], v.strength,
				                            (float)v.mode, v.falloff, seed, 0.0f });
			}
			r->setBendVolumes(vflat.empty() ? nullptr : vflat.data(), (int)vols.size());
		}

		// Edit mode: World::Update doesn't run, so tick the wind clock here off the real frame
		// delta, or foliage/VFX sway freezes in the edit viewport.
		{
			AppInstance* app = AppInstance::GetSingleton();
			if (app->isEditor() && app->playState == 0)
				Wind::Advance(Time::getSingleton()->delta);
		}
		float wd[4], wp[4];
		Wind::ShaderParams(wd, wp);
		r->setWind(wd, wp);
	}


	// The scene TLAS must be built before the probe capture and camera passes, which ray-query
	// g_TLAS. Auxiliary worlds skip it: the TLAS is global and the live scene owns it.
	// Instanced sets are drawn in every pass below, so gather them once per frame.
	prePhase.reset();   // end "rnd.pre": lights, audio, environment, wind and gizmos are done

	std::vector<InstancedMesh*> instSets;
	CollectInstancedMeshes(*hierarchy, instSets);

	if (r->rtAvailable() && !auxiliary)
	{
		Profiler::Scope pr("rnd.tlas");
		r->beginRTScene();
		std::vector<DrawItem> rtItems; CollectMeshes(*hierarchy, rtItems);
		// Skinned meshes enter via rtProxy, their static bind-pose stand-in.
		for (auto& it : rtItems) if (it.anyOpaque)
		{
			bool cs = !it.mat || it.mat->castShadows;   // same gate as the raster shadow pass
			for (int sl = 0; sl < it.matCount && !cs; ++sl) if (it.mats[sl]) cs = it.mats[sl]->castShadows;
			if (!it.inReflections && !cs) continue;     // invisible to every ray kind
			Mesh* rm = it.mesh->rtProxy ? it.mesh->rtProxy : it.mesh;
			if (it.matCount > 1)
				r->addRTInstanceMulti(rm, it.mats, it.matCount, it.pos, it.quat, it.scale, it.inReflections, cs);
			else
				r->addRTInstance(rm, it.mat, it.pos, it.quat, it.scale, it.inReflections, cs);
		}
		// Instanced sets enter merged: each spatial chunk is one baked Mesh and one TLAS entry.
		// The renderer runs NukeBend over these and refits their BLAS every frame, so RT
		// shadows and reflections sway with the raster blades.
		for (InstancedMesh* im : instSets)
		{
			const bool csI = im->castShadows && (!im->mat || im->mat->castShadows);
			if ((!im->inReflections && !csI) || im->rtMaxInstances <= 0 || !im->EnsureRenderReady(r)) continue;
			if (im->rtChunkMeshes.empty()) continue;
			Transform& t = im->atom->GetTransform();
			Vector3 P = t.globalPosition(); Quaternion Q = t.globalRotation();
			Vector3 S = im->ScaleWithAtom() ? t.globalScale() : Vector3(1, 1, 1);   // foliage frame is T*R only
			float pos[3]   = { (float)P.x, (float)P.y, (float)P.z };
			float quat[4]  = { (float)Q.x, (float)Q.y, (float)Q.z, (float)Q.w };
			float scale[3] = { (float)S.x, (float)S.y, (float)S.z };
			for (Mesh* rm : im->rtChunkMeshes)
				if (rm && rm->numVerts > 0)
					r->addRTInstance(rm, im->mat, pos, quat, scale, im->inReflections, csI);
		}
		// Module components add their own TLAS entries; the gather is open only between
		// beginRTScene and buildRTScene.
		DrawComponentHooks(*hierarchy, r, RenderPhase::RTScene);
		r->buildRTScene();
	}

	// Module loop hooks run before the shadow/probe/camera passes. The submitter draws every
	// opaque mesh through the shadow path into whatever depth target the hook has bound.
	if (!auxiliary)
		for (WorldRenderHook* hk : WorldRenderHooks())
		{
			Profiler::Scope pr("rnd.hooks");
			hk->preRender(r, [&]()
			{
				std::vector<DrawItem> bitems; CollectMeshes(*hierarchy, bitems);
				for (DrawItem& di : bitems)
					if (di.anyOpaque)   // opaque only: glass/particles are not a ground
					{
						if (di.matCount > 1) r->renderShadowObjectMulti(di.mesh, di.mats, di.matCount, di.pos, di.quat, di.scale);
						else if (di.blend == 0) r->renderShadowObject(di.mesh, di.pos, di.quat, di.scale, di.mat);
					}
			});
		}

	// Shadow depth passes, one per shadow-casting dir/spot light, before any camera pass.
	// Skipped under ray tracing: everything shadows via TLAS rays and nothing samples the maps.
	if (!r->rtAvailable())
		for (int sp = 0, spc = r->shadowPassCount(); sp < spc; ++sp)
		{
			Profiler::Scope pr("rnd.shadow");
			r->beginShadowPass(sp);
			RenderShadowMeshes(*hierarchy, r);
			DrawInstancedShadows(instSets, r);
			r->endShadowPass();
		}

	// Reflection probe: capture into its cubemap after shadows (so reflections are lit), then
	// bind it for the camera passes. Static probes capture once; realtime/Bake re-capture.
	ReflectionProbe* probe = FindReflectionProbe(*hierarchy);
	if (probe && probe->transform)
	{
		int res = probe->Res();
		if (probe->cubeId == 0 || probe->builtRes != res)
		{ probe->cubeId = r->createReflectionCube(res); probe->builtRes = res; probe->captured = false; }
		Vector3 pp = probe->transform->globalPosition();
		float pos[3] = { (float)pp.x, (float)pp.y, (float)pp.z };
		if (probe->cubeId && (!probe->captured || probe->realtime || probe->bake))
		{
			Profiler::Scope pr("rnd.probe");
			// Bracket non-realtime captures in the log so a GPU fault is attributable.
			if (!probe->realtime)
				cout << "[World]\t\t\tprobe capture begin (res " << probe->Res() << ")" << endl;
			std::vector<DrawItem> items; CollectMeshes(*hierarchy, items);   // no cull for capture
			// Faces Per Frame budget: 6 (default) is fully live, 1-5 time-slices round-robin.
			// The first capture and Bake are always full.
			const bool slice  = probe->realtime && probe->captured && !probe->bake &&
			                    probe->sliceFaces > 0 && probe->sliceFaces < 6;
			const int  budget = slice ? probe->sliceFaces : 6;
			for (int k = 0; k < budget; ++k)
			{
				const int f = slice ? (probe->sliceFace + k) % 6 : k;
				r->beginCubeFace(probe->cubeId, f, pos, probe->nearZ, probe->farZ);
				for (auto& it : items) if (it.anyOpaque)
				{
					if (it.matCount > 1) r->renderObjectMulti(it.mesh, it.mats, it.matCount, it.pos, it.quat, it.scale, 0);
					else if (it.blend == 0) r->renderObject(it.mesh, it.mat, it.pos, it.quat, it.scale);
				}
				DrawInstancedMeshes(instSets, r, false);   // no cull for capture
				r->endCubeFace(probe->cubeId, f);
			}
			if (slice) probe->sliceFace = (probe->sliceFace + budget) % 6;
			if (!probe->realtime)
				cout << "[World]\t\t\tprobe capture done" << endl;
			probe->captured = true; probe->bake = false;
		}
		float boxHalf[3] = { 0, 0, 0 };
		if (probe->boxProjection)
		{
			// Box projection is a VOLUME: it follows the atom's scale like every other one.
			const Vector3 ph = ScaleExtents(Vector3(probe->boxSize.x * 0.5, probe->boxSize.y * 0.5,
			                                        probe->boxSize.z * 0.5),
			                                probe->transform->globalScale());
			boxHalf[0] = (float)ph.x; boxHalf[1] = (float)ph.y; boxHalf[2] = (float)ph.z;
		}
		r->setReflectionProbe(probe->cubeId, pos, probe->intensity, probe->farZ, boxHalf);
	}
	else { float z[3] = { 0, 0, 0 }; r->setReflectionProbe(0, z, 0.0f, 0.0f, z); }

	const bool editor = AppInstance::GetSingleton()->isEditor();
	for (Camera* cam : cams)
	{
		Profiler::Scope pr("rnd.cameras");
		if (!cam->transform) continue;
		// A camera with a RenderTexture target renders into that texture's RT.
		if (!cam->targetTexGuid.empty())
		{
			Texture* rt = ResDB::getSingleton()->GetTexture(cam->targetTexGuid);
			if (rt && rt->renderTexture && rt->rtId) cam->renderTarget = rt->rtId;
		}
		// In the editor the world is drawn only into off-screen RTs; a target-0 camera would
		// paint the backbuffer full-window. Those still render normally in the Player.
		if (editor && cam->renderTarget == 0) continue;
		NukeCameraDesc d;
		d.target = cam->renderTarget;
		d.vpW = 0; d.vpH = 0; // renderer uses the target's full size
		d.clear[0] = cam->background.r; d.clear[1] = cam->background.g;
		d.clear[2] = cam->background.b; d.clear[3] = cam->background.a;
		Vector3 cp = cam->transform->globalPosition();
		Vector3 cf = cam->transform->direction();
		Vector3 cu = cam->transform->up();
		{
			// View shake: compose the camera-local impulse offset into the eye position.
			float sh[3];
			cam->ShakeOffset(sh);
			if (sh[0] != 0.0f || sh[1] != 0.0f || sh[2] != 0.0f)
			{
				Vector3 cr(cf.y * cu.z - cf.z * cu.y, cf.z * cu.x - cf.x * cu.z, cf.x * cu.y - cf.y * cu.x);
				cp.x += cr.x * sh[0] + cu.x * sh[1] + cf.x * sh[2];
				cp.y += cr.y * sh[0] + cu.y * sh[1] + cf.y * sh[2];
				cp.z += cr.z * sh[0] + cu.z * sh[1] + cf.z * sh[2];
			}
		}
		d.camPos[0] = (float)cp.x; d.camPos[1] = (float)cp.y; d.camPos[2] = (float)cp.z;
		d.camFwd[0] = (float)cf.x; d.camFwd[1] = (float)cf.y; d.camFwd[2] = (float)cf.z;
		d.camUp[0]  = (float)cu.x; d.camUp[1]  = (float)cu.y; d.camUp[2]  = (float)cu.z;
		d.fov   = (float)cam->fov * 0.01745329252f; // degrees -> radians
		d.nearZ = cam->_near;
		d.farZ  = cam->_far;
		d.editorCamera = cam->editorCamera ? 1 : 0;
		// Ease the projection blend toward the target (Perspective 0 / Orthographic 1).
		{
			float tgt = (cam->projection == Projection::Orthographic) ? 1.0f : 0.0f;
			if (!cam->projBlendInit) { cam->projBlend = tgt; cam->projBlendInit = true; }   // no open-time animation
			else if (cam->projTransition <= 0.0f) cam->projBlend = tgt;                     // instant
			else
			{
				float step = (float)Time::getSingleton()->delta * cam->projTransition;
				if (step > 1.0f) step = 1.0f;
				cam->projBlend += (tgt - cam->projBlend) * step;
				if (fabsf(tgt - cam->projBlend) < 0.0005f) cam->projBlend = tgt;             // snap when settled
			}
			d.ortho     = cam->projBlend;
			d.orthoSize = cam->orthoSize;
		}

		// This camera's post chain: the effects on the PostProcess component sharing its transform.
		// Each effect = a post-shader pipeline plus its packed PostParams bytes.
		std::vector<std::vector<float>> ppBlobs; std::vector<uint64_t> ppHandles;
		bool hasSSR = false;   // needs the G-buffer prepass
		bool hasTAA = false;   // needs the depth prepass + camera jitter
		for (PostProcess* pp : pps)
			if (pp->transform == cam->transform)
			{
				pp->EnsureParsed();
				for (PostEffect& e : pp->effects)
				{
					if (!e.enabled) continue;
					Shader* sh = ResDB::getSingleton()->GetShader(e.shaderGuid);
					if (!sh || !sh->isPost || sh->rendererHandle == 0) continue;
					if (sh->name == "ssr" || sh->name == "rtreflect") hasSSR = true;
					if (sh->name == "musicvis") hasSSR = true;   // samples G-buffer normals + depth
					if (sh->name == "taa") hasTAA = true;
					std::vector<float> blob(64, 0.0f);   // 256-byte PostParams
					for (const ShaderProp& sp : sh->props)
					{
						const float* v = sp.def;
						auto pit = e.props.find(sp.name);
						if (pit != e.props.end()) v = pit->second.data();
						// g_Nuke* params are engine-filled from the audio analysis; user values
						// and defaults never apply to them.
						float sys[4];
						if (sp.name.compare(0, 6, "g_Nuke") == 0 && Audio::SystemParam(sp.name, sys)) v = sys;
						for (int c = 0; c < sp.components && (sp.offset / 4 + c) < 64; ++c)
							blob[sp.offset / 4 + c] = v[c];
					}
					ppHandles.push_back(sh->rendererHandle); ppBlobs.push_back(std::move(blob));
				}
				break;
			}
		std::vector<NukePostStage> ppStages(ppHandles.size());
		for (size_t k = 0; k < ppHandles.size(); ++k)   // blobs are stable now -> safe to take .data()
		{ ppStages[k].pipeline = ppHandles[k]; ppStages[k].params = ppBlobs[k].data(); ppStages[k].paramFloats = 64; }
		r->setPostChain(ppStages.empty() ? nullptr : ppStages.data(), (int)ppStages.size());

		r->setCameraTAA(hasTAA);   // enable jitter + history for this camera

		// This camera renders only atoms whose layer bit is set.
		const unsigned int camMask = (unsigned int)cam->layerMask;

		// Decals reconstruct surfaces from the depth prepass, so their presence also forces it.
		std::vector<Decal*> decals;
		std::vector<InstancedMesh*> camInstSets;
		{
			Profiler::Scope ps("rnd.cam.collect");
			CollectDecals(*hierarchy, decals, camMask);
			CollectInstancedMeshes(*hierarchy, camInstSets, camMask);
		}

		// Module hooks may demand scene depth too.
		bool hookPrepass = false;
		for (WorldRenderHook* hk : WorldRenderHooks())
			if (hk->wantsScenePrepass()) { hookPrepass = true; break; }
		if (hasSSR || hasTAA || !decals.empty() || hookPrepass)
		{
			Profiler::Scope ps("rnd.cam.gbuf");
			r->beginGBufferPass(d);
			std::vector<DrawItem> gitems; CollectMeshes(*hierarchy, gitems, camMask);
			DrawGBuffer(gitems, r, settings.frustumCull);
			DrawInstancedGBuffer(camInstSets, r, settings.frustumCull);
			r->endGBufferPass();
		}

		{ Profiler::Scope ps("rnd.cam.begin"); r->beginCamera(d); }
		{
			std::vector<DrawItem> items;
			{ Profiler::Scope ps("rnd.cam.collect"); CollectMeshes(*hierarchy, items, camMask); }
			{ Profiler::Scope ps("rnd.cam.opaque"); DrawCollected(items, cp, r, settings.frustumCull); }
			{ Profiler::Scope ps("rnd.cam.inst");   DrawInstancedMeshes(camInstSets, r, settings.frustumCull); }
			{ Profiler::Scope ps("rnd.cam.hooks");  DrawComponentHooks(*hierarchy, r, RenderPhase::Opaque, camMask); }
			{ Profiler::Scope ps("rnd.cam.decals"); DrawDecals(decals, r); }
			{ Profiler::Scope ps("rnd.cam.sprites"); DrawSprites(*hierarchy, d, cp, r, cam, camMask); }   // after opaque, back-to-front
			{
				Profiler::Scope ps("rnd.cam.trans");
				DrawComponentHooks(*hierarchy, r, RenderPhase::Transparent, camMask);
				if (editor && cam->editorCamera) DrawCanvasGizmos(*hierarchy, r, AppInstance::GetSingleton()->selectedInHieararchy);
				if (editor) DrawDecalGizmos(*hierarchy, r, AppInstance::GetSingleton()->selectedInHieararchy);
				DrawComponentHooks(*hierarchy, r, RenderPhase::Overlay, camMask);
			}
		}
		// Editor only: outline the selection after the scene. A model is a SUBTREE — the root
		// usually carries no mesh of its own, so every mesh below it goes into ONE mask and the
		// edge pass closes it once: a composite object gets a single silhouette.
		if (editor)
			if (Atom* sel = AppInstance::GetSingleton()->selectedInHieararchy)
			{
				Profiler::Scope ps("rnd.cam.outline");
				bool opened = false;
				std::function<void(Atom*)> outline = [&](Atom* a)
				{
					if (!a || !a->enabled) return;
					if (MeshRenderer* mr = a->GetComponent<MeshRenderer>())
						if (mr->enabled && mr->mesh)
						{
							if (!opened) { r->selectionOutlineBegin(); opened = true; }
							Transform& t = a->GetTransform();
							Vector3    p = t.globalPosition();
							Quaternion q = t.globalRotation();
							Vector3    s = t.globalScale();
							float pos[3]   = { (float)p.x, (float)p.y, (float)p.z };
							float quat[4]  = { (float)q.x, (float)q.y, (float)q.z, (float)q.w };
							float scale[3] = { (float)s.x, (float)s.y, (float)s.z };
							r->selectionOutlineAdd(mr->mesh, pos, quat, scale);
						}
					for (Atom* c : a->children) outline(c);
				};
				outline(sel);
				if (opened) r->selectionOutlineEnd();
			}
		{ Profiler::Scope ps("rnd.cam.end"); r->endCamera(); }
	}
	{
		Profiler::Scope ps("rnd.postframe");
		UpdatePrevTransforms(*hierarchy);   // snapshot transforms for next frame's TAA motion vectors
		Game::FlushScreenshot();            // queued Game.Screenshot: the frame is complete here
	}
}

// --- world serialization (.nuworld JSON via reflection) ---

static void SaveAtom(Atom* atom, json& j)
{
	j["name"] = atom->GetName();
	j["id"]   = atom->id.id;   // stable identity across rebuilds
	if (!atom->prefabGuid.empty()) j["prefab"] = atom->prefabGuid;   // instance link to a .nuprefab
	if (atom->layer != 0) j["layer"] = atom->layer;                  // 0 = Default, omitted
	if (atom->persistent) j["persistent"] = true;                    // survives world switches
	if (!atom->enabled) j["enabled"] = false;
	Transform& t = atom->GetTransform();
	if (TypeInfo* tti = t.GetType())
		SaveObject(*tti, &t, j["transform"]);
	for (Component* c : atom->components)
	{
		if (UnknownComponent* uc = dynamic_cast<UnknownComponent*>(c))
		{
			// Plugin type not loaded: write the preserved type + props back verbatim.
			json cj;
			cj["type"]  = uc->typeName;
			cj["props"] = uc->rawProps.empty() ? json::object()
			                                   : json::parse(uc->rawProps, nullptr, false);
			if (!uc->requiredPlugin.empty()) cj["plugin"] = uc->requiredPlugin;
			cj["enabled"] = uc->enabled;
			cj["cid"]     = uc->id.id;
			j["components"].push_back(cj);
			continue;
		}
		TypeInfo* ti = c->GetType();
		if (!ti) continue;                       // unreflected component
		c->OnBeforeSave();                       // live-state components re-encode into their props first
		json cj;
		cj["type"] = ti->name;
		SaveObject(*ti, c, cj["props"]);
		// Material instance overrides save with the world; the referenced .numat asset is never
		// modified by world edits.
		if (MeshRenderer* mr = dynamic_cast<MeshRenderer*>(c))
			if (mr->mat)
			{
				json jm;
				jm["color"]  = { mr->mat->color.r, mr->mat->color.g, mr->mat->color.b, mr->mat->color.a };
				jm["shader"] = mr->mat->shaderGuid;
				jm["diffuse"]    = mr->mat->diffuseGuid;
				jm["normal"]     = mr->mat->normalGuid;
				jm["specularMap"]= mr->mat->specularGuid;
				jm["metalRough"] = mr->mat->metalRoughGuid;
				jm["occlusion"]  = mr->mat->occlusionGuid;
				jm["emissiveMap"]= mr->mat->emissiveGuid;
				jm["metallic"]   = mr->mat->metallic;
				jm["roughness"]  = mr->mat->roughness;
				jm["specularFactor"] = mr->mat->specular;
				jm["emissive"]   = { mr->mat->emissive.r, mr->mat->emissive.g, mr->mat->emissive.b };
				jm["emissiveIntensity"] = mr->mat->emissiveIntensity;
				jm["castShadows"] = mr->mat->castShadows;
				jm["receiveShadows"] = mr->mat->receiveShadows;
				jm["blendMode"]   = mr->mat->blendMode;
				if (!mr->mat->props.empty())
				{
					json jp = json::object();
					for (const auto& kv : mr->mat->props)
						jp[kv.first] = { kv.second[0], kv.second[1], kv.second[2], kv.second[3] };
					jm["props"] = jp;
				}
				cj["material"] = jm;
			}
		const char* pl = PluginForType(ti->name);   // tag which plugin a component requires
		if (pl && pl[0]) cj["plugin"] = pl;
		cj["enabled"] = c->enabled;
		if (c->tickEvery > 1) cj["tick"] = c->tickEvery;   // 1 = default, omitted
		cj["cid"]     = c->id.id;
		j["components"].push_back(cj);
	}
	for (Atom* ch : atom->children)
	{
		json chj;
		SaveAtom(ch, chj);
		j["children"].push_back(chj);
	}
}

static Atom* LoadAtom(const json& j)
{
	Atom* atom = new Atom(j.value("name", std::string("Atom")).c_str());
	if (j.contains("id")) { atom->id.id = j["id"].get<long>(); ID::observe(atom->id.id); }   // keep the saved identity
	atom->prefabGuid = j.value("prefab", std::string());
	atom->modOrigin  = j.value("__mod", std::string());        // merge provenance, runtime only
	atom->layer      = std::max(0, std::min(31, j.value("layer", 0)));
	atom->persistent = j.value("persistent", false);
	atom->enabled    = j.value("enabled", true);
	if (j.contains("transform"))
	{
		Transform& t = atom->GetTransform();
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
				c->enabled   = cj.value("enabled", true);
				c->tickEvery = cj.value("tick", 1);
				c->id.id   = cj.value("cid", c->id.id); ID::observe(c->id.id);
				c->modOrigin = cj.value("__mod", std::string());   // merge provenance, runtime only
				if (cj.contains("props")) LoadObject(*ti, c, cj["props"]);
				atom->AddComponent(c);             // Init() wires transform/owner + clones the material
				// Saved overrides must be applied after Init, onto the cloned instance.
				if (MeshRenderer* mr = dynamic_cast<MeshRenderer*>(c))
					if (cj.contains("material") && cj["material"].is_object())
					{
						const json& jm = cj["material"];
						if (!mr->mat) mr->mat = new Material();   // matGuid empty or asset missing
						if (jm.contains("color") && jm["color"].is_array() && jm["color"].size() == 4)
						{
							mr->mat->color.r = jm["color"][0]; mr->mat->color.g = jm["color"][1];
							mr->mat->color.b = jm["color"][2]; mr->mat->color.a = jm["color"][3];
						}
						if (jm.contains("shader")) mr->mat->shaderGuid = jm.value("shader", std::string("world"));
						// Applied only when present, so older worlds keep the cloned asset maps.
						if (jm.contains("diffuse"))     mr->mat->diffuseGuid    = jm.value("diffuse", std::string());
						if (jm.contains("normal"))      mr->mat->normalGuid     = jm.value("normal", std::string());
						if (jm.contains("specularMap")) mr->mat->specularGuid   = jm.value("specularMap", std::string());
						if (jm.contains("metalRough"))  mr->mat->metalRoughGuid = jm.value("metalRough", std::string());
						if (jm.contains("occlusion"))   mr->mat->occlusionGuid  = jm.value("occlusion", std::string());
						if (jm.contains("emissiveMap")) mr->mat->emissiveGuid   = jm.value("emissiveMap", std::string());
						if (jm.contains("metallic"))    mr->mat->metallic       = jm.value("metallic", 0.0f);
						if (jm.contains("roughness"))   mr->mat->roughness      = jm.value("roughness", 0.6f);
						if (jm.contains("specularFactor")) mr->mat->specular    = jm.value("specularFactor", 1.0f);
						if (jm.contains("emissiveIntensity")) mr->mat->emissiveIntensity = jm.value("emissiveIntensity", 0.0f);
						if (jm.contains("castShadows"))    mr->mat->castShadows    = jm.value("castShadows", true);
						if (jm.contains("receiveShadows")) mr->mat->receiveShadows = jm.value("receiveShadows", true);
						if (jm.contains("blendMode"))      mr->mat->blendMode      = (Material::Blend)jm.value("blendMode", 0);
						if (jm.contains("emissive") && jm["emissive"].is_array() && jm["emissive"].size() == 3)
						{
							mr->mat->emissive.r = jm["emissive"][0]; mr->mat->emissive.g = jm["emissive"][1];
							mr->mat->emissive.b = jm["emissive"][2];
						}
						if (jm.contains("props") && jm["props"].is_object())
							for (auto it = jm["props"].begin(); it != jm["props"].end(); ++it)
								if (it.value().is_array())
								{
									std::array<float, 4> a = { 0, 0, 0, 0 };
									for (int i = 0; i < 4 && i < (int)it.value().size(); ++i) a[i] = it.value()[i].get<float>();
									mr->mat->props[it.key()] = a;
								}
						mr->mat->Resolve();   // re-bind shader/textures
					}
			}
			else
			{
				// Type inactive: keep it inert, preserve its data and the plugin it needs.
				UnknownComponent* uc = new UnknownComponent();
				uc->typeName = type;
				uc->enabled = cj.value("enabled", true);
				uc->id.id   = cj.value("cid", uc->id.id); ID::observe(uc->id.id);
				if (cj.contains("props")) uc->rawProps = cj["props"].dump();
				uc->requiredPlugin = cj.value("plugin", std::string(PluginForType(type)));
				atom->AddComponent(uc);
			}
		}
	if (j.contains("children"))
		for (const json& chj : j["children"])
			LoadAtom(chj)->SetParent(atom);
	return atom;
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
		*it = uc;            // replace in place, preserving order
		// Destroy() before delete: the lifecycle hook unsubscribes module-code std::functions
		// from engine statics, which would otherwise dangle after the DLL unloads.
		c->Destroy();
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
		a->AddComponent(c);   // Init wires transform/owner + side effects
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

// Assigns fresh unique ids across a subtree, recording old->new so queued AtomRef fixups
// pointing inside the subtree follow their clones.
static void RegenIds(Atom* a, std::map<unsigned long, unsigned long>& oldToNew)
{
	if (!a) return;
	const unsigned long old = (unsigned long)a->id.id;
	a->id.generate();
	oldToNew[old] = (unsigned long)a->id.id;
	for (Component* c : a->components) c->id.generate();
	for (Atom* c : a->children) RegenIds(c, oldToNew);
}

Atom* LoadPrefab(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream f(p);
	if (!f) return nullptr;
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return LoadPrefabFromString(text);
}

Atom* LoadPrefabFromString(const std::string& text)
{
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded()) return nullptr;
	Atom* a = LoadAtom(j);
	std::map<unsigned long, unsigned long> ids;
	RegenIds(a, ids);                    // instances must not share the prefab's saved ids
	Reflect_RemapPendingAtomRefs(ids);   // refs inside the subtree follow their clones
	Reflect_ResolveAtomRefs();           // remaining world-external ids -> null
	return a;
}

// The prefab's own GUID (the "prefab" field of its root), or "" if it predates prefab linking.
std::string PrefabGuid(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream f(p);
	if (!f) return std::string();
	std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	return PrefabGuidFromString(text);
}

std::string PrefabGuidFromString(const std::string& text)
{
	json j = json::parse(text, nullptr, false);
	if (j.is_discarded()) return std::string();
	return j.value("prefab", std::string());
}

// Script-facing prefab spawn: reads content through the engine's layered resolution,
// reconstructs the subtree and adds it to the current world root.
Atom* Prefabs::Spawn(const std::string& contentRelPath)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (!app || !app->currentWorld || contentRelPath.empty()) return nullptr;
	std::string text;
	if (!app->ReadContent(contentRelPath, text) || text.empty())
	{
		std::cout << "[Prefab]\t\tInstantiate: '" << contentRelPath << "' not found in content" << std::endl;
		return nullptr;
	}
	Atom* a = LoadPrefabFromString(text);
	if (!a)
	{
		std::cout << "[Prefab]\t\tInstantiate: '" << contentRelPath << "' is not a valid prefab" << std::endl;
		return nullptr;
	}
	app->currentWorld->LockGame();   // scripts spawn mid-frame
	app->currentWorld->Add(a);
	app->currentWorld->UnlockGame();
	return a;
}

std::string SaveAtomToString(Atom* root)
{
	if (!root) return std::string();
	json j;
	SaveAtom(root, j);
	return j.dump();
}

Atom* LoadAtomFromString(const std::string& data)
{
	if (data.empty()) return nullptr;
	json j = json::parse(data, nullptr, false);
	if (j.is_discarded()) return nullptr;
	Atom* a = LoadAtom(j);
	Reflect_ResolveAtomRefs();   // AtomRef props in the restored subtree
	return a;
}

// Clone variant of LoadAtomFromString: the subtree gets fresh ids, since two atoms must never
// share a stable id. AtomRefs inside the subtree follow their clones; external refs are kept.
Atom* CloneAtomFromString(const std::string& data)
{
	if (data.empty()) return nullptr;
	json j = json::parse(data, nullptr, false);
	if (j.is_discarded()) return nullptr;
	Atom* a = LoadAtom(j);
	if (!a) return nullptr;
	std::map<unsigned long, unsigned long> ids;
	RegenIds(a, ids);
	Reflect_RemapPendingAtomRefs(ids);
	Reflect_ResolveAtomRefs();
	return a;
}

// --- undo support: remove an atom subtree by id; insert one at a placement (parentId 0 = root) ---
static void DeleteSubtree(Atom* a)
{
	if (!a) return;
	for (Atom* c : a->children) DeleteSubtree(c);
	// Components must release what they own, and script handles into the atom must be dropped
	// before the memory goes.
	for (Component* c : a->components)
		if (c) { c->Destroy(); Reflect_DropObject(c); delete c; }
	a->components.clear();
	Reflect_DropObject(&a->GetTransform());
	delete a;
}

void World::RemoveAtomById(long id)
{
	Atom* a = GetById(id);
	if (!a) return;
	if (a->parent) a->parent->children.remove(a);
	else           hierarchy->remove(a);
	DeleteSubtree(a);
}

void World::InsertAtom(Atom* a, long parentId, int index)
{
	if (!a) return;
	Atom* parent = parentId ? GetById(parentId) : nullptr;
	a->parent = parent;
	bc::list<Atom*>& lst = parent ? parent->children : *hierarchy;
	if (index < 0 || index >= (int)lst.size()) { lst.push_back(a); return; }
	auto it = lst.begin();
	std::advance(it, index);
	lst.insert(it, a);
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
	j["name"] = name;
	j["settings"] = {
		{"shadowRes", settings.shadowRes}, {"shadowDistance", settings.shadowDistance},
		{"shadowDepthBias", settings.shadowDepthBias}, {"shadowNormalBias", settings.shadowNormalBias},
		{"shadowSoftness", settings.shadowSoftness}, {"frustumCull", settings.frustumCull},
		{"gravity", { settings.gravity[0], settings.gravity[1], settings.gravity[2] }},
		{"fixedDt", settings.fixedDt} };
	// Capture the live calendar + pending event schedule so a savegame resumes at the exact
	// in-game moment. Only the real world owns the clock; auxiliary worlds must not clobber it.
	if (!auxiliary)
	{
		Time* t = Time::getSingleton();
		j["calendar"] = { {"gtr", t->gtr}, {"year", t->year}, {"month", t->month}, {"day", t->day},
		                  {"hour", t->hour}, {"minute", t->minute}, {"sec", t->sec},
		                  {"totalgt", t->totalgt}, {"totalgd", t->totalgd} };
		j["events"] = Events::SaveJson();
		Wind::SaveJson(j);   // "wind" block, omitted when windless
	}
	j["atoms"] = json::array();
	for (Atom* atom : *hierarchy)
	{
		if (atom->GetName() == "Editor Camera") continue;   // editor infra, not part of the world
		json gj;
		SaveAtom(atom, gj);
		j["atoms"].push_back(gj);
	}
	return j.dump(2);
}

// Repairs duplicate ids: atom ids must be globally unique, component ids unique within an atom.
static void FixDuplicateIds(bc::list<Atom*>& gos, std::set<unsigned long>& seenAtoms)
{
	for (Atom* atom : gos)
	{
		if (atom->id.id == 0 || seenAtoms.count(atom->id.id)) atom->id.generate();
		seenAtoms.insert(atom->id.id);
		std::set<unsigned long> seenComps;
		for (Component* c : atom->components)
		{
			if (c->id.id == 0 || seenComps.count(c->id.id)) c->id.generate();
			seenComps.insert(c->id.id);
		}
		FixDuplicateIds(atom->children, seenAtoms);
	}
}

// ---- packed-world layer merge (mods) ----
// World files merge semantically, not by file: every layer is diffed against the base copy and
// the diffs apply bottom-up, at atom (id), component (cid) and scalar-field granularity.
// Additions union, deletions apply, conflicts go to the higher layer.
namespace {
struct MergeRec
{
	json atom;          // the atom object without "children"; hierarchy tracked via parent
	long parent = 0;    // 0 = world root
	int  order  = 0;    // emit order (base order first, additions after)
};
void FlattenAtoms(const json& arr, long parentId, std::map<long, MergeRec>& out, int& counter)
{
	if (!arr.is_array()) return;
	for (const json& a : arr)
	{
		if (!a.is_object()) continue;
		long id = a.value("id", 0L);
		if (!id) continue;
		MergeRec r;
		r.atom = a; r.atom.erase("children");
		r.parent = parentId;
		r.order  = counter++;
		out[id] = std::move(r);
		if (a.contains("children")) FlattenAtoms(a["children"], id, out, counter);
	}
}
// Three-way per-field object merge: imposes onto `cur` only the keys this layer changed vs
// its baseline. `depth` recurses that many levels into nested objects; arrays and scalars are
// atomic. A shape change to non-object makes the layer win outright.
json MergeObject3(const json& baseO, json cur, const json& layerO, int depth)
{
	if (!cur.is_object() || !layerO.is_object())
		return layerO;
	std::set<std::string> keys;
	if (baseO.is_object())
		for (auto it = baseO.begin(); it != baseO.end(); ++it) keys.insert(it.key());
	for (auto it = layerO.begin(); it != layerO.end(); ++it) keys.insert(it.key());
	for (const std::string& k : keys)
	{
		const json b = baseO.is_object() && baseO.contains(k) ? baseO[k] : json();
		const json l = layerO.contains(k) ? layerO[k] : json();
		if (l == b) continue;                              // untouched by this layer
		if (l.is_null()) { cur.erase(k); continue; }       // removed by this layer
		if (depth > 0 && l.is_object() && cur.contains(k))
			cur[k] = MergeObject3(b, cur[k], l, depth - 1);
		else
			cur[k] = l;
	}
	return cur;
}

// Applies one layer's atom onto the merged atom, three-way against the base. `layerName` tags
// components the layer adds with "__mod" for provenance.
json MergeAtomJson(const json& baseA, json cur, const json& layerA, const std::string& layerName)
{
	for (const char* key : { "name", "prefab" })
	{
		const json b = baseA.contains(key) ? baseA[key] : json();
		const json l = layerA.contains(key) ? layerA[key] : json();
		if (l != b) { if (l.is_null()) cur.erase(key); else cur[key] = l; }
	}
	{
		// Transform merges per-subkey so a layer that moved an atom doesn't undo a rescale.
		const json b = baseA.contains("transform") ? baseA["transform"] : json();
		const json l = layerA.contains("transform") ? layerA["transform"] : json();
		if (l != b)
		{
			if (l.is_null()) cur.erase("transform");
			else cur["transform"] = MergeObject3(b, cur.contains("transform") ? cur["transform"] : json::object(), l, 1);
		}
	}
	auto index = [](const json& a) {
		std::map<long, json> m;
		if (a.contains("components") && a["components"].is_array())
			for (const json& c : a["components"])
				if (c.is_object()) m[c.value("cid", 0L)] = c;
		return m;
	};
	std::map<long, json> bC = index(baseA), lC = index(layerA);
	json comps = json::array();
	std::set<long> emitted;
	if (cur.contains("components") && cur["components"].is_array())
		for (const json& c : cur["components"])
		{
			long cid = c.is_object() ? c.value("cid", 0L) : 0L;
			if (bC.count(cid) && !lC.count(cid)) continue;          // deleted by this layer
			auto lit = lC.find(cid);
			const bool changed = lit != lC.end() && (!bC.count(cid) || bC[cid] != lit->second);
			// A changed component merges per-prop, so the layer imposes only edited fields.
			comps.push_back(changed ? MergeObject3(bC.count(cid) ? bC[cid] : json(), c, lit->second, 2) : c);
			emitted.insert(cid);
		}
	for (auto& kv : lC)                                             // components ADDED by this layer
		if (!emitted.count(kv.first) && !bC.count(kv.first))
		{
			json add = kv.second;
			if (!layerName.empty()) add["__mod"] = layerName;       // provenance badge
			comps.push_back(add);
		}
	cur["components"] = comps;
	return cur;
}
json EmitAtomTree(long id, std::map<long, MergeRec>& recs, std::map<long, std::vector<long>>& kids)
{
	json j = recs[id].atom;
	auto it = kids.find(id);
	if (it != kids.end())
		for (long ch : it->second)
			j["children"].push_back(EmitAtomTree(ch, recs, kids));
	return j;
}
// The running merged state: the flat atom map + the world settings object.
struct MergeState
{
	std::map<long, MergeRec> atoms;
	json settings = json::object();
};
// Applies one layer onto `cur`, three-way against `baseline` (what that layer's author saw).
// The counter pointers are optional.
void ApplyLayer(MergeState& cur, const MergeState& baseline,
                const json& Ldoc, const std::map<long, MergeRec>& lMap,
                const std::string& layerName,
                int* adds, int* changes, int* dels)
{
	if (Ldoc.contains("settings") && Ldoc["settings"].is_object())
		for (auto it = Ldoc["settings"].begin(); it != Ldoc["settings"].end(); ++it)
		{
			const json b = baseline.settings.contains(it.key()) ? baseline.settings[it.key()] : json();
			if (it.value() != b) cur.settings[it.key()] = it.value();
		}
	for (auto& kv : lMap)
	{
		auto bit = baseline.atoms.find(kv.first);
		if (bit == baseline.atoms.end())
		{
			// Added by this layer; if two layers add the same id the higher wins.
			if (adds && !cur.atoms.count(kv.first)) ++*adds;
			MergeRec r = kv.second;
			if (!layerName.empty()) r.atom["__mod"] = layerName;   // provenance badge
			cur.atoms[kv.first] = std::move(r);
		}
		else if (cur.atoms.count(kv.first))
		{
			MergeRec& c = cur.atoms[kv.first];
			if (kv.second.atom != bit->second.atom)
			{
				c.atom = MergeAtomJson(bit->second.atom, c.atom, kv.second.atom, layerName);
				if (changes) ++*changes;
			}
			if (kv.second.parent != bit->second.parent) c.parent = kv.second.parent;   // reparented
		}
	}
	for (auto& kv : baseline.atoms)                          // deleted by this layer
		if (!lMap.count(kv.first) && cur.atoms.erase(kv.first) && dels) ++*dels;
}
}  // namespace

std::string World::MergeWorldLayers(const std::vector<std::string>& layers)
{
	return MergeWorldLayers(layers, std::vector<std::vector<int>>(), std::vector<std::string>());
}

std::string World::MergeWorldLayers(const std::vector<std::string>& layers,
                                    const std::vector<std::vector<int>>& deps)
{
	return MergeWorldLayers(layers, deps, std::vector<std::string>());
}

std::string World::MergeWorldLayers(const std::vector<std::string>& layers,
                                    const std::vector<std::vector<int>>& deps,
                                    const std::vector<std::string>& basis)
{
	return MergeWorldLayers(layers, deps, basis, std::vector<std::string>());
}

std::string World::MergeWorldLayers(const std::vector<std::string>& layers,
                                    const std::vector<std::vector<int>>& deps,
                                    const std::vector<std::string>& basis,
                                    const std::vector<std::string>& names)
{
	if (layers.empty())     return std::string();
	if (layers.size() == 1) return layers[0];
	json base = json::parse(layers[0], nullptr, false);
	if (base.is_discarded() || !base.is_object()) return layers.back();   // unmergeable base: top wins

	// Parse + flatten every layer once. A layer's additions take order slots after the base
	// block, so rebuilt hierarchies keep the base order with additions appended.
	struct Parsed { json doc; std::map<long, MergeRec> flat; bool ok = false; };
	std::vector<Parsed> P(layers.size());
	int counter = 0;
	{
		P[0].doc = base; P[0].ok = true;
		FlattenAtoms(base.contains("atoms") ? base["atoms"] : json::array(), 0, P[0].flat, counter);
	}
	for (size_t i = 1; i < layers.size(); ++i)
	{
		P[i].doc = json::parse(layers[i], nullptr, false);
		P[i].ok = !P[i].doc.is_discarded() && P[i].doc.is_object();
		if (!P[i].ok) continue;   // corrupt layer: skipped, the rest still merges
		int c = counter;
		FlattenAtoms(P[i].doc.contains("atoms") ? P[i].doc["atoms"] : json::array(), 0, P[i].flat, c);
	}

	// Recorded baselines: what layer i's author saw at pack time. A layer with one never
	// phantom-deletes content the base gained after it was authored.
	std::vector<Parsed> B(layers.size());
	for (size_t i = 1; i < layers.size() && i < basis.size(); ++i)
	{
		if (basis[i].empty()) continue;
		B[i].doc = json::parse(basis[i], nullptr, false);
		B[i].ok = !B[i].doc.is_discarded() && B[i].doc.is_object();
		if (!B[i].ok) continue;
		int c = counter;
		FlattenAtoms(B[i].doc.contains("atoms") ? B[i].doc["atoms"] : json::array(), 0, B[i].flat, c);
	}

	// Transitive dependency closure per layer, ascending (mount order); base 0 implicit. A
	// layer's diff baseline is base + its dependencies, so a patch built on A+B doesn't
	// re-impose A's and B's changes as its own.
	auto closureOf = [&](int idx) {
		std::set<int> s;
		std::function<void(int)> walk = [&](int i) {
			if (i <= 0 || i >= (int)deps.size()) return;
			for (int d : deps[i])
				if (d > 0 && d < (int)layers.size() && s.insert(d).second) walk(d);
		};
		walk(idx);
		return std::vector<int>(s.begin(), s.end());   // std::set: already ascending
	};

	// Merged state of base + an ascending index set, memoized since dependency sets repeat.
	std::map<std::string, MergeState> memo;
	std::function<MergeState(const std::vector<int>&, int*, int*, int*)> mergeSet =
		[&](const std::vector<int>& S, int* adds, int* changes, int* dels) -> MergeState
	{
		std::string key;
		for (int i : S) key += std::to_string(i) + ",";
		if (!adds)   // memo only the counter-less baseline computations
		{
			auto it = memo.find(key);
			if (it != memo.end()) return it->second;
		}
		MergeState st;
		st.atoms = P[0].flat;
		if (P[0].doc.contains("settings") && P[0].doc["settings"].is_object()) st.settings = P[0].doc["settings"];
		for (int idx : S)
		{
			if (!P[idx].ok) continue;
			MergeState bl;
			if (idx < (int)B.size() && B[idx].ok)
			{
				// A recorded basis wins over any reconstruction: it is frozen at pack time.
				bl.atoms = B[idx].flat;
				if (B[idx].doc.contains("settings") && B[idx].doc["settings"].is_object())
					bl.settings = B[idx].doc["settings"];
			}
			else
				bl = mergeSet(closureOf(idx), nullptr, nullptr, nullptr);
			const std::string lname = idx < (int)names.size() ? names[idx] : std::string();
			ApplyLayer(st, bl, P[idx].doc, P[idx].flat, lname, adds, changes, dels);
		}
		if (!adds) memo[key] = st;
		return st;
	};

	std::vector<int> all;
	for (size_t i = 1; i < layers.size(); ++i) all.push_back((int)i);
	int adds = 0, changes = 0, dels = 0;
	MergeState fin = mergeSet(all, &adds, &changes, &dels);

	// Rebuild the nested hierarchy: children grouped by parent (orphans -> root), emitted in
	// (order, id) so base atoms keep their order and additions follow.
	std::map<long, std::vector<long>> kids;
	std::vector<long> roots;
	for (auto& kv : fin.atoms)
	{
		long p = kv.second.parent;
		if (p && fin.atoms.count(p)) kids[p].push_back(kv.first);
		else                         roots.push_back(kv.first);
	}
	auto byOrder = [&](long a, long b) {
		return fin.atoms[a].order != fin.atoms[b].order ? fin.atoms[a].order < fin.atoms[b].order : a < b;
	};
	std::sort(roots.begin(), roots.end(), byOrder);
	for (auto& kv : kids) std::sort(kv.second.begin(), kv.second.end(), byOrder);
	json merged = base;
	merged["settings"] = fin.settings;
	json atoms = json::array();
	for (long id : roots) atoms.push_back(EmitAtomTree(id, fin.atoms, kids));
	merged["atoms"] = atoms;
	std::cout << "[World]\t\t\t" << "merged " << layers.size() << " layers (" << adds << " added, "
	          << changes << " changed, " << dels << " removed atoms)" << std::endl;
	return merged.dump();
}

void World::LoadFromString(const std::string& data)
{
	json j = json::parse(data, nullptr, false);
	if (j.is_discarded()) { std::cout << "[World]\t\t\t" << "LoadFromString: bad JSON" << std::endl; return; }
	LoadFromJson(j);
}

void World::LoadFromJson(const json& j)
{
	LoadHeaderFromJson(j);
	if (j.contains("atoms"))
		for (const json& gj : j["atoms"])
			Add(LoadAtom(gj));
	FinalizeIncrementalLoad();
}

void World::LoadHeaderFromJson(const json& j)
{
	// Empty when the file carries no name; AppInstance::OpenWorld then fills it from the stem.
	name = j.value("name", std::string());
	settings = Settings{};   // defaults, then override from file
	if (j.contains("settings") && j["settings"].is_object())
	{
		const json& s = j["settings"];
		settings.shadowRes        = s.value("shadowRes", settings.shadowRes);
		settings.shadowDistance   = s.value("shadowDistance", settings.shadowDistance);
		settings.shadowDepthBias  = s.value("shadowDepthBias", settings.shadowDepthBias);
		settings.shadowNormalBias = s.value("shadowNormalBias", settings.shadowNormalBias);
		settings.shadowSoftness   = s.value("shadowSoftness", settings.shadowSoftness);
		settings.frustumCull      = s.value("frustumCull", settings.frustumCull);
		if (s.contains("gravity") && s["gravity"].is_array() && s["gravity"].size() == 3)
			for (int i = 0; i < 3; ++i) settings.gravity[i] = s["gravity"][i].get<float>();
		settings.fixedDt = s.value("fixedDt", settings.fixedDt);
	}
	// Restore the calendar + pending event schedule. Only the real world touches the global
	// clock. A world without a calendar block resets the schedule rather than inheriting the
	// previous world's pending incidents.
	if (!auxiliary)
	{
		if (j.contains("calendar") && j["calendar"].is_object())
		{
			const json& c = j["calendar"];
			Time* t = Time::getSingleton();
			t->Init(c.value("gtr", t->gtr), c.value("day", 1), c.value("month", 1), c.value("year", 2000),
			        c.value("hour", 8), c.value("minute", 0), c.value("sec", 0));
			t->totalgt = c.value("totalgt", 0.0);
			t->totalgd = c.value("totalgd", (long long unsigned int)0);
		}
		if (j.contains("events") && j["events"].is_string())
			Events::LoadJson(j["events"].get<std::string>());
		else
			Events::ResetSchedule();
		Wind::LoadJson(j);   // "wind" block, or windless defaults when absent
	}
	// Old atoms get a full teardown (Component::Destroy + delete): a leaked component keeps
	// module-owned resources (e.g. std::functions whose code lives in a module DLL) alive past
	// the DLL. The lock keeps the fixed thread off a hierarchy that is being torn down.
	boost::recursive_mutex::scoped_lock fixedGuard(gameLock);
	if (iPhysics* p = GetService<iPhysics>()) p->reset();
	if (iAudio* au = GetService<iAudio>()) au->reset();   // silence game voices
	AppInstance::GetSingleton()->selectedInHieararchy = nullptr;   // would dangle otherwise
	std::function<void(Atom*)> hardDestroy = [&](Atom* a)
	{
		if (!a) return;
		for (Atom* ch : a->children) hardDestroy(ch);
		a->children.clear();
		for (Component* c : a->components) if (c) { c->Destroy(); delete c; }
		a->components.clear();
		delete a;
	};
	// Persistent root atoms survive a switch during play, but never in editor edit mode (world
	// B's file must not absorb world A's atoms) and never on a savegame load (the snapshot
	// already holds them). Carried physics handles are stale after the reset above and must be
	// zeroed so the fixed-step driver re-creates them.
	AppInstance* app = AppInstance::GetSingleton();
	const bool persistOk = !suppressPersistOnce && (!app->isEditor() || app->playState != 0);
	suppressPersistOnce = false;
	int carried = 0;
	std::function<void(Atom*)> healPhysics = [&](Atom* a)
	{
		if (!a) return;
		if (Collider* col = a->GetComponent<Collider>()) col->bodyId = 0;
		if (CharacterController* cc = a->GetComponent<CharacterController>()) cc->charId = 0;
		for (Atom* ch : a->children) healPhysics(ch);
	};
	for (auto it = hierarchy->begin(); it != hierarchy->end(); )   // keep editor camera + persistent
	{
		if ((*it)->GetName() == "Editor Camera") { ++it; continue; }
		if (persistOk && (*it)->persistent) { healPhysics(*it); ++carried; ++it; continue; }
		Atom* a = *it; it = hierarchy->erase(it); hardDestroy(a);
	}
	if (carried > 0)
		std::cout << "[World]\t\t\t" << carried << " persistent atom(s) carried across the world switch" << std::endl;
	destroyQueue.clear();   // its entries reference the just-freed atoms
}

Atom* World::AddAtomFromJson(const json& atomJ)
{
	boost::recursive_mutex::scoped_lock fixedGuard(gameLock);   // atoms may pop in over frames
	Atom* a = LoadAtom(atomJ);
	if (a) Add(a);
	return a;
}

void World::FinalizeIncrementalLoad()
{
	boost::recursive_mutex::scoped_lock fixedGuard(gameLock);
	std::set<unsigned long> seen;
	FixDuplicateIds(*hierarchy, seen);
	Reflect_ResolveAtomRefs();           // ids -> live atoms, now that everything exists
}

void World::Clear()
{
	boost::recursive_mutex::scoped_lock fixedGuard(gameLock);   // don't tear down under the fixed thread
	if (iPhysics* p = GetService<iPhysics>()) p->reset();   // atoms drop without Destroy: wipe bodies
	if (iAudio* au = GetService<iAudio>()) au->reset();     // and their voices
	for (auto it = hierarchy->begin(); it != hierarchy->end(); )   // keep editor camera
	{
		if ((*it)->GetName() == "Editor Camera") ++it;
		else it = hierarchy->erase(it);
	}
}

static bool IsDescendantOf(Atom* node, Atom* maybeAncestor)
{
	for (Atom* p = node ? node->parent : nullptr; p; p = p->parent)
		if (p == maybeAncestor) return true;
	return false;
}

void World::Reparent(Atom* a, Atom* newParent)
{
	if (!a || a == newParent) return;
	if (newParent && (newParent == a || IsDescendantOf(newParent, a))) return;   // would create a cycle
	if (a->parent) a->parent->children.remove(a);
	else           hierarchy->remove(a);
	if (newParent) { newParent->children.push_back(a); a->parent = newParent; }
	else           { hierarchy->push_back(a);          a->parent = nullptr;   }
}

void World::ReparentBefore(Atom* a, Atom* sibling)
{
	if (!a || !sibling || a == sibling) return;
	Atom* newParent = sibling->parent;
	if (newParent && (newParent == a || IsDescendantOf(newParent, a))) return;   // cycle
	if (a->parent) a->parent->children.remove(a);
	else           hierarchy->remove(a);
	bc::list<Atom*>& lst = newParent ? newParent->children : *hierarchy;
	auto it = std::find(lst.begin(), lst.end(), sibling);
	lst.insert(it, a);
	a->parent = newParent;
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