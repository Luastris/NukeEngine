#include "API/Model/Texture.h"
#include <sstream>
#include <boost/filesystem/fstream.hpp>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cmath>
#include <vector>
#include <initializer_list>
#include <stb_dxt.h>   // BC encode (stb_compress_*); implementation lives in assimporter.cpp (same binary)

namespace nuke {

// ---- BC decode (compressed mip0 -> RGBA), for re-compressing to another format in the inspector ----
namespace {
inline void bc1Block(const uint8_t* s, uint8_t out[64])
{
	uint16_t c0 = (uint16_t)(s[0] | (s[1] << 8)), c1 = (uint16_t)(s[2] | (s[3] << 8));
	uint8_t col[4][4];
	auto exp = [](uint16_t c, uint8_t* o) { o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31); o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); o[3] = 255; };
	exp(c0, col[0]); exp(c1, col[1]);
	for (int k = 0; k < 3; ++k)
	{
		if (c0 > c1) { col[2][k] = (uint8_t)((2 * col[0][k] + col[1][k]) / 3); col[3][k] = (uint8_t)((col[0][k] + 2 * col[1][k]) / 3); }
		else         { col[2][k] = (uint8_t)((col[0][k] + col[1][k]) / 2); col[3][k] = 0; }
	}
	col[2][3] = 255; col[3][3] = (c0 > c1) ? 255 : 0;
	uint32_t idx = (uint32_t)(s[4] | (s[5] << 8) | (s[6] << 16) | (s[7] << 24));
	for (int p = 0; p < 16; ++p) { int i = (idx >> (p * 2)) & 3; out[p * 4] = col[i][0]; out[p * 4 + 1] = col[i][1]; out[p * 4 + 2] = col[i][2]; out[p * 4 + 3] = col[i][3]; }
}
inline void bc4Chan(const uint8_t* s, uint8_t* out /*64*/, int ofs)   // one channel (BC4) into out[p*4+ofs]
{
	uint8_t a0 = s[0], a1 = s[1], a[8]; a[0] = a0; a[1] = a1;
	if (a0 > a1) for (int i = 1; i < 7; ++i) a[i + 1] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
	else { for (int i = 1; i < 5; ++i) a[i + 1] = (uint8_t)(((5 - i) * a0 + i * a1) / 5); a[6] = 0; a[7] = 255; }
	uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= (uint64_t)s[2 + i] << (8 * i);
	for (int p = 0; p < 16; ++p) out[p * 4 + ofs] = a[(bits >> (p * 3)) & 7];
}
inline void bc3Block(const uint8_t* s, uint8_t out[64]) { bc1Block(s + 8, out); bc4Chan(s, out, 3); }   // color + alpha
inline void bc5Block(const uint8_t* s, uint8_t out[64])
{
	for (int p = 0; p < 16; ++p) { out[p * 4 + 2] = 0; out[p * 4 + 3] = 255; }
	bc4Chan(s, out, 0); bc4Chan(s + 8, out, 1);   // R, G
	for (int p = 0; p < 16; ++p)   // reconstruct B = normal z (BC5 is normals-only)
	{
		float x = out[p * 4] / 255.0f * 2 - 1, y = out[p * 4 + 1] / 255.0f * 2 - 1;
		float z = std::sqrt(std::max(0.0f, 1.0f - x * x - y * y));
		out[p * 4 + 2] = (uint8_t)((z * 0.5f + 0.5f) * 255.0f);
	}
}
// Decode this texture's mip0 to a tight w*h*4 RGBA buffer.
std::vector<uint8_t> decodeMip0(const Texture* t)
{
	const int w = t->width, h = t->height;
	std::vector<uint8_t> out((size_t)w * h * 4, 255);
	if (t->format == Texture::FMT_RGBA8) { if (t->pixels.size() >= (size_t)w * h * 4) memcpy(out.data(), t->pixels.data(), (size_t)w * h * 4); return out; }
	const int bb = (t->format == Texture::FMT_BC1) ? 8 : 16;
	const int bx = (w + 3) / 4, by = (h + 3) / 4;
	const uint8_t* p = t->pixels.data();
	for (int byi = 0; byi < by; ++byi)
		for (int bxi = 0; bxi < bx; ++bxi, p += bb)
		{
			uint8_t blk[64];
			if      (t->format == Texture::FMT_BC1) bc1Block(p, blk);
			else if (t->format == Texture::FMT_BC3) bc3Block(p, blk);
			else if (t->format == Texture::FMT_BC5) bc5Block(p, blk);
			else break;
			for (int py = 0; py < 4; ++py)
				for (int px = 0; px < 4; ++px)
				{
					int x = bxi * 4 + px, y = byi * 4 + py; if (x >= w || y >= h) continue;
					memcpy(&out[((size_t)y * w + x) * 4], &blk[(py * 4 + px) * 4], 4);
				}
		}
	return out;
}
// Encode an RGBA buffer (w,h multiples of 4) to BC1/BC3/BC5 with a box mip chain. Returns mip count; fills `bytes`.
int encodeBC(std::vector<uint8_t>& bytes, std::vector<uint8_t> cur, int w, int h, int fmt)
{
	bytes.clear();
	const bool bc5 = (fmt == Texture::FMT_BC5), bc3 = (fmt == Texture::FMT_BC3);
	const int blockBytes = (fmt == Texture::FMT_BC1) ? 8 : 16;
	int mips = 0;
	while (true)
	{
		const int bx = (w + 3) / 4, by = (h + 3) / 4;
		size_t base = bytes.size(); bytes.resize(base + (size_t)bx * by * blockBytes);
		uint8_t* dst = bytes.data() + base;
		for (int byi = 0; byi < by; ++byi)
			for (int bxi = 0; bxi < bx; ++bxi, dst += blockBytes)
			{
				uint8_t block[64], R[16], G[16];
				for (int py = 0; py < 4; ++py)
					for (int px = 0; px < 4; ++px)
					{
						int sx = bxi * 4 + px; if (sx >= w) sx = w - 1;
						int sy = byi * 4 + py; if (sy >= h) sy = h - 1;
						const uint8_t* s = &cur[((size_t)sy * w + sx) * 4];
						memcpy(&block[(py * 4 + px) * 4], s, 4); R[py * 4 + px] = s[0]; G[py * 4 + px] = s[1];
					}
				if (bc5) { stb_compress_bc4_block(dst, R); stb_compress_bc4_block(dst + 8, G); }
				else     stb_compress_dxt_block(dst, block, bc3 ? 1 : 0, STB_DXT_NORMAL);
			}
		++mips;
		if (w == 1 && h == 1) break;
		int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
		std::vector<uint8_t> nx((size_t)nw * nh * 4);
		for (int y = 0; y < nh; ++y)
			for (int x = 0; x < nw; ++x)
			{
				int x0 = x * 2, y0 = y * 2, x1 = (x * 2 + 1 < w) ? x * 2 + 1 : x0, y1 = (y * 2 + 1 < h) ? y * 2 + 1 : y0;
				for (int c = 0; c < 4; ++c)
					nx[((size_t)y * nw + x) * 4 + c] = (uint8_t)((cur[((size_t)y0 * w + x0) * 4 + c] + cur[((size_t)y0 * w + x1) * 4 + c] + cur[((size_t)y1 * w + x0) * 4 + c] + cur[((size_t)y1 * w + x1) * 4 + c]) / 4);
			}
		cur.swap(nx); w = nw; h = nh;
	}
	return mips;
}
}  // namespace

