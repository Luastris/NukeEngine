#pragma once
#ifndef NUKEE_TEXTURE_H
#define NUKEE_TEXTURE_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include <cstdint>

namespace nuke {

class NUKEENGINE_API Texture
{
public:
    unsigned int id = 0;
    char name[64] = { 0 };
    char path[1024] = { 0 };

    std::string guid;                      // asset id
    int width = 0, height = 0;             // base (mip0) dimensions
    // pixels = the full data: for RAW it's mip0 RGBA8 (w*h*4); for BC it's every mip's compressed
    // blocks concatenated mip0..mipN (renderer derives per-mip size from format + dims).
    std::vector<unsigned char> pixels;
    enum Format { FMT_RGBA8 = 0, FMT_BC1 = 1, FMT_BC3 = 3 };
    int format   = FMT_RGBA8;              // texel format (BC1 = opaque, BC3 = alpha)
    int mipCount = 1;                       // number of mip levels stored in `pixels`

    // RenderTexture: a GPU render target a Camera draws into + materials sample. No CPU pixels.
    bool     renderTexture = false;        // serialized in the .nutex
    uint64_t rtId = 0;                      // runtime iRender render-target id (created on load)

	Texture(char* path);
	Texture();

    // Native asset format (.nutex, binary): header + decoded RGBA8 pixels. Import decodes
    // source images (PNG/JPG/... via stb_image, incl. embedded) into this so nothing
    // references the source at runtime.
    bool            SaveToFile(const std::string& path) const;
    static Texture* LoadFromFile(const std::string& path);
};
}  // namespace nuke

#endif // !NUKEE_TEXTURE_H
