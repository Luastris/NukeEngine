#include "API/Model/Ragdoll.h"
#include "API/Model/Atom.h"
#include "API/Model/Mesh.h"
#include "API/Model/Skeleton.h"
#include "API/Model/SkinnedMeshRenderer.h"
#include "API/Model/Transform.h"
#include "API/Model/resdb.h"
#include "interface/AppInstance.h"
#include "service/iPhysics.h"
#include "interface/Services.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem/fstream.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <iostream>
#include <sstream>

namespace nuke {

namespace bfs = boost::filesystem;

// ---- asset ----------------------------------------------------------------------------------

std::string RagdollDef::ToString() const
{
	nlohmann::json j;
	j["type"] = "Ragdoll";
	j["guid"] = guid;
	j["name"] = name;
	j["skelGuid"] = skelGuid;
	nlohmann::json bs = nlohmann::json::array();
	for (const Body& b : bodies)
		bs.push_back({ { "bone", b.bone },
		               { "center", { b.center[0], b.center[1], b.center[2] } },
		               { "axis", { b.axis[0], b.axis[1], b.axis[2] } },
		               { "radius", b.radius }, { "halfHeight", b.halfHeight }, { "mass", b.mass } });
	j["bodies"] = bs;
	nlohmann::json js = nlohmann::json::array();
	for (const Joint& jt : joints)
		js.push_back({ { "bone", jt.bone }, { "twistMin", jt.twistMin }, { "twistMax", jt.twistMax },
		               { "swing1", jt.swing1 }, { "swing2", jt.swing2 } });
	j["joints"] = js;
	return j.dump(1);
}

RagdollDef* RagdollDef::FromString(const std::string& json)
{
	try
	{
		nlohmann::json j = nlohmann::json::parse(json);
		RagdollDef* r = new RagdollDef();
		r->guid = j.value("guid", "");
		r->name = j.value("name", "");
		r->skelGuid = j.value("skelGuid", "");
		if (j.contains("bodies"))
			for (const auto& e : j["bodies"])
			{
				Body b;
				b.bone = e.value("bone", "");
				if (e.contains("center")) for (int k = 0; k < 3; ++k) b.center[k] = e["center"][k].get<float>();
				if (e.contains("axis"))   for (int k = 0; k < 3; ++k) b.axis[k] = e["axis"][k].get<float>();
				b.radius = e.value("radius", 0.05f);
				b.halfHeight = e.value("halfHeight", 0.1f);
				b.mass = e.value("mass", 1.0f);
				r->bodies.push_back(b);
			}
		if (j.contains("joints"))
			for (const auto& e : j["joints"])
			{
				Joint jt;
				jt.bone = e.value("bone", "");
				jt.twistMin = e.value("twistMin", -0.6f);
				jt.twistMax = e.value("twistMax", 0.6f);
				jt.swing1 = e.value("swing1", 0.8f);
				jt.swing2 = e.value("swing2", 0.8f);
				r->joints.push_back(jt);
			}
		return r;
	}
	catch (const std::exception&) { return nullptr; }
}

bool RagdollDef::SaveToFile(const std::string& path) const
{
	bfs::ofstream o(bfs::path(path), std::ios::binary);
	if (!o) return false;
	const std::string s = ToString();
	o.write(s.data(), (std::streamsize)s.size());
	return (bool)o;
}
RagdollDef* RagdollDef::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	std::stringstream ss;
	ss << i.rdbuf();
	return FromString(ss.str());
}
RagdollDef* RagdollDef::LoadFromMemory(const std::string& data) { return FromString(data); }