// Re-compress this texture to another BC format in place (decode current mip0 -> re-encode). For the inspector's
// Compression override (e.g. a normal map BC5<->BC1: quality vs half the size). No-op for render textures / GIFs.
std::vector<unsigned char> Texture::DecodeRGBA() const
{
	if (renderTexture || width <= 0 || height <= 0) return {};
	return decodeMip0(this);   // RGBA8 passthrough (frame 0) or BC decode
}

bool Texture::Recompress(int targetFormat)
{
	if (renderTexture || frameCount > 1 || width <= 0 || height <= 0) return false;
	if (targetFormat != FMT_BC1 && targetFormat != FMT_BC3 && targetFormat != FMT_BC5) return false;
	if (targetFormat == format) return false;
	std::vector<uint8_t> rgba = decodeMip0(this);   // width/height already a multiple of 4 (padded at import)
	std::vector<uint8_t> enc;
	mipCount = encodeBC(enc, std::move(rgba), width, height, targetFormat);
	pixels.swap(enc);
	format = targetFormat;
	return true;
}

namespace bfs = boost::filesystem;

Texture::Texture(char* path) {
	strcpy(this->path, path);
}

Texture::Texture() {}

// Guess a texture's semantic usage from its filename suffix/tokens (bare-image import has no other signal).
// Tokenizes the stem on _ - . space and matches known role tokens; priority Normal > Emissive > Data > Color.
int Texture::GuessUsage(const std::string& filename)
{
	// stem: strip directory then extension, lowercase.
	size_t slash = filename.find_last_of("/\\");
	std::string s = (slash == std::string::npos) ? filename : filename.substr(slash + 1);
	size_t dot = s.find_last_of('.');
	if (dot != std::string::npos) s = s.substr(0, dot);
	for (char& c : s) c = (char)std::tolower((unsigned char)c);

	// tokenize
	std::vector<std::string> tok; std::string cur;
	for (char c : s) { if (c == '_' || c == '-' || c == '.' || c == ' ') { if (!cur.empty()) { tok.push_back(cur); cur.clear(); } } else cur += c; }
	if (!cur.empty()) tok.push_back(cur);

	auto has = [&](std::initializer_list<const char*> set) {
		for (const std::string& t : tok) for (const char* k : set) if (t == k) return true;
		return false;
	};
	if (has({ "normal", "normalmap", "norm", "nor", "nrm", "nml", "n" }))                                   return UsageNormal;
	if (has({ "emissive", "emission", "emit", "emis", "ems", "glow" }))                                      return UsageEmissive;
	if (has({ "metallic", "metalness", "metal", "roughness", "rough", "mr", "orm", "rma", "arm", "ao",
	          "occlusion", "occ", "gloss", "glossiness", "mask", "height", "disp", "displacement", "bump" })) return UsageData;
	return UsageColor;
}

