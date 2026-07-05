#pragma once
#ifndef NUKEE_JOBS_H
#define NUKEE_JOBS_H
#include "NukeAPI.h"
#include <boost/function.hpp>
#include <memory>

namespace nuke {

struct JobState;

// Completion handle of a scheduled job (copyable; shared state).
class NUKEENGINE_API JobHandle
{
public:
	JobHandle() = default;
	bool Valid() const;   // refers to a scheduled job
	bool Done() const;    // the job finished (or the handle is empty)
	void Wait();          // block until finished; call from the MAIN thread, not a worker
private:
	friend class Jobs;
	std::shared_ptr<JobState> state;
};

// Core job system (roadmap 2.4): ONE worker pool for the whole process, available to
// the editor AND the game (both hosts Init it at boot; any engine/plugin code may
// schedule). Load spreads across the CPU: each worker gets its own IDEAL core (soft
// affinity — the OS may still migrate a preempted worker; a hard pin stalled waiters),
// skipping core 0 (the OS + main/render thread) and the physics core ("physicsCore"),
// so background work never fights the frame or the fixed step for a core.
// config/main.json:  "jobs": { "workers": -1, "pinCores": true }   // -1 = auto
class NUKEENGINE_API Jobs
{
public:
	// workers -1 = auto (cores minus the reserved ones, at least 1). Safe to call once;
	// later calls are ignored. Schedule() lazily Inits with config defaults if needed.
	static void Init(int workers = -1, bool pinCores = true);
	static void Shutdown();
	static int  WorkerCount();
	static int  Pending();   // jobs queued, not yet picked up (status-bar jobs list)
	static int  Busy();      // jobs executing right now

	// Queue a job on the pool.
	static JobHandle Schedule(const boost::function<void()>& fn);

	// Data-parallel [begin, end): indices fan out to the workers in `grain`-sized
	// chunks and the CALLING thread crunches chunks too; returns when all indices ran.
	static void ParallelFor(int begin, int end, int grain, const boost::function<void(int)>& fn);

	// Post a callback to the MAIN/game thread — how background jobs deliver results
	// into engine state that is not thread-safe (ResDB, the world, GPU uploads).
	static void RunOnMain(const boost::function<void()>& fn);
	// Drain the RunOnMain queue. The hosts call it once per frame on the game thread.
	static void PumpMain();
};

}  // namespace nuke

#endif // !NUKEE_JOBS_H
