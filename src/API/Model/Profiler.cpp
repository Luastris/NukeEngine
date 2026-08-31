// Header-only boost.chrono BEFORE any boost include (project rule — the lib flavor
// double-defines steady_clock::now inside the engine DLL).
#define BOOST_CHRONO_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#include "API/Model/Profiler.h"
#include "API/Model/Screen.h"
#include "API/Model/Time.h"
#include "API/Model/DevConsole.h"
#include "interface/iGUI.h"

#include <boost/chrono.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <map>
#include <vector>

namespace nuke {

namespace {
boost::mutex g_profMutex;   // reports come from the game, fixed and worker threads
// std::map keeps a stable alphabetical order for Phases().
std::map<std::string, double> g_phases;   // phase -> EMA milliseconds

double NowMs()
{
	using clock = boost::chrono::steady_clock;
	return boost::chrono::duration<double, boost::milli>(clock::now().time_since_epoch()).count();
}
}  // namespace

void Profiler::Report(const std::string& phase, double ms)
{
	if (phase.empty()) return;
	boost::mutex::scoped_lock lock(g_profMutex);
	double& v = g_phases[phase];
	v = (v == 0.0) ? ms : v * 0.9 + ms * 0.1;   // EMA: readable, not twitchy
}

double Profiler::Ms(const std::string& phase)
{
	boost::mutex::scoped_lock lock(g_profMutex);
	auto it = g_phases.find(phase);
	return it != g_phases.end() ? it->second : 0.0;
}

std::string Profiler::Phases()
{
	boost::mutex::scoped_lock lock(g_profMutex);
	std::string out;
	for (auto& kv : g_phases) { if (!out.empty()) out += "\n"; out += kv.first; }
	return out;
}

bool Profiler::Capture(const std::string& file)
{
	if (file.empty()) return false;
	std::vector<std::pair<std::string, double>> rows;
	{
		boost::mutex::scoped_lock lock(g_profMutex);
		rows.assign(g_phases.begin(), g_phases.end());
	}
	std::sort(rows.begin(), rows.end(),
	          [](const auto& a, const auto& b) { return a.second > b.second; });
	boost::filesystem::ofstream out{ boost::filesystem::path(file), std::ios::trunc };
	if (!out) return false;
	out << "phase;ms\n";
	for (const auto& r : rows) out << r.first << ";" << r.second << "\n";
	std::cout << "[Profiler]\tcaptured " << rows.size() << " phase(s) -> "
	          << boost::filesystem::absolute(file).string() << std::endl;
	return true;
}

Profiler::Scope::Scope(const char* phase) : name(phase), t0(NowMs()) {}
Profiler::Scope::~Scope() { Report(name ? name : "", NowMs() - t0); }

// ---- perf overlays (FPS counter + frame-time graph, drawn through the iGUI seam) ----------------

namespace {
bool g_showFps   = false;
bool g_showGraph = false;

// Frame-time history ring: five curves over the last kHist frames (GUI-frame cadence).
const int kHist = 240;
struct OverlayCurve { const char* name; float col[3]; float v[kHist]; };
OverlayCurve g_curves[5] = {
	{ "frame",  { 0.92f, 0.92f, 0.92f }, {} },
	{ "update", { 0.31f, 0.78f, 1.00f }, {} },
	{ "fixed",  { 1.00f, 0.78f, 0.27f }, {} },
	{ "render", { 0.67f, 0.47f, 1.00f }, {} },
	{ "gpu",    { 0.43f, 0.90f, 0.51f }, {} },
};
int g_histHead = 0, g_histCount = 0;

// FPS readout refreshed twice a second (a per-frame number is unreadable).
double g_fpsAccum = 0.0; int g_fpsFrames = 0;
char   g_fpsText[48] = "-- FPS";
}  // namespace

void Profiler::ShowFps(bool on)   { g_showFps = on; }
void Profiler::ShowGraph(bool on) { g_showGraph = on; if (!on) { g_histHead = g_histCount = 0; } }
bool Profiler::FpsShown()   { return g_showFps; }
bool Profiler::GraphShown() { return g_showGraph; }

void Profiler::EmitOverlay()
{
	if (!g_showFps && !g_showGraph) return;
	if (Console::IsOpen()) return;   // the console owns the top of the screen — never draw under it
	iGUI* ui = GUI();
	const float sw = (float)Screen::Width(), sh = (float)Screen::Height();
	if (sw <= 1.0f || sh <= 1.0f) return;
	const double dt = Time::getSingleton()->delta;
	const float pad = 8.0f;
	float top = pad;

	if (g_showFps)
	{
		g_fpsAccum += dt; ++g_fpsFrames;
		if (g_fpsAccum >= 0.5)
		{
			const double avg = g_fpsAccum / (double)g_fpsFrames;
			snprintf(g_fpsText, sizeof(g_fpsText), "%.0f FPS (%.1f ms)",
			         avg > 0.0 ? 1.0 / avg : 0.0, avg * 1000.0);
			g_fpsAccum = 0.0; g_fpsFrames = 0;
		}
		float tw = 0, th = 0;
		ui->OverlayTextSize(g_fpsText, &tw, &th);
		if (th <= 0.0f) th = 16.0f;
		ui->OverlayRect(sw - pad - tw - 8, top, tw + 8, th + 4, 0, 0, 0, 0.6f, 3.0f);
		ui->OverlayText(sw - pad - tw - 4, top + 2, 1, 1, 1, 1, g_fpsText);
		top += th + 4 + 6;
	}

	if (g_showGraph)
	{
		float gpu = 0.0f;
		{
			// Sum the renderer's duration queries ("gpu.*").
			const std::string phases = Phases();
			size_t st = 0;
			while (st < phases.size())
			{
				size_t nl = phases.find('\n', st);
				if (nl == std::string::npos) nl = phases.size();
				const std::string ph = phases.substr(st, nl - st);
				st = nl + 1;
				if (ph.rfind("gpu.", 0) == 0) gpu += (float)Ms(ph);
			}
		}
		g_curves[0].v[g_histHead] = (float)(dt * 1000.0);
		g_curves[1].v[g_histHead] = (float)Ms("update");
		g_curves[2].v[g_histHead] = (float)Ms("fixed");
		g_curves[3].v[g_histHead] = (float)Ms("render");
		g_curves[4].v[g_histHead] = gpu;
		g_histHead = (g_histHead + 1) % kHist;
		if (g_histCount < kHist) ++g_histCount;
		if (g_histCount < 2) return;

		const float w = std::min(360.0f, sw - 2 * pad), h = 92.0f;
		const float x = sw - pad - w, y = top;
		ui->OverlayRect(x, y, w, h, 0, 0, 0, 0.6f, 4.0f);
		// Scale from the visible peak, never below a 60 Hz frame so idle stays readable.
		float peak = 16.7f;
		for (const OverlayCurve& c : g_curves)
			for (int i = 0; i < g_histCount; ++i) peak = std::max(peak, c.v[i]);
		for (float guide : { 16.6f, 33.3f })
			if (guide < peak)
			{
				const float gy = y + h - h * (guide / peak);
				ui->OverlayLine(x, gy, x + w, gy, 1, 1, 1, 0.16f, 1.0f);
			}
		for (const OverlayCurve& c : g_curves)
		{
			float px = 0, py = 0;
			for (int i = 0; i < g_histCount; ++i)
			{
				const int idx = (g_histHead - g_histCount + i + 2 * kHist) % kHist;
				const float cx = x + w * (float)i / (float)(kHist - 1);
				const float cy = y + h - h * std::min(1.0f, c.v[idx] / peak);
				if (i > 0) ui->OverlayLine(px, py, cx, cy, c.col[0], c.col[1], c.col[2], 1.0f, 1.0f);
				px = cx; py = cy;
			}
		}
		// Legend: name + the freshest value.
		float lx = x + 6;
		const float ly = y + 4;
		for (const OverlayCurve& c : g_curves)
		{
			const int last = (g_histHead - 1 + kHist) % kHist;
			char buf[48];
			snprintf(buf, sizeof(buf), "%s %.1f", c.name, c.v[last]);
			ui->OverlayText(lx, ly, c.col[0], c.col[1], c.col[2], 1.0f, buf);
			float tw = 0, th = 0;
			ui->OverlayTextSize(buf, &tw, &th);
			lx += (tw > 0.0f ? tw : 48.0f) + 12.0f;
		}
	}
}

}  // namespace nuke
