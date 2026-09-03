// C1 canonical rig: one name table drives the canonical skeleton, the rename presets,
// the importer's scheme detection and the rig-chain stamping (see NukeRig.h).
#include "API/Model/NukeRig.h"
#include "API/Model/BoneMap.h"
#include "API/Model/Material.h"
#include "API/Model/Skeleton.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace nuke {

namespace {

// One canonical bone: its name in every supported scheme + the canonical T-pose local
// offset (meters, Y-up, facing +Z; left side authored, right side mirrors X). Mixamo names
// are stored WITHOUT the "mixamorig:"/"mixamorig_" prefix (both prefixes are accepted).
struct RigBone
{
	const char* canon;
	const char* parent;    // canonical parent name ("" = root)
	const char* cc;        // Character Creator (CC_Base_*)
	const char* mix;       // Mixamo, prefix stripped
	const char* ue;        // UE mannequin
	float x, y, z;         // local bind offset
};

// clang-format off
static const RigBone kAxial[] = {
	{ "Hips",   "",       "CC_Base_Hip",         "Hips",   "pelvis",   0.0f, 0.95f, 0.0f },
	{ "Spine",  "Hips",   "CC_Base_Waist",       "Spine",  "spine_01", 0.0f, 0.10f, 0.0f },
	{ "Spine1", "Spine",  "CC_Base_Spine01",     "Spine1", "spine_02", 0.0f, 0.12f, 0.0f },
	{ "Spine2", "Spine1", "CC_Base_Spine02",     "Spine2", "spine_03", 0.0f, 0.12f, 0.0f },
	{ "Neck",   "Spine2", "CC_Base_NeckTwist01", "Neck",   "neck_01",  0.0f, 0.15f, 0.0f },
	{ "Head",   "Neck",   "CC_Base_Head",        "Head",   "head",     0.0f, 0.10f, 0.0f },
};

// Left side; canonical/mixamo "Left"->"Right", cc "_L_"->"_R_", ue "_l"->"_r" mirror it.
static const RigBone kLeft[] = {
	{ "LeftShoulder",    "Spine2",           "CC_Base_L_Clavicle", "LeftShoulder", "clavicle_l",  0.08f,  0.13f,  0.0f  },
	{ "LeftArm",         "LeftShoulder",     "CC_Base_L_Upperarm", "LeftArm",      "upperarm_l",  0.12f,  0.0f,   0.0f  },
	{ "LeftForeArm",     "LeftArm",          "CC_Base_L_Forearm",  "LeftForeArm",  "lowerarm_l",  0.28f,  0.0f,   0.0f  },
	{ "LeftHand",        "LeftForeArm",      "CC_Base_L_Hand",     "LeftHand",     "hand_l",      0.26f,  0.0f,   0.0f  },
	{ "LeftHandThumb1",  "LeftHand",         "CC_Base_L_Thumb1",   "LeftHandThumb1",  "thumb_01_l",  0.03f, -0.01f, 0.03f },
	{ "LeftHandThumb2",  "LeftHandThumb1",   "CC_Base_L_Thumb2",   "LeftHandThumb2",  "thumb_02_l",  0.035f, 0.0f,  0.01f },
	{ "LeftHandThumb3",  "LeftHandThumb2",   "CC_Base_L_Thumb3",   "LeftHandThumb3",  "thumb_03_l",  0.03f,  0.0f,  0.01f },
	{ "LeftHandIndex1",  "LeftHand",         "CC_Base_L_Index1",   "LeftHandIndex1",  "index_01_l",  0.09f,  0.0f,  0.025f },
	{ "LeftHandIndex2",  "LeftHandIndex1",   "CC_Base_L_Index2",   "LeftHandIndex2",  "index_02_l",  0.035f, 0.0f,  0.0f  },
	{ "LeftHandIndex3",  "LeftHandIndex2",   "CC_Base_L_Index3",   "LeftHandIndex3",  "index_03_l",  0.025f, 0.0f,  0.0f  },
	{ "LeftHandMiddle1", "LeftHand",         "CC_Base_L_Mid1",     "LeftHandMiddle1", "middle_01_l", 0.095f, 0.0f,  0.005f },
	{ "LeftHandMiddle2", "LeftHandMiddle1",  "CC_Base_L_Mid2",     "LeftHandMiddle2", "middle_02_l", 0.037f, 0.0f,  0.0f  },
	{ "LeftHandMiddle3", "LeftHandMiddle2",  "CC_Base_L_Mid3",     "LeftHandMiddle3", "middle_03_l", 0.027f, 0.0f,  0.0f  },
	{ "LeftHandRing1",   "LeftHand",         "CC_Base_L_Ring1",    "LeftHandRing1",   "ring_01_l",   0.09f,  0.0f, -0.015f },
	{ "LeftHandRing2",   "LeftHandRing1",    "CC_Base_L_Ring2",    "LeftHandRing2",   "ring_02_l",   0.033f, 0.0f,  0.0f  },
	{ "LeftHandRing3",   "LeftHandRing2",    "CC_Base_L_Ring3",    "LeftHandRing3",   "ring_03_l",   0.025f, 0.0f,  0.0f  },
	{ "LeftHandPinky1",  "LeftHand",         "CC_Base_L_Pinky1",   "LeftHandPinky1",  "pinky_01_l",  0.08f,  0.0f, -0.035f },
	{ "LeftHandPinky2",  "LeftHandPinky1",   "CC_Base_L_Pinky2",   "LeftHandPinky2",  "pinky_02_l",  0.028f, 0.0f,  0.0f  },
	{ "LeftHandPinky3",  "LeftHandPinky2",   "CC_Base_L_Pinky3",   "LeftHandPinky3",  "pinky_03_l",  0.02f,  0.0f,  0.0f  },
	{ "LeftUpLeg",       "Hips",             "CC_Base_L_Thigh",    "LeftUpLeg",    "thigh_l",      0.09f, -0.06f,  0.0f  },
	{ "LeftLeg",         "LeftUpLeg",        "CC_Base_L_Calf",     "LeftLeg",      "calf_l",       0.0f,  -0.42f,  0.0f  },
	{ "LeftFoot",        "LeftLeg",          "CC_Base_L_Foot",     "LeftFoot",     "foot_l",       0.0f,  -0.42f,  0.0f  },
	{ "LeftToe",         "LeftFoot",         "CC_Base_L_ToeBase",  "LeftToeBase",  "ball_l",       0.0f,  -0.05f,  0.12f },
};

// Canonical rig chains, root -> tip (the retargeter and IK address the rig through these).
struct RigChain { const char* name; const char* bones[4]; };
static const RigChain kChains[] = {
	{ "Spine",       { "Hips", "Spine", "Spine1", "Spine2" } },
	{ "Neck",        { "Neck", "Head", nullptr } },
	{ "LeftArm",     { "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand" } },
	{ "RightArm",    { "RightShoulder", "RightArm", "RightForeArm", "RightHand" } },
	{ "LeftLeg",     { "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToe" } },
	{ "RightLeg",    { "RightUpLeg", "RightLeg", "RightFoot", "RightToe" } },
	{ "LeftThumb",   { "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3", nullptr } },
	{ "LeftIndex",   { "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3", nullptr } },
	{ "LeftMiddle",  { "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", nullptr } },
	{ "LeftRing",    { "LeftHandRing1", "LeftHandRing2", "LeftHandRing3", nullptr } },
	{ "LeftPinky",   { "LeftHandPinky1", "LeftHandPinky2", "LeftHandPinky3", nullptr } },
	{ "RightThumb",  { "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", nullptr } },
	{ "RightIndex",  { "RightHandIndex1", "RightHandIndex2", "RightHandIndex3", nullptr } },
	{ "RightMiddle", { "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3", nullptr } },
	{ "RightRing",   { "RightHandRing1", "RightHandRing2", "RightHandRing3", nullptr } },
	{ "RightPinky",  { "RightHandPinky1", "RightHandPinky2", "RightHandPinky3", nullptr } },
};
// clang-format on

// Scheme-name mirrors of a LEFT-side row.
static std::string MirrorCanon(const std::string& s) { std::string r = s; r.replace(0, 4, "Right"); return r; }
static std::string MirrorMix(const std::string& s)   { std::string r = s; r.replace(0, 4, "Right"); return r; }
static std::string MirrorCC(const std::string& s)
{
	std::string r = s;
	const size_t p = r.find("_L_");
	if (p != std::string::npos) r.replace(p, 3, "_R_");
	return r;
}
static std::string MirrorUE(const std::string& s)
{
	std::string r = s;
	if (r.size() >= 2 && r.compare(r.size() - 2, 2, "_l") == 0) r.replace(r.size() - 2, 2, "_r");
	return r;
}

// The full table row list (axial + left + mirrored right), built once.
struct Row { std::string canon, cc, mix, ue; };
static const std::vector<Row>& Rows()
{
	static std::vector<Row> rows = []
	{
		std::vector<Row> r;
		for (const RigBone& b : kAxial) r.push_back({ b.canon, b.cc, b.mix, b.ue });
		for (const RigBone& b : kLeft)  r.push_back({ b.canon, b.cc, b.mix, b.ue });
		for (const RigBone& b : kLeft)
			r.push_back({ MirrorCanon(b.canon), MirrorCC(b.cc), MirrorMix(b.mix), MirrorUE(b.ue) });
		return r;
	}();
	return rows;
}

// canonical name -> the scheme's name ("" when the scheme has no such bone).
static std::string SchemeNameOf(const std::string& canon, NukeRig::Scheme s)
{
	for (const Row& r : Rows())
		if (r.canon == canon)
			switch (s)
			{
				case NukeRig::Canonical: return r.canon;
				case NukeRig::CC:        return r.cc;
				case NukeRig::Mixamo:    return r.mix;   // prefix applied by the caller
				case NukeRig::UE:        return r.ue;
				default:                 return std::string();
			}
	return std::string();
}

}  // namespace

