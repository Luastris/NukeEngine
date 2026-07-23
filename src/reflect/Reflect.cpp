#include "reflect/ReflectJson.h"
#include "API/Model/Atom.h"          // AtomRef resolution (stable id <-> live atom)
#include "API/Model/World.h"
#include "interface/AppInstance.h"
#include <unordered_map>

namespace nuke {

// --- registry: type name -> TypeInfo (stable addresses, so we use pointers) ---
static std::unordered_map<std::string, TypeInfo*>& registry()
{
    static std::unordered_map<std::string, TypeInfo*> r;
    return r;
}

TypeInfo& Registry_GetOrCreate(const std::string& name)
{
    auto& r = registry();
    auto it = r.find(name);
    if (it != r.end()) return *it->second;
    TypeInfo* ti = new TypeInfo();   // leaked intentionally (lives for the program)
    ti->name = name;
    r[name] = ti;
    return *ti;
}

TypeInfo* Registry_Find(const std::string& name)
{
    auto& r = registry();
    auto it = r.find(name);
    return (it != r.end()) ? it->second : nullptr;
}

std::vector<TypeInfo*> Registry_All()
{
    std::vector<TypeInfo*> out;
    for (auto& kv : registry())
        out.push_back(kv.second);
    return out;
}

// --- reflected enums: name -> ordered labels (index = enum value) ---------------------
static std::unordered_map<std::string, std::vector<std::string>>& enumRegistry()
{
    static std::unordered_map<std::string, std::vector<std::string>> r;
    return r;
}

void Reflect_RegisterEnum(const std::string& name, const std::vector<std::string>& labels)
{
    if (!name.empty()) enumRegistry()[name] = labels;   // idempotent (re-register overwrites)
}

const std::vector<std::string>* Reflect_EnumLabels(const std::string& name)
{
    auto it = enumRegistry().find(name);
    return it != enumRegistry().end() ? &it->second : nullptr;
}

std::vector<std::string> Reflect_AllEnumNames()
{
    std::vector<std::string> out;
    for (auto& kv : enumRegistry()) out.push_back(kv.first);
    return out;
}

// --- AtomRef prop fixup: refs load as stable ids and resolve AFTER the whole world exists ------------
// LoadField can't resolve an Atom* immediately (the target atom may not be loaded yet), so it queues
// the slot + id here; World::Load* / prefab instantiation call Reflect_ResolveAtomRefs() once the
// hierarchy is complete. Unresolvable ids become null.
static std::vector<std::pair<Atom**, unsigned long>>& pendingAtomRefs()
{
    static std::vector<std::pair<Atom**, unsigned long>> v;
    return v;
}
void Reflect_QueueAtomRefFixup(Atom** slot, unsigned long id)
{
    if (slot) pendingAtomRefs().push_back({ slot, id });
}
void Reflect_ResolveAtomRefs()
{
    for (auto& p : pendingAtomRefs())
        *p.first = Reflect_AtomById(p.second);
    pendingAtomRefs().clear();
}

// --- single value <-> json by tag ---
void SaveField(FT t, const void* a, json& j)
{
    switch (t)
    {
        case FT::Bool:   j = *(const bool*)a; break;
        case FT::Int:    j = *(const int*)a; break;
        case FT::Float:  j = *(const float*)a; break;
        case FT::Double: j = *(const double*)a; break;
        case FT::String: j = *(const std::string*)a; break;
        case FT::Vec2: { auto v = (const Vector2*)a;    j = { v->x, v->y }; } break;
        case FT::Vec3: { auto v = (const Vector3*)a;    j = { v->x, v->y, v->z }; } break;
        case FT::Vec4: { auto v = (const Vector4*)a;    j = { v->x, v->y, v->z, v->w }; } break;
        case FT::Quat: { auto v = (const Quaternion*)a; j = { v->x, v->y, v->z, v->w }; } break;
        case FT::Color:{ auto v = (const Color*)a;      j = { v->r, v->g, v->b, v->a }; } break;
        case FT::AtomRef: j = (unsigned long long)Reflect_AtomId(*(Atom* const*)a); break;   // stable id (0 = null)
        case FT::IntList:    j = *(const std::vector<int>*)a; break;
        case FT::FloatList:  j = *(const std::vector<float>*)a; break;
        case FT::DoubleList: j = *(const std::vector<double>*)a; break;
        case FT::StringList: j = *(const std::vector<std::string>*)a; break;
        default: break;
    }
}

void LoadField(FT t, void* a, const json& j)
{
    switch (t)
    {
        case FT::Bool:   *(bool*)a   = j.get<bool>(); break;
        case FT::Int:    *(int*)a    = j.get<int>(); break;
        case FT::Float:  *(float*)a  = j.get<float>(); break;
        case FT::Double: *(double*)a = j.get<double>(); break;
        case FT::String: *(std::string*)a = j.get<std::string>(); break;
        case FT::Vec2: { auto v = (Vector2*)a;    v->x = j.at(0); v->y = j.at(1); } break;
        case FT::Vec3: { auto v = (Vector3*)a;    v->x = j.at(0); v->y = j.at(1); v->z = j.at(2); } break;
        case FT::Vec4: { auto v = (Vector4*)a;    v->x = j.at(0); v->y = j.at(1); v->z = j.at(2); v->w = j.at(3); } break;
        case FT::Quat: { auto v = (Quaternion*)a; v->x = j.at(0); v->y = j.at(1); v->z = j.at(2); v->w = j.at(3); } break;
        case FT::Color:{ auto v = (Color*)a;      v->r = j.at(0); v->g = j.at(1); v->b = j.at(2); v->a = j.at(3); } break;
        case FT::AtomRef:   // resolve AFTER the world finishes loading (the target may not exist yet)
        {
            *(Atom**)a = nullptr;
            unsigned long id = (unsigned long)j.get<unsigned long long>();
            if (id) Reflect_QueueAtomRefFixup((Atom**)a, id);
        } break;
        case FT::IntList:    if (j.is_array()) *(std::vector<int>*)a         = j.get<std::vector<int>>(); break;
        case FT::FloatList:  if (j.is_array()) *(std::vector<float>*)a       = j.get<std::vector<float>>(); break;
        case FT::DoubleList: if (j.is_array()) *(std::vector<double>*)a      = j.get<std::vector<double>>(); break;
        case FT::StringList: if (j.is_array()) *(std::vector<std::string>*)a = j.get<std::vector<std::string>>(); break;
        default: break;
    }
}

// --- whole object <-> json ---
void SaveObject(const TypeInfo& ti, const void* obj, json& j)
{
    for (const Field& f : ti.fields)
        SaveField(f.type, f.addr(const_cast<void*>(obj)), j[f.name]);
}

void LoadObject(const TypeInfo& ti, void* obj, const json& j)
{
    for (const Field& f : ti.fields)
        if (j.contains(f.name))
            LoadField(f.type, f.addr(obj), j.at(f.name));
}

// --- AtomRef resolution (FT::AtomRef travels as the atom's stable id) -----------------
Atom* Reflect_AtomById(unsigned long id)
{
    if (!id) return nullptr;
    World* w = AppInstance::GetSingleton()->currentWorld;
    return w ? w->GetById((long)id) : nullptr;
}

unsigned long Reflect_AtomId(Atom* a)
{
    return a ? a->id.id : 0;
}

} // namespace nuke
