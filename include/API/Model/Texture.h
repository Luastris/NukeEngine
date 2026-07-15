#pragma once
#ifndef NUKEE_TEXTURE_H
#define NUKEE_TEXTURE_H
#include "NukeAPI.h"
#include <istream>
#include <string>
#include <vector>
#include <cstdint>
#include "reflect/Reflect.h"   // NUKE_CLASS (reflected asset)

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
    // pixels = the full data: for RAW it's mip0 RGBA8 (w*h*4); for BC it's every mip's compressed
    // blocks concatenated mip0..mipN (renderer derives per-mip size from format + dims).
    std::vector<unsigned char> pixels;
    enum Format { FMT_RGBA8 = 0, FMT_BC1 = 1, FMT_BC3 = 3, FMT_BC5 = 5 };   // BC5 = 2-channel (RG), for normal maps
    int format   = FMT_RGBA8;              // texel format (BC1 = opaque, BC3 = alpha, BC5 = RG normals)
    int mipCount = 1;                       // number of mip levels stored in `pixels`

    // Semantic usage (drives color-space, compression choice, normal green-flip). Set at import — from the assimp
    // texture type (model import) or a filename-suffix heuristic (bare image drop/picker); overridable in the asset
    // inspector. Color/Emissive = sRGB source; Normal/Data = linear; Normal -> BC5.
    // Sprite = a colour texture flagged as a sprite/sprite-sheet — carries slicing (grid) metadata so
    // every Sprite using it shares one setup instead of configuring each atom. Colour-space/compression
    // is like Color (sRGB, not BC5).
    enum Usage { UsageColor = 0, UsageNormal = 1, UsageData = 2, UsageEmissive = 3, UsageSprite = 4 };
    [[nuke::prop(label="Usage", enum="Color,Normal,Data,Emissive,Sprite")]] Usage usage = UsageColor;   // serialized in the .nutex (v5)
    [[nuke::prop(label="Invert Green")]] bool invertGreen = true;                // normal maps only: green convention (true = OpenGL +Y, flip; false = DirectX) — .nutex v6
    // Sprite-sheet grid (Usage=Sprite): the texture is a columns×rows grid of cells; a SpriteAnimator
    // reads this to slice frames (so the setup lives on the texture, shared by all sprites). Configured in
    // the Sprite Slicer editor (not the inspector). Margin = border in PIXELS before the first cell;
    // spacing = gap in PIXELS between cells — needed because real sheets are rarely a clean whole-texture
    // division, and ignoring padding makes frames "drift". Cell size is DERIVED so the grid tiles exactly.
    // .nutex v7 = cols/rows; v8 = margin/spacing + 9-slice.
    [[nuke::prop(label="Sprite Columns", min=1, max=256)]] int spriteColumns = 1;
    [[nuke::prop(label="Sprite Rows",    min=1, max=256)]] int spriteRows    = 1;
    // Per-side margin in PIXELS (border before the grid on each edge — draggable in the slicer, so each
    // side is independent; real sheets aren't always symmetric).
    [[nuke::prop(label="Margin Left",   min=0)]] int spriteMarginLeft   = 0;
    [[nuke::prop(label="Margin Right",  min=0)]] int spriteMarginRight  = 0;
    [[nuke::prop(label="Margin Top",    min=0)]] int spriteMarginTop    = 0;
    [[nuke::prop(label="Margin Bottom", min=0)]] int spriteMarginBottom = 0;
    [[nuke::prop(label="Sprite Spacing X", min=0)]] int spriteSpacingX = 0;   // px gap between columns
    [[nuke::prop(label="Sprite Spacing Y", min=0)]] int spriteSpacingY = 0;   // px gap between rows
    // 9-slice borders in PIXELS (fixed insets from each edge). When `nineSlice` is on, EVERY sprite
    // drawing this texture stretches nine-sliced (corners keep size, edges stretch one axis, the
    // centre both). The whole setup lives ON THE TEXTURE (the asset) — sprites just use it.
    [[nuke::prop(label="Nine Slice")]] bool nineSlice = false;
    [[nuke::prop(label="Slice Left",   min=0)]] int sliceLeft   = 0;
    [[nuke::prop(label="Slice Right",  min=0)]] int sliceRight  = 0;
    [[nuke::prop(label="Slice Top",    min=0)]] int sliceTop    = 0;
    [[nuke::prop(label="Slice Bottom", min=0)]] int sliceBottom = 0;

    [[nuke::func]] int  SpriteCount() const;   // spriteColumns * spriteRows (>=1)
    // Pixel rect of sprite cell `index` (row-major, 0 = top-left), accounting for margin+spacing.
    // Returns false (and leaves outputs untouched) if the grid is degenerate. Single source of truth
    // shared by SpriteAnimator (UV slicing) and the Sprite Slicer editor (overlay drawing). Not reflected
    // (reference out-params don't cross the script seam) — engine/editor call it directly.
    bool SpriteCellRect(int index, int& x0, int& y0, int& cw, int& ch) const;
    [[nuke::func]] static int GuessUsage(const std::string& filename);   // filename-suffix heuristic -> Usage
    [[nuke::func]] bool Recompress(int targetFormat);     // decode mip0 -> re-encode to FMT_BC1/BC3/BC5 (inspector override)
    // Background removal: pixels within `tolerance` (per-channel, 0..255) of (r,g,b) become fully transparent.
    // outsideOnly=true keys ONLY the background connected to the image border (4-way flood fill from the
    // edges) — enclosed same-colour regions (e.g. white eyes) are kept; false keys every matching pixel.
    // Decodes mip0, keys, re-encodes (BC -> BC3 to carry alpha; RGBA8 stays RGBA8). Single-frame only.
    [[nuke::func]] bool ApplyChromaKey(int r, int g, int b, int tolerance, bool outsideOnly);
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
    static Texture* LoadFromMemory(const std::string& data);   // packed content (3.2)
    static Texture* LoadFromStream(std::istream& i);
};
}  // namespace nuke

#endif // !NUKEE_TEXTURE_H
