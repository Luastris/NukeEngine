// Header-only boost.chrono, BEFORE any include that pulls boost headers (the lib flavor
// double-defines steady_clock::now inside the engine DLL - same trap as Clock.cpp).
#define BOOST_CHRONO_HEADER_ONLY
#include <boost/chrono.hpp>
#include "API/Model/World.h"
#include "render/irender.h"
#include "API/Model/Camera.h"
#include "API/Model/MeshRenderer.h"
#include "API/Model/Material.h"   // material instance (mr->mat) save/load
#include "API/Model/Light.h"      // scene lights -> iRender::setLights (PBR)
#include "API/Model/Environment.h"// world sky/ambient -> iRender::setSky
#include "API/Model/PostProcess.h"// per-camera post-process -> iRender::setPostChain
#include "API/Model/Shader.h"     // post-effect shader props (pack PostParams per stage)
#include "API/Model/ReflectionProbe.h" // scene-captured reflection cubemap
#include "API/Model/Time.h"       // animated-texture (GIF) frame advance + fixed-step accumulator
#include "API/Model/Collider.h"   // physics: shape components -> iPhysics bodies
#include "API/Model/Jobs.h"       // PumpMain each game-thread frame (2.4)
#include "API/Model/Rigidbody.h"  // physics: dynamic/kinematic body settings
#include "API/Model/DebugDraw.h"   // editor gizmos (selection wire shapes)
#include "interface/Services.h"   // GetService<iPhysics>()
#include "service/iPhysics.h"     // the physics service seam (fixed-step driver below)
#include <cmath>
#include <map>
#include <set>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>  // glm::rotation (sun look-rotation for time-of-day)
#include "API/Model/Mesh.h"
#include "API/Model/Texture.h"
#include "API/Model/resdb.h"
#include "API/Model/UnknownComponent.h"
#include "API/Model/Prefab.h"
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
	for (Atom* go : GetHierarchy())
		if (Atom* r = FindById(go, id)) return r;
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

void World::LockGame()   { gameLock.lock(); }
void World::UnlockGame() { gameLock.unlock(); }

void World::Update()
{
	Jobs::PumpMain();   // deliver background-job results on the game thread (2.4)
	static int slowLog = 0;
	auto t0 = boost::chrono::steady_clock::now();
	boost::recursive_mutex::scoped_lock lock(gameLock);
	{
		double ms = boost::chrono::duration_cast<boost::chrono::duration<double, boost::milli>>(
			boost::chrono::steady_clock::now() - t0).count();
		if (ms > 50.0 && slowLog < 20)
			{ std::cout << "[World]			Update lock wait: " << ms << " ms" << std::endl; ++slowLog; }
	}
	for (Atom* go : *hierarchy)
	{
		go->Update();
	}
}

// --- fixed-step physics driver (service/iPhysics.h; no-op without a provider) ---------

