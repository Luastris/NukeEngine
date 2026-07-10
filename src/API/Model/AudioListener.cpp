#include "API/Model/AudioListener.h"
#include "API/Model/Atom.h"

namespace nuke {

AudioListener::AudioListener() : Component("AudioListener") {}

void AudioListener::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

void AudioListener::Destroy()     {}
void AudioListener::Update()      {}
void AudioListener::FixedUpdate() {}
void AudioListener::Pause()       {}
void AudioListener::Reset()       {}

}  // namespace nuke
