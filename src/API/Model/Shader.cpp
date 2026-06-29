#include "API/Model/Shader.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iterator>
#include <sstream>
#include <cctype>

namespace nuke {

namespace bfs = boost::filesystem;

static std::string ReadAll(const std::string& path)
{
	bfs::ifstream f(bfs::path(path), std::ios::binary);
	if (!f) return std::string();
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// float/float2/.. -> component count (0 = not a supported scalar/vector type, skip).
static int CompsOf(const std::string& t)
{
	if (t == "float" || t == "int" || t == "uint" || t == "bool") return 1;
	if (t == "float2" || t == "int2") return 2;
	if (t == "float3" || t == "int3") return 3;
	if (t == "float4" || t == "int4") return 4;
	return 0;
}

void Shader::ParseCBProps(const std::string& src, const char* cbName, std::vector<ShaderProp>& out)
{
	out.clear();
	const bool isMat = std::string(cbName) == "MatCB";
	size_t cb = src.find(std::string("cbuffer ") + cbName);
	if (cb == std::string::npos) return;
	size_t open = src.find('{', cb);
	size_t close = (open == std::string::npos) ? std::string::npos : src.find('}', open);
	if (close == std::string::npos) return;
	std::string raw = src.substr(open + 1, close - open - 1);
	// Strip // line comments up front: one can sit between a ';' and the next declaration, so
	// trimming per-statement would also eat the following field. Comments end at the newline.
	std::string body; body.reserve(raw.size());
	for (size_t i = 0; i < raw.size(); )
	{
		if (raw[i] == '/' && i + 1 < raw.size() && raw[i + 1] == '/')
			while (i < raw.size() && raw[i] != '\n') ++i;
		else body += raw[i++];
	}

	uint32_t off = 0;   // running byte offset; follows HLSL cbuffer packing
	size_t p = 0;
	while (p < body.size())
	{
		size_t sc = body.find(';', p);
		if (sc == std::string::npos) break;
		std::string stmt = body.substr(p, sc - p);
		p = sc + 1;
		std::string init;
		size_t eq = stmt.find('=');
		if (eq != std::string::npos) { init = stmt.substr(eq + 1); stmt = stmt.substr(0, eq); }  // split initializer
		std::istringstream is(stmt);
		std::string type, name;
		if (!(is >> type >> name)) continue;
		int n = CompsOf(type);
		if (n == 0) continue;                                  // unknown type (or a comment line)
		std::string ident;                                     // keep only the identifier chars of `name`
		for (char c : name) { if (std::isalnum((unsigned char)c) || c == '_') ident += c; else break; }
		if (ident.empty()) continue;
		if ((off % 16) + (uint32_t)n * 4 > 16) off = (off + 15u) & ~15u;   // can't straddle a 16-byte register
		ShaderProp sp; sp.name = ident; sp.components = n; sp.offset = off;
		off += (uint32_t)n * 4;
		// Default values: pull any numeric literals out of the initializer (= 1.0 / = float3(1,0.5,0)).
		if (!init.empty())
		{
			for (char& c : init)
				if (!(std::isdigit((unsigned char)c) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) c = ' ';
			std::istringstream ns(init);
			float f; int di = 0;
			while (di < 4 && (ns >> f)) sp.def[di++] = f;
		}
		// In MatCB the standard lit fields are renderer-driven (material params), NOT editable custom props —
		// else the prop loop overwrites them with 0 and kills emissive / metal-rough / AO maps. PostParams has
		// no such reserved fields (every member is a user param).
		if (!isMat || (ident != "g_Color" && ident != "g_Params" && ident != "g_Params2" && ident != "g_Emissive2"))
			out.push_back(sp);
	}
}

Shader* Shader::LoadPair(const std::string& name, const std::string& vsPath, const std::string& psPath)
{
	std::string vs = ReadAll(vsPath);
	std::string ps = ReadAll(psPath);
	if (vs.empty() || ps.empty()) return nullptr;
	Shader* s = new Shader();
	s->guid     = name;
	s->name     = name;
	s->vsSource = vs;
	s->psSource = ps;
	s->vsPath   = vsPath;
	s->psPath   = psPath;
	boost::system::error_code ec;
	s->vsTime = bfs::last_write_time(bfs::path(vsPath), ec);
	s->psTime = bfs::last_write_time(bfs::path(psPath), ec);
	ParseMatCBProps(s->psSource, s->props);   // engine-side reflection from the source text
	return s;
}

Shader* Shader::LoadPostShader(const std::string& name, const std::string& psPath)
{
	std::string ps = ReadAll(psPath);
	if (ps.empty()) return nullptr;
	Shader* s = new Shader();
	s->guid = name; s->name = name; s->isPost = true;
	s->psSource = ps; s->psPath = psPath;          // vsSource stays empty: the renderer uses the built-in post.vs
	boost::system::error_code ec;
	s->psTime = bfs::last_write_time(bfs::path(psPath), ec);
	ParseCBProps(s->psSource, "PostParams", s->props);
	return s;
}
}  // namespace nuke