// Lazily create the physics body for an atom's Collider, and drive KINEMATIC bodies from
// the Transform (gameplay moves them; the simulation follows and pushes dynamics away).
// Fills `bodyMap` (bodyId -> Collider) for contact-event dispatch after the step.
static void SyncBodies(bc::list<Atom*>& gos, iPhysics* p, std::map<uint64_t, Collider*>& bodyMap, float dt)
{
	for (Atom* go : gos)
	{
		if (!go) continue;
		if (Collider* col = go->GetComponent<Collider>())
		{
			Rigidbody* rb = go->GetComponent<Rigidbody>();
			Transform& t = go->GetTransform();

			// Motion type changed live (Rigidbody added/removed, Kinematic toggled in the
			// inspector mid-play)? Recreate — e.g. MoveKinematic on a stale STATIC body is
			// silently ignored by the backend ("kinematic doesn't move at all").
			const int desiredMotion = rb ? (rb->isKinematic ? 2 : 1) : 0;
			if (col->bodyId && col->bodyMotion != desiredMotion)
			{
				p->destroyBody(col->bodyId);
				col->bodyId = 0;
				col->hasSync = false;
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
					MeshRenderer* mr = go->GetComponent<MeshRenderer>();
					Mesh* mesh = mr ? mr->mesh : nullptr;
					if (!mesh || !mesh->vertexArray || mesh->numVerts < 3)
					{
						std::cout << "[World]\t\t\tmesh collider on '" << go->name
						          << "' has no sibling MeshRenderer mesh - skipped" << std::endl;
						continue;
					}
					scaledVerts.resize((size_t)mesh->numVerts * 3);
					for (int i = 0; i < mesh->numVerts; ++i)
					{
						scaledVerts[(size_t)i * 3 + 0] = mesh->vertexArray[i * 3 + 0] * (float)scl.x;
						scaledVerts[(size_t)i * 3 + 1] = mesh->vertexArray[i * 3 + 1] * (float)scl.y;
						scaledVerts[(size_t)i * 3 + 2] = mesh->vertexArray[i * 3 + 2] * (float)scl.z;
					}
					d.meshVerts     = scaledVerts.data();
					d.meshVertCount = mesh->numVerts;
				}
				col->bodyId = p->createBody(d);
				col->bodyMotion = desiredMotion;
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
					// Kinematic tracking servo. Scripts write the transform at RENDER cadence,
					// the simulation consumes it at the FIXED cadence, and the two BEAT:
					// per-step deltas jitter between zero and twice the true speed. Riders are
					// punted by velocity JUMPS (acceleration), so the body is driven with a
					// velocity that is smooth BY CONSTRUCTION: the exponentially-smoothed
					// write velocity (feedforward) plus a soft proportional pull towards the
					// newest write (kills lag and drift). A script that moves the platform in
					// fixedUpdate makes this exact tracking; update-rate scripts get a
					// millimetre-level ripple instead of kicks.
					const double dx = pos.x - col->lastSyncPos.x, dy = pos.y - col->lastSyncPos.y, dz = pos.z - col->lastSyncPos.z;
					const double qd = fabs(Quaternion::Dot(rot, col->lastSyncRot));
					const bool moved = !col->hasSync || (dx * dx + dy * dy + dz * dz) > 1e-10 || qd < 1.0 - 1e-7;
					if (moved && col->quietSteps > 30)
					{
						// Parked (>0.5 s idle) and then moved: that's a TELEPORT, not a glide -
						// snap, don't crawl there at (jump / idle-time) speed.
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
							// Fresh write: instantaneous velocity = the delta since the LAST
							// write over the time that actually elapsed. Adopt instantly out of
							// rest, then smooth (the beat spikes single deltas to ~2x).
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
							// Short gaps ARE the beat - keep the feedforward alive. A long gap
							// means the writes stopped: bleed the velocity off so the body
							// settles instead of sailing past the parked transform.
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
							// v = feedforward + Kp * error, under an absolute ceiling so a huge
							// error (missed teleport, spawn) closes fast but never violently.
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

							// Compose next step's pose from the velocity command and let the
							// backend derive the (smooth) velocities riders are carried with.
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
					// Static/dynamic: push only EXTERNAL transform changes (script, moving
					// parent, editor teleport). The driver's own dynamic write-backs refresh
					// lastSync in PullDynamicPoses, so they don't re-trigger here.
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
		if (!go->children.empty())
			SyncBodies(go->children, p, bodyMap, dt);
	}
}

// Route drained contact transitions to BOTH atoms' components: trigger pairs (either
// collider is a trigger) -> OnTriggerEnter/Exit, the rest -> OnCollisionEnter/Exit.
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
	for (Atom* go : gos)
	{
		if (!go) continue;
		Collider* col = go->GetComponent<Collider>();
		Rigidbody* rb = go->GetComponent<Rigidbody>();
		if (col && rb && !rb->isKinematic && col->bodyId)
		{
			float fp[3], fq[4];
			if (p->getBodyPose(col->bodyId, fp, fq))
			{
				Transform& t = go->GetTransform();
				t.SetGlobal(Vector3(fp[0], fp[1], fp[2]),
				            Quaternion(fq[0], fq[1], fq[2], fq[3]),
				            t.globalScale());
				// This write is OURS - refresh the cache so SyncBodies doesn't see it as an
				// external move next step (which would teleport-fight the simulation).
				// CRITICAL: cache what the transform will actually RETURN, not the raw values
				// written - the write goes through the transform's internal representation
				// (quat->euler->quat), which shifts a tumbling body's rotation by a couple of
				// degrees. Caching the raw quat made every rotating dynamic body register as
				// "externally rotated" and get pose-snapped EVERY step, fighting the solver
				// (boxes bounced off platforms as if elastic).
				col->lastSyncPos = t.globalPosition();
				col->lastSyncRot = t.globalRotation();
				col->hasSync = true;
			}
		}
		if (!go->children.empty())
			PullDynamicPoses(go->children, p);
	}
}

void World::FixedUpdate()
{
	// ONE fixed step. The CADENCE lives in AppInstance::FixedThread (fixed frequency,
	// frame-independent) — this function must not know about frames or accumulate time.
	// Locking: world phases hold the game lock; the Jolt solve itself runs OUTSIDE it,
	// so the simulation overlaps the frame (Update/render) instead of serializing with it.
	const float dt = settings.fixedDt > 0.0001f ? settings.fixedDt : 1.0f / 60.0f;
	iPhysics* p = GetService<iPhysics>();
	std::map<uint64_t, Collider*> bodyMap;   // this step's live bodies (contact dispatch)

	if (p)
	{
		boost::recursive_mutex::scoped_lock lock(gameLock);
		p->init();   // idempotent
		p->setGravity(settings.gravity);
		SyncBodies(*hierarchy, p, bodyMap, dt);
	}

	if (p)
		p->step(dt);   // UNLOCKED: Jolt solves on its workers while the frame proceeds

	{
		boost::recursive_mutex::scoped_lock lock(gameLock);
		if (p)
		{
			PullDynamicPoses(*hierarchy, p);
			DispatchContacts(p, bodyMap);    // OnCollision/OnTrigger hooks — DIRECT, incl. scripts
		}
		for (Atom* go : *hierarchy)          // the EXISTING fixed chain: Atom -> components
			go->FixedUpdate();               // (scripts' fixedUpdate runs HERE, under the lock)
	}
}

// --- render pass (one render per camera) ---

static void CollectCameras(bc::list<Atom*>& gos, std::vector<Camera*>& out)
{
	for (auto go : gos)
	{
		if (auto* c = go->GetComponent<Camera>())
			if (c->enabled) out.push_back(c);   // a disabled camera renders nothing
		if (go->children.size() > 0)
			CollectCameras(go->children, out);
	}
}

static void CollectLights(bc::list<Atom*>& gos, std::vector<Light*>& out)
{
	for (auto go : gos)
	{
		if (auto* l = go->GetComponent<Light>())
			if (l->enabled) out.push_back(l);
		if (go->children.size() > 0)
			CollectLights(go->children, out);
	}
}

// Post-process components (one may sit beside each Camera on the same atom — matched by shared transform).
static void CollectPostProcess(bc::list<Atom*>& gos, std::vector<PostProcess*>& out)
{
	for (auto go : gos)
	{
		if (auto* p = go->GetComponent<PostProcess>())
			if (p->enabled) out.push_back(p);
		if (go->children.size() > 0)
			CollectPostProcess(go->children, out);
	}
}

static ReflectionProbe* FindReflectionProbe(bc::list<Atom*>& gos)
{
	for (auto go : gos)
	{
		if (auto* p = go->GetComponent<ReflectionProbe>()) if (p->enabled) return p;
		if (go->children.size() > 0) if (ReflectionProbe* p = FindReflectionProbe(go->children)) return p;
	}
	return nullptr;
}

static Environment* FindEnvironment(bc::list<Atom*>& gos)
{
	for (auto go : gos)
	{
		if (auto* e = go->GetComponent<Environment>()) if (e->enabled) return e;
		if (go->children.size() > 0) if (Environment* e = FindEnvironment(go->children)) return e;
	}
	return nullptr;
}

// One queued draw (gathered before drawing so transparent objects can be sorted back-to-front).
struct DrawItem { Mesh* mesh; Material* mat; float pos[3], quat[4], scale[3]; Vector3 wpos; int blend; bool inReflections;
                  float prevPos[3], prevQuat[4], prevScale[3]; bool hasPrev; };   // prev transform for TAA velocity

static void CollectMeshes(bc::list<Atom*>& gos, std::vector<DrawItem>& out)
{
	for (auto go : gos)
	{
		if (auto* mr = go->GetComponent<MeshRenderer>())
			if (mr->enabled && mr->mesh)
			{
				Transform& t = go->GetTransform();
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
				it.inReflections = mr->inReflections;
				it.hasPrev = mr->hasPrev;   // previous global transform (TAA velocity); false the first frame -> zero motion
				for (int k = 0; k < 3; ++k) it.prevPos[k] = mr->prevPos[k];
				for (int k = 0; k < 4; ++k) it.prevQuat[k] = mr->prevQuat[k];
				for (int k = 0; k < 3; ++k) it.prevScale[k] = mr->prevScale[k];
				out.push_back(it);
			}
		if (go->children.size() > 0)
			CollectMeshes(go->children, out);
	}
}

// Frustum cull: transform the mesh's 8 local-AABB corners to clip space (via the renderer's view*proj) and
// drop the object when all 8 are outside the same frustum plane (D3D clip: x/y in [-w,w], z in [0,w]).
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

// Combined view*proj for the current camera (renderer's row-major matrices), for frustum culling.
static void CameraVP(iRender* r, float vp[16])
{
	float view[16], proj[16];
	r->getViewProj(view, proj);
	for (int i = 0; i < 4; ++i)
		for (int j = 0; j < 4; ++j)
		{ float s = 0; for (int k = 0; k < 4; ++k) s += view[i*4+k] * proj[k*4+j]; vp[i*4+j] = s; }
}

// End of frame: snapshot each MeshRenderer's current global transform as its "previous" for next frame's TAA
// motion vectors. Runs once per frame (transforms are camera-independent), after all cameras have rendered.
static void UpdatePrevTransforms(bc::list<Atom*>& gos)
{
	for (auto go : gos)
	{
		if (auto* mr = go->GetComponent<MeshRenderer>())
		{
			Transform& t = go->GetTransform();
			Vector3 p = t.globalPosition(); Quaternion q = t.globalRotation(); Vector3 s = t.globalScale();
			mr->prevPos[0] = (float)p.x; mr->prevPos[1] = (float)p.y; mr->prevPos[2] = (float)p.z;
			mr->prevQuat[0] = (float)q.x; mr->prevQuat[1] = (float)q.y; mr->prevQuat[2] = (float)q.z; mr->prevQuat[3] = (float)q.w;
			mr->prevScale[0] = (float)s.x; mr->prevScale[1] = (float)s.y; mr->prevScale[2] = (float)s.z;
			mr->hasPrev = true;
		}
		UpdatePrevTransforms(go->children);
	}
}

// SSR G-buffer prepass: opaque geometry only (transparent doesn't occlude / write the colour-pass depth).
// Same cull setting as the colour pass so depths line up.
static void DrawGBuffer(std::vector<DrawItem>& items, iRender* r, bool cull)
{
	float vp[16]; if (cull) CameraVP(r, vp);
	for (auto& it : items)
		if (it.blend == 0 && !(cull && FrustumCull(it, vp)))
			r->renderGBufferObject(it.mesh, it.mat, it.pos, it.quat, it.scale,
			                       it.hasPrev ? it.prevPos : nullptr, it.hasPrev ? it.prevQuat : nullptr, it.hasPrev ? it.prevScale : nullptr);
}

// Draw a gathered scene for one camera: opaque first (depth write on), then transparent/additive sorted
// back-to-front by distance from the camera (the renderer disables depth write for those). Frustum-culled.
static void DrawCollected(std::vector<DrawItem>& items, const Vector3& camPos, iRender* r, bool cull)
{
	float vp[16]; if (cull) CameraVP(r, vp);
	auto culled = [&](const DrawItem& it) { return cull && FrustumCull(it, vp); };

	for (auto& it : items)
		if (it.blend == 0 && !culled(it))
			r->renderObject(it.mesh, it.mat, it.pos, it.quat, it.scale);

	std::vector<DrawItem*> tr;
	for (auto& it : items) if (it.blend != 0 && !culled(it)) tr.push_back(&it);
	if (!tr.empty())
	{
		auto dist2 = [&](const DrawItem* it) {
			double dx = it->wpos.x - camPos.x, dy = it->wpos.y - camPos.y, dz = it->wpos.z - camPos.z;
			return dx*dx + dy*dy + dz*dz;
		};
		std::sort(tr.begin(), tr.end(), [&](const DrawItem* a, const DrawItem* b) { return dist2(a) > dist2(b); });
		for (auto* it : tr) r->renderObject(it->mesh, it->mat, it->pos, it->quat, it->scale);
	}
}

// Depth-only traversal for the shadow pass: every enabled mesh whose material casts shadows
// (null material = casts by default). Transparency is handled in the renderer (alpha-dither).
static void RenderShadowMeshes(bc::list<Atom*>& gos, iRender* r)
{
	for (auto go : gos)
	{
		if (auto* mr = go->GetComponent<MeshRenderer>())
		{
			if (mr->enabled && mr->mesh && (!mr->mat || mr->mat->castShadows))
			{
				Transform& t = go->GetTransform();
				Vector3    p = t.globalPosition();
				Quaternion q = t.globalRotation();
				Vector3    s = t.globalScale();
				float pos[3]   = { (float)p.x, (float)p.y, (float)p.z };
				float quat[4]  = { (float)q.x, (float)q.y, (float)q.z, (float)q.w };
				float scale[3] = { (float)s.x, (float)s.y, (float)s.z };
				r->renderShadowObject(mr->mesh, pos, quat, scale, mr->mat);
			}
		}
		if (go->children.size() > 0)
			RenderShadowMeshes(go->children, r);
	}
}

// --- editor gizmos (debug-draw, roadmap 2.1): wire shapes for the SELECTED atom --------
static void EmitSelectionGizmos(Atom* a)
{
	Transform& t = a->GetTransform();
	Vector3 pos = t.globalPosition();
	Quaternion rot = t.globalRotation();
	Vector3 scl = t.globalScale();

	if (Light* l = a->GetComponent<Light>())
	{
		const Color c(1.0, 0.9, 0.2, 1.0);   // lights: yellow
		Vector3 fwd = t.direction();
		if (l->type == 0)        // directional: an arrow along the travel direction
			DebugDraw::Arrow(pos, Vector3(pos.x + fwd.x * 3, pos.y + fwd.y * 3, pos.z + fwd.z * 3), c);
		else if (l->type == 1)   // point: range sphere
			DebugDraw::WireSphere(pos, l->range, c);
		else                     // spot: cone. world.ps lights the cone around -dir
		                         // (cd = dot(-dir, -L)), so the gizmo opens along -forward.
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
			// far plane clamped for readability (a 10km frustum is just noise in the viewport)
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
			DebugDraw::WireBox(pos, Vector3(rp->boxSize.x * 0.5, rp->boxSize.y * 0.5, rp->boxSize.z * 0.5), rot, c);
		else
			DebugDraw::WireSphere(pos, 1.0, c);   // infinite probe: a small marker sphere
	}
}

void World::Render(iRender* r)
{
	if (!r) return;

	// Editor gizmos for the selection (edit mode only; lines live for one frame).
	{
		AppInstance* app = AppInstance::GetSingleton();
		if (app->isEditor() && app->playState == 0 && app->selectedInHieararchy)
			EmitSelectionGizmos(app->selectedInHieararchy);
	}

	// Advance animated textures (GIF) by real frame time — the renderer samples Texture::curFrame.
	// Only the CURRENT scene advances them: auxiliary worlds rendered in the same frame (the
	// editor's asset preview) must not double-step the global animation clock.
	if (this == AppInstance::GetSingleton()->currentScene)
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

	// Gather scene lights once for this frame (the renderer keeps them for every camera pass).
	std::vector<Light*> lights;
	CollectLights(*hierarchy, lights);

	// Post-process components (each applies only to its own camera = the Camera on the same atom).
	std::vector<PostProcess*> pps;
	CollectPostProcess(*hierarchy, pps);

	// Time of day: drive the FIRST directional light (rotation/color/intensity) from the Environment's hour.
	// The sky colours follow below. todElev: sun elevation (-1..1), used by the sky build.
	float todElev = 0.0f;
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
		// Daylight ramps up quickly once the sun clears the horizon (so the sky isn't black at dawn/dusk
		// while the sun is still visible). Full day a little above the horizon; night only when well below.
		float day = (todElev + 0.1f) / 0.3f; day = day < 0 ? 0.0f : (day > 1 ? 1.0f : day);
		for (Light* L : lights)
			if (L->type == 0 && L->transform)   // the first directional light = the sun
			{
				glm::quat q = glm::rotation(glm::vec3(0, 0, 1), glm::vec3(-sx, -sy, -sz));   // forward = travel dir
				L->transform->rotation = Quaternion(q.x, q.y, q.z, q.w);
				L->color = Color(1.0, 0.55 + 0.45 * day, 0.25 + 0.75 * day, 1.0);            // warm -> white
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
	// Global shadow settings BEFORE setLights (the directional ortho extent uses shadowDistance).
	r->setShadowSettings(settings.shadowRes, settings.shadowDistance, settings.shadowDepthBias,
	                     settings.shadowNormalBias, settings.shadowSoftness);
	r->setLights(gpuLights.empty() ? nullptr : gpuLights.data(), (int)gpuLights.size());

	// Environment (sky + ambient) — first Environment component; default sky if none (keeps old ambient).
	{
		NukeSky sky;   // defaults match the renderer's old hardcoded ambient
		if (Environment* env = FindEnvironment(*hierarchy))
		{
			sky.mode = env->mode;
			sky.top[0]=(float)env->skyTop.r; sky.top[1]=(float)env->skyTop.g; sky.top[2]=(float)env->skyTop.b;
			sky.horizon[0]=(float)env->skyHorizon.r; sky.horizon[1]=(float)env->skyHorizon.g; sky.horizon[2]=(float)env->skyHorizon.b;
			sky.ground[0]=(float)env->skyGround.r; sky.ground[1]=(float)env->skyGround.g; sky.ground[2]=(float)env->skyGround.b;
			sky.skyIntensity = env->skyIntensity;
			sky.ambient[0]=(float)env->ambient.r; sky.ambient[1]=(float)env->ambient.g; sky.ambient[2]=(float)env->ambient.b;
			sky.ambientIntensity = env->ambientIntensity;
			sky.exposure = env->exposure; sky.whitePoint = env->whitePoint;   // SDR tonemap (post.ps + world.ps)
			if (env->useTimeOfDay)   // override sky colours from the time of day (day=blue, dusk=orange, night=dark)
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
			if (env->stars)   // fade stars in as the sky darkens (ToD), or always-on when ToD is off
			{
				float night = 0.4f;
				if (env->useTimeOfDay) { float d = (todElev + 0.1f) / 0.3f; d = d < 0 ? 0 : (d > 1 ? 1 : d); night = 1.0f - d; }
				sky.stars = night < 0 ? 0 : night;
				if (!env->starsTexGuid.empty()) sky.starsTex = ResDB::getSingleton()->GetTexture(env->starsTexGuid);
			}
			if (env->moon && !env->moonTexGuid.empty())   // textured moon opposite the sun, visible at night
			{
				sky.moonTex  = ResDB::getSingleton()->GetTexture(env->moonTexGuid);
				sky.moonSize = env->moonSize * 0.01745329252f;   // deg -> radians
				float mx = 0, my = 1, mz = 0;
				for (Light* L : lights)
					if (L->type == 0 && L->transform) { Vector3 d = L->transform->direction(); mx=(float)d.x; my=(float)d.y; mz=(float)d.z; break; }
				float ml = std::sqrt(mx*mx + my*my + mz*mz); if (ml > 1e-6f) { mx/=ml; my/=ml; mz/=ml; }
				sky.moonDir[0] = mx; sky.moonDir[1] = my; sky.moonDir[2] = mz;   // sun travel dir = opposite the sun
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

	// Ray tracing (D3D12): build the scene TLAS from opaque meshes FIRST — before the probe capture and camera
	// passes, both of which draw with the world PSO and ray-query g_TLAS (RT shadows). Must exist before any draw.
	// Auxiliary worlds (asset previews) skip it: the TLAS is GLOBAL and the live scene must stay its last
	// writer (and a preview doesn't need RT reflections).
	if (r->rtAvailable() && !auxiliary)
	{
		r->beginRTScene();
		std::vector<DrawItem> rtItems; CollectMeshes(*hierarchy, rtItems);
		// Dynamic meshes (skinned instances) provide a static stand-in for BLAS/TLAS —
		// per-frame BLAS rebuilds are a non-goal (rtProxy = the bind-pose source).
		for (auto& it : rtItems) if (it.blend == 0)
			r->addRTInstance(it.mesh->rtProxy ? it.mesh->rtProxy : it.mesh,
			                 it.mat, it.pos, it.quat, it.scale, it.inReflections);
		r->buildRTScene();
	}

	// Shadow depth passes (one per shadow-casting dir/spot light) before any camera pass — the renderer
	// samples them during the world pass. Only shadow-casting surfaces (Material::castShadows) contribute.
	for (int sp = 0, spc = r->shadowPassCount(); sp < spc; ++sp)
	{
		r->beginShadowPass(sp);
		RenderShadowMeshes(*hierarchy, r);
		r->endShadowPass();
	}

	// Reflection probe: capture the scene into its cubemap (after shadows so reflections are lit/shadowed),
	// then bind it for the camera passes. Static probes capture once; realtime / Bake re-capture.
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
			std::vector<DrawItem> items; CollectMeshes(*hierarchy, items);   // no cull for capture
			for (int f = 0; f < 6; ++f)
			{
				r->beginCubeFace(probe->cubeId, f, pos, probe->nearZ, probe->farZ);
				for (auto& it : items) if (it.blend == 0) r->renderObject(it.mesh, it.mat, it.pos, it.quat, it.scale);
				r->endCubeFace(probe->cubeId, f);
			}
			probe->captured = true; probe->bake = false;
		}
		float boxHalf[3] = { 0, 0, 0 };
		if (probe->boxProjection)
		{ boxHalf[0] = (float)probe->boxSize.x * 0.5f; boxHalf[1] = (float)probe->boxSize.y * 0.5f; boxHalf[2] = (float)probe->boxSize.z * 0.5f; }
		r->setReflectionProbe(probe->cubeId, pos, probe->intensity, probe->farZ, boxHalf);
	}
	else { float z[3] = { 0, 0, 0 }; r->setReflectionProbe(0, z, 0.0f, 0.0f, z); }

	const bool editor = AppInstance::GetSingleton()->isEditor();
	for (Camera* cam : cams)
	{
		if (!cam->transform) continue;
		// A camera with a RenderTexture target renders into that texture's RT (resolved live).
		if (!cam->targetTexGuid.empty())
		{
			Texture* rt = ResDB::getSingleton()->GetTexture(cam->targetTexGuid);
			if (rt && rt->renderTexture && rt->rtId) cam->renderTarget = rt->rtId;
		}
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

		// This camera's post-process chain: the effects on the PostProcess component sitting on the same atom
		// (shared transform). Each effect = a custom post-shader pipeline + its packed PostParams bytes.
		std::vector<std::vector<float>> ppBlobs; std::vector<uint64_t> ppHandles;
		bool hasSSR = false;   // an SSR or RT-reflection effect in this camera's chain -> run the G-buffer prepass first
		bool hasTAA = false;   // temporal AA -> also needs the depth prepass + camera jitter
		for (PostProcess* pp : pps)
			if (pp->transform == cam->transform)
			{
				pp->EnsureParsed();
				for (PostEffect& e : pp->effects)
				{
					if (!e.enabled) continue;
					Shader* sh = ResDB::getSingleton()->GetShader(e.shaderGuid);
					if (!sh || !sh->isPost || sh->rendererHandle == 0) continue;
					if (sh->name == "ssr" || sh->name == "rtreflect") hasSSR = true;   // both need the G-buffer prepass
					if (sh->name == "taa") hasTAA = true;                              // TAA needs the depth prepass + jitter
					std::vector<float> blob(64, 0.0f);   // 256-byte PostParams
					for (const ShaderProp& sp : sh->props)
					{
						const float* v = sp.def;
						auto pit = e.props.find(sp.name);
						if (pit != e.props.end()) v = pit->second.data();
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

		r->setCameraTAA(hasTAA);   // enable jitter + history for this camera (advances the jitter when on)

		// SSR / RT reflections / TAA need scene depth (+ normals for SSR) — a single-sample prepass before colour.
		if (hasSSR || hasTAA)
		{
			r->beginGBufferPass(d);
			std::vector<DrawItem> gitems; CollectMeshes(*hierarchy, gitems);
			DrawGBuffer(gitems, r, settings.frustumCull);
			r->endGBufferPass();
		}

		r->beginCamera(d);
		{
			std::vector<DrawItem> items;
			CollectMeshes(*hierarchy, items);
			DrawCollected(items, cp, r, settings.frustumCull);
		}
		// Selection highlight (editor only): outline the selected object after the scene.
		if (editor)
			if (Atom* sel = AppInstance::GetSingleton()->selectedInHieararchy)
				if (auto* mr = sel->GetComponent<MeshRenderer>())
					if (mr->mesh)
					{
						Transform& t = sel->GetTransform();
						Vector3    p = t.globalPosition();
						Quaternion q = t.globalRotation();
						Vector3    s = t.globalScale();
						float pos[3]   = { (float)p.x, (float)p.y, (float)p.z };
						float quat[4]  = { (float)q.x, (float)q.y, (float)q.z, (float)q.w };
						float scale[3] = { (float)s.x, (float)s.y, (float)s.z };
						r->renderSelectionOutline(mr->mesh, pos, quat, scale);
					}
		r->endCamera();
	}
	UpdatePrevTransforms(*hierarchy);   // snapshot transforms for next frame's TAA motion vectors
}

// --- scene serialization (.nuworld JSON via reflection) ---

static void SaveAtom(Atom* go, json& j)
{
	j["name"] = go->GetName();
	j["id"]   = go->id.id;   // stable identity (survives the scene rebuild on PIE stop / reload)
	if (!go->prefabGuid.empty()) j["prefab"] = go->prefabGuid;   // instance link to a .nuprefab
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
			cj["enabled"] = uc->enabled;
			cj["cid"]     = uc->id.id;
			j["components"].push_back(cj);
			continue;
		}
		TypeInfo* ti = c->GetType();
		if (!ti) continue;                       // unreflected component — skip
		json cj;
		cj["type"] = ti->name;
		SaveObject(*ti, c, cj["props"]);
		// Material INSTANCE overrides (color/shader/props) — these live on the per-object instance and
		// save with the world; the referenced .numat asset (matGuid) is never modified by scene edits.
		if (MeshRenderer* mr = dynamic_cast<MeshRenderer*>(c))
			if (mr->mat)
			{
				json jm;
				jm["color"]  = { mr->mat->color.r, mr->mat->color.g, mr->mat->color.b, mr->mat->color.a };
				jm["shader"] = mr->mat->shaderGuid;
				// PBR instance overrides (maps + scalar params).
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
		cj["cid"]     = c->id.id;
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
	if (j.contains("id")) { go->id.id = j["id"].get<long>(); ID::observe(go->id.id); }   // keep the saved identity
	go->prefabGuid = j.value("prefab", std::string());       // instance link (if any)
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
				c->enabled = cj.value("enabled", true);
				c->id.id   = cj.value("cid", c->id.id); ID::observe(c->id.id);
				if (cj.contains("props")) LoadObject(*ti, c, cj["props"]);
				go->AddComponent(c);             // Init() wires transform/owner + clones the material instance
				// Apply saved material-instance overrides onto the cloned instance (after Init).
				if (MeshRenderer* mr = dynamic_cast<MeshRenderer*>(c))
					if (cj.contains("material") && cj["material"].is_object())
					{
						const json& jm = cj["material"];
						if (!mr->mat) mr->mat = new Material();   // matGuid empty / asset missing -> bare instance
						if (jm.contains("color") && jm["color"].is_array() && jm["color"].size() == 4)
						{
							mr->mat->color.r = jm["color"][0]; mr->mat->color.g = jm["color"][1];
							mr->mat->color.b = jm["color"][2]; mr->mat->color.a = jm["color"][3];
						}
						if (jm.contains("shader")) mr->mat->shaderGuid = jm.value("shader", std::string("world"));
						// PBR instance overrides (only when present, so older worlds keep the cloned asset maps).
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
						mr->mat->Resolve();   // re-bind shader/textures (shaderGuid may differ from the asset)
					}
			}
			else
			{
				// Type inactive (its plugin isn't loaded) — keep it inert + preserve its data,
				// and remember which plugin it needs so the editor can show that.
				UnknownComponent* uc = new UnknownComponent();
				uc->typeName = type;
				uc->enabled = cj.value("enabled", true);
				uc->id.id   = cj.value("cid", uc->id.id); ID::observe(uc->id.id);
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

static void RegenIds(Atom* a)
{
	if (!a) return;
	a->id.generate();                    // each instantiated atom gets a fresh unique id
	for (Component* c : a->components) c->id.generate();   // and its components
	for (Atom* c : a->children) RegenIds(c);
}

Atom* LoadPrefab(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream f(p);
	if (!f) return nullptr;
	json j = json::parse(f, nullptr, false);
	if (j.is_discarded()) return nullptr;
	Atom* a = LoadAtom(j);
	RegenIds(a);                         // instances are unique — don't share the prefab's saved ids
	return a;
}

// The prefab's own GUID (the "prefab" field of its root), or "" if it predates prefab linking.
std::string PrefabGuid(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream f(p);
	if (!f) return std::string();
	json j = json::parse(f, nullptr, false);
	if (j.is_discarded()) return std::string();
	return j.value("prefab", std::string());
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
	return LoadAtom(j);
}

// --- undo support: remove an atom subtree by id; insert one at a placement (parentId 0 = root) ---
static void DeleteSubtree(Atom* a)
{
	if (!a) return;
	for (Atom* c : a->children) DeleteSubtree(c);
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
	j["settings"] = {                                     // world-level render settings (shadow globals, ...)
		{"shadowRes", settings.shadowRes}, {"shadowDistance", settings.shadowDistance},
		{"shadowDepthBias", settings.shadowDepthBias}, {"shadowNormalBias", settings.shadowNormalBias},
		{"shadowSoftness", settings.shadowSoftness}, {"frustumCull", settings.frustumCull},
		{"gravity", { settings.gravity[0], settings.gravity[1], settings.gravity[2] }},
		{"fixedDt", settings.fixedDt} };
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

// Repair duplicate ids (e.g. worlds saved by the old address-based ID::generate, or prefab/duplicate
// round-trips before the fix): atom ids must be globally unique; component ids unique within their atom.
static void FixDuplicateIds(bc::list<Atom*>& gos, std::set<unsigned long>& seenAtoms)
{
	for (Atom* go : gos)
	{
		if (go->id.id == 0 || seenAtoms.count(go->id.id)) go->id.generate();
		seenAtoms.insert(go->id.id);
		std::set<unsigned long> seenComps;
		for (Component* c : go->components)
		{
			if (c->id.id == 0 || seenComps.count(c->id.id)) c->id.generate();
			seenComps.insert(c->id.id);
		}
		FixDuplicateIds(go->children, seenAtoms);
	}
}

void World::LoadFromString(const std::string& data)
{
	json j = json::parse(data, nullptr, false);
	if (j.is_discarded()) { std::cout << "[World]\t\t\t" << "LoadFromString: bad JSON" << std::endl; return; }
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
	// The old atoms are dropped WITHOUT component Destroy (fast path), so their physics
	// bodies would leak — wipe the whole simulation; new colliders re-create lazily.
	// gameLock: the fixed thread must not step a hierarchy that is being torn down.
	boost::recursive_mutex::scoped_lock fixedGuard(gameLock);
	if (iPhysics* p = GetService<iPhysics>()) p->reset();
	for (auto it = hierarchy->begin(); it != hierarchy->end(); )   // keep editor camera, drop the rest
	{
		if ((*it)->GetName() == "Editor Camera") ++it;
		else it = hierarchy->erase(it);
	}
	if (j.contains("atoms"))
		for (const json& gj : j["atoms"])
			Add(LoadAtom(gj));
	std::set<unsigned long> seen;
	FixDuplicateIds(*hierarchy, seen);   // heal any colliding ids from old saves / duplicates
}

void World::Clear()
{
	boost::recursive_mutex::scoped_lock fixedGuard(gameLock);   // don't tear down under the fixed thread
	if (iPhysics* p = GetService<iPhysics>()) p->reset();   // atoms drop without Destroy — wipe bodies
	for (auto it = hierarchy->begin(); it != hierarchy->end(); )   // keep editor camera, drop the rest
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
	// detach from current location
	if (a->parent) a->parent->children.remove(a);
	else           hierarchy->remove(a);
	// attach to the new location
	if (newParent) { newParent->children.push_back(a); a->parent = newParent; }
	else           { hierarchy->push_back(a);          a->parent = nullptr;   }
}

void World::ReparentBefore(Atom* a, Atom* sibling)
{
	if (!a || !sibling || a == sibling) return;
	Atom* newParent = sibling->parent;
	if (newParent && (newParent == a || IsDescendantOf(newParent, a))) return;   // cycle
	// detach a from its current location
	if (a->parent) a->parent->children.remove(a);
	else           hierarchy->remove(a);
	// insert before `sibling` in the target list (sibling's parent, or the root list)
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