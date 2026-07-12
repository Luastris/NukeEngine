# NukeEngine

The CORE of [NukeEngine](https://github.com/Luastris/NukeEngine-Eco) by
[Luastris](https://luastris.com): a modular C++20 game engine shared library
(`NukeEngine.dll`) with curated `NUKEENGINE_API` exports. Everything else — the editor,
the player, renderers, physics, audio, scripting, runtime GUI — is a separate host or a
hot-pluggable module behind POD service seams.

For the full picture (quick start, ecosystem, packaging, modding, scripting) see the
ecosystem root: **[NukeEngine-Eco](https://github.com/Luastris/NukeEngine-Eco)** — clone
it with submodules and build everything with one command via the superbuild.

## What lives here

- The Model API (`nuke::` namespace): Atom / World / Component, Transform (quaternions),
  Material, Mesh, Texture, Shader, AnimClip, Prefab, ResDB, Package (NUPAK), Audio,
  Physics facades, Jobs, Log, Math.
- The reflection registry (`[[nuke::prop]]` / `[[nuke::func]]` + nukegen codegen for
  C++ < 26) — the single source the inspector, serialization, Lua, C# and modding ride.
- Service seams (`iRender`, `iPhysics`, `iAudio`, `iScript`, `iGUI`) and the module
  loader (`NUKEModule`, unified plugin export, shared services).
- World serialization (`.nuworld` JSON), the layered content resolver (raw project over
  mounted paks/mods), the world-merge for mods (per-property point diffs).

## Building

Preferred: the superbuild at the ecosystem root (`cmake -S . -B build` there) — it drives
this repo's `NukeEngine.sln` plus every present module in dependency order.

Standalone: `NukeEngine.sln` builds the engine + the editor (VS2022, v143, x64, C++20).
The engine is vcpkg MANIFEST mode (`vcpkg.json` → `vcpkg_installed/`); `VCPKG_ROOT` must
be set. Reflection codegen (`tools/nukegen.py` in the eco root) needs Python on PATH.
Run dir = `x64/<Config>` — the editor, player, `modules/` and `shaders/` all land there.

**ABI rule:** new virtuals go at the END of seam vtables (`iRender`, `iAudio`,
`iPhysics`, `NUKEModule`) — inserting mid-vtable silently corrupts stale module DLLs.
After any seam change rebuild ALL modules in the same batch.

## Shaders & ray-traced reflections

The RT reflection pass (`rtreflect` post effect) traces the real scene and shades each hit with the **same**
material model the raster pass uses. To stay correct for arbitrary materials it auto-generates a per-shader
closest-hit shader at load time.

**Convention — opt in with `<name>.surf.hlsl`:** a shader gets its own auto-generated RT closest-hit **only** if it
ships a `shaders/<name>.surf.hlsl` file defining `void Surface(SurfaceIn IN, inout SurfaceOut O)` (see
`unlit.surf.hlsl`). The renderer concatenates `rt_common.hlsl` + the material's `cbuffer MatCB` field loads +
your `Surface()` + the lighting/recursion harness. Materials **without** a `.surf.hlsl` fall back to the standard
metallic-roughness closest-hit (`rt_rchit.hlsl`).

This is deliberately **not** applied to every shader: post-process shaders (and other non-surface shaders) have no
surface to shade in a reflection, so they must not get an RT hit group. Provide a `.surf.hlsl` for a lit/world-type
shader you want reflected faithfully; otherwise the standard PBR hit shader is used.

Material maps (base color, normal, metallic-roughness, occlusion, emissive, specular) are honoured in **both** the
raster pass and RT reflections (bindless), including analytic tangent-space normal mapping in RT.
