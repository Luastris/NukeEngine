#include "interface/ComponentIcons.h"
#include <iostream>

namespace nuke {

static std::vector<ComponentIcon>& Pool() { static std::vector<ComponentIcon> p; return p; }

void RegisterComponentIcon(const ComponentIcon& desc)
{
	if (desc.component.empty() || desc.glyph.empty()) return;
	for (ComponentIcon& c : Pool())
		if (c.component == desc.component) { c = desc; return; }   // re-register (hot reload) = replace
	Pool().push_back(desc);
	std::cout << "[ComponentIcons]\tregistered '" << desc.component << "'" << std::endl;
}

const std::vector<ComponentIcon>& ComponentIcons() { return Pool(); }

// The engine's own mesh-less components (Lucide glyphs; the editor's camera / light / probe /
// environment / decal icons are picked by state in the editor itself).
void RegisterBuiltinComponentIcons()
{
	RegisterComponentIcon({ "AudioSource",   "\xee\x85\xa6" /* ICON_LC_SPEAKER */,       { 0.95f, 0.8f,  0.45f, 0.92f } });
	RegisterComponentIcon({ "AudioListener", "\xee\x8e\x82" /* ICON_LC_EAR */,           { 0.95f, 0.8f,  0.45f, 0.92f } });
	RegisterComponentIcon({ "ReverbZone",    "\xee\x95\x9b" /* ICON_LC_AUDIO_WAVEFORM */, { 0.45f, 0.8f,  1.0f,  0.92f } });
	RegisterComponentIcon({ "AudioVolume",   "\xee\x86\xab" /* ICON_LC_VOLUME_2 */,      { 0.45f, 1.0f,  0.6f,  0.92f } });
	RegisterComponentIcon({ "TimeVolume",    "\xee\x87\xa0" /* ICON_LC_TIMER */,         { 1.0f,  0.8f,  0.2f,  0.92f } });
	RegisterComponentIcon({ "PostFXVolume",  "\xee\x8d\x97" /* ICON_LC_WAND_SPARKLES */, { 0.85f, 0.55f, 1.0f,  0.92f } });
	RegisterComponentIcon({ "WindZone",      "\xee\x86\xb0" /* ICON_LC_WIND */,          { 0.35f, 0.9f,  0.75f, 0.92f } });
}

}  // namespace nuke
