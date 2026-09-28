// 7.8 localization: string tables + lookup with fallback + live language switch.
#include "API/Model/Loc.h"
#include "API/Model/Events.h"
#include "interface/AppInstance.h"
#include "config.h"
#include <nlohmann/json.hpp>
#include <boost/thread/mutex.hpp>
#include <algorithm>
#include <iostream>
#include <set>

using json = nlohmann::json;

namespace nuke {

namespace {
struct Table { std::map<std::string, std::string> kv; std::string source; };
boost::mutex g_mx;
std::map<std::string, Table> g_tables;   // lang -> table
std::string g_lang;                       // "" = not resolved yet (config on first use)
int g_version = 1;

// The current language: config window.language on first use, "en" when unset.
const std::string& CurLang()
{
	if (g_lang.empty())
	{
		Config* c = Config::getSingleton();
		g_lang = (c && !c->window.language.empty()) ? c->window.language : "en";
	}
	return g_lang;
}

bool LookupLocked(const std::string& lang, const std::string& key, std::string& out)
{
	auto t = g_tables.find(lang);
	if (t == g_tables.end()) return false;
	auto k = t->second.kv.find(key);
	if (k == t->second.kv.end()) return false;
	out = k->second;
	return true;
}

void FlattenInto(const json& j, const std::string& prefix, std::map<std::string, std::string>& out)
{
	if (j.is_object())
	{
		for (auto it = j.begin(); it != j.end(); ++it)
			FlattenInto(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key(), out);
	}
	else if (j.is_array())
	{
		for (size_t i = 0; i < j.size(); ++i) FlattenInto(j[i], prefix + "." + std::to_string(i), out);
	}
	else if (j.is_string()) out[prefix] = j.get<std::string>();
	else if (!j.is_null())  out[prefix] = j.dump();
}

void Changed(const std::string& lang)
{
	++g_version;
	Events::EmitEngine("loc.changed", lang);
}
}  // namespace

std::string Loc::Flatten(const std::string& jsonText, std::map<std::string, std::string>& out)
{
	json j = json::parse(jsonText, nullptr, false, true);
	if (j.is_discarded()) return "not valid JSON";
	if (!j.is_object()) return "the top level must be an object";
	out.clear();
	FlattenInto(j, "", out);
	return "";
}

bool Loc::LoadTable(const std::string& lang, const std::string& jsonText, const std::string& source)
{
	std::map<std::string, std::string> kv;
	const std::string err = Flatten(jsonText, kv);
	if (!err.empty())
	{
		std::cout << "[Loc]\t\t'" << lang << "' (" << (source.empty() ? "pak" : source) << "): " << err << std::endl;
		return false;
	}
	{
		boost::mutex::scoped_lock lock(g_mx);
		Table& t = g_tables[lang];
		t.kv.swap(kv);
		t.source = source;
	}
	std::cout << "[Loc]\t\tlanguage '" << lang << "' " << g_tables[lang].kv.size() << " strings" << (source.empty() ? " (pak)" : "") << std::endl;
	Changed(lang);
	return true;
}

void Loc::RemoveTable(const std::string& lang)
{
	{
		boost::mutex::scoped_lock lock(g_mx);
		if (!g_tables.erase(lang)) return;
	}
	std::cout << "[Loc]\t\tlanguage '" << lang << "' removed" << std::endl;
	Changed(lang);
}

std::string Loc::Get(const std::string& key)
{
	boost::mutex::scoped_lock lock(g_mx);
	std::string s;
	const std::string& lang = CurLang();
	if (LookupLocked(lang, key, s)) return s;
	if (lang != "en" && LookupLocked("en", key, s)) return s;
	return key;
}

std::string Loc::Format(const std::string& key, const std::string& args)
{
	const std::string tpl = Get(key);
	std::map<std::string, std::string> a;
	json j = json::parse(args, nullptr, false, true);
	if (j.is_discarded() || (!j.is_object() && !j.is_array())) a["0"] = args;
	else FlattenInto(j, "", a);
	if (j.is_array()) { std::map<std::string, std::string> b; for (auto& kv : a) b[kv.first.substr(1)] = kv.second; a.swap(b); }   // ".0" -> "0"
	std::string out; out.reserve(tpl.size() + 16);
	for (size_t i = 0; i < tpl.size(); ++i)
	{
		const char c = tpl[i];
		if (c == '{' && i + 1 < tpl.size() && tpl[i + 1] == '{') { out += '{'; ++i; continue; }
		if (c == '}' && i + 1 < tpl.size() && tpl[i + 1] == '}') { out += '}'; ++i; continue; }
		if (c == '{')
		{
			const size_t e = tpl.find('}', i + 1);
			if (e == std::string::npos) { out += c; continue; }
			const std::string name = tpl.substr(i + 1, e - i - 1);
			auto it = a.find(name);
			if (it != a.end()) { out += it->second; i = e; continue; }
			out += c; continue;
		}
		out += c;
	}
	return out;
}

bool Loc::Has(const std::string& key)
{
	boost::mutex::scoped_lock lock(g_mx);
	std::string s;
	return LookupLocked(CurLang(), key, s);
}

std::string Loc::Language() { boost::mutex::scoped_lock lock(g_mx); return CurLang(); }

void Loc::SetLanguage(const std::string& lang)
{
	if (lang.empty()) return;
	{
		boost::mutex::scoped_lock lock(g_mx);
		if (CurLang() == lang) return;
		g_lang = lang;
	}
	Config* c = Config::getSingleton();
	AppInstance* app = AppInstance::GetSingleton();
	if (c)
	{
		c->window.language = lang;
		if (app && !app->isEditor()) c->saveWindow();   // the game owns its config/main.json; the editor's is formed at packaging
	}
	std::cout << "[Loc]\t\tlanguage -> '" << lang << "'" << std::endl;
	Changed(lang);
}

int Loc::LanguageCount() { boost::mutex::scoped_lock lock(g_mx); return (int)g_tables.size(); }

std::string Loc::LanguageAt(int i)
{
	boost::mutex::scoped_lock lock(g_mx);
	if (i < 0 || i >= (int)g_tables.size()) return "";
	auto it = g_tables.begin(); std::advance(it, i);
	return it->first;
}

std::string Loc::LanguageName(const std::string& lang)
{
	boost::mutex::scoped_lock lock(g_mx);
	std::string s;
	return LookupLocked(lang, "_name", s) ? s : lang;
}

int Loc::Version() { boost::mutex::scoped_lock lock(g_mx); return g_version; }

std::vector<std::string> Loc::Languages()
{
	boost::mutex::scoped_lock lock(g_mx);
	std::vector<std::string> v;
	for (auto& kv : g_tables) v.push_back(kv.first);
	return v;
}

std::vector<std::string> Loc::Keys()
{
	boost::mutex::scoped_lock lock(g_mx);
	std::set<std::string> s;
	for (auto& t : g_tables) for (auto& kv : t.second.kv) if (kv.first.empty() || kv.first[0] != '_') s.insert(kv.first);
	return std::vector<std::string>(s.begin(), s.end());
}

std::vector<std::string> Loc::Missing(const std::string& lang)
{
	std::vector<std::string> all = Keys(), out;
	boost::mutex::scoped_lock lock(g_mx);
	auto t = g_tables.find(lang);
	for (const std::string& k : all)
		if (t == g_tables.end() || !t->second.kv.count(k) || t->second.kv.at(k).empty()) out.push_back(k);
	return out;
}

bool Loc::Lookup(const std::string& lang, const std::string& key, std::string& out)
{
	boost::mutex::scoped_lock lock(g_mx);
	return LookupLocked(lang, key, out);
}

std::string Loc::SourcePath(const std::string& lang)
{
	boost::mutex::scoped_lock lock(g_mx);
	auto t = g_tables.find(lang);
	return t == g_tables.end() ? "" : t->second.source;
}

void Loc::Set(const std::string& lang, const std::string& key, const std::string& value)
{
	if (lang.empty() || key.empty()) return;
	{
		boost::mutex::scoped_lock lock(g_mx);
		g_tables[lang].kv[key] = value;
	}
	Changed(lang);
}

void Loc::Erase(const std::string& lang, const std::string& key)
{
	{
		boost::mutex::scoped_lock lock(g_mx);
		auto t = g_tables.find(lang);
		if (t == g_tables.end() || !t->second.kv.erase(key)) return;
	}
	Changed(lang);
}

std::string Loc::ToJson(const std::string& lang)
{
	boost::mutex::scoped_lock lock(g_mx);
	json root = json::object();
	auto t = g_tables.find(lang);
	if (t != g_tables.end())
		for (auto& kv : t->second.kv)
		{
			// "a.b.c" -> nested objects (a key that is itself a leaf elsewhere stays flat).
			json* cur = &root;
			size_t b = 0;
			while (true)
			{
				const size_t e = kv.first.find('.', b);
				const std::string part = kv.first.substr(b, e == std::string::npos ? std::string::npos : e - b);
				if (e == std::string::npos) { if (!cur->contains(part) || !(*cur)[part].is_object()) (*cur)[part] = kv.second; break; }
				json& next = (*cur)[part];
				if (!next.is_object()) { if (!next.is_null()) { (*cur)[kv.first.substr(b)] = kv.second; break; } next = json::object(); }
				cur = &next; b = e + 1;
			}
		}
	return root.dump(4);
}

}  // namespace nuke
