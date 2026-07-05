#include "API/iGUI.h"
#include "interface/iGUI.h"
#include <boost/thread/mutex.hpp>
#include <cstring>
#include <map>
#include <vector>

namespace nuke {

// --- retained tree ------------------------------------------------------------------------
namespace {

enum NodeType { NT_Window, NT_Text, NT_Button, NT_Checkbox, NT_Slider, NT_Input, NT_Combo,
                NT_Image, NT_Progress, NT_Separator };

struct Node
{
	int         type = NT_Text;
	std::string id, parent;
	std::string label, text, items, texGuid;   // items: ';'-separated combo entries
	double      value = 0.0, lo = 0.0, hi = 1.0;
	float       w = 0.0f, h = 0.0f;
	bool        visible = true, sameLine = false;
	bool        clicked = false, changed = false;   // latched until read
	// window placement (applied on the next emit after SetRect)
	float rect[4] = { 0, 0, 0, 0 };
	bool  rectDirty = false;
	std::vector<std::string> children;              // windows only, declaration order
};

boost::mutex g_lock;
std::map<std::string, Node> g_nodes;
std::vector<std::string>    g_windows;   // declaration order

// Create-or-fetch a node of `type` under `parent` (windows: parent empty). A widget id
// is appended to its window's child list once, on creation.
Node& Declare(const std::string& id, int type, const std::string& parent)
{
	auto it = g_nodes.find(id);
	if (it != g_nodes.end()) { it->second.type = type; return it->second; }
	Node& n = g_nodes[id];
	n.id = id; n.type = type; n.parent = parent;
	if (type == NT_Window) g_windows.push_back(id);
	else
	{
		auto pit = g_nodes.find(parent);
		if (pit != g_nodes.end() && pit->second.type == NT_Window)
			pit->second.children.push_back(id);
	}
	return n;
}

Node* Find(const std::string& id)
{
	auto it = g_nodes.find(id);
	return it != g_nodes.end() ? &it->second : nullptr;
}

}  // namespace

// --- declaration ----------------------------------------------------------------------------
void Ui::Window(const std::string& id, const std::string& title)
{
	boost::mutex::scoped_lock l(g_lock);
	Declare(id, NT_Window, std::string()).label = title;
}
void Ui::Text(const std::string& id, const std::string& parent, const std::string& text)
{
	boost::mutex::scoped_lock l(g_lock);
	Declare(id, NT_Text, parent).text = text;
}
void Ui::Button(const std::string& id, const std::string& parent, const std::string& label)
{
	boost::mutex::scoped_lock l(g_lock);
	Declare(id, NT_Button, parent).label = label;
}
void Ui::Checkbox(const std::string& id, const std::string& parent, const std::string& label, bool value)
{
	boost::mutex::scoped_lock l(g_lock);
	const bool fresh = (g_nodes.find(id) == g_nodes.end());
	Node& n = Declare(id, NT_Checkbox, parent);
	n.label = label;
	if (fresh) n.value = value ? 1.0 : 0.0;   // re-declaration must not stomp live state
}
void Ui::Slider(const std::string& id, const std::string& parent, const std::string& label,
                double value, double lo, double hi)
{
	boost::mutex::scoped_lock l(g_lock);
	const bool fresh = (g_nodes.find(id) == g_nodes.end());
	Node& n = Declare(id, NT_Slider, parent);
	n.label = label; n.lo = lo; n.hi = hi;
	if (fresh) n.value = value;
}
void Ui::Input(const std::string& id, const std::string& parent, const std::string& label,
               const std::string& value)
{
	boost::mutex::scoped_lock l(g_lock);
	const bool fresh = (g_nodes.find(id) == g_nodes.end());
	Node& n = Declare(id, NT_Input, parent);
	n.label = label;
	if (fresh) n.text = value;
}
void Ui::Combo(const std::string& id, const std::string& parent, const std::string& label,
               const std::string& items, double index)
{
	boost::mutex::scoped_lock l(g_lock);
	const bool fresh = (g_nodes.find(id) == g_nodes.end());
	Node& n = Declare(id, NT_Combo, parent);
	n.label = label; n.items = items;
	if (fresh) n.value = index;
}
void Ui::Image(const std::string& id, const std::string& parent, const std::string& texGuid,
               double w, double h)
{
	boost::mutex::scoped_lock l(g_lock);
	Node& n = Declare(id, NT_Image, parent);
	n.texGuid = texGuid; n.w = (float)w; n.h = (float)h;
}
void Ui::Progress(const std::string& id, const std::string& parent, double fraction,
                  const std::string& overlay)
{
	boost::mutex::scoped_lock l(g_lock);
	Node& n = Declare(id, NT_Progress, parent);
	n.value = fraction; n.text = overlay;
}
void Ui::Separator(const std::string& id, const std::string& parent)
{
	boost::mutex::scoped_lock l(g_lock);
	Declare(id, NT_Separator, parent);
}

// --- layout / lifetime ------------------------------------------------------------------------
void Ui::SameLine(const std::string& id, bool on)
{
	boost::mutex::scoped_lock l(g_lock);
	if (Node* n = Find(id)) n->sameLine = on;
}
void Ui::Show(const std::string& id, bool visible)
{
	boost::mutex::scoped_lock l(g_lock);
	if (Node* n = Find(id)) n->visible = visible;
}
void Ui::SetRect(const std::string& id, double x, double y, double w, double h)
{
	boost::mutex::scoped_lock l(g_lock);
	if (Node* n = Find(id))
	{
		n->rect[0] = (float)x; n->rect[1] = (float)y; n->rect[2] = (float)w; n->rect[3] = (float)h;
		n->rectDirty = true;
	}
}
void Ui::Remove(const std::string& id)
{
	boost::mutex::scoped_lock l(g_lock);
	auto it = g_nodes.find(id);
	if (it == g_nodes.end()) return;
	if (it->second.type == NT_Window)
	{
		for (const std::string& c : it->second.children) g_nodes.erase(c);
		for (auto w = g_windows.begin(); w != g_windows.end(); ++w)
			if (*w == id) { g_windows.erase(w); break; }
	}
	else if (Node* p = Find(it->second.parent))
	{
		for (auto c = p->children.begin(); c != p->children.end(); ++c)
			if (*c == id) { p->children.erase(c); break; }
	}
	g_nodes.erase(it);
}
void Ui::Clear()
{
	boost::mutex::scoped_lock l(g_lock);
	g_nodes.clear();
	g_windows.clear();
}

// --- state / events -----------------------------------------------------------------------------
bool Ui::Clicked(const std::string& id)
{
	boost::mutex::scoped_lock l(g_lock);
	Node* n = Find(id);
	if (!n || !n->clicked) return false;
	n->clicked = false;
	return true;
}
bool Ui::Changed(const std::string& id)
{
	boost::mutex::scoped_lock l(g_lock);
	Node* n = Find(id);
	if (!n || !n->changed) return false;
	n->changed = false;
	return true;
}
double Ui::Value(const std::string& id)
{
	boost::mutex::scoped_lock l(g_lock);
	Node* n = Find(id);
	return n ? n->value : 0.0;
}
std::string Ui::TextOf(const std::string& id)
{
	boost::mutex::scoped_lock l(g_lock);
	Node* n = Find(id);
	return n ? n->text : std::string();
}
void Ui::SetValue(const std::string& id, double v)
{
	boost::mutex::scoped_lock l(g_lock);
	if (Node* n = Find(id)) n->value = v;
}
void Ui::SetText(const std::string& id, const std::string& text)
{
	boost::mutex::scoped_lock l(g_lock);
	if (Node* n = Find(id)) n->text = text;
}
void Ui::SetLabel(const std::string& id, const std::string& label)
{
	boost::mutex::scoped_lock l(g_lock);
	if (Node* n = Find(id)) n->label = label;
}

// --- per-frame emit through the immediate backend --------------------------------------------------
void Ui::Emit()
{
	boost::mutex::scoped_lock l(g_lock);
	iGUI* g = GUI();
	for (const std::string& wid : g_windows)
	{
		Node* win = Find(wid);
		if (!win || !win->visible) continue;
		if (win->rectDirty)
		{
			g->SetNextWindowRect(win->rect[0], win->rect[1], win->rect[2], win->rect[3]);
			win->rectDirty = false;
		}
		// "###id": the retained id is the window identity — retitling keeps pos/size.
		const std::string wname = win->label + "###" + win->id;
		const bool open = g->Begin(wname.c_str());
		if (open)
		{
			for (const std::string& cid : win->children)
			{
				Node* n = Find(cid);
				if (!n || !n->visible) continue;
				if (n->sameLine) g->SameLine();
				// "label###id": the retained id is the widget identity too — identical
				// labels in one window can't collide, relabeling keeps widget state.
				const std::string wl = n->label + "###" + n->id;
				switch (n->type)
				{
				case NT_Text:      g->Text(n->text.c_str()); break;
				case NT_Button:    if (g->Button(wl.c_str())) n->clicked = true; break;
				case NT_Checkbox:
				{
					bool v = n->value != 0.0;
					if (g->Checkbox(wl.c_str(), &v)) { n->value = v ? 1.0 : 0.0; n->changed = true; }
					break;
				}
				case NT_Slider:
				{
					float v = (float)n->value;
					if (g->SliderFloat(wl.c_str(), &v, (float)n->lo, (float)n->hi))
					{ n->value = v; n->changed = true; }
					break;
				}
				case NT_Input:
				{
					char buf[1024];
					const size_t len = n->text.size() < sizeof(buf) - 1 ? n->text.size() : sizeof(buf) - 1;
					std::memcpy(buf, n->text.data(), len);
					buf[len] = 0;
					if (g->InputText(wl.c_str(), buf, (int)sizeof(buf)))
					{ n->text = buf; n->changed = true; }
					break;
				}
				case NT_Combo:
				{
					// split items (';'-separated) into the seam's c-string array
					std::vector<std::string> keep;
					size_t start = 0;
					while (start <= n->items.size())
					{
						size_t e = n->items.find(';', start);
						if (e == std::string::npos) e = n->items.size();
						keep.push_back(n->items.substr(start, e - start));
						start = e + 1;
					}
					if (!keep.empty() && keep.back().empty()) keep.pop_back();
					if (keep.empty()) break;
					std::vector<const char*> ptrs;
					ptrs.reserve(keep.size());
					for (const std::string& s : keep) ptrs.push_back(s.c_str());
					int cur = (int)n->value;
					if (cur < 0) cur = 0;
					if (cur >= (int)ptrs.size()) cur = (int)ptrs.size() - 1;
					if (g->Combo(wl.c_str(), &cur, ptrs.data(), (int)ptrs.size()))
					{ n->value = cur; n->changed = true; }
					break;
				}
				case NT_Image:     g->Image(n->texGuid.c_str(), n->w, n->h); break;
				case NT_Progress:  g->ProgressBar((float)n->value, n->text.c_str()); break;
				case NT_Separator: g->Separator(); break;
				default: break;
				}
			}
		}
		g->End();   // ALWAYS paired with Begin (collapsed included) — backend contract
	}
}

}  // namespace nuke