Skeleton* NukeRig::CreateCanonical()
{
	Skeleton* sk = new Skeleton();
	sk->guid = "builtin:nukerig";
	sk->name = "NukeRig";
	std::map<std::string, int> index;
	auto add = [&](const std::string& name, const std::string& parent, float x, float y, float z)
	{
		MeshBone b;
		b.name = name;
		b.parent = parent.empty() ? -1 : index[parent];
		b.localPos[0] = x; b.localPos[1] = y; b.localPos[2] = z;
		index[name] = (int)sk->bones.size();
		sk->bones.push_back(b);
	};
	for (const RigBone& b : kAxial) add(b.canon, b.parent, b.x, b.y, b.z);
	for (const RigBone& b : kLeft)  add(b.canon, b.parent, b.x, b.y, b.z);
	for (const RigBone& b : kLeft)
		add(MirrorCanon(b.canon), b.parent[0] ? (std::strncmp(b.parent, "Left", 4) == 0
		    ? MirrorCanon(b.parent) : b.parent) : "", -b.x, b.y, b.z);

	// Inverse bind from the T-pose globals (identity rotations, pure translation rig).
	std::vector<glm::mat4> g(sk->bones.size());
	for (size_t i = 0; i < sk->bones.size(); ++i)
	{
		const MeshBone& b = sk->bones[i];
		glm::mat4 local = glm::translate(glm::mat4(1.0f), glm::vec3(b.localPos[0], b.localPos[1], b.localPos[2]));
		g[i] = b.parent >= 0 ? g[b.parent] * local : local;
		glm::mat4 inv = glm::inverse(g[i]);
		std::memcpy(sk->bones[i].invBind, &inv[0][0], sizeof(float) * 16);
	}

	for (const RigChain& c : kChains)
	{
		SkeletonChain ch;
		ch.name = c.name;
		for (const char* b : c.bones) if (b) ch.bones.push_back(b);
		sk->chains.push_back(ch);
	}

	auto group = [&](const char* name, std::vector<std::string> roots)
	{
		SkeletonGroup gr;
		gr.name = name;
		// A group = its roots + every descendant.
		std::set<int> in;
		for (const std::string& r : roots) if (index.count(r)) in.insert(index[r]);
		for (size_t i = 0; i < sk->bones.size(); ++i)
			if (sk->bones[i].parent >= 0 && in.count(sk->bones[i].parent)) in.insert((int)i);
		for (int i : in) gr.bones.push_back(sk->bones[i].name);
		sk->groups.push_back(gr);
	};
	group("UpperBody", { "Spine" });
	group("LowerBody", { "LeftUpLeg", "RightUpLeg" });
	group("LeftArm",   { "LeftShoulder" });
	group("RightArm",  { "RightShoulder" });

	auto socket = [&](const char* name, const char* bone, float x, float y, float z)
	{
		SkeletonSocket s;
		s.name = name; s.bone = bone;
		s.localPos[0] = x; s.localPos[1] = y; s.localPos[2] = z;
		sk->sockets.push_back(s);
	};
	socket("HandL",   "LeftHand",  0.08f, 0, 0);
	socket("HandR",   "RightHand", -0.08f, 0, 0);
	socket("HeadTop", "Head",      0, 0.15f, 0);
	return sk;
}

