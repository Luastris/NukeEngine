// Header-only boost.chrono BEFORE any include that may pull boost (AppInstance.h does) —
// the lib flavor double-defines steady_clock::now inside the engine DLL.
#define BOOST_CHRONO_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#include "API/Model/Time.h"
#include "API/Model/Events.h"
#include "interface/AppInstance.h"
#include <boost/chrono.hpp>

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

void Time::NewFrame()
{
	using clock = boost::chrono::steady_clock;
	static clock::time_point last;
	static bool have = false;
	clock::time_point now = clock::now();
	if (have)
	{
		delta = boost::chrono::duration<double>(now - last).count();
		if (delta > 0.25) delta = 0.25;   // hitch/debugger clamp — no giant catch-up steps
		elapsed += delta;
	}
	last = now;
	have = true;
	// Game clock: scaled while PLAYING; equal to real time in edit mode so editor previews
	// (animators in asset editors, etc.) never freeze on the game-speed setting.
	const bool playing = AppInstance::GetSingleton()->playState == 1;
	gameDelta = playing ? delta * scale : delta;
}

Time::Time() {}

Time::~Time() {}

// Advance the calendar by gameDeltaSeconds of GAME-CLOCK time (already speed-scaled):
// game-world seconds = gameDeltaSeconds × gtr, ticked through the date chain whole-second
// at a time (at gtr=60 and 3x speed that's ~180 iterations/frame at 60fps — trivial).
void Time::Advance(double gameDeltaSeconds)
{
	if (gameDeltaSeconds <= 0.0) return;
	secCarry += gameDeltaSeconds * gtr;
	while (secCarry >= 1.0)
	{
		secCarry -= 1.0;
		Tick();
	}
	// Sub-second precision for schedulers/shaders: totalgt/tod include the fraction.
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

// One game second, frame-driven (Advance() calls this; no sleeping — the old blocking
// wall-clock ticker is gone, the calendar rides the frame loop now).
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
