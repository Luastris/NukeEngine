#include "API/Model/Storage.h"
#include "API/Model/Jobs.h"
#include <atomic>
#include <sstream>

namespace nuke {

static Storage::Provider* g_provider = nullptr;
static std::atomic<uint64_t> g_cpuReq(0), g_cpuBytes(0);

void Storage::SetProvider(Provider* p) { g_provider = p; }
Storage::Provider* Storage::GetProvider() { return g_provider; }
bool Storage::GpuTextures() { return g_provider && g_provider->GpuTextures(); }

bool Storage::TryProvider(const std::string& rel, int priority, const ReadDone& done)
{
	Package::Location loc;
	return g_provider && Package::Locate(rel, loc) && g_provider->ReadAsync(loc, priority, done);
}

void Storage::ReadAsync(const std::string& rel, int priority, const ReadDone& done)
{
	if (TryProvider(rel, priority, done)) return;
	// CPU path: the layer stack resolves again inside Read (raw overlay first). Inline once the
	// pool is stopping — a queued job would never run.
	auto read = [rel, done]()
	{
		std::string bytes;
		const bool ok = Package::Read(rel, bytes);
		++g_cpuReq; g_cpuBytes += bytes.size();
		done(ok, bytes);
	};
	if (Jobs::Stopping() || Jobs::WorkerCount() == 0) read(); else Jobs::Schedule(read);
}

void Storage::Flush() { if (g_provider) g_provider->Flush(); }

Storage::Stats Storage::GetStats()
{
	Stats s;
	s.cpuRequests = g_cpuReq; s.cpuBytes = g_cpuBytes;
	if (g_provider)
	{
		g_provider->Stats(s.providerRequests, s.providerBytes, s.gpuDecompression);
		s.provider = g_provider->Name();
	}
	return s;
}

std::string Storage::Describe()
{
	Stats s = GetStats();
	std::ostringstream o;
	if (g_provider)
		o << s.provider << ": " << s.providerRequests << " requests, " << (s.providerBytes >> 20) << " MB"
		  << (s.gpuDecompression ? " (GPU decompression)" : " (CPU decompression)") << "; ";
	o << "cpu: " << s.cpuRequests << " requests, " << (s.cpuBytes >> 20) << " MB";
	return o.str();
}

}  // namespace nuke