// Capsule auto-fit: dominant-weight vertices per bone, taken into the bone's BIND-local frame.
RagdollDef* RagdollDef::Build(const Skeleton* sk, const Mesh* mesh, float totalMass)
{
	if (!sk || sk->bones.empty() || !mesh || !mesh->vertexArray || !mesh->boneIndex || !mesh->boneWeight)
		return nullptr;
	const size_t nb = sk->bones.size();
	std::vector<std::vector<glm::vec3>> verts(nb);
	for (int v = 0; v < mesh->numVerts; ++v)
	{
		int best = -1;
		float bw = 0.4f;   // dominance threshold
		for (int k = 0; k < 4; ++k)
		{
			const float w = mesh->boneWeight[v * 4 + k];
			if (w > bw) { bw = w; best = mesh->boneIndex[v * 4 + k]; }
		}
		if (best < 0 || best >= (int)nb) continue;
		const glm::vec4 p(mesh->vertexArray[v * 3], mesh->vertexArray[v * 3 + 1], mesh->vertexArray[v * 3 + 2], 1.0f);
		verts[best].push_back(glm::vec3(glm::make_mat4(sk->bones[best].invBind) * p));
	}

	RagdollDef* r = new RagdollDef();
	r->guid = ResDB::NewGuid();
	r->name = sk->name;
	r->skelGuid = sk->guid;
	float volSum = 0;
	std::vector<float> vols;
	for (size_t b = 0; b < nb; ++b)
	{
		if (verts[b].size() < 8) continue;   // too little geometry: fingers, helpers
		// axis: toward the first child bone, else the dominant spread axis
		glm::vec3 axis(0);
		for (size_t c = 0; c < nb; ++c)
			if (sk->bones[c].parent == (int)b)
			{
				axis = glm::vec3(sk->bones[c].localPos[0], sk->bones[c].localPos[1], sk->bones[c].localPos[2]);
				break;
			}
		glm::vec3 mn(1e9f), mx(-1e9f);
		for (const glm::vec3& p : verts[b]) { mn = glm::min(mn, p); mx = glm::max(mx, p); }
		if (glm::dot(axis, axis) < 1e-8f)
		{
			const glm::vec3 ext = mx - mn;
			axis = ext.x > ext.y ? (ext.x > ext.z ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1))
			                     : (ext.y > ext.z ? glm::vec3(0, 1, 0) : glm::vec3(0, 0, 1));
		}
		axis = glm::normalize(axis);
		float lo = 1e9f, hi = -1e9f, r2 = 0;
		glm::vec3 centroid(0);
		for (const glm::vec3& p : verts[b]) centroid += p;
		centroid /= (float)verts[b].size();
		for (const glm::vec3& p : verts[b])
		{
			const float along = glm::dot(p - centroid, axis);
			lo = std::min(lo, along);
			hi = std::max(hi, along);
			const glm::vec3 perp = (p - centroid) - axis * along;
			r2 += glm::dot(perp, perp);
		}
		Body body;
		body.bone = sk->bones[b].name;
		body.radius = std::max(0.02f, sqrtf(r2 / (float)verts[b].size()) * 1.4f);
		body.halfHeight = std::max(0.01f, (hi - lo) * 0.5f - body.radius);
		const glm::vec3 c = centroid + axis * (lo + hi) * 0.5f;
		body.center[0] = c.x; body.center[1] = c.y; body.center[2] = c.z;
		body.axis[0] = axis.x; body.axis[1] = axis.y; body.axis[2] = axis.z;
		const float vol = 3.14159f * body.radius * body.radius * (2.0f * body.halfHeight)
		                + 4.18879f * body.radius * body.radius * body.radius;
		vols.push_back(vol);
		volSum += vol;
		r->bodies.push_back(body);
	}
	if (r->bodies.empty()) { delete r; return nullptr; }
	for (size_t i = 0; i < r->bodies.size(); ++i)
		r->bodies[i].mass = std::max(0.2f, totalMass * vols[i] / std::max(volSum, 1e-4f));
	// joints: every body whose bone has a bodied ancestor
	auto hasBody = [&](int bone) -> bool
	{
		for (const Body& b : r->bodies)
			if (sk->BoneIndex(b.bone) == bone) return true;
		return false;
	};
	for (const Body& b : r->bodies)
	{
		int bone = sk->BoneIndex(b.bone);
		int p = bone >= 0 ? sk->bones[bone].parent : -1;
		while (p >= 0 && !hasBody(p)) p = sk->bones[p].parent;
		if (p >= 0)
		{
			Joint jt;
			jt.bone = b.bone;
			r->joints.push_back(jt);
		}
	}
	return r;
}

// ---- runtime component ----------------------------------------------------------------------

static iPhysics* Phys() { return GetService<iPhysics>(); }

