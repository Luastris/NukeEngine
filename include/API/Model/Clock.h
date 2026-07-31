#pragma once
#ifndef NUKEE_CLOCK_H
#define NUKEE_CLOCK_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

// A user-owned, pausable stopwatch for gameplay timing, complementing the per-frame Time
// singleton. Runs on the MONOTONIC clock (immune to wall-clock changes); all values in SECONDS.
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

	// Monotonic seconds since process start — the timeline every Clock measures on.
	[[nuke::func]] static double Now();

private:
	double startS       = 0.0;     // Now() at (re)start
	double pausedAtS    = 0.0;     // Now() when Pause() hit (valid while paused)
	double pausedTotalS = 0.0;     // accumulated paused duration since (re)start
	bool   paused       = false;
};

}  // namespace nuke

#endif // !NUKEE_CLOCK_H
