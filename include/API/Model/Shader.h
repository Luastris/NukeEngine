#pragma once
#ifndef NUKEE_SHADER_H
#define NUKEE_SHADER_H
#include "NukeAPI.h"
#include <string>
#include <cstdint>
#include <ctime>
#include <vector>

namespace nuke {

// One custom material parameter, parsed from the shader's `cbuffer MatCB` (excludes the standard
// g_Color/g_Params). `offset` is the byte offset inside MatCB (HLSL packing); `components` 1..4.
struct ShaderProp
{
	std::string name;
	int         components = 1;
	uint32_t    offset     = 0;
	float       def[4]     = { 0, 0, 0, 0 };   // default values (parsed from the HLSL initializer)
};

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
	std::vector<ShaderProp> props;    // custom MatCB params parsed from psSource (for material inspector)

	// Build a Shader from a VS/PS file pair. Returns nullptr if either file can't be read.
	static Shader* LoadPair(const std::string& name, const std::string& vsPath, const std::string& psPath);
	// Parse the `cbuffer MatCB { ... }` block of a pixel shader into ShaderProp entries (engine-side
	// reflection from source text; the renderer never reads shader files). Excludes g_Color/g_Params.
	static void ParseMatCBProps(const std::string& psSource, std::vector<ShaderProp>& out);
};
}  // namespace nuke

#endif // !NUKEE_SHADER_H