Ragdoll::Ragdoll() : Component("Ragdoll") {}

void Ragdoll::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	atom = parent;
	parent->components.push_back(this);
}

void Ragdoll::Destroy() { Deactivate(); }
void Ragdoll::FixedUpdate() {}
void Ragdoll::Pause() {}
void Ragdoll::Reset()
{
	Deactivate();
	def = nullptr;
	liveMode = Mode::Off;
}

void Ragdoll::SetMode(double m) { mode = (Mode)(int)m; }
double Ragdoll::GetMode() { return (double)(int)mode; }

void Ragdoll::Impulse(const std::string& bone, const Vector3& imp)
{
	iPhysics* ph = Phys();
	if (!ph || !active || !def) return;
	Skeleton* sk = ResDB::getSingleton()->GetSkeleton(def->skelGuid);
	if (!sk) return;
	const int bi = sk->BoneIndex(bone);
	for (const BodyRt& b : bodiesRt)
		if (b.bone == bi && b.body)
		{
			const float v[3] = { (float)imp.x, (float)imp.y, (float)imp.z };
			ph->addImpulse(b.body, v);
			return;
		}
}

RagdollDef* Ragdoll::EnsureDef(SkinnedMeshRenderer* smr)
{
	if (def) return def;
	ResDB* db = ResDB::getSingleton();
	if (!ragGuid.empty()) { def = db->GetRagdoll(ragGuid); return def; }
	// the checkbox: auto-resolve by the live skeleton
	if (smr && smr->skeleton)
		for (RagdollDef* r : db->ragdolls)
			if (r && r->skelGuid == smr->skeleton->guid) { def = r; return def; }
	return nullptr;
}

bool Ragdoll::InPartial(const Skeleton* sk, int bone) const
{
	if (liveMode != Mode::Partial) return true;
	const int root = sk->BoneIndex(partialRoot);
	if (root < 0) return true;
	for (int j = bone; j >= 0; j = sk->bones[j].parent)
		if (j == root) return true;
	return false;
}

