#include "interface/Services.h"
#include <iostream>
#include <map>
#include <string>

namespace nuke {

// Single instance inside the engine DLL (like GetModules) — every host/plugin sees the
// same registry through the NUKEENGINE_API functions.
static std::map<std::string, void*>& Registry()
{
	static std::map<std::string, void*> s;
	return s;
}

void Services_Provide(const char* service, void* iface)
{
	if (!service || !*service || !iface) return;
	auto& reg = Registry();
	if (reg.count(service) && reg[service] != iface)
		std::cout << "[Services]\tWARNING: replacing live provider of '" << service
		          << "' (loader should have revoked it first)" << std::endl;
	reg[service] = iface;
	std::cout << "[Services]\t'" << service << "' provided" << std::endl;
}

void Services_Revoke(const char* service)
{
	if (!service || !*service) return;
	if (Registry().erase(service))
		std::cout << "[Services]\t'" << service << "' revoked" << std::endl;
}

void* Services_GetRaw(const char* service)
{
	if (!service || !*service) return nullptr;
	auto& reg = Registry();
	auto it = reg.find(service);
	return (it != reg.end()) ? it->second : nullptr;
}

}  // namespace nuke
