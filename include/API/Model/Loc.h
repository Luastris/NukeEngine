#pragma once
#ifndef NUKEE_LOC_H
#define NUKEE_LOC_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <map>
#include <string>
#include <vector>

namespace nuke {

// 7.8 localization (built in, like Input). String tables live in content/localization/<lang>.json:
// nested objects are namespaces ("menu": {"start": "Start"} -> "menu.start"); "_name" at the top
// is the language's display name. ResDB feeds the tables (disk scan, pak, hot reload). Lookup
// falls back lang -> en -> the key itself. The language comes from config window.language
// ("" = en) and switches live: "loc.changed" (payload = the language) tells UI to re-read, and
// Version() bumps on every table or language change.
class NUKEENGINE_API Loc
{
	NUKE_CLASS_NOCREATE(Loc, Object)
public:
	[[nuke::func]] static std::string Get(const std::string& key);
	// Get + placeholders. `args` is JSON: an object fills {name}, an array fills {0} {1} ...; any
	// other text is the single argument {0}. "{{" / "}}" print braces. Unknown names stay as written.
	[[nuke::func]] static std::string Format(const std::string& key, const std::string& args);
	[[nuke::func]] static bool        Has(const std::string& key);          // in the CURRENT language (no fallback)
	[[nuke::func]] static std::string Language();                            // current code ("en", "ru", ...)
	[[nuke::func]] static void        SetLanguage(const std::string& lang);  // live switch; the game saves it to config
	[[nuke::func]] static int         LanguageCount();                       // tables loaded
	[[nuke::func]] static std::string LanguageAt(int i);
	[[nuke::func]] static std::string LanguageName(const std::string& lang); // "_name" of the table, else the code
	[[nuke::func]] static int         Version();                             // bumps on any change (UI re-read gate)

	// --- native (ResDB, editor) ---
	static bool LoadTable(const std::string& lang, const std::string& jsonText, const std::string& source);   // replaces the table
	static void RemoveTable(const std::string& lang);
	static std::vector<std::string> Languages();                    // sorted codes
	static std::vector<std::string> Keys();                         // union of every table's keys, sorted (meta "_*" excluded)
	static std::vector<std::string> Missing(const std::string& lang);   // union keys this table lacks
	static bool        Lookup(const std::string& lang, const std::string& key, std::string& out);   // exact, no fallback
	static std::string SourcePath(const std::string& lang);         // the file it came from ("" = pak / editor-made)
	static void        Set(const std::string& lang, const std::string& key, const std::string& value);   // editor edit (creates the table)
	static void        Erase(const std::string& lang, const std::string& key);
	static std::string ToJson(const std::string& lang);             // the table as nested JSON text
	static std::string Flatten(const std::string& jsonText, std::map<std::string, std::string>& out);   // "" = ok, else the parse error
};

}  // namespace nuke

#endif // !NUKEE_LOC_H
