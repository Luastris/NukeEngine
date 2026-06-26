#pragma once
#ifndef NUKEE_HOTKEYS_H
#define NUKEE_HOTKEYS_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace nuke {

// Centralized, conflict-aware hotkey registry shared by the editor AND plugins. A chord is stored as
// an opaque int (callers use ImGui key-chord values, e.g. ImGuiMod_Ctrl | ImGuiKey_S); the engine
// never interprets it — it only stores, compares, serializes, and hands chords back for dispatch.
// Conflict rule: each chord maps to at most ONE bound hotkey. If two hotkeys want the same chord
// (e.g. two plugins), the later one registers UNBOUND so nothing fires twice; the user assigns it a
// free chord manually in Project Settings. Bindings save/load with the project.
struct Hotkey
{
    std::string           id;             // stable unique id, e.g. "editor.world.save"
    std::string           name;           // display name, e.g. "Save World"
    int                   chord = 0;      // current binding (0 = unbound); opaque ImGuiKeyChord
    int                   defaultChord = 0;
    bool                  bound = false;  // false = no binding (conflict / cleared) -> never fires
    std::function<void()> action;
};

class NUKEENGINE_API Hotkeys
{
public:
    static Hotkeys* Get();

    // Register (or refresh the action/name of) a hotkey. If defaultChord is free it's bound; if it's
    // already taken by another bound hotkey, this registers UNBOUND (a conflict to resolve in UI).
    void Register(const std::string& id, const std::string& name, int defaultChord, std::function<void()> action);

    bool Rebind(const std::string& id, int chord);   // false if the chord is taken by another bound hotkey
    void Unbind(const std::string& id);
    void ResetToDefault(const std::string& id);

    Hotkey*                    Find(const std::string& id);
    const std::vector<Hotkey>& All() const { return hotkeys; }
    bool                       ChordTaken(int chord, const std::string& exceptId = "") const;

    // Persistence (id -> chord; 0 = unbound). The editor stores this in the .nuproj.
    std::map<std::string, int> ExportBindings() const;
    void                       ApplyBindings(const std::map<std::string, int>& binds);

private:
    std::vector<Hotkey> hotkeys;
    Hotkey* findInternal(const std::string& id);
};

} // namespace nuke
#endif // !NUKEE_HOTKEYS_H
