#pragma once
#ifndef NUKEE_PAIREDANIM_H
#define NUKEE_PAIREDANIM_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

class Atom;
class World;

// Two rigs in one synchronized interaction. A `.nupair` asset (JSON) names a clip per role
// plus each role's entry offset from a shared anchor; Start aligns the flagged roles, plays
// both clips in lockstep (role A is the master clock, B is drift-corrected), broadcasts
// "anim.pair" bus events, and restores the previous clips when the pair ends or is stopped.
// A role's atom without an Animator simply skips its clip (doors, static props).
class NUKEENGINE_API PairedAnim
{
	NUKE_CLASS_NOCREATE(PairedAnim, Object)
public:
	// `pair` = content-relative .nupair path. The anchor is `b`'s transform.
	[[nuke::func]] static bool Start(const std::string& pair, Atom* a, Atom* b);
	// Explicit anchor atom (an interaction slot: its transform IS the entry pose).
	[[nuke::func]] static bool StartAt(const std::string& pair, Atom* a, Atom* b, Atom* anchor);
	[[nuke::func]] static void Stop(Atom* any);      // ends the session `any` participates in
	[[nuke::func]] static bool Active(Atom* any);
	[[nuke::func]] static double PairTime(Atom* any);   // master clip time; -1 = no session

	static void Tick(World* w);                          // World::Update, after the traversal
	static void Reload(const std::string& contentRel);   // drop the cached asset ("" = all)
};

}  // namespace nuke

#endif // !NUKEE_PAIREDANIM_H
