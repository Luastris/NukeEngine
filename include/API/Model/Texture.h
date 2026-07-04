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
    enum Format { FMT_RGBA8 = 0, FMT_BC1 = 1, FMT_BC3 = 3, FMT_BC5 = 5 };   // BC5 = 2-channel (RG), for normal maps
    int format   = FMT_RGBA8;              // texel format (BC1 = opaque, BC3 = alpha, BC5 = RG normals)
    int mipCount = 1;                       // number of mip levels stored in `pixels`

    // Semantic usage (drives color-space, compression choice, normal green-flip). Set at import — from the assimp
    // texture type (model import) or a filename-suffix heuristic (bare image drop/picker); overridable in the asset
    // inspector. Color/Emissive = sRGB source; Normal/Data = linear; Normal -> BC5.
    enum Usage { UsageColor = 0, UsageNormal = 1, UsageData = 2, UsageEmissive = 3 };
    int  usage = UsageColor;                // serialized in the .nutex (v5)
    bool invertGreen = true;                // normal maps only: green convention (true = OpenGL +Y, flip; false = DirectX) — .nutex v6
    static int GuessUsage(const std::string& filename);   // filename-suffix heuristic -> Usage
    bool Recompress(int targetFormat);                    // decode mip0 -> re-encode to FMT_BC1/BC3/BC5 (inspector override)
    // Decode mip0 (frame 0 for animated textures) to a tight width*height*4 RGBA8 buffer —
    // CPU-side preview/inspection (the editor uploads it via iRender::createTexture2D).
    std::vector<unsigned char> DecodeRGBA() const;

    // Animation (GIF): pixels holds `frameCount` frames back-to-back (RGBA8, w*h*4 each, no mips/BC).
    int              frameCount = 1;
    std::vector<int> frameDelaysMs;        // per-frame delay (ms); size == frameCount (empty => 100ms)
    int    curFrame   = 0;                  // runtime: current frame, advanced by World (time-based)
    double animTimeMs = 0.0;                // runtime: time accumulator within the current frame

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
