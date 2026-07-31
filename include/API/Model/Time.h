#pragma once
#ifndef NUKEE_TIME_H
#define NUKEE_TIME_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

// Frame timing + the GAME CALENDAR. Two clocks: the REAL clock (wall-clock delta/elapsed, driven
// by NewFrame()) and the GAME clock (real delta × Game.TimeScale; equal to the real clock in edit
// mode so previews never freeze). The calendar advances on the game clock scaled by `gtr` and
// SAVES WITH THE WORLD.
class NUKEENGINE_API Time
{
	NUKE_CLASS_NOCREATE(Time, Object)
private:
	Time();
	~Time();
public:
	[[nuke::func]] static double Elapsed();        // real seconds since the first frame
	// GAME frame delta: real delta × time scale (Game.SetTimeScale) — what gameplay reads.
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
	// seconds). Per-world (serialized); may change mid-game.
	[[nuke::func]] static double GameToReal();
	[[nuke::func]] static void   SetGameToReal(double gameSecondsPerRealSecond);
	// Set the calendar date/time (mapgen/scenario start). Resets TotalGameSeconds to 0.
	[[nuke::func]] static void   SetDate(int year, int month, int day, int hour, int minute);

	static Time * getSingleton()
	{
		static Time instance;
		return &instance;
	}

	double delta = 0.0;      // REAL seconds since the previous frame
	double elapsed = 0.0;    // total real seconds since the first NewFrame()
	double gameDelta = 0.0;  // delta × scale while playing, == delta in edit mode
	double scale = 1.0;      // Game.SetTimeScale: 0 frozen .. fast-forward (clamped in Game)
	unsigned long long frame = 0;   // frame counter
	void NewFrame();         // call once per rendered frame (from the host loop)

	double gtr = 60.0;       // game seconds advanced per (scaled) real second

	double tod = 0;          // time of day, 0..1 across the current day

	int year = 2000;
	int doy = 1;    // day of year
	int woy = 1;    // week of year
	int month = 1;  // 1..12
	int day = 1;    // day of month
	int dow = 1;    // day of week, 1..7

	double totalgt = 0;                 // total game seconds since the calendar start
	long long unsigned int totalgd = 0; // total whole game days elapsed

	int hour = 8;
	int minute = 0;
	int sec = 0;

	// Advance the calendar by a GAME-CLOCK delta: adds gameDeltaSeconds × gtr game seconds,
	// ticking the date chain per whole second and emitting time events on hour/day rollover.
	void Advance(double gameDeltaSeconds);

	void TickMonth();
	void TickDay();
	void TickHour();
	void TickMinute();
	void TickSecond();

	// Advance exactly one game second (use Advance for frame-driven stepping).
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
