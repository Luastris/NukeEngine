#pragma once
#ifndef NUKEE_DESKTOP_INPUT_H
#define NUKEE_DESKTOP_INPUT_H
#include "NukeAPI.h"

namespace nuke {
class iRender;

// Install the built-in keyboard + mouse provider: chains the renderer's neutral input callbacks and
// feeds them to Input as string controls ("Key.W", "Mouse.Left", "Mouse.DeltaX", "Mouse.ScrollY", ...).
// Call ONCE at host startup AFTER the UI is mounted — it preserves the UI's existing callbacks.
NUKEENGINE_API void InstallDesktopInput(iRender* render);

}  // namespace nuke
#endif // !NUKEE_DESKTOP_INPUT_H
