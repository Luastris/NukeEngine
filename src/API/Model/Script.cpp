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

bool Script::Run(const std::string& code, const std::string& chunkName)
{
	iScript* s = GetService<iScript>();
	return s && s->Run(code.c_str(), chunkName.c_str());
}

}  // namespace nuke