void Ragdoll::Activate(SkinnedMeshRenderer* smr)
{
	iPhysics* ph = Phys();
	if (!ph || !smr || !smr->skeleton || !EnsureDef(smr)) return;
	Skeleton* sk = smr->skeleton;
	const std::vector<float>& g = smr->Globals();
	if (g.size() < sk->bones.size() * 16) return;
	Vector3 gp = transform->globalPosition();
	Quaternion gq = transform->globalRotation();
	const glm::quat aq((float)gq.w, (float)gq.x, (float)gq.y, (float)gq.z);
	const glm::vec3 ap((float)gp.x, (float)gp.y, (float)gp.z);
	liveMode = mode;

	std::map<int, uint64_t> bodyOfBone;
	for (const RagdollDef::Body& b : def->bodies)
	{
		const int bi = sk->BoneIndex(b.bone);
		if (bi < 0 || !InPartial(sk, bi)) continue;
		const glm::mat4 G = glm::make_mat4(&g[bi * 16]);
		const glm::quat boneRot = glm::quat_cast(G);
		const glm::vec3 axis(b.axis[0], b.axis[1], b.axis[2]);
		const glm::quat qAxis = glm::rotation(glm::vec3(0, 1, 0), axis);   // capsule Y -> bone axis
		const glm::vec3 cModel = glm::vec3(G * glm::vec4(b.center[0], b.center[1], b.center[2], 1.0f));
		const glm::vec3 wpos = ap + aq * cModel;
		const glm::quat wrot = aq * boneRot * qAxis;

		NukeBodyDesc d;
		d.shape = 2;
		d.radius = b.radius;
		d.halfHeight = b.halfHeight;
		d.motion = 1;
		d.mass = b.mass * (totalMass / 70.0f);
		d.friction = 0.6f;
		d.angularDamping = 0.3f;
		d.pos[0] = wpos.x; d.pos[1] = wpos.y; d.pos[2] = wpos.z;
		d.quat[0] = wrot.x; d.quat[1] = wrot.y; d.quat[2] = wrot.z; d.quat[3] = wrot.w;
		const uint64_t id = ph->createBody(d);
		if (!id) continue;
		BodyRt rt;
		rt.bone = bi;
		rt.body = id;
		const glm::quat inv = glm::inverse(qAxis);
		rt.invOffRot[0] = inv.x; rt.invOffRot[1] = inv.y; rt.invOffRot[2] = inv.z; rt.invOffRot[3] = inv.w;
		bodiesRt.push_back(rt);
		bodyOfBone[bi] = id;
	}
	for (const RagdollDef::Joint& jt : def->joints)
	{
		const int bi = sk->BoneIndex(jt.bone);
		if (bi < 0 || !bodyOfBone.count(bi)) continue;
		int p = sk->bones[bi].parent;
		while (p >= 0 && !bodyOfBone.count(p)) p = sk->bones[p].parent;
		if (p < 0) continue;
		const glm::mat4 G = glm::make_mat4(&g[bi * 16]);
		const glm::vec3 headModel = glm::vec3(G[3]);
		const glm::vec3 pivot = ap + aq * headModel;
		// twist axis = the body's capsule axis in world
		glm::vec3 axisL(0, 1, 0);
		for (const RagdollDef::Body& b : def->bodies)
			if (b.bone == jt.bone) { axisL = glm::vec3(b.axis[0], b.axis[1], b.axis[2]); break; }
		const glm::vec3 tw = aq * (glm::quat_cast(G) * axisL);
		NukeJointDesc jd;
		jd.bodyA = bodyOfBone[p];
		jd.bodyB = bodyOfBone[bi];
		jd.pivot[0] = pivot.x; jd.pivot[1] = pivot.y; jd.pivot[2] = pivot.z;
		jd.twistAxis[0] = tw.x; jd.twistAxis[1] = tw.y; jd.twistAxis[2] = tw.z;
		const glm::vec3 pl = fabsf(tw.y) < 0.99f ? glm::normalize(glm::cross(tw, glm::vec3(0, 1, 0)))
		                                         : glm::vec3(1, 0, 0);
		jd.planeAxis[0] = pl.x; jd.planeAxis[1] = pl.y; jd.planeAxis[2] = pl.z;
		jd.twistMin = jt.twistMin; jd.twistMax = jt.twistMax;
		jd.swing1 = jt.swing1; jd.swing2 = jt.swing2;
		const uint64_t j = ph->createSwingTwistJoint(jd);
		if (!j) continue;
		jointsRt.push_back(j);
		jointBone.push_back(bi);
		jointParentBone.push_back(p);
		if (liveMode == Mode::Powered) ph->setJointMotor(j, true, motorFrequency, motorDamping);
	}
	active = !bodiesRt.empty();
	std::cout << "[Ragdoll]\tactivated: " << bodiesRt.size() << " bodies, "
	          << jointsRt.size() << " joints" << std::endl;
}

void Ragdoll::Deactivate()
{
	iPhysics* ph = Phys();
	if (ph)
	{
		for (uint64_t j : jointsRt) ph->destroyJoint(j);
		for (const BodyRt& b : bodiesRt) ph->destroyBody(b.body);
	}
	jointsRt.clear();
	jointBone.clear();
	jointParentBone.clear();
	bodiesRt.clear();
	active = false;
}

void Ragdoll::Update()
{
	if (!atom) return;
	SkinnedMeshRenderer* smr = atom->GetComponent<SkinnedMeshRenderer>();
	if (!smr)
		for (Atom* ch : atom->children)
			if (ch && (smr = ch->GetComponent<SkinnedMeshRenderer>())) break;
	if (!smr || !smr->EnsureSkeleton()) return;
	const bool want = enabled && mode != Mode::Off;
	if (want && !active) Activate(smr);
	else if ((!want || mode != liveMode) && active)
	{
		Deactivate();
		if (want) Activate(smr);
	}
	if (active && !fed) BlendInto(smr);   // no Animator handed the pose over — self-drive
	fed = false;
}

