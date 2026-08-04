#pragma once
#ifndef NUKEE_REFLECT_H
#define NUKEE_REFLECT_H
#include "NukeAPI.h"
// Lightweight reflection: a per-type schema built at static-init time from member pointers.
// A field stores only a TYPE TAG (FT) + a raw address accessor — the engine must not depend
// on ImGui, so the editor switches on FT to pick a widget.

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <map>
#include <functional>
#include "API/Model/Vector.h"
#include "API/Model/Color.h"
// No <nlohmann/json.hpp> here on purpose — this header is pulled in by widely included component
// headers. JSON (de)serialization lives in ReflectJson.h.

namespace nuke {

class Atom;

// Supported field types. AtomRef = a live Atom carried as its stable id (a dead atom resolves to
// null, never freed memory). ObjectRef = a pointer to any reflected instance, carried as its
// engine object-handle id. ABI: append new tags at the END, never mid-enum.
enum class FT { Unknown, Bool, Int, Float, Double, String, Vec2, Vec3, Vec4, Quat, Color, AtomRef, ObjectRef,
                IntList, FloatList, DoubleList, StringList };   // std::vector<T> props (arrays in the inspector)

// AtomRef <-> live atom: walks the CURRENT world by stable id.
NUKEENGINE_API Atom*         Reflect_AtomById(unsigned long id);
NUKEENGINE_API unsigned long Reflect_AtomId(Atom* a);
// AtomRef props serialize as stable ids; loading queues a fixup resolved once the whole
// hierarchy exists.
NUKEENGINE_API void Reflect_QueueAtomRefFixup(Atom** slot, unsigned long id);
NUKEENGINE_API void Reflect_ResolveAtomRefs();
// Remap queued fixup ids through a clone's old->new atom id map, BEFORE Resolve, so a duplicated
// subtree references its own copies. Ids not in the map stay put.
NUKEENGINE_API void Reflect_RemapPendingAtomRefs(const std::map<unsigned long, unsigned long>& oldToNew);

// ObjectRef <-> live reflected instance. Reflect_ObjectPtr is IS-A checked against `typeName`
// (base chain), else null. Reflect_WrapObjectPtr wraps without ownership, deduped per instance.
// Reflect_DropObject invalidates every handle to the instance — owners MUST call it right
// before deleting a wrapped object.
NUKEENGINE_API void*         Reflect_ObjectPtr(unsigned long id, const char* typeName);
NUKEENGINE_API unsigned long Reflect_WrapObjectPtr(void* obj, const char* typeName);
NUKEENGINE_API void          Reflect_DropObject(void* obj);

namespace detail {
// Compile-time "is a NUKE_CLASS type": detected by the macro-provided __NukeTypeName().
template<class T, class = void> struct IsReflected : std::false_type {};
template<class T> struct IsReflected<T, std::void_t<decltype(T::__NukeTypeName())>> : std::true_type {};
}  // namespace detail

// ---- reflected ENUM types ----------------------------------------------------------------
// Specialize NukeEnumInfo for an enum (see Game.h / WindowMode) and [[nuke::func]] slots of that
// type generate a real typed enum in every binding. Unspecialized enums stay ints.
NUKEENGINE_API void Reflect_RegisterEnum(const std::string& name, const std::vector<std::string>& labels);
NUKEENGINE_API const std::vector<std::string>* Reflect_EnumLabels(const std::string& name);   // null if unknown
NUKEENGINE_API std::vector<std::string>        Reflect_AllEnumNames();

template<class E> struct NukeEnumInfo {
	static constexpr bool reflected = false;
	static const char* Name() { return ""; }
	static void        Register() {}
};

namespace detail {
// The reflected-enum name of a param/return type ("" = plain int). Registers the enum's labels
// on first use.
template<class T>
inline const char* EnumNameRegister()
{
	using U = std::decay_t<T>;
	if constexpr (std::is_enum_v<U> && NukeEnumInfo<U>::reflected)
	{
		NukeEnumInfo<U>::Register();
		return NukeEnumInfo<U>::Name();
	}
	else
		return "";
}
}  // namespace detail

// Map a C++ type -> FT tag. Enums reflect as Int (engine prop-enums declare `: int` so the
// addr-based int read/write is layout-exact); pointers to reflected classes are ObjectRef
// (func params/returns only, never a serialized field); everything else is Unknown unless
// specialized below.
template<class T> constexpr FT FieldTypeOf()
{
	if constexpr (std::is_pointer_v<T>)
		return detail::IsReflected<std::remove_cv_t<std::remove_pointer_t<T>>>::value ? FT::ObjectRef : FT::Unknown;
	else
		return std::is_enum_v<T> ? FT::Int : FT::Unknown;
}
template<> constexpr FT FieldTypeOf<bool>()        { return FT::Bool; }
template<> constexpr FT FieldTypeOf<int>()         { return FT::Int; }
template<> constexpr FT FieldTypeOf<long>()        { return FT::Int; }   // engine ids (Atom/Component ID)
template<> constexpr FT FieldTypeOf<unsigned long>() { return FT::Int; }
template<> constexpr FT FieldTypeOf<long long>()   { return FT::Int; }
template<> constexpr FT FieldTypeOf<float>()       { return FT::Float; }
template<> constexpr FT FieldTypeOf<double>()      { return FT::Double; }
template<> constexpr FT FieldTypeOf<std::string>() { return FT::String; }
template<> constexpr FT FieldTypeOf<Vector2>()     { return FT::Vec2; }
template<> constexpr FT FieldTypeOf<Vector3>()     { return FT::Vec3; }
template<> constexpr FT FieldTypeOf<Vector4>()     { return FT::Vec4; }
template<> constexpr FT FieldTypeOf<Quaternion>()  { return FT::Quat; }
template<> constexpr FT FieldTypeOf<Color>()       { return FT::Color; }
template<> constexpr FT FieldTypeOf<Atom*>()       { return FT::AtomRef; }
template<> constexpr FT FieldTypeOf<std::vector<int>>()         { return FT::IntList; }
template<> constexpr FT FieldTypeOf<std::vector<float>>()       { return FT::FloatList; }
template<> constexpr FT FieldTypeOf<std::vector<double>>()      { return FT::DoubleList; }
template<> constexpr FT FieldTypeOf<std::vector<std::string>>() { return FT::StringList; }

struct Field {
    std::string name;
    FT type = FT::Unknown;
    std::function<void*(void*)> addr;   // returns &(obj->field)
    bool hidden = false;                // serialized, but not drawn in the auto-inspector
    // [[nuke::prop(asset="...")]]: this String field holds an asset GUID of that kind
    // ("mesh"/"material"/"shader"/"texture") — the inspector draws an asset picker. "" = plain field.
    std::string asset;
    std::string label;   // [[nuke::prop(label="...")]] inspector display name (else `name`)
    float fmin = 0.0f, fmax = 0.0f;     // [[nuke::prop(min,max)]] slider range; fmax > fmin = has a range
    std::vector<std::string> enumLabels;// [[nuke::prop(enum="A,B,C")]] dropdown labels; the int is the index
    std::string tip;                    // [[nuke::prop(tip="...")]] inspector tooltip
    // [[nuke::prop(widget="...")]] named custom widget. Known: "layers" (int bitmask over
    // nuke::Layers). Unknown names fall back to the default widget.
    std::string widget;
};

// One reflected value crossing the scripting boundary. `type` says which member is valid:
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
	unsigned long obj  = 0;   // ObjectRef: the engine object-handle id (0 = null)
};

