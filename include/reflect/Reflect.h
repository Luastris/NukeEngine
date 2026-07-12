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

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <functional>
#include "API/Model/Vector.h"
#include "API/Model/Color.h"
// NOTE: no <nlohmann/json.hpp> here on purpose — this header is pulled in by widely
// included component headers (and the render/UI modules). JSON (de)serialization lives
// in ReflectJson.h, included only where serialization actually happens.

namespace nuke {

class Atom;   // AtomRef values resolve through the live world (Reflect_AtomById below)

// Supported field types. Extend as needed (Color/asset-refs/etc. added later).
// AtomRef = a reference to a live Atom, carried across the boundary as its stable id —
// stale-safe: a dead atom resolves to null, never to freed memory.
enum class FT { Unknown, Bool, Int, Float, Double, String, Vec2, Vec3, Vec4, Quat, Color, AtomRef };

// AtomRef <-> live atom (defined in Reflect.cpp; walks the CURRENT world by stable id).
NUKEENGINE_API Atom*         Reflect_AtomById(unsigned long id);
NUKEENGINE_API unsigned long Reflect_AtomId(Atom* a);

// Map a C++ type -> FT tag. Primary: ENUMS reflect as Int (the label list comes from the
// [[nuke::prop(enum="...")]] metadata; engine prop-enums declare `: int` so the generic
// addr-based int read/write is layout-exact); everything else unknown unless specialized.
template<class T> constexpr FT FieldTypeOf()
{
	return std::is_enum_v<T> ? FT::Int : FT::Unknown;
}
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
template<> constexpr FT FieldTypeOf<Atom*>()       { return FT::AtomRef; }

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

// ---- language-neutral value (reflection <-> scripting boundary) --------------------
// One reflected value crossing the boundary. `type` says which member is valid:
// Bool -> b; Int/Float/Double -> num; String -> str;
// Vec2/Vec3/Vec4/Quat -> v as (x,y,z,w); Color -> v as (r,g,b,a). Unknown = void/none.
struct ReflectValue
{
	FT          type = FT::Unknown;
	bool        b    = false;
	double      num  = 0.0;
	std::string str;
	double      v[4] = { 0, 0, 0, 0 };
	unsigned long atom = 0;   // AtomRef: the atom's stable id (0 = null)
};

namespace detail {
// ReflectValue -> typed argument. Only the supported FT types specialize — tagging a
// method with an unsupported parameter type is a COMPILE error, not a runtime surprise.
template<class T> T FromRV(const ReflectValue& v);
template<> inline bool        FromRV<bool>(const ReflectValue& v)        { return v.b; }
template<> inline int         FromRV<int>(const ReflectValue& v)         { return (int)v.num; }
template<> inline float       FromRV<float>(const ReflectValue& v)       { return (float)v.num; }
template<> inline double      FromRV<double>(const ReflectValue& v)      { return v.num; }
template<> inline std::string FromRV<std::string>(const ReflectValue& v) { return v.str; }
template<> inline Vector2     FromRV<Vector2>(const ReflectValue& v)     { return Vector2(v.v[0], v.v[1]); }
template<> inline Vector3     FromRV<Vector3>(const ReflectValue& v)     { return Vector3(v.v[0], v.v[1], v.v[2]); }
template<> inline Vector4     FromRV<Vector4>(const ReflectValue& v)     { return Vector4(v.v[0], v.v[1], v.v[2], v.v[3]); }
template<> inline Quaternion  FromRV<Quaternion>(const ReflectValue& v)  { return Quaternion(v.v[0], v.v[1], v.v[2], v.v[3]); }
template<> inline Color       FromRV<Color>(const ReflectValue& v)       { return Color(v.v[0], v.v[1], v.v[2], v.v[3]); }
template<> inline Atom*       FromRV<Atom*>(const ReflectValue& v)       { return Reflect_AtomById(v.atom); }

// Typed return -> ReflectValue.
inline void ToRV(bool x, ReflectValue& o)               { o.type = FT::Bool;   o.b = x; }
inline void ToRV(int x, ReflectValue& o)                { o.type = FT::Int;    o.num = x; }
inline void ToRV(float x, ReflectValue& o)              { o.type = FT::Float;  o.num = x; }
inline void ToRV(double x, ReflectValue& o)             { o.type = FT::Double; o.num = x; }
inline void ToRV(const std::string& x, ReflectValue& o) { o.type = FT::String; o.str = x; }
inline void ToRV(const Vector2& x, ReflectValue& o)     { o.type = FT::Vec2;  o.v[0] = x.x; o.v[1] = x.y; }
inline void ToRV(const Vector3& x, ReflectValue& o)     { o.type = FT::Vec3;  o.v[0] = x.x; o.v[1] = x.y; o.v[2] = x.z; }
inline void ToRV(const Vector4& x, ReflectValue& o)     { o.type = FT::Vec4;  o.v[0] = x.x; o.v[1] = x.y; o.v[2] = x.z; o.v[3] = x.w; }
inline void ToRV(const Quaternion& x, ReflectValue& o)  { o.type = FT::Quat;  o.v[0] = x.x; o.v[1] = x.y; o.v[2] = x.z; o.v[3] = x.w; }
inline void ToRV(const Color& x, ReflectValue& o)       { o.type = FT::Color; o.v[0] = x.r; o.v[1] = x.g; o.v[2] = x.b; o.v[3] = x.a; }
inline void ToRV(Atom* x, ReflectValue& o)              { o.type = FT::AtomRef; o.atom = Reflect_AtomId(x); }
}  // namespace detail

// ---- method reflection (tag with [[nuke::func]]; nukegen emits MakeMethod calls) ----
// A reflected method: FT-typed signature + a type-erased invoker. Scripting backends
// (Lua/C#) call ANY tagged method through this — no per-class wrappers. Overloads are
// NOT supported (one reflected method per name); parameters/returns must be supported
// FT types (by value or const&; return void or by value).
struct Method {
    std::string name;
    FT ret = FT::Unknown;                 // FT::Unknown = void
    std::vector<FT> params;               // declared parameter types, in order
    bool isStatic = false;                // static/free function: invoke ignores `obj` —
                                          // script binders expose it as <Type>.<name>(...)
    // Invoke on an instance of the owning type (null for statics). `args` must match
    // params (count is checked, types are trusted — the caller converted by `params`).
    std::function<bool(void* obj, const ReflectValue* args, std::size_t n, ReflectValue& ret)> invoke;
};

struct TypeInfo {
    std::string name;
    std::string base;
    std::vector<Field> fields;
    std::vector<Method> methods;          // [[nuke::func]]-tagged methods
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

namespace detail {
template<class C, class M, class R, class... A, std::size_t... I>
bool InvokeImpl(M mf, C* obj, const ReflectValue* a, ReflectValue& out, std::index_sequence<I...>) {
    (void)a;   // unused for 0-arg methods
    if constexpr (std::is_void_v<R>) {
        (obj->*mf)(FromRV<std::decay_t<A>>(a[I])...);
        out.type = FT::Unknown;
    } else {
        ToRV((obj->*mf)(FromRV<std::decay_t<A>>(a[I])...), out);
    }
    return true;
}

template<class C, class M, class R, class... A>
Method MakeMethodImpl(const char* name, M mf) {
    Method m;
    m.name = name;
    if constexpr (!std::is_void_v<R>) m.ret = FieldTypeOf<std::decay_t<R>>();
    m.params = { FieldTypeOf<std::decay_t<A>>()... };
    m.invoke = [mf](void* o, const ReflectValue* a, std::size_t n, ReflectValue& out) -> bool {
        if (n != sizeof...(A)) return false;
        return InvokeImpl<C, M, R, A...>(mf, (C*)o, a, out, std::index_sequence_for<A...>{});
    };
    return m;
}
}  // namespace detail

// Build a Method from a member-function pointer (deduces the FT signature). Same void*
// contract as Field::addr: invoke's `obj` is an instance of C.
template<class C, class R, class... A>
Method MakeMethod(const char* name, R (C::*mf)(A...)) {
    return detail::MakeMethodImpl<C, decltype(mf), R, A...>(name, mf);
}
template<class C, class R, class... A>
Method MakeMethod(const char* name, R (C::*mf)(A...) const) {
    return detail::MakeMethodImpl<C, decltype(mf), R, A...>(name, mf);
}

namespace detail {
template<class F, class R, class... A, std::size_t... I>
bool InvokeFreeImpl(F fn, const ReflectValue* a, ReflectValue& out, std::index_sequence<I...>) {
    (void)a;
    if constexpr (std::is_void_v<R>) {
        fn(FromRV<std::decay_t<A>>(a[I])...);
        out.type = FT::Unknown;
    } else {
        ToRV(fn(FromRV<std::decay_t<A>>(a[I])...), out);
    }
    return true;
}
}  // namespace detail

// STATIC/free function overload ([[nuke::func]] on a `static` method — a static member
// pointer IS a plain function pointer, so overload resolution picks this automatically).
// Script binders expose these as <Type>.<name>(...) — facade APIs bind with zero hardcode.
template<class R, class... A>
Method MakeMethod(const char* name, R (*fn)(A...)) {
    Method m;
    m.name = name;
    m.isStatic = true;
    if constexpr (!std::is_void_v<R>) m.ret = FieldTypeOf<std::decay_t<R>>();
    m.params = { FieldTypeOf<std::decay_t<A>>()... };
    m.invoke = [fn](void*, const ReflectValue* a, std::size_t n, ReflectValue& out) -> bool {
        if (n != sizeof...(A)) return false;
        return detail::InvokeFreeImpl<decltype(fn), R, A...>(fn, a, out, std::index_sequence_for<A...>{});
    };
    return m;
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
