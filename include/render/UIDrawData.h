#pragma once
#ifndef NUKE_UIDRAWDATA_H
#define NUKE_UIDRAWDATA_H
#include <cstdint>

// Backend-neutral 2D draw data for immediate-mode UI.
//
// This is the ONLY thing that connects a UI module to a renderer module. It
// deliberately contains no ImGui (or any UI library) types, and no graphics-API
// types: a UI module fills it from its own draw lists, and a renderer module
// turns it into GPU draw calls. Either side can be swapped without touching the
// other. The layout mirrors the common ImGui-style vertex/index/command format
// so a UI module can forward its buffers without copying.

struct NukeUIVert
{
	float    pos[2];
	float    uv[2];
	uint32_t col;   // packed RGBA8 (as ImGui produces)
};

struct NukeUICmd
{
	float    clipRect[4];  // x0, y0, x1, y1 in framebuffer pixels
	uint64_t texId;        // handle from iRender::createTexture2D (0 = none)
	uint32_t elemCount;    // number of indices to draw
	uint32_t idxOffset;    // first index within the list's index buffer
	uint32_t vtxOffset;    // value added to every index (base vertex)
};

struct NukeUIDrawList
{
	const NukeUIVert* vtx;
	int               vtxCount;
	const uint16_t*   idx;       // 16-bit indices
	int               idxCount;
	const NukeUICmd*  cmds;
	int               cmdCount;
};

struct NukeUIDrawData
{
	const NukeUIDrawList* lists;
	int                   listCount;
	float                 dispPos[2];   // top-left, framebuffer space
	float                 dispSize[2];  // width, height (for ortho + scissor)
};

#endif // !NUKE_UIDRAWDATA_H
