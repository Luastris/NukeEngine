#pragma once
#ifndef NUKEE_ASSET_CREATORS_H
#define NUKEE_ASSET_CREATORS_H
#include "NukeAPI.h"
#include <functional>
#include <string>
#include <vector>

namespace nuke {

// A plugin-registered FILE-TYPE DESCRIPTOR (roadmap 0.6). The plugin supplies only DATA
// (no boost / no file IO): the EDITOR creates files from it (browser "New ..." menu:
// UniquePath, write template, rename), picks menu/browser icons, chooses the text
// editor's syntax highlighting (file editor, roadmap 2.2), and calls the optional
// inspector hook when a file of this type is selected.
struct AssetCreator
{
	std::string label;     // menu entry / type display name, e.g. "Lua Script"
	std::string ext;       // file extension incl. dot, e.g. ".lua"
	std::string baseName;  // default file name (no ext), e.g. "New Script"
	std::string content;   // initial file text (the template)

	// -- descriptor fields; defaults keep the plain 4-arg registration working --
	std::string category;             // "New" menu group, e.g. "Scripts" ("" = ungrouped)
	std::string icon;                 // UTF-8 icon glyph for menus ("" = editor default)
	bool        textEditable = false; // the file editor may open it as text
	std::string syntaxLanguage;       // highlighting id for the file editor, e.g. "lua"
	// Optional: draw type-specific inspector UI for a selected file of this type.
	std::function<void(const std::string& path)> inspector;
};

NUKEENGINE_API void RegisterAssetCreator(const std::string& label, const std::string& ext,
                                         const std::string& baseName, const std::string& content);
NUKEENGINE_API void RegisterAssetCreator(const AssetCreator& desc);
NUKEENGINE_API const std::vector<AssetCreator>& AssetCreators();

// The registered descriptor for a file extension (".lua", case-insensitive), or null.
// The browser / file editor / inspector resolve a file's type through this.
NUKEENGINE_API const AssetCreator* AssetCreatorForExt(const std::string& ext);

// MODULE-SUPPLIED ASSET EDITORS: the module that OWNS a file type registers the editor for
// it too (an editor-only companion module, e.g. NukeTilemapEditor for NukeTilemap's
// .nutile) — the editor core stays format-blind. Double-click / "Open in Editor" route
// through this registry FIRST; the callback opens (or focuses) the module's own editor
// window for that file. Order-independent with the type registration itself.
NUKEENGINE_API void RegisterAssetEditor(const std::string& ext,
                                        std::function<void(const std::string& path)> open);
NUKEENGINE_API const std::function<void(const std::string& path)>*
                    AssetEditorForExt(const std::string& ext);

}  // namespace nuke

#endif // !NUKEE_ASSET_CREATORS_H