const char* NukeRig::PresetGuid(Scheme s)
{
	switch (s)
	{
		case CC:     return "builtin:bonemap-cc";
		case Mixamo: return "builtin:bonemap-mixamo";
		case UE:     return "builtin:bonemap-ue";
		default:     return "";
	}
}

const char* NukeRig::SchemeName(Scheme s)
{
	switch (s)
	{
		case Canonical: return "NukeRig";
		case CC:        return "Character Creator";
		case Mixamo:    return "Mixamo";
		case UE:        return "UE mannequin";
		default:        return "unknown";
	}
}

BoneMap* NukeRig::CreatePreset(Scheme s)
{
	if (s != CC && s != Mixamo && s != UE) return nullptr;
	BoneMap* bm = new BoneMap();
	bm->guid = PresetGuid(s);
	bm->name = std::string(SchemeName(s)) + " -> NukeRig";
	for (const Row& r : Rows())
	{
		switch (s)
		{
			case CC: bm->map[r.cc] = r.canon; break;
			case Mixamo:
				bm->map["mixamorig:" + r.mix] = r.canon;   // both export prefixes seen in the wild
				bm->map["mixamorig_" + r.mix] = r.canon;
				break;
			case UE: bm->map[r.ue] = r.canon; break;
			default: break;
		}
	}
	return bm;
}