namespace detail {
// ReflectValue -> typed argument. The primary handles enums and pointers to reflected classes;
// every other supported FT specializes below, so an unsupported parameter type is a compile error.
template<class T> T FromRV(const ReflectValue& v)
{
	if constexpr (std::is_enum_v<T>)
		return (T)(long long)v.num;
	else
	{
		static_assert(std::is_pointer_v<T> &&
		              IsReflected<std::remove_cv_t<std::remove_pointer_t<T>>>::value,
		              "[[nuke::func]]: unsupported parameter type");
		using P = std::remove_cv_t<std::remove_pointer_t<T>>;
		return (T)Reflect_ObjectPtr(v.obj, P::__NukeTypeName());
	}
}
template<> inline bool        FromRV<bool>(const ReflectValue& v)        { return v.b; }
template<> inline int         FromRV<int>(const ReflectValue& v)         { return (int)v.num; }
template<> inline long        FromRV<long>(const ReflectValue& v)        { return (long)v.num; }
template<> inline unsigned long FromRV<unsigned long>(const ReflectValue& v) { return (unsigned long)v.num; }
template<> inline long long   FromRV<long long>(const ReflectValue& v)   { return (long long)v.num; }
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
inline void ToRV(long x, ReflectValue& o)               { o.type = FT::Int;    o.num = (double)x; }
inline void ToRV(unsigned long x, ReflectValue& o)      { o.type = FT::Int;    o.num = (double)x; }
inline void ToRV(long long x, ReflectValue& o)          { o.type = FT::Int;    o.num = (double)x; }
// Enum return -> its numeric value (matches the FieldTypeOf primary: enums are Int).
template<class T> inline std::enable_if_t<std::is_enum_v<T>> ToRV(T x, ReflectValue& o)
{ o.type = FT::Int; o.num = (double)(long long)x; }
// Reflected-class pointer return -> a deduped, non-owning object handle. The exact ToRV(Atom*)
// overload below still wins for Atom*, keeping atoms on the AtomRef channel.
template<class T> inline std::enable_if_t<IsReflected<T>::value> ToRV(T* x, ReflectValue& o)
{ o.type = FT::ObjectRef; o.obj = Reflect_WrapObjectPtr((void*)x, T::__NukeTypeName()); }
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
// A reflected method: FT-typed signature + a type-erased invoker. Overloads are NOT supported
// (one reflected method per name); parameters/returns must be supported FT types (by value or
// const&; return void or by value).
struct Method {
    std::string name;
    FT ret = FT::Unknown;                 // FT::Unknown = void
    std::vector<FT> params;               // declared parameter types, in order
    // Reflected class names behind ref-typed slots ("" = plain value); "Atom" for AtomRef.
    // Typed-wrapper generators read these to emit real classes.
    std::vector<std::string> paramClass;
    std::string retClass;
    // Reflected enum names behind Int-typed slots ("" = plain int), so generators emit the
    // real enum type instead of an int.
    std::vector<std::string> paramEnum;
    std::string retEnum;
    bool isStatic = false;                // static/free function: invoke ignores `obj`
    // Invoke on an instance of the owning type (null for statics). `args` must match params:
    // the count is checked, the types are trusted.
    std::function<bool(void* obj, const ReflectValue* args, std::size_t n, ReflectValue& ret)> invoke;
};

struct TypeInfo {
    std::string name;
    std::string base;
    std::vector<Field> fields;
    std::vector<Method> methods;          // [[nuke::func]]-tagged methods
    std::function<void*()> create;        // factory for create-by-name on load
    std::string category;                 // Add Component menu grouping; "" = Other
    // When `base` names another reflected type its fields/methods fold in lazily. Valid only for
    // single non-virtual inheritance (base subobject at offset 0, so addr functors stay valid).
    bool baseMerged = false;
};

// ---- SCRIPT classes in the shared registry --------------------------------------------------
// A scripting module publishes the classes it can run (one Lua file, one C# behaviour class...)
// and their editable props HERE, so every reflection-driven UI — the animation/sequencer prop
// pickers above all — sees script props exactly like native reflected ones. The class is not a
// C++ type: it is REACHED through a host component (ScriptComponent / CSharpScript) whose
// `selector` field (script path / class name) names it, and its props are read and written
// through Component::DynamicProps / SetDynamicProp.
struct ScriptProp
{
	std::string name;
	FT type = FT::Unknown;   // Bool / Double / String (what a script prop can be)
};
struct ScriptClass
{
	std::string name;        // display name ("player.lua", "PlayerController")
	std::string component;   // host component TYPE name ("ScriptComponent", "CSharpScript")
	std::string selector;    // value of the host's script/class field that selects this class
	std::vector<ScriptProp> props;
};
// Register (replacing by component+selector) / drop one module's classes on reload / list all.
NUKEENGINE_API void Reflect_RegisterScriptClass(const ScriptClass& c);
NUKEENGINE_API void Reflect_ClearScriptClasses(const std::string& component);
NUKEENGINE_API std::vector<ScriptClass> Reflect_ScriptClasses();

// Registry (defined in Reflect.cpp).
NUKEENGINE_API TypeInfo& Registry_GetOrCreate(const std::string& name);
NUKEENGINE_API TypeInfo* Registry_Find(const std::string& name);
NUKEENGINE_API std::vector<TypeInfo*> Registry_All();   // every registered type
// True when the type is a component through the whole reflected base chain.
NUKEENGINE_API bool Registry_IsComponentType(const TypeInfo* ti);

// Registers every reflected type's schema + factory; defined in the generated Reflect.gen.cpp.
// Must be called once so the generated .obj is linked and runs.
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

// The reflected class name behind a ref-typed slot ("Atom" for Atom*, the NUKE_CLASS name for
// reflected pointers, "" for plain values). See Method::paramClass.
template<class T>
inline const char* RefClassOf()
{
    if constexpr (std::is_same_v<T, Atom*>) return "Atom";
    else if constexpr (std::is_pointer_v<T>)
    {
        using P = std::remove_cv_t<std::remove_pointer_t<T>>;
        if constexpr (IsReflected<P>::value) return P::__NukeTypeName();
        else return "";
    }
    else return "";
}

template<class R, class... A>
inline void FillMethodClasses(Method& m) {
    if constexpr (!std::is_void_v<R>) { m.retClass = RefClassOf<std::decay_t<R>>(); m.retEnum = EnumNameRegister<R>(); }
    m.paramClass = { std::string(RefClassOf<std::decay_t<A>>())... };
    m.paramEnum  = { std::string(EnumNameRegister<A>())... };
}

template<class C, class M, class R, class... A>
Method MakeMethodImpl(const char* name, M mf) {
    Method m;
    m.name = name;
    if constexpr (!std::is_void_v<R>) m.ret = FieldTypeOf<std::decay_t<R>>();
    m.params = { FieldTypeOf<std::decay_t<A>>()... };
    FillMethodClasses<R, A...>(m);
    m.invoke = [mf](void* o, const ReflectValue* a, std::size_t n, ReflectValue& out) -> bool {
        if (n != sizeof...(A)) return false;
        return InvokeImpl<C, M, R, A...>(mf, (C*)o, a, out, std::index_sequence_for<A...>{});
    };
    return m;
}
}  // namespace detail

