// Header-only boost.chrono BEFORE any boost include (project rule — the lib flavor
// double-defines steady_clock::now inside the engine DLL).
#define BOOST_CHRONO_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#include "API/Model/Profiler.h"

#include <boost/chrono.hpp>
#include <boost/thread/mutex.hpp>
#include <map>

namespace nuke {

namespace {
boost::mutex g_profMutex;   // reports come from the game, fixed and worker threads
// std::map keeps a stable alphabetical order for Phases().
std::map<std::string, double> g_phases;   // phase -> EMA milliseconds

double NowMs()
{
	using clock = boost::chrono::steady_clock;
	return boost::chrono::duration<double, boost::milli>(clock::now().time_since_epoch()).count();
}
}  // namespace

void Profiler::Report(const std::string& phase, double ms)
{
	if (phase.empty()) return;
	boost::mutex::scoped_lock lock(g_profMutex);
	double& v = g_phases[phase];
	v = (v == 0.0) ? ms : v * 0.9 + ms * 0.1;   // EMA: readable, not twitchy
}

double Profiler::Ms(const std::string& phase)
{
	boost::mutex::scoped_lock lock(g_profMutex);
	auto it = g_phases.find(phase);
	return it != g_phases.end() ? it->second : 0.0;
}

std::string Profiler::Phases()
{
	boost::mutex::scoped_lock lock(g_profMutex);
	std::string out;
	for (auto& kv : g_phases) { if (!out.empty()) out += "\n"; out += kv.first; }
	return out;
}

Profiler::Scope::Scope(const char* phase) : name(phase), t0(NowMs()) {}
Profiler::Scope::~Scope() { Report(name ? name : "", NowMs() - t0); }

}  // namespace nuke
