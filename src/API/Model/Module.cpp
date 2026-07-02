#include "API/Model/Module.h"
#include "interface/Modular.h"

namespace nuke {

static Module Snapshot(NUKEModule* m)
{
	Module out;
	out.title    = m->title;
	out.version  = m->version;
	out.author   = m->author;
	out.site     = m->site;
	out.file     = m->moduleFile;
	out.provides = m->provides();
	out.tags     = m->tags;
	out.loaded   = m->loaded;
	out.phase    = m->phase();
	return out;
}

std::vector<Module> Module::All()
{
	std::vector<Module> out;
	for (auto& m : GetModules())
		if (m) out.push_back(Snapshot(m.get()));
	return out;
}

bool Module::IsLoaded(const std::string& fileOrTitle)
{
	for (auto& m : GetModules())
		if (m && m->loaded && (m->moduleFile == fileOrTitle || fileOrTitle == m->title))
			return true;
	return false;
}

}  // namespace nuke
