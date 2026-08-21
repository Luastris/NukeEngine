#pragma once
#ifndef NUKEE_SHADER_H
#define NUKEE_SHADER_H
#include "NukeAPI.h"
#include <string>
#include <cstdint>
#include <ctime>
#include <vector>
#include "reflect/Reflect.h"   // NUKE_CLASS (reflected asset)

namespace nuke {

// One custom material parameter, parsed from the shader's `cbuffer MatCB` (excludes the standard
// g_Color/g_Params). `offset` is the byte offset inside MatCB (HLSL packing); `components` 1..4.
struct ShaderProp
{
	std::string name;
	int         components = 1;
	uint32_t    offset     = 0;
	float       def[4]     = { 0, 0, 0, 0 };   // default values (parsed from the HLSL initializer)
	bool        isColor    = false;            // `// @color` annotation -> inspector shows a (HDR) color picker, not a vector
};

// A shader asset: a VS + PS HLSL source pair, loaded from "<name>.vs.hlsl" + "<name>.ps.hlsl"
// in the engine's built-in `shaders/` and the project content folder. Materials reference one
// by GUID (default = the engine "world" shader).
class NUKEENGINE_API Shader
{
	// Reflected ASSET class (scripts look shaders up by name and assign them to materials).
	NUKE_CLASS(Shader, Object)
public:
	std::string guid;          // asset id (== name; "world" is the engine default)
	[[nuke::prop(label="Name")]] std::string name;
	std::string vsSource;      // HLSL vertex shader source
	std::string psSource;      // HLSL pixel shader source
	std::string vsPath, psPath;// source file paths (for hot-reload)
	std::time_t vsTime = 0, psTime = 0;   // last-write times (hot-reload change detection)
	uint64_t    rendererHandle = 0;   // renderer pipeline handle (0 until built)
	[[nuke::prop(label="Post Effect")]] bool isPost = false;   // post-process effect shader (fullscreen PS over a "PostParams" cbuffer)
	std::vector<ShaderProp> props;    // custom params parsed from psSource (MatCB for materials, PostParams for post)

	// Build a Shader from a VS/PS file pair. Returns nullptr if either file can't be read.
	static Shader* LoadPair(const std::string& name, const std::string& vsPath, const std::string& psPath);
	// Build a POST effect shader from a single fullscreen ".post.hlsl" PS (paired with the built-in
	// post.vs); params come from its `cbuffer PostParams`.
	static Shader* LoadPostShader(const std::string& name, const std::string& psPath);
	// Packed content (3.2): construct from source text (pak entries; no hot-reload paths).
	static Shader* FromSources(const std::string& name, const std::string& vsSrc, const std::string& psSrc);
	static Shader* PostFromSource(const std::string& name, const std::string& psSrc);
	// Parse a named `cbuffer` block of a pixel shader into ShaderProp entries. For MatCB the
	// standard lit fields (g_Color/g_Params/...) are excluded.
	static void ParseCBProps(const std::string& psSource, const char* cbName, std::vector<ShaderProp>& out);
	static void ParseMatCBProps(const std::string& psSource, std::vector<ShaderProp>& out) { ParseCBProps(psSource, "MatCB", out); }

	// APPEND-ONLY (module ABI): names of props declared inside #include'd files (matcb_std &
	// co) — engine-driven plumbing the editor hides from Shader Params. Offsets still count
	// them, so they stay in `props`.
	std::vector<std::string> includeProps;
	bool FromInclude(const std::string& propName) const
	{ for (const std::string& n : includeProps) if (n == propName) return true; return false; }

	// APPEND-ONLY (abi 24): optional CUSTOM tessellation stages. A shader whose vertex layout
	// or displacement differs from the world defaults (terrain splats) ships its own hull +
	// domain sources; empty = the shared world.hs/world.ds pair when the shader opts into
	// NUKE_TESS at all.
	std::string hsSource, dsSource;
	// FromSources + custom tessellation stages (module-embedded shaders).
	static Shader* FromSourcesTess(const std::string& name, const std::string& vsSrc,
	                               const std::string& psSrc, const std::string& hsSrc,
	                               const std::string& dsSrc);
};
}  // namespace nuke

#endif // !NUKEE_SHADER_H
