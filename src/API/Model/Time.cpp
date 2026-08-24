// Header-only boost.chrono BEFORE any include that pulls boost — the lib flavor
// double-defines steady_clock::now inside the engine DLL.
#define BOOST_CHRONO_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#include "API/Model/Time.h"
#include "API/Model/Events.h"
#include "interface/AppInstance.h"
#include "config.h"
#include <boost/chrono.hpp>
#include <boost/thread/thread.hpp>   // sleep_for: the J2 FPS-cap pacer
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>                 // timeBeginPeriod: 1ms sleep granularity for the pacer
#pragma comment(lib, "winmm.lib")
#endif

namespace nuke {

double Time::Elapsed()       { return getSingleton()->elapsed; }
double Time::Delta()         { return getSingleton()->gameDelta; }
double Time::UnscaledDelta() { return getSingleton()->delta; }

double Time::TotalGameSeconds() { return getSingleton()->totalgt; }
double Time::TimeOfDay()        { return getSingleton()->tod; }
int    Time::Second()           { return getSingleton()->sec; }
int    Time::Minute()           { return getSingleton()->minute; }
int    Time::Hour()             { return getSingleton()->hour; }
int    Time::Day()              { return getSingleton()->day; }
int    Time::Month()            { return getSingleton()->month; }
int    Time::Year()             { return getSingleton()->year; }
int    Time::DayOfYear()        { return getSingleton()->doy; }
int    Time::DayOfWeek()        { return getSingleton()->dow; }
double Time::GameToReal()       { return getSingleton()->gtr; }

void Time::SetGameToReal(double gameSecondsPerRealSecond)
{
	Time* t = getSingleton();
	t->gtr = gameSecondsPerRealSecond > 0.0 ? gameSecondsPerRealSecond : t->gtr;
}

void Time::SetDate(int year, int month, int day, int hour, int minute)
{
	Time* t = getSingleton();
	t->Init(t->gtr, day, month, year, hour, minute, 0);
	t->totalgt = 0.0;
	t->totalgd = 0;
	t->secCarry = 0.0;
}

// -1 = unresolved (config window.fpsLimit on first use), 0 = uncapped.
static double g_fpsCap = -1.0;

void Time::SetFpsCap(double fps) { g_fpsCap = fps > 0.0 ? fps : 0.0; }

double Time::FpsCap()
{
	if (g_fpsCap < 0.0)
	{
		const Config* cfg = Config::getSingleton();
		g_fpsCap = (cfg && cfg->window.fpsLimit > 0) ? (double)cfg->window.fpsLimit : 0.0;
		if (g_fpsCap > 0.0)
			std::cout << "[Time]\t\tfps cap " << (int)g_fpsCap << " (config window.fpsLimit)" << std::endl;
	}
	return g_fpsCap;
}

void Time::NewFrame()
{
	using clock = boost::chrono::steady_clock;
	static clock::time_point last;
	static bool have = false;
	// Frame cap: wait out the remainder of the frame period. Sleep to ~2ms short of the
	// deadline, then spin (raw OS sleep granularity would wobble the cadence).
	const double cap = FpsCap();
	if (have && cap > 0.0)
	{
#ifdef _WIN32
		static bool period = false;
		if (!period) { timeBeginPeriod(1); period = true; }
#endif
		const clock::time_point target = last
			+ boost::chrono::duration_cast<clock::duration>(boost::chrono::duration<double>(1.0 / cap));
		for (;;)
		{
			const clock::time_point t = clock::now();
			if (t >= target) break;
			const double rem = boost::chrono::duration<double>(target - t).count();
			if (rem > 0.0025)
				boost::this_thread::sleep_for(boost::chrono::milliseconds((long long)((rem - 0.002) * 1000.0)));
			// else: spin out the tail for a steady cadence
		}
	}
	clock::time_point now = clock::now();
	// Dev hook: NUKE_TIME_DEBUG=1 — print the measured frame rate every ~2s (frame-pacing probes).
	{
		static int dbg = -1;
		if (dbg < 0) { const char* e = std::getenv("NUKE_TIME_DEBUG"); dbg = (e && *e == '1') ? 1 : 0; }
		if (dbg)
		{
			static clock::time_point t0 = now;
			static int frames = 0;
			++frames;
			const double span = boost::chrono::duration<double>(now - t0).count();
			if (span >= 2.0)
			{
				std::cout << "[Time]\t\tfps ~" << (int)(frames / span + 0.5)
				          << " (cap " << (int)FpsCap() << ")" << std::endl;
				t0 = now; frames = 0;
			}
		}
	}
	if (have)
	{
		delta = boost::chrono::duration<double>(now - last).count();
		if (delta > 0.25) delta = 0.25;   // hitch/debugger clamp — no giant catch-up steps
		elapsed += delta;
	}
	last = now;
	have = true;
	++frame;   // tick-interval stagger
	// Game clock: scaled while playing, frozen while paused, real time in edit mode.
	const int ps = AppInstance::GetSingleton()->playState;   // 0 edit, 1 playing, 2 paused
	gameDelta = (ps == 1) ? delta * scale : (ps == 2 ? 0.0 : delta);
}

Time::Time() {}

Time::~Time() {}

// Advance the calendar by gameDeltaSeconds of already-scaled game-clock time
// (game-world seconds = gameDeltaSeconds × gtr, ticked one whole second at a time).
void Time::Advance(double gameDeltaSeconds)
{
	if (gameDeltaSeconds <= 0.0) return;
	secCarry += gameDeltaSeconds * gtr;
	while (secCarry >= 1.0)
	{
		secCarry -= 1.0;
		Tick();
	}
	// Sub-second precision: tod includes the carry fraction.
	tod = (sec + minute * 60 + hour * 3600 + secCarry) / 86400.0;
}

void Time::TickMonth()
{
	if (month == 12)
	{
		year++;
		month = 1;
		woy = 1;
	}
	else
		month++;
	Events::EmitEngine("time.newMonth", "");
}

void Time::TickDay()
{
	if ((doy == 366 && year % 4 == 0) || (doy == 365 && year % 4))
	{
		doy = 0;
	}
	if (dow == 7)
	{
		dow = 0;
		woy++;
	}
	if (
		(day == 31 && (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12))
		||
		(day == 30 && (month == 4 || month == 6 || month == 9 || month == 11))
		||
		(day == 28 && month == 2 && year % 4)
		||
		(day == 29 && month == 2 && year % 4 == 0))
	{
		day = 0;
		TickMonth();
	}
	day++;
	dow++;
	doy++;
	totalgd++;
	Events::EmitEngine("time.newDay", "");
}

void Time::TickHour()
{
	hour++;
	if (hour == 24)
	{
		TickDay();
		hour = 0;
	}
	Events::EmitEngine("time.newHour", "");
}

void Time::TickMinute()
{
	minute++;
	if (minute == 60)
	{
		TickHour();
		minute = 0;
	}
}

void Time::TickSecond()
{
	sec++;
	if (sec == 60)
	{
		TickMinute();
		sec = 0;
	}
	tod = (sec + minute * 60 + hour * 3600) / 86400.0;
}

// One game second, frame-driven (called by Advance).
void Time::Tick()
{
	totalgt += 1.0;
	TickSecond();
}

int Time::CalcDayOfYear(int day, int month, int year)
{
	int dayofy = 0;
	for (int i = 1; i < month; i++)
	{
		switch (i)
		{
		case 1:
		case 3:
		case 5:
		case 7:
		case 8:
		case 10:
			dayofy += 31;
			break;
		case 4:
		case 6:
		case 9:
		case 11:
			dayofy += 30;
			break;
		case 2:
			if (year % 4 == 0)
				dayofy += 29;
			else
				dayofy += 28;
			break;
		default:
			break;
		}
	}
	dayofy += day;
	return dayofy;
}

int Time::CalcWeekOfYear(int dayofyear)
{
	return (dayofyear / 7) + 1;
}

void Time::Init()
{
	gtr = 60.0;
	day = 1;
	year = 2000;
	month = 1;
	woy = 1;
}

void Time::Init(double gtr, int day, int month, int year)
{
	this->gtr= gtr;
	this->day = day;
	this->year = year;
	this->month = month;
	doy = CalcDayOfYear(day, month, year);
	woy = CalcWeekOfYear(doy);
	dow = ((doy - 1) % 7) + 1;
}

void Time::Init(double gtr, int day, int month, int year, int hour)
{
	this->Init(gtr, day, month, year);
	this->hour = hour;
}

void Time::Init(double gtr, int day, int month, int year, int hour, int minute)
{
	this->Init(gtr, day, month, year, hour);
	this->minute = minute;
}

void Time::Init(double gtr, int day, int month, int year, int hour, int minute, int sec)
{
	this->Init(gtr, day, month, year, hour, minute);
	this->sec = sec;
	tod = (sec + minute * 60 + hour * 3600) / 86400.0;
}
}  // namespace nuke
