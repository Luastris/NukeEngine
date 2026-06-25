#pragma once
#ifndef NUKEE_REFLECT_JSON_H
#define NUKEE_REFLECT_JSON_H
#include "NukeAPI.h"
// JSON (de)serialization for the reflection schema. Include ONLY where serialization
// happens (Reflect.cpp, World save/load) — keeps nlohmann out of component headers.

#include "reflect/Reflect.h"
#include <nlohmann/json.hpp>

namespace nuke {

using json = nlohmann::json;

// Single value <-> json by tag.
NUKEENGINE_API void SaveField(FT t, const void* addr, json& j);
NUKEENGINE_API void LoadField(FT t, void* addr, const json& j);

// Whole object <-> json using its TypeInfo.
NUKEENGINE_API void SaveObject(const TypeInfo& ti, const void* obj, json& j);
NUKEENGINE_API void LoadObject(const TypeInfo& ti, void* obj, const json& j);

} // namespace nuke

#endif // NUKEE_REFLECT_JSON_H
