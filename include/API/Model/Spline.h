#pragma once
#ifndef NUKEE_SPLINE_H
#define NUKEE_SPLINE_H
#include "API/Model/InstancedMesh.h"
#include "API/Model/Vector.h"
#include "reflect/Reflect.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nuke {

class Mesh;
class MeshRenderer;

// One resampled curve station: atom-local position, unit tangent, parallel-transport normal
// and the cumulative LOCAL arc length up to it.
struct SplineSample { float p[3]; float t[3]; float n[3]; float d; };

// An editable control curve (atom-LOCAL points; Catmull-Rom or cubic Bezier): the authoring
// backbone for roads/rails/cables (SplineMesh), props along a path (SplineScatter), platforms
// and camera rails (SplineMover), rivers, and scripts (world-space queries by meters).
// Bezier point layout: anchor, handle, handle, anchor, ... — every 3rd point is an anchor.
class NUKEENGINE_API Spline : public Component
{
	NUKE_CLASS(Spline, Component, "World")
public:
	[[nuke::prop(label="Points", tip="Control points, atom-local x,y,z per point. Drag them in the viewport while selected; Ctrl+Click a point deletes it, Ctrl+Click near the curve appends one.")]] std::vector<float> points =
		{ -6.0f, 0.0f, 0.0f,   0.0f, 0.0f, 4.0f,   6.0f, 0.0f, 0.0f };
	[[nuke::prop(label="Type", enum="Catmull-Rom,Bezier", tip="Catmull-Rom passes through every point; Bezier reads anchor,handle,handle,anchor,... and the handles shape each span.")]] int type = 0;
	[[nuke::prop(label="Closed", tip="Join the last point back to the first.")]] bool closed = false;

	Spline();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
	void OnRender(iRender* r, RenderPhase phase) override;

	// ---- world-space queries (distances in meters along the curve) ----
	[[nuke::func]] double  Length();
	[[nuke::func]] Vector3 PositionAt(double dist);
	[[nuke::func]] Vector3 TangentAt(double dist);
	[[nuke::func]] Vector3 NormalAt(double dist);
	[[nuke::func]] double  ClosestDistance(const Vector3& worldPos);
	// ---- point editing (atom-local; in Bezier mode AddPoint appends handle,handle,anchor) ----
	[[nuke::func]] int     PointCount();
	[[nuke::func]] Vector3 GetPoint(int index);
	[[nuke::func]] void    SetPoint(int index, const Vector3& localPos);
	[[nuke::func]] void    AddPoint(const Vector3& localPos);
	[[nuke::func]] void    RemovePoint(int index);

	// Atom-local resample (consumers build content in the spline atom's local space, so the
	// atom's own transform moves/scales it with zero rebuilds). Lazy; keyed by a point stamp.
	const std::vector<SplineSample>& LocalSamples();
	double LocalLength();
	// Interpolated local frame at a local arc distance (clamped to [0, LocalLength]).
	void FrameAtLocal(double dist, Vector3& pos, Vector3& tan, Vector3& nrm);
	// Stamp of the local geometry (points/type/closed) — consumers rebuild when it changes.
	uint64_t Stamp();

private:
	std::vector<SplineSample> samples;
	uint64_t sampleStamp = ~0ull;   // local cache key (points/type/closed)
	std::vector<float> worldD;      // per-sample cumulative WORLD arc length
	double   worldLen = 0.0;
	uint64_t worldStamp = ~0ull;    // world table key (local stamp + atom pose/scale)
	void RebuildLocal();
	void EnsureWorld();
	Vector3 ToWorld(const float p[3]) const;   // full atom transform (scale included)
};

// A source mesh laid along the atom's Spline: Deform bends stretched copies end-to-end into a
// continuous strip (roads/rails/cables), Repeat places rigid copies at a fixed pitch (fence
// links, sleepers). The generated mesh renders through a transient MeshRenderer on the same
// atom — shadows/G-buffer/RT come from the standard path — so keep the atom's own renderer slot
// free for it.
class NUKEENGINE_API SplineMesh : public Component
{
	NUKE_CLASS(SplineMesh, Component, "World")
public:
	[[nuke::prop(asset="mesh", label="Mesh", tip="Source segment; its extent along Forward Axis is the tile length.")]] std::string meshGuid;
	[[nuke::prop(asset="material", label="Material")]] std::string matGuid;
	[[nuke::prop(label="Mode", enum="Deform,Repeat", tip="Deform bends stretched copies into a continuous strip; Repeat places rigid copies every tile length + Spacing.")]] int mode = 0;
	[[nuke::prop(label="Forward Axis", enum="X,Y,Z", tip="Source-mesh axis that runs along the curve.")]] int axis = 2;
	[[nuke::prop(label="Scale", min=0.01, tip="Uniform scale of the source profile.")]] float scale = 1.0f;
	[[nuke::prop(label="Spacing", min=0, tip="Repeat only: extra gap between copies, meters.")]] float spacing = 0.0f;
	[[nuke::prop(label="Offset", tip="Profile offset in the curve frame: x = right, y = up, z = along the curve.")]] Vector3 offset = Vector3(0, 0, 0);

