#pragma once
#ifndef NUKEE_CURSOR_H
#define NUKEE_CURSOR_H
// Custom cursors. A .nucursor asset is an animated flipbook:
//   { "frames": [ { "image": "Cursors/point.png", "hotX": 2, "hotY": 2 }, ... ],
//     "fps": 12, "loop": true }
// Assets bind to named states ("default", "hover"...); shown as a hardware OS cursor or a
// software one the renderer draws on top of the frame.
#include "NukeAPI.h"
#include <string>

namespace nuke {

class iRender;

class NUKEENGINE_API Cursor
{
public:
	// Bind a .nucursor asset to a state; empty path unbinds. False = load failed (logged).
	static bool Bind(const std::string& state, const std::string& contentRel);
	static void SetState(const std::string& state);   // unknown state falls back to "default"
	static const char* State();
	static void SetSoftware(bool on);                 // force the software path
	static bool Software();
	static void Reset();                              // drop bindings, restore the OS arrow
	static void Tick(iRender* r);                     // per frame, game thread
};

}  // namespace nuke

#endif  // NUKEE_CURSOR_H
