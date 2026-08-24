#pragma once
#ifndef NUKEE_JSONDOC_H
#define NUKEE_JSONDOC_H
#include "NukeAPI.h"
#include <nlohmann/json.hpp>
#include <string>

namespace nuke {

// Document containers (worlds, cells, prefabs) travel as TEXT JSON in the project and may be
// COOKED to CBOR at package time ("NCBR" magic + CBOR payload: ~3x smaller, several times
// faster to parse). Every loader goes through ParseDoc, which sniffs the magic — the rest of
// the engine never knows which encoding a document arrived in.
static constexpr char kCborDocMagic[4] = { 'N', 'C', 'B', 'R' };

// Parse a document (text JSON or cooked CBOR). A parse failure returns a DISCARDED json
// (`is_discarded()`), exactly like json::parse(..., false).
NUKEENGINE_API nlohmann::json ParseDoc(const std::string& bytes);
// Encode a document as the cooked binary form (magic + CBOR).
NUKEENGINE_API std::string EncodeDocCbor(const nlohmann::json& doc);
// Cook a text-JSON file into its binary form at `dst` (dst = src is allowed). False on
// unreadable/invalid input or write failure.
NUKEENGINE_API bool CookDocFile(const std::string& src, const std::string& dst);

}  // namespace nuke

#endif // !NUKEE_JSONDOC_H