NukeRig::Scheme NukeRig::Detect(const std::vector<MeshBone>& bones)
{
	std::set<std::string> names;
	bool mixPrefix = false;
	for (const MeshBone& b : bones)
	{
		names.insert(b.name);
		if (b.name.rfind("mixamorig", 0) == 0)
		{
			mixPrefix = true;
			if (b.name.size() > 9) names.insert(b.name.substr(10));   // strip "mixamorig:"/"_"
		}
	}
	int canon = 0, cc = 0, mix = 0, ue = 0;
	for (const Row& r : Rows())
	{
		if (names.count(r.canon)) ++canon;
		if (names.count(r.cc))    ++cc;
		if (mixPrefix && names.count(r.mix)) ++mix;
		if (names.count(r.ue))    ++ue;
	}
	int best = std::max(std::max(canon, cc), std::max(mix, ue));
	if (best < 6) return None;
	if (best == mix && mixPrefix) return Mixamo;   // mixamo names == canonical minus prefix
	if (best == cc)   return CC;
	if (best == ue)   return UE;
	return Canonical;
}

void NukeRig::StampChains(Skeleton* sk, Scheme s)
{
	if (!sk || s == None) return;
	// Mixamo skeletons carry the export prefix on every bone: detect which one.
	std::string mixPre = "mixamorig:";
	if (s == Mixamo)
		for (const MeshBone& b : sk->bones)
			if (b.name.rfind("mixamorig_", 0) == 0) { mixPre = "mixamorig_"; break; }
	for (const RigChain& c : kChains)
	{
		bool exists = false;
		for (const SkeletonChain& have : sk->chains) if (have.name == c.name) { exists = true; break; }
		if (exists) continue;
		SkeletonChain ch;
		ch.name = c.name;
		for (const char* cb : c.bones)
		{
			if (!cb) break;
			std::string n = SchemeNameOf(cb, s);
			if (n.empty()) continue;
			if (s == Mixamo) n = mixPre + n;
			if (sk->BoneIndex(n) >= 0) ch.bones.push_back(n);
		}
		if (ch.bones.size() >= 2) sk->chains.push_back(ch);
	}
}

// A material name that reads as CC content regardless of the rig naming: CC5's UE5 export
// preset renames the bones but keeps the Reallusion material slots ("Std_*", game-base "Ga_*").
bool NukeRig::LooksLikeCCMaterial(const std::string& name)
{
	std::string n = name;
	for (char& c : n) c = (char)tolower((unsigned char)c);
	return n.rfind("std_", 0) == 0 || n.rfind("ga_", 0) == 0;
}

