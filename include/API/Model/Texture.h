#pragma once
#ifndef NUKEE_TEXTURE_H
#define NUKEE_TEXTURE_H
#include "NukeAPI.h"
#include <istream>
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "reflect/Reflect.h"   // NUKE_CLASS (reflected asset)
#include "API/Model/Package.h" // pak cook layout + the header-only pak source

namespace nuke {

class NUKEENGINE_API Texture
{
    // Reflected ASSET class (scripts create/edit/assign it like any engine object).
    NUKE_CLASS(Texture, Object)
public:
    unsigned int id = 0;
    char name[64] = { 0 };
    char path[1024] = { 0 };

    std::string guid;                      // asset id
    [[nuke::prop(label="Width")]]  int width = 0;    // base (mip0) dimensions
    [[nuke::prop(label="Height")]] int height = 0;
    // Full pixel data: RAW = mip0 RGBA8 (w*h*4); BC = every mip's blocks concatenated mip0..mipN.
    std::vector<unsigned char> pixels;
    enum Format { FMT_RGBA8 = 0, FMT_BC1 = 1, FMT_BC3 = 3, FMT_BC5 = 5 };   // BC5 = 2-channel (RG), for normal maps
    int format   = FMT_RGBA8;              // texel format (BC1 = opaque, BC3 = alpha, BC5 = RG normals)
    int mipCount = 1;                       // number of mip levels stored in `pixels`
    bool healedOnLoad = false;              // transient: a pre-v11 file was re-encoded on load (re-save it as v11)

    // Semantic usage — drives color-space, compression choice and normal green-flip:
    // Color/Emissive/Sprite = sRGB source; Normal/Data = linear; Normal -> BC5.
    // Sprite additionally carries the sheet slicing (grid) metadata below.
    enum Usage { UsageColor = 0, UsageNormal = 1, UsageData = 2, UsageEmissive = 3, UsageSprite = 4 };
    [[nuke::prop(label="Usage", enum="Color,Normal,Data,Emissive,Sprite")]] Usage usage = UsageColor;   // serialized in the .nutex (v5)
    [[nuke::prop(label="Invert Green")]] bool invertGreen = true;                // normal maps only: green convention (true = OpenGL +Y, flip; false = DirectX) — .nutex v6
    // Sprite-sheet grid (Usage=Sprite): a columns×rows grid of cells, with margin/spacing in PIXELS;
    // cell size is DERIVED so the grid tiles exactly. .nutex v7 = cols/rows; v8 = margin/spacing + 9-slice.
    [[nuke::prop(label="Sprite Columns", min=1, max=256)]] int spriteColumns = 1;
    [[nuke::prop(label="Sprite Rows",    min=1, max=256)]] int spriteRows    = 1;
    // Per-side margin in PIXELS (border before the grid on each edge).
    [[nuke::prop(label="Margin Left",   min=0)]] int spriteMarginLeft   = 0;
    [[nuke::prop(label="Margin Right",  min=0)]] int spriteMarginRight  = 0;
    [[nuke::prop(label="Margin Top",    min=0)]] int spriteMarginTop    = 0;
    [[nuke::prop(label="Margin Bottom", min=0)]] int spriteMarginBottom = 0;
    [[nuke::prop(label="Sprite Spacing X", min=0)]] int spriteSpacingX = 0;   // px gap between columns
    [[nuke::prop(label="Sprite Spacing Y", min=0)]] int spriteSpacingY = 0;   // px gap between rows
    // 9-slice borders in PIXELS (fixed insets from each edge). With `nineSlice` on, EVERY sprite
    // drawing this texture stretches nine-sliced.
    [[nuke::prop(label="Nine Slice")]] bool nineSlice = false;
    [[nuke::prop(label="Slice Left",   min=0)]] int sliceLeft   = 0;
    [[nuke::prop(label="Slice Right",  min=0)]] int sliceRight  = 0;
    [[nuke::prop(label="Slice Top",    min=0)]] int sliceTop    = 0;
    [[nuke::prop(label="Slice Bottom", min=0)]] int sliceBottom = 0;

