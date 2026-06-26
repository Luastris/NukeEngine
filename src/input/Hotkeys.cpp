#include "input/Hotkeys.h"

namespace nuke {

Hotkeys* Hotkeys::Get() { static Hotkeys instance; return &instance; }

Hotkey* Hotkeys::findInternal(const std::string& id)
{
	for (auto& h : hotkeys) if (h.id == id) return &h;
	return nullptr;
}
Hotkey* Hotkeys::Find(const std::string& id) { return findInternal(id); }

bool Hotkeys::ChordTaken(int chord, const std::string& exceptId) const
{
	if (chord == 0) return false;
	for (auto& h : hotkeys)
		if (h.bound && h.chord == chord && h.id != exceptId) return true;
	return false;
}

void Hotkeys::Register(const std::string& id, const std::string& name, int defaultChord, std::function<void()> action)
{
	Hotkey* h = findInternal(id);
	if (!h) { hotkeys.push_back(Hotkey{}); h = &hotkeys.back(); h->id = id; }
	h->name         = name;
	h->action       = std::move(action);
	h->defaultChord = defaultChord;
	// Only auto-bind if not already bound (don't clobber a user/project rebind on re-registration).
	if (!h->bound)
	{
		if (defaultChord != 0 && !ChordTaken(defaultChord, id)) { h->chord = defaultChord; h->bound = true; }
		else                                                    { h->chord = 0;            h->bound = false; }
	}
}

bool Hotkeys::Rebind(const std::string& id, int chord)
{
	Hotkey* h = findInternal(id);
	if (!h) return false;
	if (chord != 0 && ChordTaken(chord, id)) return false;   // would conflict with another binding
	h->chord = chord;
	h->bound = (chord != 0);
	return true;
}

void Hotkeys::Unbind(const std::string& id)
{
	if (Hotkey* h = findInternal(id)) { h->chord = 0; h->bound = false; }
}

void Hotkeys::ResetToDefault(const std::string& id)
{
	Hotkey* h = findInternal(id);
	if (!h) return;
	if (h->defaultChord != 0 && !ChordTaken(h->defaultChord, id)) { h->chord = h->defaultChord; h->bound = true; }
	else                                                          { h->chord = 0;                h->bound = false; }
}

std::map<std::string, int> Hotkeys::ExportBindings() const
{
	std::map<std::string, int> m;
	for (auto& h : hotkeys) m[h.id] = h.bound ? h.chord : 0;
	return m;
}

void Hotkeys::ApplyBindings(const std::map<std::string, int>& binds)
{
	for (auto& kv : binds)
	{
		Hotkey* h = findInternal(kv.first);
		if (!h) continue;
		int c = kv.second;
		if (c != 0 && !ChordTaken(c, kv.first)) { h->chord = c; h->bound = true; }
		else                                    { h->chord = 0; h->bound = false; }   // 0 / conflict -> unbound
	}
}

} // namespace nuke
