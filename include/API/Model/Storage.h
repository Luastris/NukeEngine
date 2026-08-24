#pragma once
#ifndef NUKEE_STORAGE_H
#define NUKEE_STORAGE_H
#include "NukeAPI.h"
#include "API/Model/Package.h"
#include <cstdint>
#include <functional>
#include <string>

namespace nuke {
class Texture;

// Direct IO for packed content (Fast loading 4). Reads of pak entries go to a PROVIDER when one
// is registered — the D3D12 renderer's DirectStorage queue: NVMe request queue past the Win32
// file stack, GDeflate inflated on the GPU, textures landing straight in VRAM. Without a
// provider (Vulkan/D3D11, other platforms, dev trees) the same calls run on the Jobs pool
// through Package. Callers never know which one served them.
class NUKEENGINE_API Storage
{
public:
	enum Priority { Low = -1, Normal = 0, High = 1, Realtime = 2 };
	// Completion: `bytes` = the ORIGINAL file bytes (cooks undone). Runs on an arbitrary thread.
	typedef std::function<void(bool ok, std::string& bytes)> ReadDone;

	class Provider
	{
	public:
		virtual ~Provider() {}
		virtual const char* Name() const = 0;
		// Serve a located pak entry to memory. False = cannot take it (the CPU path runs instead).
		virtual bool ReadAsync(const Package::Location& loc, int priority, const ReadDone& done) = 0;
		// True when the renderer streams cooked textures (Texture::kPakLayout) into VRAM itself:
		// the content scan then loads such textures header-only.
		virtual bool GpuTextures() const { return false; }
		// A header-only texture just registered by the content scan: bulk-load its mips at low
		// priority (the whole content set rides the NVMe queue while the world stages).
		virtual void PrefetchTexture(Texture* t) { (void)t; }
		virtual void Flush() {}   // submit what was queued so far
		// Requests and bytes served since init, and whether the GPU inflates (else CPU threads).
		virtual void Stats(uint64_t& requests, uint64_t& bytes, bool& gpuDecompression) const
		{ requests = 0; bytes = 0; gpuDecompression = false; }
	};
	static void      SetProvider(Provider* p);   // the renderer registers at init, clears at deinit
	static Provider* GetProvider();
	static bool      GpuTextures();

	// Read a project-relative path asynchronously: the provider for pak entries, the Jobs
	// pool for raw files and when no provider is present. `done` always fires exactly once.
	static void ReadAsync(const std::string& rel, int priority, const ReadDone& done);
	// Provider only: true = accepted (`done` will fire from the provider's thread); false = no
	// provider / not a pak entry / refused — the caller reads on its own thread instead.
	static bool TryProvider(const std::string& rel, int priority, const ReadDone& done);
	static void Flush();

	// Counters since boot (status line / probes): what the provider served vs the CPU path.
	struct Stats
	{
		uint64_t providerRequests = 0, providerBytes = 0;
		uint64_t cpuRequests = 0, cpuBytes = 0;
		bool     gpuDecompression = false;
		const char* provider = "";
	};
	static Stats GetStats();
	static std::string Describe();   // one line for logs / Game.StorageInfo
};

}  // namespace nuke

#endif // !NUKEE_STORAGE_H
