#pragma once
#ifndef NUKEE_VARIANTSET_H
#define NUKEE_VARIANTSET_H
#include "NukeAPI.h"
#include "Component.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

class Atom;

// Switches the atom's child VARIANTS grouped by name prefix ("H - Fluffy" -> group "H",
// item "Fluffy"): an exclusive group keeps at most ONE child enabled (hairstyles, shoes),
// a multi group toggles freely (accessories). The state IS the children's enabled flags —
// nothing else is serialized. Stamped by the VRM importer on outfit avatars; works on any
// prefab whose children follow the "<Group> - <Item>" convention.
class NUKEENGINE_API VariantSet : public Component
{
	NUKE_CLASS(VariantSet, Component, "Gameplay")
public:
	[[nuke::prop(label="Multi Groups", tip="';'-separated group prefixes that allow SEVERAL active items at once (accessories); every other group is exclusive - selecting an item disables its siblings. The VRM importer fills this from what the avatar ships enabled; edit freely.")]]
	std::string multi;

	// "<Group> - <Item>" name split (trailing spaces trimmed); false = not a variant child.
	static bool Split(const std::string& name, std::string& group, std::string& item);

	// --- script surface (auto-bound) ---
	[[nuke::func]] std::string Groups();                            // ';'-joined group prefixes
	[[nuke::func]] std::string Items(const std::string& group);     // ';'-joined item names
	[[nuke::func]] std::string Selected(const std::string& group);  // ';'-joined ENABLED items
	[[nuke::func]] bool IsMulti(const std::string& group);
	// Enable/disable one item; enabling in an exclusive group disables its siblings.
	[[nuke::func]] void Select(const std::string& group, const std::string& item, bool on);

	VariantSet() : Component("VariantSet") {}
	void Init(Atom* parent) override;
	void Destroy() override {}
	void Update() override {}
	void FixedUpdate() override {}
	void Pause() override {}
	void Reset() override {}
};

}  // namespace nuke

#endif // !NUKEE_VARIANTSET_H