namespace {
	const char kMagic[8] = { 'N','U','T','E','X','\0','\0','\0' };
	const uint32_t kVersion = 6;   // v2: rt; v3: format+mips; v4: frames; v5: usage; v6: invertGreen
}

bool Texture::SaveToFile(const std::string& path) const
{
	bfs::path p(path);
	bfs::ofstream o(p, std::ios::binary);
	if (!o) return false;
	o.write(kMagic, 8);
	o.write((const char*)&kVersion, sizeof(kVersion));
	uint32_t glen = (uint32_t)guid.size(); o.write((const char*)&glen, 4); if (glen) o.write(guid.data(), glen);
	int32_t w = width, h = height; o.write((const char*)&w, 4); o.write((const char*)&h, 4);
	uint8_t rt = renderTexture ? 1 : 0; o.write((const char*)&rt, 1);   // v2
	int32_t fmt = format, mips = mipCount; o.write((const char*)&fmt, 4); o.write((const char*)&mips, 4);   // v3
	int32_t fc = frameCount < 1 ? 1 : frameCount; o.write((const char*)&fc, 4);                            // v4
	for (int k = 0; k < fc; ++k) { int32_t d = (k < (int)frameDelaysMs.size()) ? frameDelaysMs[k] : 100; o.write((const char*)&d, 4); }
	int32_t u = usage; o.write((const char*)&u, 4);                                                         // v5
	uint8_t ig = invertGreen ? 1 : 0; o.write((const char*)&ig, 1);                                          // v6
	uint32_t bytes = (uint32_t)pixels.size(); o.write((const char*)&bytes, 4);
	if (bytes) o.write((const char*)pixels.data(), bytes);
	return (bool)o;
}

Texture* Texture::LoadFromFile(const std::string& path)
{
	bfs::ifstream i(bfs::path(path), std::ios::binary);
	if (!i) return nullptr;
	return LoadFromStream(i);
}

Texture* Texture::LoadFromMemory(const std::string& data)
{
	std::istringstream i(data, std::ios::binary);
	return LoadFromStream(i);
}

Texture* Texture::LoadFromStream(std::istream& i)
{
	char magic[8]; i.read(magic, 8);
	if (memcmp(magic, kMagic, 8) != 0) return nullptr;
	uint32_t version = 0; i.read((char*)&version, sizeof(version)); (void)version;
	Texture* t = new Texture();
	uint32_t glen = 0; i.read((char*)&glen, 4);
	if (glen) { std::string g(glen, '\0'); i.read(&g[0], glen); t->guid = g; }
	int32_t w = 0, h = 0; i.read((char*)&w, 4); i.read((char*)&h, 4);
	t->width = w; t->height = h;
	if (version >= 2) { uint8_t rt = 0; i.read((char*)&rt, 1); t->renderTexture = (rt != 0); }
	if (version >= 3) { int32_t fmt = 0, mips = 1; i.read((char*)&fmt, 4); i.read((char*)&mips, 4); t->format = fmt; t->mipCount = mips < 1 ? 1 : mips; }
	if (version >= 4)
	{
		int32_t fc = 1; i.read((char*)&fc, 4); t->frameCount = fc < 1 ? 1 : fc;
		t->frameDelaysMs.resize(t->frameCount);
		for (int k = 0; k < t->frameCount; ++k) { int32_t d = 100; i.read((char*)&d, 4); t->frameDelaysMs[k] = d; }
	}
	if (version >= 5) { int32_t u = 0; i.read((char*)&u, 4); t->usage = u; }   // v5: semantic usage
	if (version >= 6) { uint8_t ig = 1; i.read((char*)&ig, 1); t->invertGreen = (ig != 0); }   // v6: normal green convention
	uint32_t bytes = 0; i.read((char*)&bytes, 4);
	if (bytes) { t->pixels.resize(bytes); i.read((char*)t->pixels.data(), bytes); }
	if (!i && !i.eof()) { delete t; return nullptr; }
	return t;
}
}  // namespace nuke
