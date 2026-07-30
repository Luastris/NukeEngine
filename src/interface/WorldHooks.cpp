#include "interface/WorldHooks.h"
#include <algorithm>

namespace nuke {

static std::vector<WorldRenderHook*>& Hooks() { static std::vector<WorldRenderHook*> v; return v; }

void RegisterWorldRenderHook(WorldRenderHook* h)
{
	if (!h) return;
	auto& v = Hooks();
	if (std::find(v.begin(), v.end(), h) == v.end()) v.push_back(h);
}

void UnregisterWorldRenderHook(WorldRenderHook* h)
{
	auto& v = Hooks();
	v.erase(std::remove(v.begin(), v.end(), h), v.end());
}

const std::vector<WorldRenderHook*>& WorldRenderHooks() { return Hooks(); }

}  // namespace nuke
