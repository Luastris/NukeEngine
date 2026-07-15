#pragma once
#ifndef NUKEE_DESKTOP_INPUT_H
#define NUKEE_DESKTOP_INPUT_H
#include "NukeAPI.h"

namespace nuke {
class iRender;

// Built-in keyboard + mouse input provider. Chains the renderer's neutral input callbacks
// (iRender::_UIkey/_UImouse/_UImove/_UImouseWheel — GLFW event codes, no GLFW linkage needed here) and
// feeds them into the Input system as string controls: "Key.W", "Key.LeftCtrl"/"Key.Ctrl", "Mouse.Left",
// "Mouse.X/Y", "Mouse.DeltaX/Y", "Mouse.ScrollY". Call ONCE at host startup AFTER the UI is mounted (it
// preserves whatever callbacks the UI installed). Gamepads/wheels/touch come from separate providers.
NUKEENGINE_API void InstallDesktopInput(iRender* render);

}  // namespace nuke
#endif // !NUKEE_DESKTOP_INPUT_H
