#pragma once
#ifndef NUKEE_CLOCK_H
#define NUKEE_CLOCK_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

// A user-owned stopwatch for gameplay timing. Complements Time (the per-frame singleton
// driven by the host loop): a Clock is instantiated freely, runs on the MONOTONIC clock
// (immune to wall-clock changes), can be paused, and is cheap enough to keep per-object.
// All values are SECONDS. Reflected: scripts create their own (Clock.Create() / nuke.Clock.Create()).
class NUKEENGINE_API Clock
{
	NUKE_CLASS(Clock, Object)
public:
	Clock();                       // constructed running, at 0

	[[nuke::func]] void   Restart();   // back to 0, running
	[[nuke::func]] void   Pause();     // freeze Elapsed() (no-op while paused)
	[[nuke::func]] void   Resume();    // continue after Pause (no-op while running)
	[[nuke::func]] bool   IsPaused() const;
	[[nuke::func]] double Elapsed() const;   // seconds measured while running (pauses excluded)

	// Monotonic seconds since an arbitrary fixed origin (process start) — the reference
	// timeline every Clock measures on. For frame time use Time (delta/elapsed).
	[[nuke::func]] static double Now();

private:
	double startS       = 0.0;     // Now() at (re)start
	double pausedAtS    = 0.0;     // Now() when Pause() hit (valid while paused)
	double pausedTotalS = 0.0;     // accumulated paused duration since (re)start
	bool   paused       = false;
};

}  // namespace nuke

#endif // !NUKEE_CLOCK_H
