#pragma once
#ifndef NUKEE_SCREEN_H
#define NUKEE_SCREEN_H
#include "NukeAPI.h"
#include "reflect/Reflect.h"

namespace nuke {

// THE GAME SCREEN — the surface the game's cameras render to, as scripts should see it:
// the window framebuffer in the Player, the game viewport panel in the editor. Fed by the
// hosts every frame (Screen::Set); reflected for scripts (nuke.Screen.* / Screen.*).
// Distinct from Game.WindowWidth/Height (the WINDOW config) — this is the live pixel size.
class NUKEENGINE_API Screen
{
	NUKE_CLASS_NOCREATE(Screen, Object)
public:
	[[nuke::func]] static double Width();    // pixels
	[[nuke::func]] static double Height();   // pixels
	[[nuke::func]] static double Aspect();   // width / height (0-safe)

	static void Set(int w, int h);   // hosts feed the live size (player loop / editor viewport)
};

}  // namespace nuke
#endif // !NUKEE_SCREEN_H
