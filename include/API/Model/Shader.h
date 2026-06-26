#pragma once
#ifndef NUKEE_SHADER_H
#define NUKEE_SHADER_H
#include "NukeAPI.h"
#include <string>
#include <cstdint>
#include <ctime>

namespace nuke {

// A shader asset: a VS + PS source pair (HLSL). Loaded from "<name>.vs.hlsl" + "<name>.ps.hlsl"
// pairs in two roots — the engine's built-in `shaders/` and the project's content folder.
// Materials reference one by GUID (default = the engine "world" shader). The renderer will
// build a pipeline per shader (next step); for now these are registered as ResDB assets.
class NUKEENGINE_API Shader
{
public:
	std::string guid;          // asset id (== name; "world" is the engine default)
	std::string name;
	std::string vsSource;      // HLSL vertex shader source
	std::string psSource;      // HLSL pixel shader source
	std::string vsPath, psPath;// source file paths (for hot-reload)
	std::time_t vsTime = 0, psTime = 0;   // last-write times (hot-reload change detection)
	uint64_t    rendererHandle = 0;   // renderer pipeline handle (0 until built)

	// Build a Shader from a VS/PS file pair. Returns nullptr if either file can't be read.
	static Shader* LoadPair(const std::string& name, const std::string& vsPath, const std::string& psPath);
};
}  // namespace nuke

#endif // !NUKEE_SHADER_H
