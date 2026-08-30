// Child-variant switching by name prefix (see VariantSet.h).
#include "API/Model/VariantSet.h"
#include "API/Model/Atom.h"

namespace nuke {

void VariantSet::Init(Atom* parent)
{
	atom = parent;
	transform = &parent->GetTransform();
	parent->components.push_back(this);
}

bool VariantSet::Split(const std::string& name, std::string& group, std::string& item)
{
	const size_t p = name.find(" - ");
	if (p == 0 || p == std::string::npos || p + 3 >= name.size()) return false;
	group = name.substr(0, p);
	item = name.substr(p + 3);
	while (!item.empty() && item.back() == ' ') item.pop_back();   // "H - Fluffy " happens
	return !item.empty();
}

std::string VariantSet::Groups()
{
	std::string out;
	if (!atom) return out;
	for (Atom* ch : atom->children)
	{
		std::string g, it;
		if (!ch || !Split(ch->GetName(), g, it)) continue;
		const std::string tag = ";" + out + ";";
		if (tag.find(";" + g + ";") == std::string::npos)
			out += (out.empty() ? "" : ";") + g;
	}
	return out;
}

std::string VariantSet::Items(const std::string& group)
{
	std::string out;
	if (!atom) return out;
	for (Atom* ch : atom->children)
	{
		std::string g, it;
		if (!ch || !Split(ch->GetName(), g, it) || g != group) continue;
		out += (out.empty() ? "" : ";") + it;
	}
	return out;
}

std::string VariantSet::Selected(const std::string& group)
{
	std::string out;
	if (!atom) return out;
	for (Atom* ch : atom->children)
	{
		std::string g, it;
		if (!ch || !ch->enabled || !Split(ch->GetName(), g, it) || g != group) continue;
		out += (out.empty() ? "" : ";") + it;
	}
	return out;
}

bool VariantSet::IsMulti(const std::string& group)
{
	size_t p = 0;
	while (p <= multi.size())
	{
		const size_t sc = multi.find(';', p);
		const std::string tok = multi.substr(p, sc == std::string::npos ? std::string::npos : sc - p);
		if (tok == group) return true;
		if (sc == std::string::npos) break;
		p = sc + 1;
	}
	return false;
}

void VariantSet::Select(const std::string& group, const std::string& item, bool on)
{
	if (!atom) return;
	Atom* target = nullptr;
	for (Atom* ch : atom->children)
	{
		std::string g, it;
		if (!ch || !Split(ch->GetName(), g, it) || g != group) continue;
		if (it == item || ch->GetName() == item) { target = ch; break; }
	}
	if (!target) return;
	if (on && !IsMulti(group))
		for (Atom* ch : atom->children)
		{
			std::string g, it;
			if (ch && ch != target && Split(ch->GetName(), g, it) && g == group)
				ch->enabled = false;
		}
	target->enabled = on;
}

}  // namespace nuke
