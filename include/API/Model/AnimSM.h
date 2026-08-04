#pragma once
#ifndef NUKEE_ANIMSM_H
#define NUKEE_ANIMSM_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include "reflect/Reflect.h"

namespace nuke {

// Animation state-machine ASSET (.nusm, JSON): parameters + layers of states/transitions,
// shared by every Animator that references it (per-Animator runtime state lives in the
// component). States may nest (sub-machines); transitions live in condition/exit-time terms
// over the parameters.
class NUKEENGINE_API AnimSM
{
	NUKE_CLASS(AnimSM, Object)
public:
	std::string guid;                          // asset id (ResDB)
	[[nuke::prop(label="Name")]] std::string name;

	// Runtime-driven values conditions read. Triggers auto-reset when a transition consumes them.
	struct Param
	{
		std::string name;
		int   type = 0;                        // 0 float, 1 bool, 2 trigger
		float def = 0;                         // float default / bool default (!= 0)
	};

	// One condition over a parameter. ops: 0 greater, 1 less, 2 equals, 3 not-equals (floats,
	// `value`), 4 is-true, 5 is-false (bools), 6 trigger-set.
	struct Cond
	{
		std::string param;
		int   op = 0;
		float value = 0;
	};

	// A transition edge. `from` = "" or "*" matches ANY state of the machine it sits in.
	// `to` may name a sub-machine (enters its entry chain). With no conditions the transition
	// needs hasExit; with both, conditions are checked only past the exit point. List order =
	// priority (first match wins). `interrupt`: 0 = an in-flight transition finishes first,
	// 1 = new matches may cut it short (restart blending from the live pose).
	struct Transition
	{
		std::string from, to;
		std::vector<Cond> conds;
		bool  hasExit = false;
		float exitTime = 1.0f;                 // normalized state time (1 = clip end; loops each cycle)
		float duration = 0.25f;                // blend seconds
		int   mode = 0;                        // 0 inertialize (standard), 1 crossfade
		int   interrupt = 0;
	};

	// A state: motion = clip guid/name OR a .nublend guid. Non-empty `states` makes it a
	// SUB-MACHINE (its own entry/transitions; outer transitions from the sub-machine apply
	// while inside any child). `speedParam` names a float parameter multiplying `speed`.
	// `sync` puts the state in a phase-sync group (walk/run keep normalized time in step).
	struct State
	{
		std::string name;
		std::string motion;
		bool  loop = true;
		float speed = 1;
		std::string speedParam;
		bool  mirror = false;
		std::string sync;
		std::vector<State> states;             // sub-machine children
		std::vector<Transition> transitions;   // sub-machine internal edges
		std::string entry;                     // sub-machine entry child
		float nx = 0, ny = 0;                  // node-graph canvas position (0,0 = auto-layout)
	};

	// A pose layer. Layer 0 is the base (full-body); higher layers stack masked overrides or
	// additive deltas. `mask` names a skeleton bone GROUP ("" = every bone). Additive layers
	// add (pose - clip first frame) scaled by weight.
	struct Layer
	{
		std::string name;
		std::string mask;
		bool  additive = false;
		float weight = 1;
		std::vector<State> states;
		std::vector<Transition> transitions;
		std::string entry;
	};

	std::vector<Param> params;
	std::vector<Layer> layers;

	// Native asset format (.nusm): JSON — small, diff-able, hand-editable.
	bool           SaveToFile(const std::string& path) const;
	static AnimSM* LoadFromFile(const std::string& path);
	static AnimSM* LoadFromMemory(const std::string& data);   // packed content
	static AnimSM* FromString(const std::string& json);
	std::string    ToString() const;
};

}  // namespace nuke

#endif // !NUKEE_ANIMSM_H
