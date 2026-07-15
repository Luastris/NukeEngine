#include "input/DesktopInput.h"
#include "input/Input.h"
#include "render/irender.h"
#include <string>
#include <unordered_map>

namespace nuke {

// GLFW key code -> control name. Printable ASCII maps to its glyph ("Key.W"); named keys via the table.
// Unknown codes fall back to "Key.<code>" so nothing is lost. (GLFW numeric constants, no <GLFW/glfw3.h>.)
static std::string KeyName(int key)
{
	if (key >= 'A' && key <= 'Z') return std::string("Key.") + (char)key;             // 65..90
	if (key >= '0' && key <= '9') return std::string("Key.") + (char)key;             // 48..57
	static const std::unordered_map<int, const char*> named = {
		{32,"Space"},{39,"Apostrophe"},{44,"Comma"},{45,"Minus"},{46,"Period"},{47,"Slash"},
		{59,"Semicolon"},{61,"Equal"},{91,"LeftBracket"},{92,"Backslash"},{93,"RightBracket"},{96,"Grave"},
		{256,"Escape"},{257,"Enter"},{258,"Tab"},{259,"Backspace"},{260,"Insert"},{261,"Delete"},
		{262,"Right"},{263,"Left"},{264,"Down"},{265,"Up"},{266,"PageUp"},{267,"PageDown"},{268,"Home"},{269,"End"},
		{280,"CapsLock"},{284,"NumLock"},{283,"PrintScreen"},{285,"Pause"},
		{290,"F1"},{291,"F2"},{292,"F3"},{293,"F4"},{294,"F5"},{295,"F6"},{296,"F7"},{297,"F8"},
		{298,"F9"},{299,"F10"},{300,"F11"},{301,"F12"},
		{320,"KP0"},{321,"KP1"},{322,"KP2"},{323,"KP3"},{324,"KP4"},{325,"KP5"},{326,"KP6"},{327,"KP7"},{328,"KP8"},{329,"KP9"},
		{330,"KPDecimal"},{331,"KPDivide"},{332,"KPMultiply"},{333,"KPSubtract"},{334,"KPAdd"},{335,"KPEnter"},
		{340,"LeftShift"},{341,"LeftCtrl"},{342,"LeftAlt"},{343,"LeftSuper"},
		{344,"RightShift"},{345,"RightCtrl"},{346,"RightAlt"},{347,"RightSuper"},{348,"Menu"},
	};
	auto it = named.find(key);
	if (it != named.end()) return std::string("Key.") + it->second;
	return std::string("Key.") + std::to_string(key);
}

// The mouse position + scroll accumulator shared by the callbacks and the per-frame poll.
static double s_mx = 0, s_my = 0, s_prevPollX = 0, s_prevPollY = 0, s_scroll = 0;

void InstallDesktopInput(iRender* r)
{
	if (!r) return;

	// Chain the existing callbacks (the UI may have installed some) — feed Input, then call the previous.
	auto prevKey   = r->_UIkey;
	auto prevMouse = r->_UImouse;
	auto prevMove  = r->_UImove;
	auto prevWheel = r->_UImouseWheel;

	r->_UIkey = [prevKey](int key, int action, int mods) {
		float v = (action == 0) ? 0.0f : 1.0f;   // GLFW_RELEASE == 0; press/repeat -> down
		Input::SetControl(KeyName(key), v);
		// Metas as generic modifier aliases too (so a binding can require "Key.Ctrl" regardless of side).
		if (key == 340 || key == 344) Input::SetControl("Key.Shift", v);
		if (key == 341 || key == 345) Input::SetControl("Key.Ctrl",  v);
		if (key == 342 || key == 346) Input::SetControl("Key.Alt",   v);
		if (prevKey) prevKey(key, action, mods);
	};
	r->_UImouse = [prevMouse](int button, int state, int x, int y) {
		const char* name = button == 0 ? "Mouse.Left" : button == 1 ? "Mouse.Right" : button == 2 ? "Mouse.Middle" : nullptr;
		if (name) Input::SetControl(name, state != 0 ? 1.0f : 0.0f);
		s_mx = x; s_my = y;
		if (prevMouse) prevMouse(button, state, x, y);
	};
	r->_UImove = [prevMove](int x, int y) {
		s_mx = x; s_my = y;
		Input::SetControl("Mouse.X", (float)x);
		Input::SetControl("Mouse.Y", (float)y);
		if (prevMove) prevMove(x, y);
	};
	r->_UImouseWheel = [prevWheel](int button, int dir, int x, int y) {
		s_scroll += dir;
		if (prevWheel) prevWheel(button, dir, x, y);
	};

	// Per-frame poll: publish mouse delta + the frame's accumulated scroll, then clear.
	Input::RegisterProvider("desktop", [] {
		Input::SetControl("Mouse.DeltaX", (float)(s_mx - s_prevPollX));
		Input::SetControl("Mouse.DeltaY", (float)(s_my - s_prevPollY));
		s_prevPollX = s_mx; s_prevPollY = s_my;
		Input::SetControl("Mouse.ScrollY", (float)s_scroll);
		s_scroll = 0;
	});
}

}  // namespace nuke
