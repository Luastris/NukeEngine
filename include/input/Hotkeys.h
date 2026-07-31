#pragma once
#ifndef NUKEE_HOTKEYS_H
#define NUKEE_HOTKEYS_H
#include "NukeAPI.h"
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace nuke {

// Conflict-aware hotkey registry shared by the editor and plugins. A chord is an opaque int (callers
// use ImGui key-chord values); the engine only stores, compares and serializes it. Each chord maps to
// at most ONE bound hotkey — a later claimant registers UNBOUND rather than firing twice.
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

    // Register (or refresh) a hotkey. Bound if defaultChord is free, else registered UNBOUND.
    void Register(const std::string& id, const std::string& name, int defaultChord, std::function<void()> action);

    bool Rebind(const std::string& id, int chord);   // false if the chord is taken by another bound hotkey
    void Unbind(const std::string& id);
    void ResetToDefault(const std::string& id);

    Hotkey*                    Find(const std::string& id);
    const std::vector<Hotkey>& All() const { return hotkeys; }
    bool                       ChordTaken(int chord, const std::string& exceptId = "") const;

    // Persistence (id -> chord; 0 = unbound).
    std::map<std::string, int> ExportBindings() const;
    void                       ApplyBindings(const std::map<std::string, int>& binds);

private:
    std::vector<Hotkey> hotkeys;
    Hotkey* findInternal(const std::string& id);
};

} // namespace nuke
#endif // !NUKEE_HOTKEYS_H
