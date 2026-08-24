#include "API/Model/Cursor.h"
#include "API/Model/Time.h"
#include "interface/AppInstance.h"
#include "render/irender.h"
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <vector>

namespace nuke {

namespace {

struct CursorFrame
{
	std::vector<unsigned char> rgba;
	int w = 0, h = 0;
	int hotX = 0, hotY = 0;
};

struct CursorSet
{
	std::vector<CursorFrame> frames;
	double fps = 12.0;
	bool loop = true;
	uint64_t baseId = 0;   // renderer cache key of frame 0 (frame i = baseId + i)
};

std::map<std::string, CursorSet> g_states;
std::string g_active = "default";
bool     g_software = false;
double   g_animT = 0.0;          // seconds into the active set's flipbook
uint64_t g_nextBase = 1;         // renderer ids: 0 is reserved for "restore the arrow"
// What the renderer currently shows — pushes happen only on change.
uint64_t g_shownId = ~0ull;
int      g_shownMode = -1;

// The set the active state resolves to ("default" fallback), null = OS arrow.
CursorSet* Resolve()
{
	auto it = g_states.find(g_active);
	if (it == g_states.end() || it->second.frames.empty()) it = g_states.find("default");
	if (it == g_states.end() || it->second.frames.empty()) return nullptr;
	return &it->second;
}

}  // namespace

bool Cursor::Bind(const std::string& state, const std::string& contentRel)
{
	const std::string key = state.empty() ? "default" : state;
	if (contentRel.empty()) { g_states.erase(key); g_shownId = ~0ull; return true; }

	AppInstance* app = AppInstance::GetSingleton();
	std::string text;
	if (!app || !app->ReadContent(contentRel, text) || text.empty())
	{
		std::cout << "[Cursor]\t\tcan't read '" << contentRel << "'" << std::endl;
		return false;
	}
	nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object() || !j.contains("frames") || !j["frames"].is_array()
	    || j["frames"].empty())
	{
		std::cout << "[Cursor]\t\t'" << contentRel << "' is not a cursor asset (frames[] missing)" << std::endl;
		return false;
	}
	CursorSet set;
	set.fps  = j.value("fps", 12.0);
	set.loop = j.value("loop", true);
	for (const nlohmann::json& fj : j["frames"])
	{
		if (!fj.is_object()) continue;
		const std::string img = fj.value("image", std::string());
		std::string bytes;
		if (img.empty() || !app->ReadContent(img, bytes) || bytes.empty())
		{
			std::cout << "[Cursor]\t\tframe image missing: '" << img << "' (" << contentRel << ")" << std::endl;
			return false;
		}
		int w = 0, h = 0, n = 0;
		unsigned char* px = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(),
		                                          &w, &h, &n, 4);
		if (!px || w <= 0 || h <= 0)
		{
			if (px) stbi_image_free(px);
			std::cout << "[Cursor]\t\tframe image unreadable: '" << img << "' (" << contentRel << ")" << std::endl;
			return false;
		}
		CursorFrame f;
		f.w = w; f.h = h;
		f.hotX = fj.value("hotX", 0);
		f.hotY = fj.value("hotY", 0);
		f.rgba.assign(px, px + (size_t)w * h * 4);
		stbi_image_free(px);
		set.frames.push_back(std::move(f));
	}
	if (set.frames.empty())
	{
		std::cout << "[Cursor]\t\t'" << contentRel << "' has no usable frames" << std::endl;
		return false;
	}
	set.baseId = g_nextBase;
	g_nextBase += set.frames.size();
	g_states[key] = std::move(set);
	g_shownId = ~0ull;   // force a re-push
	std::cout << "[Cursor]\t\t'" << key << "' <- " << contentRel << " ("
	          << g_states[key].frames.size() << " frame(s), " << g_states[key].fps << " fps)" << std::endl;
	return true;
}

void Cursor::SetState(const std::string& state)
{
	const std::string key = state.empty() ? "default" : state;
	if (key == g_active) return;
	g_active = key;
	g_animT = 0.0;
	g_shownId = ~0ull;
}

const char* Cursor::State() { return g_active.c_str(); }

void Cursor::SetSoftware(bool on)
{
	if (g_software == on) return;
	g_software = on;
	g_shownId = ~0ull;
}

bool Cursor::Software() { return g_software; }

void Cursor::Reset()
{
	g_states.clear();
	g_active = "default";
	g_animT = 0.0;
	g_shownId = ~0ull;
}

void Cursor::Tick(iRender* r)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (!r || !app) return;
	if (app->isEditor()) return;   // a game cursor must not restyle the editor window
	if (app->playState != 1) return;

	CursorSet* set = Resolve();
	if (!set)
	{
		if (g_shownMode != 0)
		{
			r->setCursorImage(0, nullptr, 0, 0, 0, 0, 0);
			g_shownMode = 0; g_shownId = 0;
		}
		return;
	}
	g_animT += Time::getSingleton()->delta;   // real clock: the cursor ignores time scale
	size_t idx = 0;
	if (set->frames.size() > 1 && set->fps > 0.0)
	{
		const double f = g_animT * set->fps;
		idx = set->loop ? (size_t)f % set->frames.size()
		                : std::min((size_t)f, set->frames.size() - 1);
	}
	const uint64_t id = set->baseId + (uint64_t)idx;
	const int mode = g_software ? 2 : 1;
	if (id == g_shownId && mode == g_shownMode) return;
	const CursorFrame& fr = set->frames[idx];
	if (r->setCursorImage(id, fr.rgba.data(), fr.w, fr.h, fr.hotX, fr.hotY, mode))
	{
		g_shownId = id; g_shownMode = mode;
	}
	else if (mode == 1)
	{
		std::cout << "[Cursor]\t\thardware cursor refused — falling back to the software path" << std::endl;
		g_software = true;
	}
}

}  // namespace nuke
