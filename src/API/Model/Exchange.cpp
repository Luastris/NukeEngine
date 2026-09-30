// Model import / export facade over the Importer + the "exchange" service. Both directions run
// on the engine's worker pool - nothing heavy on the game thread - and report through the
// event bus: "exchange.imported" / "exchange.exported", payload "<path>|ok" or "<path>|failed".
#include "API/Model/Exchange.h"
#include "API/Model/Events.h"
#include "API/Model/Jobs.h"
#include "API/Model/Log.h"
#include "import/Importer.h"
#include "interface/AppInstance.h"
#include "interface/Importers.h"
#include "interface/Services.h"
#include "service/iExchange.h"
#include <boost/filesystem.hpp>
#include <boost/thread/mutex.hpp>

namespace nuke {

bool Exchange::Available() { return Importer::ModelImportAvailable(); }

bool Exchange::Import(const std::string& srcFile, const std::string& destDir)
{
	if (srcFile.empty()) return false;
	boost::system::error_code ec;
	if (!boost::filesystem::exists(boost::filesystem::path(srcFile), ec))
	{
		Log::Write(LOG_WARN, "Exchange", "import: no such file: " + srcFile);
		return false;
	}
	std::string dest = destDir;
	AppInstance* app = AppInstance::GetSingleton();
	if (dest.empty()) dest = app ? (boost::filesystem::path(app->contentRoot) / "Imported").string() : "Imported";
	else if (boost::filesystem::path(dest).is_relative() && app && !app->contentRoot.empty())
	{
		// A relative destination is content-relative (like every content path), unless it already exists as given.
		if (!boost::filesystem::is_directory(boost::filesystem::path(dest), ec))
			dest = (boost::filesystem::path(app->contentRoot) / dest).string();
	}
	Importer::getSingleton()->ImportAnyAsync(srcFile, dest, [srcFile](bool ok)
	{
		Events::EmitEngine("exchange.imported", srcFile + (ok ? "|ok" : "|failed"));
	});
	return true;   // queued; the event says how it went
}

bool Exchange::Export(const std::string& kind, const std::string& id, const std::string& dstFile)
{
	iExchange* x = GetService<iExchange>();
	if (!x) { Log::Write(LOG_WARN, "Exchange", "no export provider: enable the NukeExchange module"); return false; }
	if (kind.empty() || id.empty() || dstFile.empty()) return false;
	std::string src = id;
	if (kind == "prefab" && boost::filesystem::path(id).is_relative())
	{
		AppInstance* app = AppInstance::GetSingleton();
		if (app) src = app->ResolveContent(id);
	}
	boost::system::error_code ec;
	boost::filesystem::create_directories(boost::filesystem::path(dstFile).parent_path(), ec);
	Jobs::Schedule([kind, src, dstFile]()
	{
		// Exports serialize like imports: two racing over one destination folder would collide.
		static boost::mutex exportLock;
		boost::mutex::scoped_lock l(exportLock);
		bool ok = false;
		if (iExchange* svc = GetService<iExchange>()) ok = svc->Export(kind.c_str(), src.c_str(), dstFile.c_str());
		Events::EmitEngine("exchange.exported", dstFile + (ok ? "|ok" : "|failed"));
	});
	return true;   // queued; the event says how it went
}

std::string Exchange::ImportExtensions()
{
	std::string s;
	auto add = [&](const std::vector<std::string>& v) { for (const std::string& e : v) { if (!s.empty()) s += ";"; s += e; } };
	add(Importer::ModelExtensions());
	add(Importer::ImageExtensions());
	add(Importer::AudioExtensions());
	for (const AssetImporter& imp : AssetImporters()) add(imp.exts);
	return s;
}

std::string Exchange::ExportExtensions()
{
	std::string s;
	for (const std::string& e : Importer::ExportExtensions()) { if (!s.empty()) s += ";"; s += e; }
	return s;
}

}  // namespace nuke
