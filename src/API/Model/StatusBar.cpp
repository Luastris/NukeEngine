#include "API/Model/StatusBar.h"
#include <boost/thread/mutex.hpp>

namespace nuke {

// First-set order preserved: the bar is stable, fields don't jump around.
namespace {
boost::mutex g_lock;
std::vector<StatusBar::Entry> g_fields;

void SetImpl(const std::string& key, const std::string& text, float progress)
{
	boost::mutex::scoped_lock l(g_lock);
	for (auto& f : g_fields)
		if (f.key == key) { f.text = text; f.progress = progress; return; }
	g_fields.push_back({ key, text, progress });
}
}

void StatusBar::Set(const std::string& key, const std::string& text)
{
	SetImpl(key, text, kNoProgress);
}

void StatusBar::Set(const std::string& key, const std::string& text, float progress)
{
	if (progress > 1.0f) progress = 1.0f;
	if (progress < 0.0f && progress != kIndeterminate) progress = kNoProgress;
	SetImpl(key, text, progress);
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
	return g_fields;
}

}  // namespace nuke
