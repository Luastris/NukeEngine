#pragma once
#ifndef NUKEE_EXCHANGE_H
#define NUKEE_EXCHANGE_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"
#include <string>

namespace nuke {

// Model import / export for scripts and tools: the engine's Importer facade + the "exchange"
// service (NukeExchange) behind one reflected surface. Both run on the WORKER pool - nothing
// heavy on the game thread - and return true when queued; the result arrives on the event bus:
// "exchange.imported" / "exchange.exported" with payload "<path>|ok" or "<path>|failed".
// Export writes a native asset out as a scene file in the format the destination's extension names.
class NUKEENGINE_API Exchange
{
	NUKE_CLASS_NOCREATE(Exchange, Object)
public:
	[[nuke::func]] static bool        Available();                       // an exchange provider is live
	// Any supported file -> native assets in destDir (content-relative or absolute; "" = content/Imported). Queued.
	[[nuke::func]] static bool        Import(const std::string& srcFile, const std::string& destDir);
	// kind: "mesh" (id = mesh GUID), "prefab" (id = .nuprefab path, content-relative or absolute), "clip" (id = clip GUID). Queued.
	[[nuke::func]] static bool        Export(const std::string& kind, const std::string& id, const std::string& dstFile);
	[[nuke::func]] static std::string ImportExtensions();   // ";"-joined, with the dot (models + images + audio + plugin formats)
	[[nuke::func]] static std::string ExportExtensions();   // ";"-joined, with the dot ("" without a provider)
};

}  // namespace nuke

#endif // !NUKEE_EXCHANGE_H