    [[nuke::func]] int  SpriteCount() const;   // spriteColumns * spriteRows (>=1)
    // Pixel rect of sprite cell `index` (row-major, 0 = top-left), accounting for margin+spacing.
    // False (outputs untouched) if the grid is degenerate. Not reflected — reference out-params
    // don't cross the script seam.
    bool SpriteCellRect(int index, int& x0, int& y0, int& cw, int& ch) const;
    [[nuke::func]] static int GuessUsage(const std::string& filename);   // filename-suffix heuristic -> Usage
    [[nuke::func]] bool Recompress(int targetFormat);     // decode mip0 -> re-encode to FMT_BC1/BC3/BC5 (inspector override)
    // Background removal: pixels within `tolerance` (per-channel, 0..255) of (r,g,b) become transparent.
    // outsideOnly keys only the background connected to the border (flood fill). Re-encodes BC -> BC3
    // to carry alpha; single-frame only.
    [[nuke::func]] bool ApplyChromaKey(int r, int g, int b, int tolerance, bool outsideOnly);
    // Decode mip0 (frame 0 for animated textures) to a tight width*height*4 RGBA8 buffer.
    std::vector<unsigned char> DecodeRGBA() const;

    // ---- pak-resident pixels (Fast loading 4) ----
    // A texture loaded HEADER-ONLY from a pak keeps its pixel blocks in the pak: the renderer
    // streams them straight into VRAM (DirectStorage), CPU readers pull them on demand.
    struct PakSource
    {
        std::string    pakPath;
        Package::Entry entry;    // layout kPakLayout: block 0 = header, then GPU mip blocks
    };
    std::shared_ptr<const PakSource> pakSource;   // set by ResDB; null = pixels are in `pixels`
    bool HasPixelData() const { return !pixels.empty() || pakSource != nullptr; }
    // Load `pixels` from pakSource when they are not in memory (synchronous, thread-safe).
    bool EnsurePixels();

    // Pak cook layout of a .nutex (registered with Package at startup). Block 0 = the file's
    // header bytes (meta[0] = 0xFFFFFFFF); every other block holds whole mips in the D3D12
    // placed-footprint layout (meta = {firstMip, mipCount, 0}) or ONE row range of a mip that
    // is bigger than the block cap (meta = {mip, 0, firstRow << 16 | rows}, rows in texture
    // rows for RGBA8 / block rows for BC). Pitch 256, subresource placement 512.
    enum : uint32_t { kPakLayout = 1, kPitchAlign = 256, kPlaceAlign = 512 };
    struct MipGeom { int w = 0, h = 0; uint32_t rowBytes = 0, rows = 0; };   // tight geometry of one mip
    MipGeom  MipGeometry(int mip) const;
    uint64_t MipOffset(int mip) const;    // byte offset of a mip inside `pixels`
    static bool CookPak(const std::string& rawFile, uint32_t blockCap, Package::Cooked& out);
    static bool UncookPak(const std::vector<Package::Block>& blocks, std::vector<std::string>& inflated, std::string& out);
    // Bytes a GPU block occupies in its pitched layout (what a DirectStorage request delivers).
    uint64_t PakBlockPitchedSize(const Package::Block& b) const;

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

    // Native asset format (.nutex, binary): header + decoded pixels — import decodes source
    // images into this so nothing references the source at runtime.
    bool            SaveToFile(const std::string& path) const;
    static Texture* LoadFromFile(const std::string& path);
    static Texture* LoadFromMemory(const std::string& data);   // packed content (3.2)
    static Texture* LoadFromStream(std::istream& i, bool headerOnly = false);
    // Header-only load of a cooked pak entry: pixels stay in the pak (see pakSource).
    static Texture* LoadFromPak(const Package::Location& loc);
};
}  // namespace nuke

#endif // !NUKEE_TEXTURE_H
