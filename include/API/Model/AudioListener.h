#pragma once
#ifndef NUKEE_AUDIOLISTENER_H
#define NUKEE_AUDIOLISTENER_H
#include "NukeAPI.h"
#include "Component.h"
#include "reflect/Reflect.h"

namespace nuke {

// Marks the atom whose pose spatial audio is heard from. DATA ONLY — World::Render picks the
// first enabled listener each frame; without one the active camera is the listener.
class NUKEENGINE_API AudioListener : public Component
{
	NUKE_CLASS(AudioListener, Component, "Audio")
public:
	AudioListener();
	void Init(Atom* parent) override;
	void Destroy() override;
	void Update() override;
	void FixedUpdate() override;
	void Pause() override;
	void Reset() override;
};

}  // namespace nuke

#endif // !NUKEE_AUDIOLISTENER_H
