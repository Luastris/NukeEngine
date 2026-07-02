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
    World* w = AppInstance::GetSingleton()->currentScene;
    return w ? w->GetById((long)id) : nullptr;
}

unsigned long Reflect_AtomId(Atom* a)
{
    return a ? a->id.id : 0;
}

} // namespace nuke
