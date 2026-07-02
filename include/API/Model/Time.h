#pragma once
#ifndef NUKEE_TIME_H
#define NUKEE_TIME_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

class NUKEENGINE_API Time
{
	NUKE_CLASS_NOCREATE(Time, Object)
private:
	Time();
	~Time();
public:
	// Reflected script surface (auto-bound as nuke.Time.* by the generic static binder).
	[[nuke::func]] static double Elapsed();   // real seconds since the first frame
	[[nuke::func]] static double Delta();     // real seconds since the previous frame

	static Time * getSingleton() 
	{
		static Time instance;
		return &instance;
	}
	
	// Real (wall-clock) frame timing — updated once per frame by NewFrame().
	double delta = 0.0;     // seconds since the previous frame
	double elapsed = 0.0;   // total real seconds since the first NewFrame()
	void NewFrame();        // call once per rendered frame (from the host loop)

	//Game to real time attitude
	double gtr = 0.05;

	//Time of day
	double tod = 0;

	//Year
	int year = 2000;

	//Day of year
	int doy = 1;

	//Week of year
	int woy = 1;

	//Month of year
	int month = 1;

	//Day of month
	int day = 1;

	//Day of week
	int dow = 1;

	//Total game time
	long double totalgt = 0;
	
	//Total game days
	long long unsigned int totalgd = 0;

    int hour;
    int minute;
    int sec;

	void TickMonth();

	void TickDay();

	void TickHour();

	void TickMinute();

	void TickSecond();

	//Ticker
	void Tick();

	static int CalcDayOfYear(int day, int month, int year);

	static int CalcWeekOfYear(int dayofyear);

	void Init();

	void Init(double gtr, int day, int month, int year);
	
	void Init(double gtr, int day, int month, int year, int hour);
	
	void Init(double gtr, int day, int month, int year, int hour, int minute);

	void Init(double gtr, int day, int month, int year, int hour, int minute, int sec);

	void Run();
};

}  // namespace nuke

#endif // !NUKEE_TIME_H
