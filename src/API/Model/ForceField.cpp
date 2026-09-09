#include "API/Model/ForceField.h"
#include "API/Model/Atom.h"
#include "API/Model/BendVolumes.h"
#include "API/Model/DebugDraw.h"
#include "API/Model/Time.h"
#include "API/Model/Color.h"
#include "interface/AppInstance.h"
#include <boost/thread/mutex.hpp>
#include <algorithm>

namespace nuke {

static boost::mutex gFFLock;
static std::vector<ForceField*> gFields;

ForceField::ForceField() : Component("ForceField") {}
void ForceField::Init(Atom* parent)
{
	atom = parent; transform = &parent->GetTransform();
	parent->components.push_back(this);
	boost::mutex::scoped_lock l(gFFLock);
	if (std::find(gFields.begin(), gFields.end(), this) == gFields.end()) gFields.push_back(this);
}
void ForceField::Destroy()
{
	boost::mutex::scoped_lock l(gFFLock);
	gFields.erase(std::remove(gFields.begin(), gFields.end(), this), gFields.end());
}
void ForceField::Update() {}
void ForceField::FixedUpdate() {}
void ForceField::Pause() {}
void ForceField::Reset() {}

// Submits the field as a BendVolume (foliage, fluid fog) and draws the selected field's gizmo.
void ForceField::OnRender(iRender*, RenderPhase phase)
{
	if (phase != RenderPhase::Overlay || !transform) return;
	if (enabled)
	{
		const unsigned long long fr = Time::getSingleton()->frame;
		if (fr != bendSubmitFrame)   // OnRender fires per pass/camera - submit once
		{
			bendSubmitFrame = fr;
			Vector3 c = transform->globalPosition(), u = vortexAxis ? transform->up() : Vector3(0, 1, 0);
			BendVolume v;
			v.pos[0] = (float)c.x; v.pos[1] = (float)c.y; v.pos[2] = (float)c.z;
			v.dir[0] = (float)u.x; v.dir[1] = (float)u.y; v.dir[2] = (float)u.z;   // the vortex axis
			v.pull = std::max(-1.0f, std::min(1.0f, vortexPull));
			v.inner = std::max(0.0f, vortexInner); v.dentDepth = std::max(0.0f, std::min(1.0f, dentDepth)); v.dentSharp = std::max(0.0f, std::min(1.0f, dentSharp));
			v.dentSize = std::max(0.0f, dentSize); v.dentDensity = std::max(0.0f, dentDensity);
			v.radius = radius > 0.01f ? radius : 0.01f;
			v.falloff = falloff;
			switch (mode)
			{
				case 0:  v.mode = 1; v.strength = -strength; break;   // attract = inward radial
				case 1:  v.mode = 1; v.strength = strength;  break;   // repel
				case 2:  v.mode = 2; v.strength = strength;  break;   // vortex
				default: v.mode = 3; v.strength = strength;  break;   // turbulence
			}
			BendVolumes::Submit(v);
		}
	}
	AppInstance* app = AppInstance::GetSingleton();
	if (!app->isEditor() || app->selectedInHieararchy != atom) return;
	static const Color kModeCol[4] = { Color(0.4, 0.8, 1.0, 1.0),   // attract: blue
	                                   Color(1.0, 0.5, 0.3, 1.0),   // repel: orange
	                                   Color(0.7, 0.5, 1.0, 1.0),   // vortex: violet
	                                   Color(0.5, 1.0, 0.6, 1.0) }; // turbulence: green
	DebugDraw::WireSphere(transform->globalPosition(), radius, kModeCol[mode & 3]);
}

void ForceField::Snapshot(std::vector<ForceFieldSnap>& out)
{
	boost::mutex::scoped_lock l(gFFLock);
	out.clear();
	for (ForceField* f : gFields)
	{
		if (!f || !f->enabled || !f->transform) continue;
		Vector3 c = f->transform->globalPosition(), u = f->vortexAxis ? f->transform->up() : Vector3(0, 1, 0);
		ForceFieldSnap s; s.mode = f->mode; s.center[0] = (float)c.x; s.center[1] = (float)c.y; s.center[2] = (float)c.z;
		s.axis[0] = (float)u.x; s.axis[1] = (float)u.y; s.axis[2] = (float)u.z; s.pull = std::max(-1.0f, std::min(1.0f, f->vortexPull));
		s.radius = f->radius > 0.01f ? f->radius : 0.01f; s.strength = f->strength; s.falloff = f->falloff;
		out.push_back(s);
	}
}

}  // namespace nuke
