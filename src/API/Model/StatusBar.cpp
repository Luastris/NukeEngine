#include "API/Model/StatusBar.h"
#include <boost/thread/mutex.hpp>

namespace nuke {

// First-set order preserved: the bar is stable, fields don't jump around.
namespace {
boost::mutex g_lock;
std::vector<std::pair<std::string, std::string>> g_fields;
}

void StatusBar::Set(const std::string& key, const std::string& text)
{
	boost::mutex::scoped_lock l(g_lock);
	for (auto& f : g_fields)
		if (f.first == key) { f.second = text; return; }
	g_fields.emplace_back(key, text);
}

void StatusBar::Remove(const std::string& key)
{
	boost::mutex::scoped_lock l(g_lock);
	for (auto it = g_fields.begin(); it != g_fields.end(); ++it)
		if (it->first == key) { g_fields.erase(it); return; }
}

std::vector<std::pair<std::string, std::string>> StatusBar::All()
{
	boost::mutex::scoped_lock l(g_lock);
	return g_fields;
}

}  // namespace nuke
