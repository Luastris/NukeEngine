#include "input/Input.h"
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

// Runtime + serialization of the gameplay input system (model in input/InputTypes.h, public surface in
// input/Input.h). Single-threaded: fed on the game thread, evaluated once per frame in Update().
namespace nuke {

using json = nlohmann::json;

// -- raw control state (per string id) --
struct ControlState
{
	float  value = 0.0f, prev = 0.0f;
	double downTime = -1e9, upTime = -1e9;   // last down / up transition time
	double prevDownTime = -1e9;               // the down BEFORE the last (double-press)
	bool   downEdge = false, upEdge = false;  // transition happened THIS frame
};

// -- per-binding runtime (chord completion / sequence progress) --
struct BindingRT
{
	bool   wasComplete = false;
	double completeTime = -1e9, uncompleteTime = -1e9, lastCompleteTime = -1e9;
	bool   longFired = false;
	int    seqIndex = 0;
	double seqLastTime = -1e9;
};
struct EvalBinding { InputBinding b; BindingRT rt; };
struct EvalContext { std::string name; int priority = 0; bool active = false; std::vector<EvalBinding> bindings; };

// -- core state --
static std::unordered_map<std::string, ControlState> g_controls;
static std::vector<InputAction>  g_actions;
static std::vector<EvalContext>  g_ctx;
static std::unordered_map<std::string, ActionState> g_state;
static std::vector<std::pair<std::string, std::function<void()>>> g_providers;
static std::vector<std::pair<std::string, InputBinding>>          g_userBindings;   // per-action overrides layered on defaults
static std::unordered_map<std::string, std::vector<InputBinding>> g_userCtx;        // FULL-context overrides (remap editor)
struct CB { int id; std::string action; InputPhase phase; std::function<void()> fn; };
static std::vector<CB> g_cbs;
static double g_now = 0.0;
static int    g_cbSeq = 0;

static EvalContext* findCtx(const std::string& name)
{
	for (EvalContext& c : g_ctx) if (c.name == name) return &c;
	return nullptr;
}
static ActionState& stateOf(const std::string& a) { return g_state[a]; }
static const ActionValueType actionType(const std::string& a)
{
	for (const InputAction& ia : g_actions) if (ia.name == a) return ia.type;
	return ActionValueType::Bool;
}

// ---- raw feed / providers ----------------------------------------------------------------------------
void Input::SetControl(const std::string& id, float value) { g_controls[id].value = value; }
float Input::Control(const std::string& id) { auto it = g_controls.find(id); return it == g_controls.end() ? 0.0f : it->second.value; }
void Input::RegisterProvider(const std::string& name, std::function<void()> poll)
{
	for (auto& p : g_providers) if (p.first == name) { p.second = poll; return; }
	g_providers.push_back({ name, poll });
}

// ---- model authoring ---------------------------------------------------------------------------------
void Input::DefineAction(const std::string& name, ActionValueType type)
{
	for (InputAction& a : g_actions) if (a.name == name) { a.type = type; return; }
	g_actions.push_back({ name, type });
}
void Input::DefineContext(const std::string& name, int priority)
{
	if (EvalContext* c = findCtx(name)) { c->priority = priority; return; }
	g_ctx.push_back({ name, priority, false, {} });
}
void Input::AddBinding(const std::string& context, const InputBinding& b)
{
	EvalContext* c = findCtx(context);
	if (!c) { DefineContext(context, 0); c = findCtx(context); }
	c->bindings.push_back({ b, {} });
}

// ---- contexts on the fly -----------------------------------------------------------------------------
void Input::PushContext(const std::string& name)   { if (EvalContext* c = findCtx(name)) c->active = true; }
void Input::PopContext(const std::string& name)    { if (EvalContext* c = findCtx(name)) c->active = false; }
void Input::SetContextActive(const std::string& name, bool on) { if (EvalContext* c = findCtx(name)) c->active = on; }
bool Input::ContextActive(const std::string& name) { EvalContext* c = findCtx(name); return c && c->active; }

// ---- callbacks ---------------------------------------------------------------------------------------
int  Input::OnAction(const std::string& action, InputPhase phase, std::function<void()> cb)
{ int id = ++g_cbSeq; g_cbs.push_back({ id, action, phase, std::move(cb) }); return id; }
void Input::Unsubscribe(int subId)
{ g_cbs.erase(std::remove_if(g_cbs.begin(), g_cbs.end(), [subId](const CB& c) { return c.id == subId; }), g_cbs.end()); }

// ---- query -------------------------------------------------------------------------------------------
bool Input::Pressed(const std::string& a)       { auto it = g_state.find(a); return it != g_state.end() && it->second.pressed; }
bool Input::Held(const std::string& a)          { auto it = g_state.find(a); return it != g_state.end() && it->second.held; }
bool Input::Released(const std::string& a)      { auto it = g_state.find(a); return it != g_state.end() && it->second.released; }
bool Input::Tapped(const std::string& a)        { auto it = g_state.find(a); return it != g_state.end() && it->second.tapped; }
bool Input::LongPressed(const std::string& a)   { auto it = g_state.find(a); return it != g_state.end() && it->second.longPressed; }
bool Input::DoublePressed(const std::string& a) { auto it = g_state.find(a); return it != g_state.end() && it->second.doublePressed; }
float Input::Value(const std::string& a)        { auto it = g_state.find(a); return it == g_state.end() ? 0.0f : it->second.value; }
Vector2 Input::Axis2(const std::string& a)      { auto it = g_state.find(a); return it == g_state.end() ? Vector2(0, 0) : Vector2(it->second.x, it->second.y); }

// ---- evaluation --------------------------------------------------------------------------------------
static bool ctrlDown(const std::string& id) { auto it = g_controls.find(id); return it != g_controls.end() && it->second.value >= 0.5f; }

// A chord's analog magnitude (single control passes its analog value; multi-control chord is 0/1).
static float bindingAnalog(const InputBinding& b)
{
	float v = 0.0f;
	if (b.controls.size() == 1) { auto it = g_controls.find(b.controls[0]); v = (it == g_controls.end()) ? 0.0f : it->second.value; }
	else { bool all = true; for (const std::string& c : b.controls) all = all && ctrlDown(c); v = all ? 1.0f : 0.0f; }
	if (b.invert) v = -v;
	if (b.deadzone > 0.0f && v > -b.deadzone && v < b.deadzone) v = 0.0f;
	return v * b.scale;
}
static bool modsHeld(const InputBinding& b)
{ for (const std::string& m : b.modifiers) if (!ctrlDown(m)) return false; return true; }

void Input::Update(double dt)
{
	g_now += dt;

	// 1) providers feed raw controls
	for (auto& p : g_providers) if (p.second) p.second();

	// 2) control edges + timing
	for (auto& kv : g_controls)
	{
		ControlState& s = kv.second;
		bool down = s.value >= 0.5f, wasDown = s.prev >= 0.5f;
		s.downEdge = down && !wasDown;
		s.upEdge   = !down && wasDown;
		if (s.downEdge) { s.prevDownTime = s.downTime; s.downTime = g_now; }
		if (s.upEdge)   s.upTime = g_now;
		s.prev = s.value;
	}

	// 3) reset per-frame action states (values recomputed fresh each frame)
	for (auto& kv : g_state) kv.second = ActionState{};

	// 4) evaluate contexts, highest priority first; a consuming match hides controls from lower contexts
	std::vector<EvalContext*> order;
	for (EvalContext& c : g_ctx) if (c.active) order.push_back(&c);
	std::sort(order.begin(), order.end(), [](EvalContext* a, EvalContext* b) { return a->priority > b->priority; });
	std::unordered_set<std::string> consumed;

	for (EvalContext* c : order)
	{
		for (EvalBinding& eb : c->bindings)
		{
			InputBinding& b = eb.b; BindingRT& rt = eb.rt;
			if (b.controls.empty()) continue;

			// blocked if a higher-priority context consumed any of our controls
			bool blocked = false;
			for (const std::string& ctl : b.controls) if (consumed.count(ctl)) { blocked = true; break; }
			if (blocked) continue;

			ActionState& as = stateOf(b.action);
			ActionValueType vt = actionType(b.action);

			if (b.sequence)
			{
				// SEQUENTIAL combo: advance on each control's press edge within the window; fire on the last.
				if (rt.seqIndex > 0 && (g_now - rt.seqLastTime) > b.sequenceWindow) rt.seqIndex = 0;   // timed out
				const std::string& want = b.controls[rt.seqIndex];
				auto it = g_controls.find(want);
				bool wantEdge = it != g_controls.end() && it->second.downEdge;
				if (wantEdge && modsHeld(b))
				{
					rt.seqIndex++; rt.seqLastTime = g_now;
					if (rt.seqIndex >= (int)b.controls.size())
					{
						rt.seqIndex = 0;
						if (b.phase == InputPhase::Pressed || b.phase == InputPhase::Tap) { as.pressed = true; as.tapped = true; }
						as.held = true; as.value = 1.0f;
						if (b.consume) for (const std::string& ctl : b.controls) consumed.insert(ctl);
					}
				}
				continue;
			}

			// PARALLEL chord (or single control): complete = all controls down + mods held
			bool complete = modsHeld(b);
			for (const std::string& ctl : b.controls) complete = complete && ctrlDown(ctl);

			bool justComplete   = complete && !rt.wasComplete;
			bool justUncomplete = !complete && rt.wasComplete;
			if (justComplete)   { rt.lastCompleteTime = rt.completeTime; rt.completeTime = g_now; rt.longFired = false; }
			if (justUncomplete) rt.uncompleteTime = g_now;

			// continuous value + held (independent of the edge phase)
			if (complete)
			{
				float v = bindingAnalog(b);
				as.held = true;
				if (vt == ActionValueType::Axis2) { if (b.axis == 0) as.x += v; else as.y += v; }
				else                              as.value += v;
			}

			// edge/phase flags
			switch (b.phase)
			{
				case InputPhase::Pressed:  if (justComplete) as.pressed = true; break;
				case InputPhase::Released: if (justUncomplete) as.released = true; break;
				case InputPhase::Held:     /* handled by `complete` above */ break;
				case InputPhase::Tap:
					if (justUncomplete && (rt.uncompleteTime - rt.completeTime) <= b.tapMax) as.tapped = true;
					break;
				case InputPhase::LongPress:
					if (complete && !rt.longFired && (g_now - rt.completeTime) >= b.longMin) { as.longPressed = true; rt.longFired = true; }
					break;
				case InputPhase::DoublePress:
					if (justComplete && (rt.completeTime - rt.lastCompleteTime) <= b.doubleWindow) as.doublePressed = true;
					break;
			}
			if (complete && b.consume) for (const std::string& ctl : b.controls) consumed.insert(ctl);
			rt.wasComplete = complete;
		}
	}

	// 5) fire C++ callbacks for actions that reached a subscribed phase
	for (const CB& cb : g_cbs)
	{
		auto it = g_state.find(cb.action);
		if (it == g_state.end() || !cb.fn) continue;
		const ActionState& s = it->second;
		bool hit = (cb.phase == InputPhase::Pressed && s.pressed) || (cb.phase == InputPhase::Released && s.released)
		        || (cb.phase == InputPhase::Held && s.held)       || (cb.phase == InputPhase::Tap && s.tapped)
		        || (cb.phase == InputPhase::LongPress && s.longPressed) || (cb.phase == InputPhase::DoublePress && s.doublePressed);
		if (hit) cb.fn();
	}
}

// ---- serialization (.nuinput + user overrides) -------------------------------------------------------
static json bindingToJson(const InputBinding& b)
{
	json j;
	j["action"] = b.action; j["controls"] = b.controls; j["sequence"] = b.sequence;
	j["modifiers"] = b.modifiers; j["phase"] = (int)b.phase;
	j["axis"] = b.axis; j["scale"] = b.scale; j["deadzone"] = b.deadzone; j["invert"] = b.invert; j["consume"] = b.consume;
	j["tapMax"] = b.tapMax; j["longMin"] = b.longMin; j["doubleWindow"] = b.doubleWindow; j["sequenceWindow"] = b.sequenceWindow;
	return j;
}
static InputBinding bindingFromJson(const json& j)
{
	InputBinding b;
	b.action = j.value("action", ""); b.controls = j.value("controls", std::vector<std::string>{});
	b.sequence = j.value("sequence", false); b.modifiers = j.value("modifiers", std::vector<std::string>{});
	b.phase = (InputPhase)j.value("phase", 0);
	b.axis = j.value("axis", 0); b.scale = j.value("scale", 1.0f); b.deadzone = j.value("deadzone", 0.0f);
	b.invert = j.value("invert", false); b.consume = j.value("consume", false);
	b.tapMax = j.value("tapMax", 0.20f); b.longMin = j.value("longMin", 0.50f);
	b.doubleWindow = j.value("doubleWindow", 0.30f); b.sequenceWindow = j.value("sequenceWindow", 0.60f);
	return b;
}

static bool applyInputJson(const json& j)
{
	for (const json& a : j.value("actions", json::array()))
		Input::DefineAction(a.value("name", ""), (ActionValueType)a.value("type", 0));
	for (const json& c : j.value("contexts", json::array()))
	{
		std::string name = c.value("name", "");
		Input::DefineContext(name, c.value("priority", 0));
		if (c.value("active", false)) Input::PushContext(name);
		for (const json& b : c.value("bindings", json::array())) Input::AddBinding(name, bindingFromJson(b));
	}
	return true;
}
bool Input::LoadAsset(const std::string& path)
{
	boost::filesystem::path p(path);
	boost::filesystem::ifstream in(p);
	if (!in) return false;
	json j; try { in >> j; } catch (...) { return false; }
	return applyInputJson(j);
}
bool Input::LoadAssetFromString(const std::string& text)
{
	json j; try { j = json::parse(text); } catch (...) { return false; }
	return applyInputJson(j);
}

void Input::Rebind(const std::string& context, const InputBinding& b)
{
	// user override: remove any existing user binding for the same (context, action) then add.
	ClearUserBindings(context, b.action);
	g_userBindings.push_back({ context, b });
	AddBinding(context, b);
}
void Input::ClearUserBindings(const std::string& context, const std::string& action)
{
	g_userBindings.erase(std::remove_if(g_userBindings.begin(), g_userBindings.end(),
		[&](const std::pair<std::string, InputBinding>& p) { return p.first == context && p.second.action == action; }), g_userBindings.end());
	if (EvalContext* c = findCtx(context))
		c->bindings.erase(std::remove_if(c->bindings.begin(), c->bindings.end(),
			[&](const EvalBinding& eb) { return eb.b.action == action; }), c->bindings.end());
}
// The remap editor edits the WHOLE binding list of a context (WASD = 4 bindings under one "Move"
// action — a per-action override can't express that), so a full-context override replaces the live
// context's bindings wholesale and supersedes any per-action user override for the same context.
void Input::SetUserContext(const std::string& context, const std::vector<InputBinding>& bindings)
{
	g_userBindings.erase(std::remove_if(g_userBindings.begin(), g_userBindings.end(),
		[&](const std::pair<std::string, InputBinding>& p) { return p.first == context; }), g_userBindings.end());
	g_userCtx[context] = bindings;
	EvalContext* c = findCtx(context);
	if (!c) { DefineContext(context, 0); c = findCtx(context); }
	c->bindings.clear();
	for (const InputBinding& b : bindings) c->bindings.push_back({ b, {} });
}
std::vector<std::string> Input::ListControls()
{
	std::vector<std::string> out; out.reserve(g_controls.size());
	for (auto& kv : g_controls) out.push_back(kv.first);
	std::sort(out.begin(), out.end());
	return out;
}
std::string Input::SaveUserBindings()
{
	json j;
	json arr = json::array();
	for (auto& p : g_userBindings) { json e = bindingToJson(p.second); e["context"] = p.first; arr.push_back(e); }
	j["bindings"] = arr;
	json ctx = json::object();
	for (auto& kv : g_userCtx) { json a = json::array(); for (auto& b : kv.second) a.push_back(bindingToJson(b)); ctx[kv.first] = a; }
	j["contexts"] = ctx;
	return j.dump(2);
}
void Input::LoadUserBindings(const std::string& jsonStr)
{
	json j; try { j = json::parse(jsonStr); } catch (...) { return; }
	if (j.is_array()) { for (const json& e : j) Rebind(e.value("context", ""), bindingFromJson(e)); return; }   // legacy flat array
	if (j.contains("contexts") && j["contexts"].is_object())
		for (auto& kv : j["contexts"].items())
		{
			std::vector<InputBinding> bs; for (const json& b : kv.value()) bs.push_back(bindingFromJson(b));
			SetUserContext(kv.key(), bs);
		}
	if (j.contains("bindings") && j["bindings"].is_array())
		for (const json& e : j["bindings"]) Rebind(e.value("context", ""), bindingFromJson(e));
}

// ---- reflected JSON twins (in-game rebind screens in Lua/C#) ------------------------------------------
std::string Input::MapJson()
{
	InputMapData m; m.actions = g_actions; m.contexts = ListContexts();
	return SerializeMap(m);
}
std::string Input::ControlsJson()
{
	json a = json::array();
	for (const std::string& id : ListControls()) a.push_back(id);
	return a.dump();
}
void Input::RebindJson(const std::string& context, const std::string& bindingJson)
{
	json j; try { j = json::parse(bindingJson); } catch (...) { return; }
	InputBinding b = bindingFromJson(j);
	if (!b.action.empty()) Rebind(context, b);
}

std::vector<InputAction> Input::ListActions() { return g_actions; }
std::vector<InputContext> Input::ListContexts()
{
	std::vector<InputContext> out;
	for (EvalContext& c : g_ctx)
	{
		InputContext ic; ic.name = c.name; ic.priority = c.priority; ic.active = c.active;
		for (EvalBinding& eb : c.bindings) ic.bindings.push_back(eb.b);
		out.push_back(std::move(ic));
	}
	return out;
}
void Input::Clear() { g_controls.clear(); g_actions.clear(); g_ctx.clear(); g_state.clear(); g_userBindings.clear(); g_userCtx.clear(); g_cbs.clear(); }

// ---- data-only map (the .nuinput asset editor) -------------------------------------------------------
Input::InputMapData Input::ParseMapString(const std::string& text)
{
	InputMapData m;
	json j; try { j = json::parse(text); } catch (...) { return m; }
	for (const json& a : j.value("actions", json::array()))
		m.actions.push_back({ a.value("name", std::string()), (ActionValueType)a.value("type", 0) });
	for (const json& c : j.value("contexts", json::array()))
	{
		InputContext ic; ic.name = c.value("name", std::string()); ic.priority = c.value("priority", 0); ic.active = c.value("active", false);
		for (const json& b : c.value("bindings", json::array())) ic.bindings.push_back(bindingFromJson(b));
		m.contexts.push_back(std::move(ic));
	}
	return m;
}
std::string Input::SerializeMap(const InputMapData& m)
{
	json j;
	json ja = json::array();
	for (const InputAction& a : m.actions) ja.push_back(json{ {"name", a.name}, {"type", (int)a.type} });
	j["actions"] = ja;
	json jc = json::array();
	for (const InputContext& c : m.contexts)
	{
		json e; e["name"] = c.name; e["priority"] = c.priority; e["active"] = c.active;
		json jb = json::array(); for (const InputBinding& b : c.bindings) jb.push_back(bindingToJson(b));
		e["bindings"] = jb; jc.push_back(e);
	}
	j["contexts"] = jc;
	return j.dump(2);
}
void Input::ApplyMap(const InputMapData& m)
{
	for (const InputAction& a : m.actions) DefineAction(a.name, a.type);
	for (const InputContext& c : m.contexts)
	{
		DefineContext(c.name, c.priority);
		SetUserContext(c.name, c.bindings);   // wholesale replace live bindings (never duplicates)
		SetContextActive(c.name, c.active);
	}
}

}  // namespace nuke