void Ragdoll::BlendInto(SkinnedMeshRenderer* smr, bool apply)
{
	fed = !apply;   // apply=false = the Animator handover path
	iPhysics* ph = Phys();
	if (!ph || !def || !smr->skeleton) return;
	Skeleton* sk = smr->skeleton;
	const size_t nb = sk->bones.size();
	if (smr->pose.size() < nb) return;
	Vector3 gp = transform->globalPosition();
	Quaternion gq = transform->globalRotation();
	const glm::quat aq((float)gq.w, (float)gq.x, (float)gq.y, (float)gq.z);
	const glm::quat aqInv = glm::inverse(aq);
	const glm::vec3 ap((float)gp.x, (float)gp.y, (float)gp.z);

	// animated globals from the CURRENT (Animator-written) pose
	std::vector<glm::mat4> g(nb);
	auto forward = [&]()
	{
		for (size_t i = 0; i < nb; ++i)
		{
			const auto& bp = smr->pose[i];
			glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(bp.pos[0], bp.pos[1], bp.pos[2]))
			                * glm::mat4_cast(glm::quat(bp.rot[3], bp.rot[0], bp.rot[1], bp.rot[2]))
			                * glm::scale(glm::mat4(1.0f), glm::vec3(bp.scale[0], bp.scale[1], bp.scale[2]));
			const int par = sk->bones[i].parent;
			g[i] = par >= 0 ? g[par] * local : local;
		}
	};
	forward();

	if (liveMode == Mode::Powered)
	{
		// motors chase the ANIMATED pose: target = child orientation in the parent body frame
		for (size_t k = 0; k < jointsRt.size(); ++k)
		{
			const int c = jointBone[k], p = jointParentBone[k];
			const glm::quat target = glm::normalize(glm::inverse(glm::quat_cast(g[p])) * glm::quat_cast(g[c]));
			const float q[4] = { target.x, target.y, target.z, target.w };
			ph->setJointTarget(jointsRt[k], q);
		}
	}

	// physics globals overwrite the bodied bones
	std::vector<char> phys(nb, 0);
	std::vector<glm::mat4> pg = g;
	for (const BodyRt& b : bodiesRt)
	{
		float pos[3], quat[4];
		if (!ph->getBodyPose(b.body, pos, quat)) continue;
		const glm::quat wrot(quat[3], quat[0], quat[1], quat[2]);
		const glm::quat off(b.invOffRot[3], b.invOffRot[0], b.invOffRot[1], b.invOffRot[2]);
		const glm::quat boneW = wrot * off;                       // world bone rotation
		const glm::quat boneM = aqInv * boneW;                    // -> model space
		// bone origin: body center back to the bone head via the def's local center
		glm::vec3 centerL(0);
		for (const RagdollDef::Body& db : def->bodies)
			if (sk->BoneIndex(db.bone) == b.bone) { centerL = glm::vec3(db.center[0], db.center[1], db.center[2]); break; }
		const glm::vec3 wpos(pos[0], pos[1], pos[2]);
		const glm::vec3 headW = wpos - boneW * centerL;
		const glm::vec3 headM = aqInv * (headW - ap);
		pg[b.bone] = glm::translate(glm::mat4(1.0f), headM) * glm::mat4_cast(boneM);
		phys[b.bone] = 1;
	}
	// globals -> locals, blended into the SMR pose
	const float w = blend;
	for (size_t i = 0; i < nb; ++i)
	{
		if (!phys[i]) continue;
		const int par = sk->bones[i].parent;
		const glm::mat4 parentG = par >= 0 ? pg[par] : glm::mat4(1.0f);
		const glm::mat4 local = glm::inverse(parentG) * pg[i];
		const glm::quat lr = glm::normalize(glm::quat_cast(local));
		const glm::vec3 lp = glm::vec3(local[3]);
		auto& bp = smr->pose[i];
		const glm::vec3 cp(bp.pos[0], bp.pos[1], bp.pos[2]);
		const glm::quat cr(bp.rot[3], bp.rot[0], bp.rot[1], bp.rot[2]);
		const glm::vec3 np = glm::mix(cp, lp, w);
		const glm::quat nr = glm::slerp(cr, lr, w);
		bp.pos[0] = np.x; bp.pos[1] = np.y; bp.pos[2] = np.z;
		bp.rot[0] = nr.x; bp.rot[1] = nr.y; bp.rot[2] = nr.z; bp.rot[3] = nr.w;
	}
	if (apply) smr->Apply();
}

}  // namespace nuke
