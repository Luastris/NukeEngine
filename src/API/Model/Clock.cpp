#include "API/Model/Clock.h"

// Header-only boost.chrono — same mode as Time.cpp (mixing the lib flavor in one DLL
// double-defines steady_clock::now and breaks the link).
#define BOOST_CHRONO_HEADER_ONLY
#include <boost/chrono.hpp>

namespace nuke {

namespace bch = boost::chrono;

double Clock::Now()
{
	// steady_clock: monotonic, unaffected by system clock changes.
	static const bch::steady_clock::time_point origin = bch::steady_clock::now();
	return bch::duration_cast<bch::duration<double>>(bch::steady_clock::now() - origin).count();
}

Clock::Clock() { Restart(); }

void Clock::Restart()
{
	startS = Now();
	pausedAtS = 0.0;
	pausedTotalS = 0.0;
	paused = false;
}

void Clock::Pause()
{
	if (paused) return;
	pausedAtS = Now();
	paused = true;
}

void Clock::Resume()
{
	if (!paused) return;
	pausedTotalS += Now() - pausedAtS;
	paused = false;
}

bool Clock::IsPaused() const { return paused; }

double Clock::Elapsed() const
{
	const double end = paused ? pausedAtS : Now();
	return end - startS - pausedTotalS;
}

}  // namespace nuke
