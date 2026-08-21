#include "API/Model/StatusBar.h"
#include "API/Model/Log.h"
#include <boost/thread/mutex.hpp>

namespace nuke {

// First-set order preserved: the bar is stable, fields don't jump around.
namespace {
boost::mutex g_lock;
std::vector<StatusBar::Entry> g_fields;
uint64_t g_seq = 0;

void SetImpl(const std::string& key, const std::string& text, float progress, int priority, double ttl)
{
	boost::mutex::scoped_lock l(g_lock);
	const double expires = ttl > 0.0 ? Log::Uptime() + ttl : 0.0;
	for (auto& f : g_fields)
		if (f.key == key)
		{
			f.text = text; f.progress = progress; f.priority = priority; f.seq = ++g_seq; f.expiresAt = expires;
			return;
		}
	StatusBar::Entry e;
	e.key = key; e.text = text; e.progress = progress; e.priority = priority; e.seq = ++g_seq; e.expiresAt = expires;
	g_fields.push_back(e);
}
}

void StatusBar::Set(const std::string& key, const std::string& text)
{
	SetImpl(key, text, kNoProgress, 0, 10.0);
}

void StatusBar::Set(const std::string& key, const std::string& text, float progress)
{
	if (progress > 1.0f) progress = 1.0f;
	if (progress < 0.0f && progress != kIndeterminate) progress = kNoProgress;
	SetImpl(key, text, progress, 0, 0.0);
}

void StatusBar::Message(const std::string& key, const std::string& text, int priority, double ttlSeconds)
{
	SetImpl(key, text, kNoProgress, priority, ttlSeconds);
}

void StatusBar::Remove(const std::string& key)
{
	boost::mutex::scoped_lock l(g_lock);
	for (auto it = g_fields.begin(); it != g_fields.end(); ++it)
		if (it->key == key) { g_fields.erase(it); return; }
}

std::vector<StatusBar::Entry> StatusBar::All()
{
	boost::mutex::scoped_lock l(g_lock);
	const double now = Log::Uptime();
	for (auto it = g_fields.begin(); it != g_fields.end();)
		if (it->expiresAt > 0.0 && it->expiresAt < now) it = g_fields.erase(it); else ++it;
	return g_fields;
}

}  // namespace nuke
