#include "API/Model/JsonDoc.h"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <cstring>
#include <iterator>

namespace nuke {

nlohmann::json ParseDoc(const std::string& bytes)
{
	if (bytes.size() > 4 && std::memcmp(bytes.data(), kCborDocMagic, 4) == 0)
		return nlohmann::json::from_cbor(bytes.begin() + 4, bytes.end(), true, false);
	return nlohmann::json::parse(bytes, nullptr, false);
}

std::string EncodeDocCbor(const nlohmann::json& doc)
{
	std::string out(kCborDocMagic, 4);
	const std::vector<std::uint8_t> cbor = nlohmann::json::to_cbor(doc);
	out.append((const char*)cbor.data(), cbor.size());
	return out;
}

bool CookDocFile(const std::string& src, const std::string& dst)
{
	namespace bfs = boost::filesystem;
	std::string bytes;
	{
		bfs::ifstream f(bfs::path(src), std::ios::binary);
		if (!f) return false;
		bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}
	nlohmann::json doc = ParseDoc(bytes);
	if (doc.is_discarded()) return false;
	const std::string cooked = EncodeDocCbor(doc);
	boost::system::error_code ec;
	const bfs::path dp(dst);
	if (dp.has_parent_path()) bfs::create_directories(dp.parent_path(), ec);
	bfs::ofstream o(dp, std::ios::binary | std::ios::trunc);
	if (!o) return false;
	o.write(cooked.data(), (std::streamsize)cooked.size());
	return (bool)o;
}

}  // namespace nuke
