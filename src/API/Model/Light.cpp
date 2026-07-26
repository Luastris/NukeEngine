#include "API/Model/Light.h"
#include "API/Model/Atom.h"

namespace nuke {

Light::Light() : Component("Light") {}

void Light::Init(Atom* parent)
{
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void Light::Destroy()     {}
void Light::Update()      {}
void Light::FixedUpdate() {}
void Light::Pause()       {}
void Light::Reset()       {}

// ---- FrameLights: one-frame dynamic light submissions (modules/effects) --------------------
static std::vector<NukeLight> gFrameLights;
void FrameLights::Submit(const NukeLight& l)  { gFrameLights.push_back(l); }
std::vector<NukeLight>& FrameLights::Frame()  { return gFrameLights; }

}  // namespace nuke
