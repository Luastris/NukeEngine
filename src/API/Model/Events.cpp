// Header-only boost.chrono BEFORE any boost include (project rule — the lib flavor
// double-defines steady_clock::now inside the engine DLL; boost/thread pulls chrono).
#define BOOST_CHRONO_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#include "API/Model/Events.h"
#include "API/Model/Time.h"
#include <nlohmann/json.hpp>

#include <boost/thread/mutex.hpp>
#include <vector>
#include <map>

using json = nlohmann::json;

namespace nuke {

namespace {

struct QueuedEvent { std::string name, payload; };
struct Scheduled
{
	double      id = 0;
	double      due = 0;      // Time.TotalGameSeconds when it fires
	double      period = 0;   // 0 = one-shot
	std::string name, payload;
};
struct NativeSub { long long id; std::string name; Events::Handler fn; };

boost::mutex             g_evMutex;      // guards the queue (emits can come from the fixed thread)
std::vector<QueuedEvent> g_queue;
std::vector<Scheduled>   g_schedule;     // mutated only under the game lock (Pump/After/Every)
std::vector<NativeSub>   g_subs;
double                   g_nextId = 1;
long long                g_nextSubId = 1;

}  // namespace

void Events::Emit(const std::string& name, const std::string& payload)
{
	if (name.empty()) return;
	boost::mutex::scoped_lock lock(g_evMutex);
	g_queue.push_back({ name, payload });
}

void Events::EmitEngine(const std::string& name, const std::string& payload) { Emit(name, payload); }

double Events::After(double gameSeconds, const std::string& name, const std::string& payload)
{
	if (name.empty() || gameSeconds < 0.0) return 0;
	Scheduled s; s.id = g_nextId++; s.due = Time::TotalGameSeconds() + gameSeconds;
	s.period = 0; s.name = name; s.payload = payload;
	g_schedule.push_back(s);
	return s.id;
}

double Events::Every(double gameSeconds, const std::string& name, const std::string& payload)
{
	if (name.empty() || gameSeconds <= 0.0) return 0;
	Scheduled s; s.id = g_nextId++; s.due = Time::TotalGameSeconds() + gameSeconds;
	s.period = gameSeconds; s.name = name; s.payload = payload;
	g_schedule.push_back(s);
	return s.id;
}

void Events::Cancel(double id)
{
	for (size_t i = 0; i < g_schedule.size(); ++i)
		if (g_schedule[i].id == id) { g_schedule.erase(g_schedule.begin() + i); return; }
}

int Events::PendingCount() { return (int)g_schedule.size(); }

long long Events::Subscribe(const std::string& name, Handler fn)
{
	if (!fn) return 0;
	g_subs.push_back({ g_nextSubId, name, std::move(fn) });
	return g_nextSubId++;
}

void Events::Unsubscribe(long long id)
{
	for (size_t i = 0; i < g_subs.size(); ++i)
		if (g_subs[i].id == id) { g_subs.erase(g_subs.begin() + i); return; }
}

void Events::Pump(const std::function<void(const std::string&, const std::string&)>& dispatch)
{
	// 1) Due scheduled entries -> the queue (game time; frozen at pause so nothing fires).
	const double now = Time::TotalGameSeconds();
	for (size_t i = 0; i < g_schedule.size(); )
	{
		Scheduled& s = g_schedule[i];
		if (s.due <= now)
		{
			Emit(s.name, s.payload);
			if (s.period > 0.0)
			{
				s.due += s.period;
				if (s.due <= now) s.due = now + s.period;   // long pause/hitch: no burst catch-up
				++i;
			}
			else
				g_schedule.erase(g_schedule.begin() + i);
		}
		else ++i;
	}

	// 2) Drain the queue. Snapshot first: handlers may Emit (delivered NEXT pump — no
	//    infinite same-frame cascades), and emits may arrive from other threads meanwhile.
	std::vector<QueuedEvent> batch;
	{
		boost::mutex::scoped_lock lock(g_evMutex);
		batch.swap(g_queue);
	}
	for (const QueuedEvent& e : batch)
	{
		for (const NativeSub& sub : g_subs)
			if (sub.name.empty() || sub.name == e.name)
				sub.fn(e.name, e.payload);
		if (dispatch) dispatch(e.name, e.payload);
	}
}

std::string Events::SaveJson()
{
	json j = json::array();
	for (const Scheduled& s : g_schedule)
		j.push_back({ {"id", s.id}, {"due", s.due}, {"period", s.period},
		              {"name", s.name}, {"payload", s.payload} });
	json root; root["nextId"] = g_nextId; root["entries"] = j;
	return root.dump();
}

void Events::LoadJson(const std::string& js)
{
	g_schedule.clear();
	json root = json::parse(js, nullptr, false);
	if (root.is_discarded() || !root.is_object()) return;
	g_nextId = root.value("nextId", 1.0);
	if (root.contains("entries") && root["entries"].is_array())
		for (const json& e : root["entries"])
		{
			Scheduled s;
			s.id      = e.value("id", 0.0);
			s.due     = e.value("due", 0.0);
			s.period  = e.value("period", 0.0);
			s.name    = e.value("name", std::string());
			s.payload = e.value("payload", std::string());
			if (!s.name.empty()) g_schedule.push_back(s);
		}
}

void Events::ResetSchedule()
{
	g_schedule.clear();
	boost::mutex::scoped_lock lock(g_evMutex);
	g_queue.clear();
}

}  // namespace nuke
