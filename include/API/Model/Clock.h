#pragma once
#ifndef NUKEE_CLOCK_H
#define NUKEE_CLOCK_H
#include "NukeAPI.h"

namespace nuke {

// A user-owned stopwatch for gameplay timing. Complements Time (the per-frame singleton
// driven by the host loop): a Clock is instantiated freely, runs on the MONOTONIC clock
// (immune to wall-clock changes), can be paused, and is cheap enough to keep per-object.
// All values are SECONDS.
class NUKEENGINE_API Clock
{
public:
	Clock();                       // constructed running, at 0

	void   Restart();              // back to 0, running
	void   Pause();                // freeze Elapsed() (no-op while paused)
	void   Resume();               // continue after Pause (no-op while running)
	bool   IsPaused() const;
	double Elapsed() const;        // seconds measured while running (pauses excluded)

	// Monotonic seconds since an arbitrary fixed origin (process start) — the reference
	// timeline every Clock measures on. For frame time use Time (delta/elapsed).
	static double Now();

private:
	double startS       = 0.0;     // Now() at (re)start
	double pausedAtS    = 0.0;     // Now() when Pause() hit (valid while paused)
	double pausedTotalS = 0.0;     // accumulated paused duration since (re)start
	bool   paused       = false;
};

}  // namespace nuke

#endif // !NUKEE_CLOCK_H
