#include "API/Model/Shader.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <iterator>

namespace nuke {

namespace bfs = boost::filesystem;

static std::string ReadAll(const std::string& path)
{
	bfs::ifstream f(bfs::path(path), std::ios::binary);
	if (!f) return std::string();
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
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
	return s;
}
}  // namespace nuke
