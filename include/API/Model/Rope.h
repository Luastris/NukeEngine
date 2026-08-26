#pragma once
#ifndef NUKEE_ROPE_H
#define NUKEE_ROPE_H
#include "NukeAPI.h"
#include "Component.h"
#include "Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nuke {

class Mesh;
class MeshRenderer;

// A physical rope/chain. The authored polyline (viewport curve gizmo) becomes a chain of
// capsule segments linked by swing-twist joints at play; ends pin to the world or tie to
// bodies. The visual is a per-frame generated mesh on a transient MeshRenderer: a smooth
// tube, a repeated link mesh (chains), or ANY source mesh auto-rigged along the chain.
// Runtime API: Cut / Pull / re-Attach; a cut severs one link and broadcasts "rope.cut".
class NUKEENGINE_API Rope : public Component
{
	NUKE_CLASS(Rope, Component, "Physics")
public:
	[[nuke::prop(label="Points", tip="Rope path, atom-local x,y,z per point; drag the points in the viewport while selected.")]]
	std::vector<float> points = { 0.0f, 0.0f, 0.0f,   0.0f, -2.0f, 0.0f };
	[[nuke::prop(label="Segment Length", min=0.05, tip="Simulation resolution: shorter = smoother + heavier")]]
	float segmentLength = 0.25f;
	[[nuke::prop(label="Radius", min=0.005)]] float radius = 0.05f;
	[[nuke::prop(label="Total Mass", min=0.01)]] float totalMass = 2.0f;
	[[nuke::prop(label="Bend Limit", min=1, max=90, tip="Max bend per link, degrees")]] float bendLimit = 30.0f;
	[[nuke::prop(label="Twist Limit", min=0, max=180, tip="Max twist per link, degrees")]] float twistLimit = 30.0f;
	[[nuke::prop(label="Pin Start", tip="Pin the rope start to the world (ignored when Attach Start is set)")]]
	bool pinStart = true;
	[[nuke::prop(label="Pin End")]] bool pinEnd = false;
	[[nuke::prop(label="Attach Start", tip="Atom with a Collider the rope start ties to")]] Atom* attachStart = nullptr;
	[[nuke::prop(label="Attach End")]]   Atom* attachEnd = nullptr;
	[[nuke::prop(label="Visual", enum="Tube,Links,Mesh,None",
	             tip="Tube = smooth cable; Links = Link Mesh repeated per segment (chains); Mesh = the source mesh auto-rigged along the rope")]]
	int visual = 0;
	[[nuke::prop(asset="mesh", label="Link/Source Mesh", tip="Links: one chain link. Mesh: any mesh to bend along the rope.")]]
	std::string meshGuid;
	[[nuke::prop(asset="material", label="Material")]] std::string matGuid;
	[[nuke::prop(label="Tube Sides", min=3, max=24)]] int tubeSides = 8;
	[[nuke::prop(label="Smoothing", min=0, max=8, tip="Catmull-Rom subdivisions per segment for the Tube/Mesh visuals — hides the simulation segments (0 = raw chain)")]]
	int smoothing = 4;
	[[nuke::prop(label="Link Twist", min=0, max=180, tip="Links: extra twist per link, degrees (chains read 90)")]]
	float linkTwist = 90.0f;
	[[nuke::prop(label="Mesh Axis", enum="X,Y,Z", tip="Links/Mesh: source-mesh axis that runs along the rope")]]
	int meshAxis = 2;

	Rope() : Component("Rope") {}
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override {}
	void FixedUpdate() override;
	void Pause() override {}
	void Reset() override;
	void OnRender(iRender* r, RenderPhase phase) override;

	[[nuke::func]] bool    Built();
	[[nuke::func]] double  SegmentCount();
	[[nuke::func]] Vector3 SegmentPos(double i);       // segment center, world
	[[nuke::func]] Vector3 EndPos();                   // last segment tip, world
	[[nuke::func]] double  Length();                   // simulated length (sum of segments)
	[[nuke::func]] bool    Cut(const Vector3& worldPos);   // severs the link nearest to the point
	[[nuke::func]] void    Pull(const Vector3& impulse);   // impulse on the END segment
	[[nuke::func]] void    PullAt(double seg, const Vector3& impulse);
	[[nuke::func]] void    AttachStartTo(Atom* a);     // runtime re-tie (null = release the end)
	[[nuke::func]] void    AttachEndTo(Atom* a);

private:
	struct Seg { uint64_t body = 0; float len = 0.0f; };
	std::vector<Seg>      segs;
	std::vector<uint64_t> links;      // joint i ties segment i to i+1 (0 = cut)
	uint64_t startPin = 0, endPin = 0;
	bool built = false;

	Mesh*         gen = nullptr;      // OWNED generated visual (never in the asset db)
	MeshRenderer* mr  = nullptr;      // OWNED transient renderer on this atom
	std::string   matRes;

	void Build();
	void Teardown();
	uint64_t TieEnd(int segIndex, Atom* attach, const Vector3& worldPoint);
	void EnsureRenderer();
	void FreeGen();
	void EnsureGen(int vertCount, bool uvs);
	void BuildVisual();               // per-frame: segment poses -> mesh
	// Chain stations (world): every span start + segment tips, with parallel-transport frames.
	struct Station { Vector3 p, t, n; float d; int span; };
	void CollectStations(std::vector<Station>& out);
	void RefineStations(std::vector<Station>& st);   // Catmull-Rom subdivision (Tube/Mesh looks)
};

}  // namespace nuke

#endif // !NUKEE_ROPE_H
