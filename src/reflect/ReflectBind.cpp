#include "reflect/ReflectBind.h"
#include "API/Model/Atom.h"
#include "API/Model/Component.h"
#include "API/Model/World.h"
#include "interface/AppInstance.h"

namespace nuke {

const Field* Reflect_FindField(const TypeInfo* type, const std::string& name)
{
	while (type)
	{
		for (const Field& f : type->fields)
			if (f.name == name) return &f;
		type = type->base.empty() ? nullptr : Registry_Find(type->base);
	}
	return nullptr;
}

const Method* Reflect_FindMethod(const TypeInfo* type, const std::string& name)
{
	while (type)
	{
		for (const Method& m : type->methods)
			if (m.name == name) return &m;
		type = type->base.empty() ? nullptr : Registry_Find(type->base);
	}
	return nullptr;
}

bool Reflect_Invoke(void* obj, const Method& m, const ReflectValue* args, std::size_t n, ReflectValue& ret)
{
	if (!m.invoke) return false;
	if (!obj && !m.isStatic) return false;   // instance methods need an instance; statics take null
	ret = ReflectValue();
	return m.invoke(obj, args, n, ret);
}

ReflectValue Reflect_GetField(void* obj, const Field& f)
{
	ReflectValue out;
	out.type = f.type;
	void* p = f.addr ? f.addr(obj) : nullptr;
	if (!p) { out.type = FT::Unknown; return out; }
	switch (f.type)
	{
		case FT::Bool:   out.b   = *(bool*)p; break;
		case FT::Int:    out.num = *(int*)p; break;
		case FT::Float:  out.num = *(float*)p; break;
		case FT::Double: out.num = *(double*)p; break;
		case FT::String: out.str = *(std::string*)p; break;
		case FT::Vec2:  { Vector2& v = *(Vector2*)p; out.v[0] = v.x; out.v[1] = v.y; break; }
		case FT::Vec3:  { Vector3& v = *(Vector3*)p; out.v[0] = v.x; out.v[1] = v.y; out.v[2] = v.z; break; }
		case FT::Vec4:  { Vector4& v = *(Vector4*)p; out.v[0] = v.x; out.v[1] = v.y; out.v[2] = v.z; out.v[3] = v.w; break; }
		case FT::Quat:  { Quaternion& q = *(Quaternion*)p; out.v[0] = q.x; out.v[1] = q.y; out.v[2] = q.z; out.v[3] = q.w; break; }
		case FT::Color: { Color& c = *(Color*)p; out.v[0] = c.r; out.v[1] = c.g; out.v[2] = c.b; out.v[3] = c.a; break; }
		default: out.type = FT::Unknown; break;
	}
	return out;
}

bool Reflect_SetField(void* obj, const Field& f, const ReflectValue& v)
{
	void* p = f.addr ? f.addr(obj) : nullptr;
	if (!p) return false;
	switch (f.type)
	{
		case FT::Bool:   *(bool*)p        = v.b; return true;
		case FT::Int:    *(int*)p         = (int)v.num; return true;
		case FT::Float:  *(float*)p       = (float)v.num; return true;
		case FT::Double: *(double*)p      = v.num; return true;
		case FT::String: *(std::string*)p = v.str; return true;
		case FT::Vec2:  { Vector2& d = *(Vector2*)p; d.x = v.v[0]; d.y = v.v[1]; return true; }
		case FT::Vec3:  { Vector3& d = *(Vector3*)p; d.x = v.v[0]; d.y = v.v[1]; d.z = v.v[2]; return true; }
		case FT::Vec4:  { Vector4& d = *(Vector4*)p; d.x = v.v[0]; d.y = v.v[1]; d.z = v.v[2]; d.w = v.v[3]; return true; }
		case FT::Quat:  { Quaternion& q = *(Quaternion*)p; q.x = v.v[0]; q.y = v.v[1]; q.z = v.v[2]; q.w = v.v[3]; return true; }
		case FT::Color: { Color& c = *(Color*)p; c.r = v.v[0]; c.g = v.v[1]; c.b = v.v[2]; c.a = v.v[3]; return true; }
		default: return false;
	}
}

std::vector<std::string> Reflect_ComponentTypes()
{
	std::vector<std::string> out;
	for (TypeInfo* t : Registry_All())
		if (t && t->create && t->base == "Component") out.push_back(t->name);
	return out;
}

Component* Reflect_FindComponent(Atom* atom, const std::string& typeName)
{
	if (!atom || typeName.empty()) return nullptr;
	for (Component* c : atom->components)
		if (c && c->GetType() && c->GetType()->name == typeName) return c;
	return nullptr;
}

Component* Reflect_AddComponent(Atom* atom, const std::string& typeName)
{
	if (!atom) return nullptr;
	TypeInfo* ti = Registry_Find(typeName);
	if (!ti || !ti->create || ti->base != "Component") return nullptr;   // same guard as the editor menu
	Component* c = (Component*)ti->create();
	atom->AddComponent(c);   // Init: sets atom/transform + registers in atom->components
	return c;
}

Component* Reflect_ResolveComponent(unsigned long atomId, unsigned long componentId)
{
	World* w = AppInstance::GetSingleton()->currentScene;
	if (!w) return nullptr;
	Atom* a = w->GetById((long)atomId);
	if (!a) return nullptr;
	for (Component* c : a->components)
		if (c && c->id.id == componentId) return c;
	return nullptr;
}

}  // namespace nuke
