// Console variables: the registry, the archive in config/main.json and the engine's bound knobs.
#include "API/Model/Cvar.h"
#include "API/Model/DevConsole.h"
#include "API/Model/Events.h"
#include "API/Model/Game.h"
#include "API/Model/Loc.h"
#include "API/Model/Log.h"
#include "API/Model/Time.h"
#include "interface/AppInstance.h"
#include "config.h"
#include <boost/thread/mutex.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

namespace nuke {

namespace {
struct Entry
{
	CvarType type = CvarType::String;
	unsigned flags = 0;
	std::string value, def, description;
	std::function<std::string()> get;               // bound: the live value
	std::function<bool(const std::string&)> set;    // bound: apply (null = read-only)
	bool bound = false;
};
struct Hook { long long id; std::string name; std::function<void(const std::string&, const std::string&)> fn; };

boost::mutex g_mx;   // the table; callbacks and engine setters run OUTSIDE it
std::map<std::string, Entry>& Table() { static std::map<std::string, Entry> t; return t; }
std::vector<Hook>& Hooks() { static std::vector<Hook> h; return h; }
long long g_nextHook = 1;
bool g_defaults = false;

std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string FormatNumber(double v)
{
	std::ostringstream o; o.precision(9); o << v;
	return o.str();
}

void RegisterEngineDefaults();

// The registry with the engine's own knobs bound on first use (safe at static-init time: the
// lambdas touch the singletons only when called).
std::map<std::string, Entry>& Reg()
{
	std::map<std::string, Entry>& t = Table();
	if (!g_defaults) { g_defaults = true; RegisterEngineDefaults(); }
	return t;
}

std::string ArchiveValue(const std::string& name, bool* found)
{
	Config* c = Config::getSingleton();
	if (c) { auto it = c->cvars.find(name); if (it != c->cvars.end()) { *found = true; return it->second; } }
	*found = false; return "";
}

void Notify(const std::string& name, const std::string& value)
{
	std::vector<Hook> hooks;
	{ boost::mutex::scoped_lock l(g_mx); hooks = Hooks(); }
	for (const Hook& h : hooks) if (h.name.empty() || h.name == name) h.fn(name, value);
	Events::EmitEngine("cvar.changed", name);
}

void Persist(const std::string& name, const std::string& value)
{
	Config* c = Config::getSingleton();
	if (!c) return;
	c->cvars[name] = value;
	AppInstance* app = AppInstance::GetSingleton();
	if (app && !app->isEditor()) c->saveWindow();   // the game owns its config; the editor's ships at packaging
}

CvarType InferType(const std::string& def)
{
	const std::string l = Lower(def);
	if (l == "true" || l == "false") return CvarType::Bool;
	if (def.empty()) return CvarType::String;
	char* end = nullptr;
	std::strtol(def.c_str(), &end, 10);
	if (end && *end == '\0') return CvarType::Int;
	std::strtod(def.c_str(), &end);
	if (end && *end == '\0') return CvarType::Float;
	return CvarType::String;
}
}  // namespace

bool Cvars::ParseValue(CvarType type, const std::string& text, std::string& out)
{
	std::string t = text;
	while (!t.empty() && isspace((unsigned char)t.back())) t.pop_back();
	while (!t.empty() && isspace((unsigned char)t.front())) t.erase(t.begin());
	char* end = nullptr;
	switch (type)
	{
	case CvarType::Bool:
	{
		const std::string l = Lower(t);
		if (l == "1" || l == "true" || l == "on" || l == "yes") { out = "true"; return true; }
		if (l == "0" || l == "false" || l == "off" || l == "no") { out = "false"; return true; }
		return false;
	}
	case CvarType::Int:
	{
		if (t.empty()) return false;
		const double v = std::strtod(t.c_str(), &end);
		if (!end || *end != '\0') return false;
		out = std::to_string((long long)v); return true;
	}
	case CvarType::Float:
	{
		if (t.empty()) return false;
		const double v = std::strtod(t.c_str(), &end);
		if (!end || *end != '\0') return false;
		out = FormatNumber(v); return true;
	}
	default: out = text; return true;
	}
}

bool Cvars::RegisterTyped(const std::string& name, CvarType type, const std::string& defaultValue, const std::string& description, unsigned flags)
{
	if (name.empty()) return false;
	std::string def;
	if (!ParseValue(type, defaultValue, def)) { Log::Write(LOG_WARN, "Cvar", "'" + name + "': default '" + defaultValue + "' is not a valid value"); return false; }
	Entry e; e.type = type; e.flags = flags; e.def = def; e.value = def; e.description = description;
	bool fromArchive = false;
	if (flags & CvarArchive)
	{
		const std::string a = ArchiveValue(name, &fromArchive);
		std::string parsed;
		if (fromArchive && ParseValue(type, a, parsed)) e.value = parsed; else fromArchive = false;
	}
	{
		boost::mutex::scoped_lock l(g_mx);
		std::map<std::string, Entry>& t = Reg();
		if (t.count(name)) return false;
		t[name] = e;
	}
	if (fromArchive) Notify(name, e.value);
	return true;
}

bool Cvars::Register(const std::string& name, const std::string& defaultValue, const std::string& description, bool archive)
{
	return RegisterTyped(name, InferType(defaultValue), defaultValue, description, archive ? CvarArchive : CvarNone);
}

bool Cvars::Bind(const std::string& name, CvarType type, const std::string& description, unsigned flags,
                 std::function<std::string()> get, std::function<bool(const std::string&)> set)
{
	if (name.empty() || !get) return false;
	Entry e; e.type = type; e.flags = flags | (set ? 0u : (unsigned)CvarReadOnly); e.description = description;
	e.get = get; e.set = set; e.bound = true;
	boost::mutex::scoped_lock l(g_mx);
	std::map<std::string, Entry>& t = Reg();
	if (t.count(name)) return false;
	t[name] = e;
	return true;
}

bool Cvars::Unregister(const std::string& name)
{
	boost::mutex::scoped_lock l(g_mx);
	return Reg().erase(name) != 0;
}

bool Cvars::Info(const std::string& name, CvarInfo& out)
{
	Entry e;
	{
		boost::mutex::scoped_lock l(g_mx);
		auto it = Reg().find(name);
		if (it == Reg().end()) return false;
		e = it->second;
	}
	out.name = name; out.description = e.description; out.type = e.type; out.flags = e.flags; out.bound = e.bound;
	out.value = e.bound ? e.get() : e.value;
	out.defaultValue = e.bound ? "" : e.def;
	return true;
}

std::vector<std::string> Cvars::Names(const std::string& prefix)
{
	boost::mutex::scoped_lock l(g_mx);
	std::vector<std::string> v;
	const std::string p = Lower(prefix);
	for (auto& kv : Reg())
		if (p.empty() || Lower(kv.first).compare(0, p.size(), p) == 0) v.push_back(kv.first);
	return v;
}

bool Cvars::Has(const std::string& name) { boost::mutex::scoped_lock l(g_mx); return Reg().count(name) != 0; }

std::string Cvars::Get(const std::string& name)
{
	CvarInfo i;
	return Info(name, i) ? i.value : "";
}

double Cvars::GetNumber(const std::string& name)
{
	CvarInfo i;
	if (!Info(name, i)) return 0.0;
	if (i.type == CvarType::Bool) return i.value == "true" ? 1.0 : 0.0;
	char* end = nullptr;
	const double v = std::strtod(i.value.c_str(), &end);
	return (end && *end == '\0') ? v : 0.0;
}

bool Cvars::GetBool(const std::string& name)
{
	CvarInfo i;
	if (!Info(name, i)) return false;
	if (i.type == CvarType::Bool) return i.value == "true";
	return GetNumber(name) != 0.0;
}

bool Cvars::SetFrom(const std::string& name, const std::string& value, bool fromConsole, std::string* error)
{
	Entry e;
	{
		boost::mutex::scoped_lock l(g_mx);
		auto it = Reg().find(name);
		if (it == Reg().end()) { if (error) *error = "unknown cvar: " + name; return false; }
		e = it->second;
	}
	if (e.flags & CvarReadOnly) { if (error) *error = name + " is read-only"; return false; }
	if (fromConsole && (e.flags & CvarCheat) && !Console::Enabled()) { if (error) *error = name + " is a cheat cvar (the console is disabled)"; return false; }
	std::string v;
	if (!ParseValue(e.type, value, v))
	{
		static const char* kNames[] = { "bool", "int", "float", "string" };
		if (error) *error = "'" + value + "' is not a " + kNames[(int)e.type] + " (" + name + ")";
		return false;
	}
	if (e.bound)
	{
		if (!e.set(v)) { if (error) *error = name + " rejected '" + v + "'"; return false; }
	}
	else
	{
		boost::mutex::scoped_lock l(g_mx);
		auto it = Reg().find(name);
		if (it == Reg().end()) return false;
		if (it->second.value == v) return true;   // unchanged: no hooks, no write
		it->second.value = v;
	}
	if (e.flags & CvarArchive) Persist(name, v);
	Notify(name, v);
	return true;
}

bool Cvars::Set(const std::string& name, const std::string& value)
{
	std::string err;
	const bool ok = SetFrom(name, value, false, &err);
	if (!ok) Log::Write(LOG_WARN, "Cvar", err);
	return ok;
}

bool Cvars::SetNumber(const std::string& name, double value)
{
	CvarInfo i;
	if (!Info(name, i)) { Log::Write(LOG_WARN, "Cvar", "unknown cvar: " + name); return false; }
	if (i.type == CvarType::Bool) return Set(name, value != 0.0 ? "true" : "false");
	return Set(name, FormatNumber(value));
}

bool Cvars::Reset(const std::string& name)
{
	CvarInfo i;
	if (!Info(name, i) || i.bound) return false;
	return Set(name, i.defaultValue);
}

std::string Cvars::Default(const std::string& name) { CvarInfo i; return Info(name, i) ? i.defaultValue : ""; }

std::string Cvars::Describe(const std::string& name)
{
	CvarInfo i;
	if (!Info(name, i)) return "unknown cvar: " + name;
	std::ostringstream o;
	o << name << " = " << (i.type == CvarType::String ? "\"" + i.value + "\"" : i.value);
	if (!i.bound) o << " (default " << (i.type == CvarType::String ? "\"" + i.defaultValue + "\"" : i.defaultValue) << ")";
	std::string fl;
	if (i.flags & CvarArchive)  fl += "archive ";
	if (i.flags & CvarCheat)    fl += "cheat ";
	if (i.flags & CvarReadOnly) fl += "read-only ";
	if (i.bound)                fl += "engine ";
	if (!fl.empty()) { fl.pop_back(); o << " [" << fl << "]"; }
	if (!i.description.empty()) o << " - " << i.description;
	return o.str();
}

int Cvars::Count() { boost::mutex::scoped_lock l(g_mx); return (int)Reg().size(); }

std::string Cvars::NameAt(int i)
{
	boost::mutex::scoped_lock l(g_mx);
	std::map<std::string, Entry>& t = Reg();
	if (i < 0 || i >= (int)t.size()) return "";
	auto it = t.begin(); std::advance(it, i);
	return it->first;
}

long long Cvars::OnChange(const std::string& name, std::function<void(const std::string&, const std::string&)> fn)
{
	if (!fn) return 0;
	boost::mutex::scoped_lock l(g_mx);
	Hooks().push_back({ g_nextHook, name, fn });
	return g_nextHook++;
}

void Cvars::RemoveOnChange(long long id)
{
	boost::mutex::scoped_lock l(g_mx);
	std::vector<Hook>& h = Hooks();
	h.erase(std::remove_if(h.begin(), h.end(), [&](const Hook& x) { return x.id == id; }), h.end());
}

void Cvars::ApplyArchive()
{
	std::vector<std::pair<std::string, std::string>> changed;
	{
		boost::mutex::scoped_lock l(g_mx);
		for (auto& kv : Reg())
		{
			if (kv.second.bound || !(kv.second.flags & CvarArchive)) continue;
			bool found = false;
			const std::string a = ArchiveValue(kv.first, &found);
			std::string v;
			if (!found || !ParseValue(kv.second.type, a, v) || v == kv.second.value) continue;
			kv.second.value = v;
			changed.push_back({ kv.first, v });
		}
	}
	for (auto& c : changed) Notify(c.first, c.second);
}

// ---- the engine's own knobs, as bound cvars ------------------------------------------------------
namespace {
std::string B(bool v) { return v ? "true" : "false"; }
double D(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
void RegisterEngineDefaults()
{
	std::map<std::string, Entry>& t = Table();
	auto bind = [&](const char* name, CvarType type, const char* desc, std::function<std::string()> get, std::function<bool(const std::string&)> set)
	{
		Entry e; e.type = type; e.description = desc; e.get = get; e.set = set; e.bound = true;
		if (!set) e.flags |= CvarReadOnly;
		t[name] = e;
	};
	bind("g.timeScale", CvarType::Float, "Game speed (Game.SetTimeScale): 0 = frozen, 1 = normal, up to 8",
	     [] { return FormatNumber(Game::GetTimeScale()); }, [](const std::string& v) { Game::SetTimeScale(D(v)); return true; });
	bind("g.paused", CvarType::Bool, "Play-mode pause (Game.SetPaused)",
	     [] { return B(Game::IsPaused()); }, [](const std::string& v) { Game::SetPaused(v == "true"); return true; });
	bind("r.vsync", CvarType::Bool, "Present at the display refresh (window.vsync)",
	     [] { return B(Game::IsVSync()); }, [](const std::string& v) { Game::SetVSync(v == "true"); return true; });
	bind("r.fpsLimit", CvarType::Int, "Frame cap, 0 = uncapped (window.fpsLimit)",
	     [] { return FormatNumber(Game::GetFpsLimit()); }, [](const std::string& v) { Game::SetFpsLimit(D(v)); return true; });
	bind("r.textureStreamMB", CvarType::Int, "Mip-streaming VRAM budget in MB, 0 = off (window.textureStreamMB)",
	     [] { Config* c = Config::getSingleton(); return FormatNumber(c ? c->window.textureStreamMB : 0); },
	     [](const std::string& v) { Game::SetTextureStreaming(D(v)); return true; });
	bind("time.gameToReal", CvarType::Float, "Game seconds per real second (Time.SetGameToReal)",
	     [] { return FormatNumber(Time::GameToReal()); }, [](const std::string& v) { Time::SetGameToReal(D(v)); return true; });
	bind("loc.language", CvarType::String, "The string-table language (Loc.SetLanguage, window.language)",
	     [] { return Loc::Language(); }, [](const std::string& v) { if (v.empty()) return false; Loc::SetLanguage(v); return true; });
	bind("con.enabled", CvarType::Bool, "The developer console's cheat gate (Console.SetEnabled)",
	     [] { return B(Console::Enabled()); }, [](const std::string& v) { Console::SetEnabled(v == "true"); return true; });
}
}  // namespace

}  // namespace nuke