bool NukeRig::TuneCCMaterial(Material* m)
{
	if (!m) return false;
	std::string n = m->matName;
	for (char& c : n) c = (char)tolower((unsigned char)c);
	auto has = [&](const char* sub) { return n.find(sub) != std::string::npos; };
	// Order matters: the specific eye layers match before the generic "std_eye". The CC5
	// game-base exports drop the Std_ prefix ("Ga_Skin_Body", "Lash_Up_Wavy", "Brows_*") —
	// every slot matches by its bare identity too.
	if (has("eye_occlusion"))
	{ m->blendMode = Material::Transparent; m->castShadows = false; }
	else if (has("tearline"))
	{ m->blendMode = Material::Transparent; m->roughness = 0.05f; m->specular = 1.0f; m->color.a = 0.25; m->castShadows = false; }
	else if (has("cornea"))
	{ m->blendMode = Material::Transparent; m->roughness = 0.03f; m->specular = 1.0f; m->color.a = 0.15; m->castShadows = false; }
	else if (has("eyelash") || has("lash_"))
	{ m->blendMode = Material::Cutout; m->alphaCutoff = 0.35f; m->roughness = 0.7f; }
	else if (has("std_eye") || n.rfind("ga_eye", 0) == 0)
	{ m->roughness = 0.1f; m->specular = 0.9f; m->irisDepth = 0.12f; }
	else if (has("skin"))
	{
		m->roughness = 0.5f; m->specular = 0.5f;
		// Pre-integrated-style scatter + a touch of translucency for backlit ears/nose.
		// Kept SUBTLE: on textured skin an aggressive terminator tint reads as bruises.
		m->subsurface = 0.35f;
		m->translucency = 0.12f; m->translucencyTint = Color(1.0, 0.45, 0.35, 1.0);
	}
	else if (has("_teeth") || has("teeth_"))
	{ m->roughness = 0.25f; m->specular = 0.7f; }
	else if (has("tongue"))
	{ m->roughness = 0.35f; m->specular = 0.6f; }
	else if (has("nails"))
	{ m->roughness = 0.4f; }
	// NOTE: hashedAlpha stays a MANUAL opt-in — without temporal smoothing the stochastic
	// clip reads as dirty speckle on faces (brows/lashes), not soft edges.
	else if (has("hair") || has("scalp") || has("beard") || has("eyebrow") || has("brows"))
	{ m->blendMode = Material::Cutout; m->alphaCutoff = 0.4f; m->roughness = 0.45f; m->anisotropy = 0.5f; }
	else return false;
	return true;
}

const char* NukeRig::MorphPresetGuid() { return "builtin:morphmap-arkit-cc"; }

BoneMap* NukeRig::CreateMorphPreset()
{
	// The 52 ARKit blendshape names in CC ExPlus order: the CC target is "A<nn>_" + the
	// same words underscored ("browInnerUp" -> "A01_Brow_Inner_Up"), so the table derives.
	static const char* kArkit[52] = {
		"browInnerUp", "browDownLeft", "browDownRight", "browOuterUpLeft", "browOuterUpRight",
		"eyeLookUpLeft", "eyeLookUpRight", "eyeLookDownLeft", "eyeLookDownRight",
		"eyeLookOutLeft", "eyeLookInLeft", "eyeLookInRight", "eyeLookOutRight",
		"eyeBlinkLeft", "eyeBlinkRight", "eyeSquintLeft", "eyeSquintRight",
		"eyeWideLeft", "eyeWideRight", "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
		"noseSneerLeft", "noseSneerRight", "jawOpen", "jawForward", "jawLeft", "jawRight",
		"mouthFunnel", "mouthPucker", "mouthLeft", "mouthRight",
		"mouthRollUpper", "mouthRollLower", "mouthShrugUpper", "mouthShrugLower", "mouthClose",
		"mouthSmileLeft", "mouthSmileRight", "mouthFrownLeft", "mouthFrownRight",
		"mouthDimpleLeft", "mouthDimpleRight", "mouthUpperUpLeft", "mouthUpperUpRight",
		"mouthLowerDownLeft", "mouthLowerDownRight", "mouthPressLeft", "mouthPressRight",
		"mouthStretchLeft", "mouthStretchRight", "tongueOut",
	};
	BoneMap* bm = new BoneMap();
	bm->guid = MorphPresetGuid();
	bm->name = "CC -> ARKit-52";
	for (int i = 0; i < 52; ++i)
	{
		const char* a = kArkit[i];
		char cc[64];
		int k = snprintf(cc, sizeof(cc), "A%02d_", i + 1);
		for (const char* c = a; *c && k < 62; ++c)
		{
			if (c == a) cc[k++] = (char)toupper((unsigned char)*c);
			else if (*c >= 'A' && *c <= 'Z') { cc[k++] = '_'; cc[k++] = *c; }
			else cc[k++] = *c;
		}
		cc[k] = 0;
		bm->map[cc] = a;
	}
	return bm;
}

}  // namespace nuke
