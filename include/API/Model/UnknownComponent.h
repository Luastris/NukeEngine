#pragma once
#ifndef NUKEE_UNKNOWN_COMPONENT_H
#define NUKEE_UNKNOWN_COMPONENT_H
#include "NukeAPI.h"
#include "Component.h"
#include <string>

namespace nuke {

// Placeholder for a component whose type is not registered (its plugin isn't loaded).
// It preserves the original type name + serialized props verbatim and does nothing, so a
// scene that references plugin types without the plugin loads intact, stays inert, and
// round-trips on save (the data isn't lost). When the plugin loads, a reload restores the
// real component.
class NUKEENGINE_API UnknownComponent : public Component
{
public:
	std::string typeName;        // the original component type
	std::string rawProps;        // its serialized props JSON, kept verbatim
	std::string requiredPlugin;  // dll name of the plugin that provides this type (for the UI)

	UnknownComponent();

	void Init(Atom* parent) override;   // out-of-line key function (stable RTTI across DLLs)
	void Destroy() override {}
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
};

}  // namespace nuke

#endif // !NUKEE_UNKNOWN_COMPONENT_H
