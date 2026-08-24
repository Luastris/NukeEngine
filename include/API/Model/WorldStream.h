#pragma once
#ifndef NUKEE_WORLDSTREAM_H
#define NUKEE_WORLDSTREAM_H
// T2 World Partition: ONE big world streamed by XZ grid cells over the existing atom
// (de)serialization. Membership is DYNAMIC — a spatial root atom belongs to the cell under its
// current position — so the same runtime serves a cold player boot (cells read from files), a
// PIE session (everything starts in memory and far cells PARK), and live gameplay (spawned or
// moved atoms migrate cells naturally). Unloaded far cells render an auto-baked merged HLOD
// proxy. All hierarchy mutation happens on the game thread under the game lock (World::Update).
#include "NukeAPI.h"
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nuke {

class World;
class Atom;
class Mesh;
class Material;
class iRender;

class NUKEENGINE_API WorldStream
{
public:
	struct CellKey
	{
		int x = 0, z = 0;
		bool operator<(const CellKey& o) const { return x != o.x ? x < o.x : z < o.z; }
		bool operator==(const CellKey& o) const { return x == o.x && z == o.z; }
	};

	// A baked HLOD proxy: merged + decimated cell geometry, shading pre-baked into vertex color.
	struct Proxy
	{
		std::vector<float> verts;    // 3/vert
		std::vector<float> colors;   // 4/vert
		Mesh* mesh = nullptr;        // built lazily, freed when the cell loads
		bool draw = false;           // Tick-computed: unloaded + inside the HLOD range
	};

	struct Cell
	{
		bool fromFile = false;             // listed in the world's cell index (cold-loadable)
		bool coldLoaded = false;           // the file content was pulled in this session
		bool loading = false;              // async read+parse in flight
		std::vector<std::string> parked;   // unloaded root subtrees (full atom JSON, live state)
		Proxy hlod;
		uint64_t fileBytes = ~0ull;        // cell file size on disk (~0 = not measured yet)
	};

	// ST-viz: one cell's live state, snapshotted for the editor overlay (DebugCells).
	struct CellInfo
	{
		CellKey key;
		bool loaded = false;               // inside the sticky active set
		bool fromFile = false;             // has a cold file in the cell index
		bool coldLoaded = false;           // that file was pulled this session
		bool loading = false;              // async read in flight
		bool hlodDraw = false;             // HLOD proxy drawn this frame
		int  parked = 0;                   // parked root subtrees
		uint64_t parkedBytes = 0;          // memory the parked JSON holds
		uint64_t fileBytes = 0;            // cell file size on disk (0 = none/unknown)
	};

	~WorldStream();

	// Wire-up from the loaded world document: the cell index + where the cell files live
	// (content-relative dir); empty = no files (PIE snapshot — memory only).
	void Setup(const std::vector<CellKey>& fileCells, const std::string& cellsDir);
	void Reset();                          // new world incoming: drop everything (parked included)

	// One streaming step: active-set from the camera anchors (hysteresis), budgeted cold loads /
	// restores / parks. Runs at the end of World::Update, game lock held.
	void Tick(World* w);
	// HLOD proxies for unloaded cells (per camera pass, opaque section).
	void Render(World* w, iRender* r);

	// Resident-but-unloaded atoms (parked + cold files): SaveToString appends them so PIE
	// snapshots and savegames stay COMPLETE worlds.
	void AppendResident(std::vector<std::string>& outAtomJsons);

	// True when streaming should actually run for this world (enabled + not editor edit mode).
	static bool Active(World* w);

	// Membership: cell under a world position; is this root a streamed (spatial) atom at all.
	static CellKey CellOf(double x, double z, float cellSize);
	static bool    Spatial(Atom* root);

	// HLOD data (built at save by World, loaded lazily at runtime).
	bool LoadHlod(const std::string& contentRelPath);

	int CellCount() const { return (int)cells.size(); }
	int LoadedCount() const;
	// ST-viz snapshot for the editor overlay (game thread; file sizes measured once and cached).
	void DebugCells(std::vector<CellInfo>& out);

private:
	std::map<CellKey, Cell> cells;
	std::string cellsDir;                  // content-relative ("" = memory-only session)
	std::set<CellKey> loadedSet;           // cells considered LOADED (sticky/hysteresis)
	bool hlodTried = false;                // hlod.bin probed once per session
	Material* hlodMat = nullptr;           // shared vcolor material (shader "hlodproxy")

	// Async cold loads land here (game thread applies budgeted).
	struct ColdIn { CellKey key; std::shared_ptr<std::string> data; };
	std::vector<ColdIn> coldReady;
	uint32_t coldGen = 0;                  // Reset() invalidates in-flight reads

	void EnsureHlodMesh(Cell& c);
	void FreeHlodMesh(Cell& c);
	void EnsureHlodMat();
};

}  // namespace nuke

#endif  // NUKEE_WORLDSTREAM_H
