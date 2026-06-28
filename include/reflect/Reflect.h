#pragma once
#ifndef NUKEE_REFLECT_H
#define NUKEE_REFLECT_H
#include "NukeAPI.h"
// Lightweight reflection: declare fields with NUKE_PROP and they auto-(de)serialize
// (project + save) and auto-draw in the inspector. C++ has no built-in reflection,
// so a per-type schema is built at static-init time from member pointers.
//
// The engine must NOT depend on ImGui, so a field only stores a TYPE TAG (FT) and a
// raw address accessor; the editor switches on FT to pick a widget. Serialization
// (JSON) lives here in the engine.

#include <string>
#include <vector>
#include <functional>
#include "API/Model/Vector.h"
#include "API/Model/Color.h"
// NOTE: no <nlohmann/json.hpp> here on purpose — this header is pulled in by widely
// included component headers (and the render/UI modules). JSON (de)serialization lives
// in ReflectJson.h, included only where serialization actually happens.

namespace nuke {

// Supported field types. Extend as needed (Color/asset-refs/etc. added later).
enum class FT { Unknown, Bool, Int, Float, Double, String, Vec2, Vec3, Vec4, Quat, Color };

// Map a C++ type -> FT tag. Primary = Unknown; specializations for supported types.
template<class T> constexpr FT FieldTypeOf() { return FT::Unknown; }
template<> constexpr FT FieldTypeOf<bool>()        { return FT::Bool; }
template<> constexpr FT FieldTypeOf<int>()         { return FT::Int; }
template<> constexpr FT FieldTypeOf<float>()       { return FT::Float; }
template<> constexpr FT FieldTypeOf<double>()      { return FT::Double; }
template<> constexpr FT FieldTypeOf<std::string>() { return FT::String; }
template<> constexpr FT FieldTypeOf<Vector2>()     { return FT::Vec2; }
template<> constexpr FT FieldTypeOf<Vector3>()     { return FT::Vec3; }
template<> constexpr FT FieldTypeOf<Vector4>()     { return FT::Vec4; }
template<> constexpr FT FieldTypeOf<Quaternion>()  { return FT::Quat; }
template<> constexpr FT FieldTypeOf<Color>()       { return FT::Color; }

struct Field {
    std::string name;
    FT type = FT::Unknown;
    std::function<void*(void*)> addr;   // returns &(obj->field)
    bool hidden = false;                // serialized, but not drawn in the auto-inspector
    // Editor hint from [[nuke::prop(asset="...")]]: a String field that holds an asset GUID
    // ("mesh"/"material"/"shader"/"texture"). The inspector draws an asset picker instead of a
    // raw text box. Empty = plain field. Keeps asset wiring out of the editor's hardcode.
    std::string asset;
    std::string label;   // [[nuke::prop(label="...")]] display name in the inspector (else `name`)
    // [[nuke::prop(min=..,max=..)]] on a numeric field -> the inspector draws a slider in [fmin,fmax]
    // instead of a drag box. fmax > fmin means "has a range".
    float fmin = 0.0f, fmax = 0.0f;
    // [[nuke::prop(enum="A,B,C")]] on an int field -> the inspector draws a dropdown; the int is the index.
    std::vector<std::string> enumLabels;
};

struct TypeInfo {
    std::string name;
    std::string base;
    std::vector<Field> fields;
    std::function<void*()> create;   // factory (set by NUKE_REGISTER) — create-by-name on load
};

// Registry (defined in Reflect.cpp).
NUKEENGINE_API TypeInfo& Registry_GetOrCreate(const std::string& name);
NUKEENGINE_API TypeInfo* Registry_Find(const std::string& name);
NUKEENGINE_API std::vector<TypeInfo*> Registry_All();   // every registered type (for "Add Component")

// Defined in the generated Reflect.gen.cpp — registers every reflected type's schema +
// factory. Call once (from World's ctor) so the generated .obj is linked and runs.
NUKEENGINE_API bool NukeReflectInit();

// One TypeInfo per reflected type T (lazily created on first use).
template<class T>
TypeInfo& TypeOf() {
    static TypeInfo& ti = Registry_GetOrCreate(T::__NukeTypeName());
    return ti;
}

// Build a Field from a member pointer (deduces the FT tag).
template<class C, class T>
Field MakeField(const char* name, T C::* p, const char* asset = "", const char* label = "",
                float fmin = 0.0f, float fmax = 0.0f, const char* enumCsv = "") {
    Field f;
    f.name = name;
    f.type = FieldTypeOf<T>();
    f.addr = [p](void* o) -> void* { return (void*)&(((C*)o)->*p); };
    f.asset = asset;
    f.label = label;
    f.fmin = fmin;
    f.fmax = fmax;
    if (enumCsv && *enumCsv) {            // split "A,B,C" -> dropdown labels
        std::string s(enumCsv), cur;
        for (char c : s) { if (c == ',') { f.enumLabels.push_back(cur); cur.clear(); } else cur += c; }
        f.enumLabels.push_back(cur);
    }
    return f;
}

} // namespace nuke

// --- authoring macros -------------------------------------------------------
// Put NUKE_TYPE once at the top of the class, NUKE_PROP per serialized field,
// and NUKE_REGISTER(Type) once at file scope.
// UE-like reflection markers:
//   NUKE_CLASS(Camera, Component)        // one line in the class body (like UCLASS)
//   [[nuke::prop]] float fov = 90.0f;    // attribute on each field (like UPROPERTY)
//
// NUKE_CLASS provides the type name + virtual GetType(). The actual field/factory
// REGISTRATION is emitted into Reflect.gen.cpp by the codegen tool (tools/nukegen.py),
// which scans for NUKE_CLASS + [[nuke::prop]]. The compiler ignores [[nuke::prop]];
// when C++26 reflection lands, the same attribute is read natively and the tool is dropped.
#define NUKE_CLASS(Class_, Base_)                                                   \
    public:                                                                        \
        using Self = Class_;                                                       \
        static const char* __NukeTypeName() { return #Class_; }                    \
        virtual ::nuke::TypeInfo* GetType() { return &::nuke::TypeOf<Class_>(); }

// Same, for types that are never created by name (no factory generated, e.g. Transform).
#define NUKE_CLASS_NOCREATE(Class_, Base_) NUKE_CLASS(Class_, Base_)

#endif // NUKEE_REFLECT_H
