#pragma once
#ifndef NUKEE_INPUT_TYPES_H
#define NUKEE_INPUT_TYPES_H
#include "NukeAPI.h"
#include <string>
#include <vector>

// Data model for the gameplay input system (input/Input.h holds the runtime API). Everything is
// string-keyed so plugins can add arbitrary controls. Serialized in the .nuinput asset.
namespace nuke {

// Bool = digital (0/1); Axis1 = analog scalar; Axis2 = 2D vector (stick, or WASD from 4 buttons).
enum class ActionValueType { Bool = 0, Axis1 = 1, Axis2 = 2 };

// When a binding fires, derived from a control's timing (thresholds below).
//  Pressed/Held/Released - edge down / continuous / edge up
//  Tap         - pressed and released within tapMax
//  LongPress   - held past longMin (fires ONCE when the threshold is crossed)
//  DoublePress - two Pressed edges within doubleWindow
enum class InputPhase { Pressed = 0, Held = 1, Released = 2, Tap = 3, LongPress = 4, DoublePress = 5 };

// One binding: what raw control(s) drive which action, and how.
struct InputBinding
{
	std::string              action;               // the InputAction this drives (by name)
	std::vector<std::string> controls;             // control id(s): "Key.W", "Gamepad.South", ...
	bool                     sequence = false;     // false = CHORD (all held together); true = SEQUENCE (in order)
	std::vector<std::string> modifiers;            // extra controls that must ALSO be held (Ctrl/Shift/any control)
	InputPhase               phase = InputPhase::Pressed;

	int    axis   = 0;                             // Axis2: 0 = X, 1 = Y (ignored for Bool/Axis1)
	float  scale  = 1.0f;                          // multiply the control value (use -1 for the opposite direction)
	float  deadzone = 0.0f;                        // ignore |value| below this (analog sticks)
	bool   invert = false;                         // negate the value after deadzone

	bool   consume = false;                        // eat the control from LOWER-priority contexts when this matches

	// Timing thresholds in seconds, per-binding so a "hold to charge" and a "tap to jump" can
	// coexist on the same control.
	float  tapMax       = 0.20f;                   // Tap: press->release under this
	float  longMin      = 0.50f;                   // LongPress: held at least this
	float  doubleWindow = 0.30f;                   // DoublePress: two presses within this
	float  sequenceWindow = 0.60f;                 // SEQUENCE: max gap between consecutive presses
};

// A named abstract input. Gameplay binds to actions, not raw keys.
struct InputAction
{
	std::string     name;                          // "Jump", "Move", "Fire"
	ActionValueType type = ActionValueType::Bool;
};

// A mapping context ("gameplay", "menu", "vehicle", ...). Several can be active at once; higher
// priority is evaluated first and a consuming binding hides the control from lower contexts.
struct InputContext
{
	std::string               name;
	int                       priority = 0;        // higher = evaluated first
	bool                      active   = false;    // toggled by Input::PushContext / SetContextActive
	std::vector<InputBinding> bindings;
};

// Runtime evaluated state of one action this frame (what the query API reports).
struct ActionState
{
	bool  pressed = false, held = false, released = false;
	bool  tapped = false, longPressed = false, doublePressed = false;
	float value = 0.0f;                            // Axis1 (or 0/1 for Bool)
	float x = 0.0f, y = 0.0f;                       // Axis2
};

}  // namespace nuke
#endif // !NUKEE_INPUT_TYPES_H
