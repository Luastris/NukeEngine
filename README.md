# NukeEngine

NukeEngine is a free, cross-platform modular engine. This project is an engine shared library.

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

## Get it

Just create anywhere a new folder for NukeEngine projects, where will be all other projects, relating to NukeEngine, and just clone this perository into it.

Then, do

```
git submodule init
git submodule update --recursive
```

Then, go to `deps/assimp`, build it and install, if you have not it already installed.

And this should be all done.


## Building

### Windows

It is highly recommended to use Visual Studio 2019 (or up) with [vcpkg](https://github.com/Microsoft/vcpkg).
We suggest to install all dependecies that are submodules via `vcpkg`, to avoid a lot of confilcts and asspain.
Thus, you should install boost libaraies via `vcpkg`. You can install not whole library, just subs, like `boost-thread` and others.
If some dependencies could not be resolved, `Visual Studio` will automatically suggest to install it via `vcpkg`.
In other cases, generate dependency projects as told in their READMEs and chack links to them in solution.
Whih dependecies are higly recommended to install via vcpkg?:
* assimp
* glfw
* glm
* lua
* freeglut
* glew

Unfortunately, bgfx, bimg and bx are not avaliable via vcpkg, and you need to build them with your own. Just initialize submodules, then generate Visual Studio projects for them. Also, you can fix broken links to them on NukeEngine solution, if this happened.
So, try to build it!

> Note: It would be better to clone NukeEngine Editor nearby NukeEngine root directory.

> The best hierarchy scheme for solution:
+ [NE]
  + [NukeEngine] (repo)
    + .... (files)
  + [NukeEngine-Editor] (repo)
    + .... (files)

### Linux
The build system, used by project, is `qmake`, so you can just use Qt Creator to build it, or even simple qmake, if you have qt tools.

If you see next errors:
`error: cannot find -lglut`
or the same, you need to install development files of needed libraries(e.g. `freeglut`, or `glew` for this case).

If you see smth like this:
```
.../projects/NE/NukeEngine/API/Model/Include.h:3: error: boost/container/list.hpp: No such file or directory
 #include <boost/container/list.hpp>
          ^~~~~~~~~~~~~~~~~~~~~~~~~~
```
You need to install boost libraries sources.

#### Building the assimp:
Int the project dir, goto `deps/assimp`, then:

```
cmake CMakeLists.txt
make -j4 install
```

j4 means that you will use 4 threads for building.


### MacOS [Attention!!!]

You will need to do some preparations for building NukeEngine on Mac:

+ Install XQuartz - required by freeglut
+ Install homebrew - required for next
+ Install freeglut via brew
+ Install glew via brew
+ Install glfw via brew
+ Install boost via brew
+ Install assimp via brew (you should better build it own and install build into system, its always fresh)
+ Install other via brew if you cannot install it in another way
