#include "interface/Services.h"
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace nuke {

// Single instance inside the engine DLL (like GetModules) — every host/plugin sees the
// same registry through the NUKEENGINE_API functions. A service maps to a LIST of live
// providers: exclusive services hold at most one (the loader displaces), shared services
// (scripting) may hold several. Registration order is preserved — GetService() = first.
static std::map<std::string, std::vector<void*>>& Registry()
{
	static std::map<std::string, std::vector<void*>> s;
	return s;
}

void Services_Provide(const char* service, void* iface)
{
	if (!service || !*service || !iface) return;
	auto& lst = Registry()[service];
	if (std::find(lst.begin(), lst.end(), iface) != lst.end()) return;   // already registered
	lst.push_back(iface);
	std::cout << "[Services]\t'" << service << "' provided"
	          << (lst.size() > 1 ? (" (" + std::to_string(lst.size()) + " providers live)") : std::string())
	          << std::endl;
}

void Services_Revoke(const char* service)
{
	if (!service || !*service) return;
	if (Registry().erase(service))
		std::cout << "[Services]\t'" << service << "' revoked (all providers)" << std::endl;
}

void Services_RevokeIface(const char* service, void* iface)
{
	if (!service || !*service || !iface) return;
	auto& reg = Registry();
	auto it = reg.find(service);
	if (it == reg.end()) return;
	auto& lst = it->second;
	auto pos = std::find(lst.begin(), lst.end(), iface);
	if (pos == lst.end()) return;
	lst.erase(pos);
	if (lst.empty()) reg.erase(it);
	std::cout << "[Services]\t'" << service << "' provider revoked" << std::endl;
}

void* Services_GetRaw(const char* service)
{
	if (!service || !*service) return nullptr;
	auto& reg = Registry();
	auto it = reg.find(service);
	return (it != reg.end() && !it->second.empty()) ? it->second.front() : nullptr;
}

std::vector<void*> Services_GetAllRaw(const char* service)
{
	if (!service || !*service) return {};
	auto& reg = Registry();
	auto it = reg.find(service);
	return it != reg.end() ? it->second : std::vector<void*>();
}

}  // namespace nuke