// Build a Method from a member-function pointer (deduces the FT signature). invoke's `obj`
// must be an instance of C.
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

// Static/free function overload; script binders expose these as <Type>.<name>(...).
template<class R, class... A>
Method MakeMethod(const char* name, R (*fn)(A...)) {
    Method m;
    m.name = name;
    m.isStatic = true;
    if constexpr (!std::is_void_v<R>) m.ret = FieldTypeOf<std::decay_t<R>>();
    m.params = { FieldTypeOf<std::decay_t<A>>()... };
    detail::FillMethodClasses<R, A...>(m);
    m.invoke = [fn](void*, const ReflectValue* a, std::size_t n, ReflectValue& out) -> bool {
        if (n != sizeof...(A)) return false;
        return detail::InvokeFreeImpl<decltype(fn), R, A...>(fn, a, out, std::index_sequence_for<A...>{});
    };
    return m;
}

} // namespace nuke

// --- authoring macros -------------------------------------------------------
//   NUKE_CLASS(Camera, Component, "Rendering")   // one line in the class body
//   [[nuke::prop]] float fov = 90.0f;            // attribute on each serialized field
// NUKE_CLASS provides the type name + virtual GetType(); the field/factory registration is
// emitted into Reflect.gen.cpp by tools/nukegen.py, which scans for NUKE_CLASS + [[nuke::prop]].
// The optional 3rd argument is the editor category; nukegen reads it, the macro swallows it.
#define NUKE_CLASS(Class_, Base_, ...)                                              \
    public:                                                                        \
        using Self = Class_;                                                       \
        static const char* __NukeTypeName() { return #Class_; }                    \
        virtual ::nuke::TypeInfo* GetType() { return &::nuke::TypeOf<Class_>(); }

// Same, for types that are never created by name (no factory generated, e.g. Transform).
#define NUKE_CLASS_NOCREATE(Class_, Base_, ...) NUKE_CLASS(Class_, Base_)

#endif // NUKEE_REFLECT_H