	SplineMesh();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
	void OnRender(iRender* r, RenderPhase phase) override;   // lazy rebuild on any input change

	[[nuke::func]] void Rebuild();   // force a regeneration now

private:
	Mesh*         gen = nullptr;     // OWNED generated mesh (never in the asset db)
	MeshRenderer* mr  = nullptr;     // OWNED transient renderer on the same atom
	uint64_t      genStamp = ~0ull;  // inputs the mesh was generated from
	std::string   matRes;            // guid mr's material instance was cloned from
	void EnsureRenderer();
	void FreeGen();
};

// Instances of a mesh scattered along the atom's Spline (posts, lamps, pylons, plants) —
// rides the InstancedMesh path: chunked frustum culling, shadows, RT, serialized in-component.
class NUKEENGINE_API SplineScatter : public InstancedMesh
{
	NUKE_CLASS(SplineScatter, InstancedMesh, "World")
public:
	[[nuke::prop(label="Spacing", min=0.01, tip="Meters between instances along the curve.")]] float spacing = 2.0f;
	[[nuke::prop(label="Jitter", min=0, max=1, tip="Random slide along the curve, fraction of Spacing.")]] float jitter = 0.0f;
	[[nuke::prop(label="Lateral Spread", min=0, tip="Random offset to the sides of the curve, meters.")]] float lateralSpread = 0.0f;
	[[nuke::prop(label="Offset", tip="Constant offset in the curve frame: x = right, y = up, z = along.")]] Vector3 offset = Vector3(0, 0, 0);
	[[nuke::prop(label="Seed", min=0, tip="Same seed + same rules = the same scatter.")]] int seed = 1337;
	[[nuke::prop(label="Scale Min", min=0.01)]] float scaleMin = 1.0f;
	[[nuke::prop(label="Scale Max", min=0.01)]] float scaleMax = 1.0f;
	[[nuke::prop(label="Random Yaw", tip="Random rotation around the up axis.")]] bool randomYaw = false;
	[[nuke::prop(label="Align To Curve", min=0, max=1, tip="0 = instances stand world-up, 1 = they bank and pitch with the curve.")]] float alignToCurve = 1.0f;

	SplineScatter();
	[[nuke::func]] void Rebuild();                 // re-scatter from the rules now
	bool EnsureRenderReady(iRender* r) override;   // auto-rebuild when the spline or rules change

private:
	uint64_t scatterStamp = ~0ull;   // inputs the instances were scattered from
};

// Drives its atom along a Spline with a speed profile — moving platforms, camera rails, patrol
// carts. Movement writes the Transform, so with a Collider + kinematic Rigidbody on the same
// atom the physics kinematic drive carries whatever stands on it.
class NUKEENGINE_API SplineMover : public Component
{
	NUKE_CLASS(SplineMover, Component, "World")
public:
	[[nuke::prop(label="Spline", tip="Atom carrying the Spline to follow; empty = this atom's own Spline.")]] Atom* splineAtom = nullptr;
	[[nuke::prop(label="Speed", min=0, tip="Meters per second along the curve.")]] float speed = 2.0f;
	[[nuke::prop(label="Speed Over Path", widget="curve", min=0, tip="Speed multiplier over the normalized position 0..1 — ease in/out lives here. Empty = constant speed.")]] std::vector<float> speedCurve;
	[[nuke::prop(label="Mode", enum="Loop,Ping-Pong,Once")]] int mode = 0;
	[[nuke::prop(label="Align To Curve", tip="Rotate the atom to face along the curve while moving.")]] bool align = true;
	[[nuke::prop(label="Play On Start")]] bool playOnStart = true;

	SplineMover();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;

	[[nuke::func]] void   Play();
	[[nuke::func]] void   Stop();
	[[nuke::func]] void   SetProgress(double t01);   // jump to a normalized position 0..1
	[[nuke::func]] double GetProgress();

private:
	double dist = 0.0;       // current position, world meters along the curve
	int    dir = 1;          // ping-pong direction
	bool   playing = false;
	bool   started = false;  // Play On Start latched on the first playing frame
	Spline* Resolve();
};

}  // namespace nuke

#endif // !NUKEE_SPLINE_H
