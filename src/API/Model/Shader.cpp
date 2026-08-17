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

// Expand `#include "file"` textually (one level) so the PROP PARSER sees included cbuffer
// fields (matcb_std.hlsli). The GPU compiler never sees this — it resolves includes itself
// through the renderer's shader stream factory. Search: next to the source file (LoadPair
// passes psPath's dir via the thread-local below), then the exe-relative shaders/ dir.
static thread_local std::string s_parseDir;   // include search dir for the CURRENT parse
// The concatenated text of every include spliced in by the last ExpandIncludes call: prop
// declarations found in it are library plumbing (matcb_std), not the shader's own params.
static thread_local std::string s_includedText;
static std::string ExpandIncludes(const std::string& text, const std::string& selfDir)
{
	s_includedText.clear();
	if (text.find("#include") == std::string::npos) return text;
	std::string out; out.reserve(text.size() + 4096);
	std::istringstream is(text);
	std::string line;
	while (std::getline(is, line))
	{
		const size_t h = line.find("#include");
		size_t q1 = std::string::npos, q2 = std::string::npos;
		if (h != std::string::npos && line.find("//") >= h)
		{ q1 = line.find('"', h); if (q1 != std::string::npos) q2 = line.find('"', q1 + 1); }
		if (q2 != std::string::npos)
		{
			const std::string name = line.substr(q1 + 1, q2 - q1 - 1);
			std::string inc;
			if (!selfDir.empty()) inc = ReadAll(selfDir + "/" + name);
			if (inc.empty())      inc = ReadAll("shaders/" + name);
			if (!inc.empty()) { out += inc; out += '\n'; s_includedText += inc; s_includedText += '\n'; continue; }
		}
		out += line; out += '\n';
	}
	return out;
}

// `ident` is DECLARED (word-boundary match outside // comments) somewhere in the include text.
static bool DeclaredInIncludes(const std::string& ident)
{
	const std::string& t = s_includedText;
	for (size_t pos = t.find(ident); pos != std::string::npos; pos = t.find(ident, pos + 1))
	{
		if (pos > 0 && (std::isalnum((unsigned char)t[pos - 1]) || t[pos - 1] == '_')) continue;
		const size_t e = pos + ident.size();
		if (e < t.size() && (std::isalnum((unsigned char)t[e]) || t[e] == '_')) continue;
		const size_t bol = t.rfind('\n', pos);
		const size_t cmt = t.find("//", bol == std::string::npos ? 0 : bol);
		if (cmt != std::string::npos && cmt < pos) continue;   // only mentioned in a comment
		return true;
	}
	return false;
}
static std::string DirOf(const std::string& p)
{
	const size_t s = p.find_last_of("/\\");
	return s == std::string::npos ? std::string() : p.substr(0, s);
}

void Shader::ParseCBProps(const std::string& rawSrc, const char* cbName, std::vector<ShaderProp>& out)
{
	const std::string src = ExpandIncludes(rawSrc, s_parseDir);
	out.clear();
	const bool isMat = std::string(cbName) == "MatCB";
	size_t cb = src.find(std::string("cbuffer ") + cbName);
	if (cb == std::string::npos) return;
	size_t open = src.find('{', cb);
	size_t close = (open == std::string::npos) ? std::string::npos : src.find('}', open);
	if (close == std::string::npos) return;
	std::string raw = src.substr(open + 1, close - open - 1);
	// Strip // comments first: one sitting between a ';' and the next declaration would
	// otherwise be swallowed together with that declaration.
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
		// MatCB's standard lit fields are renderer-driven, not editable custom props.
		if (!isMat || (ident != "g_Color" && ident != "g_Params" && ident != "g_Params2" && ident != "g_Emissive2"))
			out.push_back(sp);
	}

	// A 3/4-component prop whose declaration line carries `// @color` gets a colour picker.
	// Scanned from the un-stripped source, where the comment still exists.
	for (ShaderProp& sp : out)
	{
		if (sp.components < 3) continue;
		for (size_t pos = raw.find(sp.name); pos != std::string::npos; pos = raw.find(sp.name, pos + 1))
		{
			size_t eol = raw.find('\n', pos);
			std::string line = raw.substr(pos, (eol == std::string::npos ? raw.size() : eol) - pos);
			if (line.find("@color") != std::string::npos) { sp.isColor = true; break; }
		}
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
	s_parseDir = DirOf(psPath);
	ParseMatCBProps(s->psSource, s->props);   // engine-side reflection from the source text
	s_parseDir.clear();
	for (const ShaderProp& p : s->props)
		if (DeclaredInIncludes(p.name)) s->includeProps.push_back(p.name);
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

// Build a shader from source text (packed content); empty paths disable hot-reload.
Shader* Shader::FromSources(const std::string& name, const std::string& vsSrc, const std::string& psSrc)
{
	if (vsSrc.empty() || psSrc.empty()) return nullptr;
	Shader* s = new Shader();
	s->guid = name; s->name = name;
	s->vsSource = vsSrc; s->psSource = psSrc;
	ParseMatCBProps(s->psSource, s->props);
	for (const ShaderProp& p : s->props)
		if (DeclaredInIncludes(p.name)) s->includeProps.push_back(p.name);
	return s;
}

Shader* Shader::PostFromSource(const std::string& name, const std::string& psSrc)
{
	if (psSrc.empty()) return nullptr;
	Shader* s = new Shader();
	s->guid = name; s->name = name; s->isPost = true;
	s->psSource = psSrc;
	ParseCBProps(s->psSource, "PostParams", s->props);
	return s;
}
}  // namespace nuke
