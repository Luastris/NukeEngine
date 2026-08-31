#include "API/Model/DevConsole.h"
#include "API/Model/Log.h"
#include "API/Model/Package.h"
#include "config.h"
#include "input/Input.h"
#include "interface/iGUI.h"
#include "interface/AppInstance.h"
#include "interface/Services.h"
#include "service/iScript.h"
#include "reflect/Reflect.h"
#include "reflect/ReflectBind.h"
#include "API/Model/World.h"
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace nuke {

namespace {

// -1 = unresolved: dev sessions (editor/raw player) default ON; a packaged game (pak mounted)
// follows config "devConsole" (the Game Build checkbox). Console::SetEnabled overrides live.
int  g_enabled = -1;
bool g_open = false;
bool g_wantFocus = false;
char g_buf[1024] = { 0 };
float g_prevGrave = 0.0f;
std::vector<std::string> g_history;         // submitted lines, oldest first (engine-walked)
// Command-line behavior state — ALL of it engine-side; the GUI backend only reports keys.
int         g_histNav = -1;                 // history cursor (-1 = fresh line)
int         g_suggSel = 0;                  // selected autocomplete candidate
std::string g_suggPrevBuf;                  // typing detection: resets the selection
std::string g_pendingSet;                   // buffer replacement applied next frame
bool        g_pendingHas = false;           // "" is a valid replacement (clear the line)

bool ResolveEnabled()
{
	if (g_enabled < 0)
		g_enabled = Package::MountedCount() > 0 ? (Config::getSingleton()->devConsole ? 1 : 0) : 1;
	return g_enabled != 0;
}

std::string Trim(const std::string& s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

// Whitespace tokens; "quoted strings" keep spaces (quotes stripped).
std::vector<std::string> Tokenize(const std::string& s)
{
	std::vector<std::string> out;
	std::string cur;
	bool q = false, have = false;
	for (char c : s)
	{
		if (q) { if (c == '"') q = false; else cur += c; continue; }
		if (c == '"') { q = true; have = true; continue; }
		if (c == ' ' || c == '\t') { if (have || !cur.empty()) { out.push_back(cur); cur.clear(); have = false; } continue; }
		cur += c;
	}
	if (have || !cur.empty()) out.push_back(cur);
	return out;
}

bool IsNumber(const std::string& t)
{
	if (t.empty()) return false;
	char* end = nullptr;
	std::strtod(t.c_str(), &end);
	return end && *end == '\0';
}

const char* FTName(FT t)
{
	switch (t)
	{
	case FT::Bool: return "bool"; case FT::Int: return "int"; case FT::Float: return "float";
	case FT::Double: return "double"; case FT::String: return "string";
	case FT::Vec2: return "vec2"; case FT::Vec3: return "vec3"; case FT::Vec4: return "vec4";
	case FT::Quat: return "quat"; case FT::Color: return "color";
	case FT::AtomRef: return "atom"; case FT::ObjectRef: return "object";
	default: return "?";
	}
}

std::string Usage(const std::string& type, const Method& m)
{
	std::string u = type + "." + m.name;
	for (size_t i = 0; i < m.params.size(); ++i)
	{
		u += " <";
		u += m.paramClass.size() > i && !m.paramClass[i].empty() ? m.paramClass[i].c_str() : FTName(m.params[i]);
		u += ">";
	}
	return u;
}

// Consume tokens for ONE parameter of type `ft` (vectors take 2-4 numbers). False = mismatch.
bool ParseArg(FT ft, const std::string& cls, const std::vector<std::string>& tk, size_t& i, ReflectValue& rv)
{
	auto num = [&](double& d) -> bool {
		if (i >= tk.size() || !IsNumber(tk[i])) return false;
		d = std::strtod(tk[i++].c_str(), nullptr);
		return true;
	};
	rv.type = ft;
	switch (ft)
	{
	case FT::Bool:
		if (i >= tk.size()) return false;
		rv.b = tk[i] == "true" || tk[i] == "1";
		if (!rv.b && tk[i] != "false" && tk[i] != "0") return false;
		++i; return true;
	case FT::Int: case FT::Float: case FT::Double:
		return num(rv.num);
	case FT::String:
		if (i >= tk.size()) return false;
		rv.str = tk[i++]; return true;
	case FT::Vec2:
		return num(rv.v[0]) && num(rv.v[1]);
	case FT::Vec3:
		return num(rv.v[0]) && num(rv.v[1]) && num(rv.v[2]);
	case FT::Vec4: case FT::Quat: case FT::Color:
		return num(rv.v[0]) && num(rv.v[1]) && num(rv.v[2]) && num(rv.v[3]);
	case FT::AtomRef:
	{
		if (i >= tk.size()) return false;
		const std::string& t = tk[i++];
		if (IsNumber(t)) { rv.atom = (unsigned long)std::strtod(t.c_str(), nullptr); return true; }
		AppInstance* app = AppInstance::GetSingleton();
		Atom* a = app && app->currentWorld ? app->currentWorld->Get(t) : nullptr;
		if (!a) return false;
		rv.atom = Reflect_AtomId(a);
		return true;
	}
	case FT::ObjectRef:
	{
		// Assets by NAME through the same lookup scripts use (case-insensitive, guid-free).
		if (i >= tk.size() || cls.empty()) return false;
		rv.obj = Reflect_FindAsset(cls.c_str(), tk[i++].c_str());
		return rv.obj != 0;
	}
	default: return false;
	}
}

std::string RvToString(const ReflectValue& v)
{
	std::ostringstream o;
	switch (v.type)
	{
	case FT::Unknown: return "";   // void
	case FT::Bool:    return v.b ? "true" : "false";
	case FT::Int:     o << (long long)v.num; return o.str();
	case FT::Float: case FT::Double: o << v.num; return o.str();
	case FT::String:  return v.str;
	case FT::Vec2:    o << v.v[0] << " " << v.v[1]; return o.str();
	case FT::Vec3:    o << v.v[0] << " " << v.v[1] << " " << v.v[2]; return o.str();
	case FT::Vec4: case FT::Quat: case FT::Color:
		o << v.v[0] << " " << v.v[1] << " " << v.v[2] << " " << v.v[3]; return o.str();
	case FT::AtomRef: o << "atom:" << v.atom; return o.str();
	case FT::ObjectRef: o << "object:" << v.obj; return o.str();
	default: return "?";
	}
}

bool TypeHasStatics(TypeInfo* ti)
{
	if (!ti) return false;
	for (const Method& m : ti->methods) if (m.isStatic) return true;
	return false;
}

std::string Help(const std::string& arg)
{
	std::ostringstream o;
	if (arg.empty())
	{
		o << "commands: Type.Method args... | help <Type> | anything else runs as Lua\ntypes:";
		for (TypeInfo* ti : Registry_All())
			if (TypeHasStatics(ti)) o << " " << ti->name;
		return o.str();
	}
	TypeInfo* ti = Registry_Find(arg);
	if (!ti) return "unknown type: " + arg;
	for (const Method& m : ti->methods)
		if (m.isStatic) o << Usage(arg, m) << "\n";
	std::string s = o.str();
	if (!s.empty() && s.back() == '\n') s.pop_back();
	return s.empty() ? arg + " has no static commands" : s;
}

// --- autocomplete -------------------------------------------------------------------------

bool StartsWithCI(const std::string& s, const std::string& prefix)
{
	if (prefix.size() > s.size()) return false;
	for (size_t i = 0; i < prefix.size(); ++i)
		if (tolower((unsigned char)s[i]) != tolower((unsigned char)prefix[i])) return false;
	return true;
}

TypeInfo* FindTypeCI(const std::string& name)
{
	if (TypeInfo* ti = Registry_Find(name)) return ti;
	for (TypeInfo* ti : Registry_All())
		if (ti && ti->name.size() == name.size() && StartsWithCI(ti->name, name)) return ti;
	return nullptr;
}

// Suggestions for the current buffer: "Prof" -> matching types, "Profiler.Sh" -> usage lines
// of the matching statics (up to 8, arrow-navigable), a complete "Type.Method ..." -> its
// usage while the arguments are typed. `lines` are the rows shown; the first cands.size() of
// them correspond 1:1 to `cands`, the full strings Tab puts into the buffer.
void BuildSuggestions(const char* bufC, std::vector<std::string>& lines, std::vector<std::string>& cands)
{
	const std::string buf = bufC;
	if (buf.empty()) return;
	const size_t space = buf.find(' ');
	if (space != std::string::npos)
	{
		// Already past the command: remind its usage while the arguments are typed.
		const std::string head = buf.substr(0, space);
		const size_t dot = head.find('.');
		if (dot == std::string::npos || dot == 0 || dot + 1 >= head.size()) return;
		TypeInfo* ti = FindTypeCI(head.substr(0, dot));
		const Method* m = ti ? Reflect_FindMethod(ti, head.substr(dot + 1)) : nullptr;
		if (m && m->isStatic && !m->params.empty()) lines.push_back(Usage(ti->name, *m));
		return;
	}
	size_t total = 0;
	const size_t dot = buf.find('.');
	if (dot == std::string::npos)
	{
		for (TypeInfo* ti : Registry_All())
			if (ti && TypeHasStatics(ti) && StartsWithCI(ti->name, buf))
			{
				++total;
				if (cands.size() < 8) { cands.push_back(ti->name + "."); lines.push_back(ti->name); }
			}
	}
	else
	{
		TypeInfo* ti = FindTypeCI(buf.substr(0, dot));
		if (!ti) return;
		const std::string mpfx = buf.substr(dot + 1);
		for (const Method& m : ti->methods)
			if (m.isStatic && StartsWithCI(m.name, mpfx))
			{
				++total;
				if (cands.size() < 8) { cands.push_back(ti->name + "." + m.name); lines.push_back(Usage(ti->name, m)); }
			}
	}
	if (total > cands.size()) lines.push_back("... (" + std::to_string(total - cands.size()) + " more)");
}

}  // namespace

void Console::SetEnabled(bool on)
{
	g_enabled = on ? 1 : 0;
	if (!on && g_open) { g_open = false; Input::SetSuppressed(false); }
}
bool Console::Enabled() { return ResolveEnabled(); }
void Console::Toggle()
{
	if (!ResolveEnabled()) return;
	g_open = !g_open;
	g_wantFocus = g_open;
	Input::SetSuppressed(g_open);
}
bool Console::IsOpen() { return g_open; }

std::string Console::Execute(const std::string& rawLine)
{
	const std::string line = Trim(rawLine);
	if (line.empty()) return "";
	Log::Write(LOG_INFO, "Console", "> " + line);
	if (g_history.empty() || g_history.back() != line) g_history.push_back(line);

	std::vector<std::string> tk = Tokenize(line);
	if (tk[0] == "help")
	{
		std::string h = Help(tk.size() > 1 ? tk[1] : "");
		Log::Write(LOG_INFO, "Console", h);
		return h;
	}

	// Type.Method over the reflection registry.
	const size_t dot = tk[0].find('.');
	if (dot != std::string::npos && dot > 0 && dot + 1 < tk[0].size())
	{
		const std::string type = tk[0].substr(0, dot), method = tk[0].substr(dot + 1);
		TypeInfo* ti = Registry_Find(type);
		const Method* m = ti ? Reflect_FindMethod(ti, method) : nullptr;
		if (m && m->isStatic)
		{
			std::vector<ReflectValue> args(m->params.size());
			size_t i = 1;
			bool ok = true;
			for (size_t p = 0; p < m->params.size() && ok; ++p)
				ok = ParseArg(m->params[p], m->paramClass.size() > p ? m->paramClass[p] : "", tk, i, args[p]);
			if (!ok || i != tk.size())
			{
				std::string u = "usage: " + Usage(type, *m);
				Log::Write(LOG_WARN, "Console", u);
				return u;
			}
			ReflectValue ret;
			if (!Reflect_Invoke(nullptr, *m, args.data(), args.size(), ret))
			{
				Log::Write(LOG_ERROR, "Console", "invoke failed: " + tk[0]);
				return "invoke failed";
			}
			std::string r = RvToString(ret);
			if (!r.empty()) Log::Write(LOG_INFO, "Console", r);
			return r;
		}
		if (ti && !m)
		{
			// A known type with an unknown method reads as a typo, not as Lua.
			std::string e = type + " has no static '" + method + "' (try: help " + type + ")";
			Log::Write(LOG_WARN, "Console", e);
			return e;
		}
	}

	// Anything else: a Lua chunk (prints land in the log through the capture).
	for (iScript* svc : GetServices<iScript>())
		if (svc && svc->Language() && std::strcmp(svc->Language(), "lua") == 0)
			return svc->Run(line.c_str(), "console") ? "" : "error";   // Lua logs its own error
	Log::Write(LOG_WARN, "Console", "not a command and no Lua runtime to run it");
	return "unknown command";
}

void Console::Emit()
{
	if (!ResolveEnabled()) return;

	// Grave toggles regardless of .nuinput bindings (raw control edge).
	const float grave = Input::Control("Key.Grave");
	if (grave > 0.5f && g_prevGrave <= 0.5f) Toggle();
	g_prevGrave = grave;
	if (!g_open) return;

	AppInstance* app = AppInstance::GetSingleton();
	iGUI* ui = GUI();
	const float w = app && app->uiW > 0 ? (float)app->uiW : 640.0f;
	const float h = app && app->uiH > 0 ? (float)app->uiH : 360.0f;
	ui->SetNextWindowRect(0, 0, w, h * 0.45f);
	if (ui->Begin("Console"))
	{
		// New entries stick the view to the bottom.
		static uint64_t lastVer = 0;
		const uint64_t ver = Log::Version();
		const bool fresh = ver != lastVer;
		lastVer = ver;

		// Autocomplete for what is typed so far: hint rows above the input, the selected
		// candidate highlighted (first by default, Up/Down move it, Tab inserts it).
		std::vector<std::string> sugg, cands;
		BuildSuggestions(g_buf, sugg, cands);
		if (g_suggPrevBuf != g_buf) { g_suggSel = 0; g_suggPrevBuf = g_buf; }   // typing resets to the first
		if (g_suggSel >= (int)cands.size()) g_suggSel = cands.empty() ? 0 : (int)cands.size() - 1;

		ui->BeginScrollRegion("##conlog", -(float)sugg.size());   // reserve a row per hint
		std::vector<LogEntry> ring = Log::Snapshot();
		const size_t first = ring.size() > 256 ? ring.size() - 256 : 0;
		for (size_t i = first; i < ring.size(); ++i)
		{
			const LogEntry& e = ring[i];
			std::string t = e.tag.empty() ? e.text : "[" + e.tag + "] " + e.text;
			if (e.count > 1) t += " (x" + std::to_string(e.count) + ")";
			if (e.level == LOG_ERROR)     ui->TextColored(1.0f, 0.35f, 0.30f, 1.0f, t.c_str());
			else if (e.level == LOG_WARN) ui->TextColored(1.0f, 0.85f, 0.30f, 1.0f, t.c_str());
			else                          ui->TextColored(0.85f, 0.85f, 0.85f, 1.0f, t.c_str());
		}
		if (fresh) ui->ScrollToBottom();
		ui->EndScrollRegion();

		for (size_t i = 0; i < sugg.size(); ++i)
		{
			if (i < cands.size() && (int)i == g_suggSel)
				ui->TextColored(1.0f, 0.90f, 0.40f, 1.0f, ("> " + sugg[i]).c_str());
			else
				ui->TextColored(0.55f, 0.75f, 0.95f, 1.0f, ("  " + sugg[i]).c_str());
		}
		if (g_wantFocus) { ui->FocusNextWidget(); g_wantFocus = false; }

		// The widget is dumb on purpose: it reports Up/Down/Tab and takes a replacement
		// string; what they MEAN — candidate selection, history walk, completion — is here.
		int key = 0;
		const bool submit = ui->InputTextKeys("##concmd", g_buf, (int)sizeof(g_buf),
		                                      g_pendingHas ? g_pendingSet.c_str() : nullptr,
		                                      "`~", &key);
		if (g_pendingHas) { g_pendingHas = false; g_pendingSet.clear(); }
		if (key == 1 || key == 2)
		{
			if (!cands.empty())   // candidates visible: arrows move the selection (wrap)
				g_suggSel = key == 1 ? (g_suggSel <= 0 ? (int)cands.size() : g_suggSel) - 1
				                     : (g_suggSel + 1) % (int)cands.size();
			else if (!g_history.empty())   // otherwise: walk the submit history
			{
				const int n = (int)g_history.size();
				if (key == 1) g_histNav = g_histNav < 0 ? n - 1 : (g_histNav > 0 ? g_histNav - 1 : 0);
				else          { if (g_histNav >= 0 && ++g_histNav >= n) g_histNav = -1; }
				g_pendingSet = g_histNav >= 0 ? g_history[g_histNav] : "";
				g_pendingHas = true;
			}
		}
		else if (key == 3 && !cands.empty())   // Tab: insert the selected candidate
		{
			g_pendingSet = cands[g_suggSel];
			g_pendingHas = true;
		}
		if (submit)
		{
			Execute(g_buf);
			g_buf[0] = 0;
			g_histNav = -1;
			g_wantFocus = true;   // keep typing
		}
	}
	ui->End();
}

}  // namespace nuke
