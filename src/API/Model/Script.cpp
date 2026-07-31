#include "API/Model/Script.h"
#include "interface/Services.h"
#include "service/iScript.h"

namespace nuke {

bool Script::Available() { return GetService<iScript>() != nullptr; }

std::string Script::Language()
{
	iScript* s = GetService<iScript>();
	return s ? s->Language() : "";
}

std::vector<std::string> Script::Languages()
{
	std::vector<std::string> out;
	for (iScript* s : GetServices<iScript>())
		if (s) out.push_back(s->Language());
	return out;
}

bool Script::Run(const std::string& code, const std::string& chunkName, const std::string& language)
{
	// "" = the first provider; otherwise route to the backend whose Language() matches.
	if (language.empty())
	{
		iScript* s = GetService<iScript>();
		return s && s->Run(code.c_str(), chunkName.c_str());
	}
	for (iScript* s : GetServices<iScript>())
		if (s && language == s->Language())
			return s->Run(code.c_str(), chunkName.c_str());
	return false;
}

}  // namespace nuke
