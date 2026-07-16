#pragma once
#ifndef NUKEE_TIME_H
#define NUKEE_TIME_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

// Frame timing + the GAME CALENDAR (colony-sim base, Phase 6.1).
//
// Two clocks live here:
//  - the REAL clock: wall-clock frame delta/elapsed, driven by NewFrame() once per frame.
//  - the GAME clock: real delta × Game.TimeScale (0 = frozen, 2/3 = fast-forward). In edit
//    mode (not playing) the game clock equals the real clock so editor previews never freeze.
// The CALENDAR advances on the game clock scaled by `gtr` (game seconds per real second at
// 1x speed — one in-game day takes 86400/gtr real seconds). It is frame-driven (advanced by
// World::Update in play mode) and SAVES WITH THE WORLD (World::SaveToString captures it,
// LoadFromString restores — a savegame resumes at the exact in-game moment).
class NUKEENGINE_API Time
{
	NUKE_CLASS_NOCREATE(Time, Object)
private:
	Time();
	~Time();
public:
	// Reflected script surface (auto-bound as nuke.Time.* / C# Time.*).
	[[nuke::func]] static double Elapsed();        // real seconds since the first frame
	// GAME frame delta: real delta × time scale (Game.SetTimeScale). What gameplay reads —
	// at 0 the world is frozen, at 3 everything moves 3× per real second.
	[[nuke::func]] static double Delta();
	[[nuke::func]] static double UnscaledDelta();  // real seconds since the previous frame (UI/editor)

	// --- game calendar (reflected getters; state saves with the world) ---
	[[nuke::func]] static double TotalGameSeconds();  // game seconds since the calendar start
	[[nuke::func]] static double TimeOfDay();         // 0..1 across the current day
	[[nuke::func]] static int    Second();
	[[nuke::func]] static int    Minute();
	[[nuke::func]] static int    Hour();
	[[nuke::func]] static int    Day();               // day of month (1-based)
	[[nuke::func]] static int    Month();             // 1..12
	[[nuke::func]] static int    Year();
	[[nuke::func]] static int    DayOfYear();
	[[nuke::func]] static int    DayOfWeek();         // 1..7
	// Calendar speed: game seconds per real second at 1x (an in-game day = 86400/gtr real
	// seconds). Per-world (serialized); changing it mid-game is legal.
	[[nuke::func]] static double GameToReal();
	[[nuke::func]] static void   SetGameToReal(double gameSecondsPerRealSecond);
	// Set the calendar date/time (mapgen/scenario start). Resets TotalGameSeconds to 0.
	[[nuke::func]] static void   SetDate(int year, int month, int day, int hour, int minute);

	static Time * getSingleton()
	{
		static Time instance;
		return &instance;
	}

	// Real (wall-clock) frame timing — updated once per frame by NewFrame().
	double delta = 0.0;      // REAL seconds since the previous frame (internal consumers: UI, smoothing)
	double elapsed = 0.0;    // total real seconds since the first NewFrame()
	// Game-clock frame delta: delta × scale while playing, == delta in edit mode (previews
	// keep animating). Gameplay-side systems (Animator in play, calendar) consume this.
	double gameDelta = 0.0;
	double scale = 1.0;      // Game.SetTimeScale: 0 frozen .. fast-forward (clamped in Game)
	unsigned long long frame = 0;   // frame counter (tick-interval stagger, 6.8)
	void NewFrame();         // call once per rendered frame (from the host loop)

	// Game-to-real ratio: game seconds advanced per (scaled) real second.
	double gtr = 60.0;

	// Time of day, 0..1 across the current day.
	double tod = 0;

	int year = 2000;
	int doy = 1;    // day of year
	int woy = 1;    // week of year
	int month = 1;  // 1..12
	int day = 1;    // day of month
	int dow = 1;    // day of week, 1..7

	// Total game seconds since the calendar start (what schedulers compare against).
	double totalgt = 0;
	// Total whole game days elapsed.
	long long unsigned int totalgd = 0;

	int hour = 8;
	int minute = 0;
	int sec = 0;

	// Advance the calendar by a GAME-CLOCK delta (seconds of scaled real time): adds
	// gameDeltaSeconds × gtr game seconds, ticking the date chain per whole second.
	// Called by World::Update in play mode. Emits time events (6.3) on hour/day rollover.
	void Advance(double gameDeltaSeconds);

	void TickMonth();
	void TickDay();
	void TickHour();
	void TickMinute();
	void TickSecond();

	// Advance exactly one game second (frame-driven — no sleeping; use Advance for frames).
	void Tick();

	static int CalcDayOfYear(int day, int month, int year);
	static int CalcWeekOfYear(int dayofyear);

	void Init();
	void Init(double gtr, int day, int month, int year);
	void Init(double gtr, int day, int month, int year, int hour);
	void Init(double gtr, int day, int month, int year, int hour, int minute);
	void Init(double gtr, int day, int month, int year, int hour, int minute, int sec);

private:
	double secCarry = 0.0;   // fractional game-second accumulator for Advance()
};

}  // namespace nuke

#endif // !NUKEE_TIME_H
