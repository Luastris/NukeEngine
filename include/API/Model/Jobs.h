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

// Core job system: one worker pool for the whole process, shared by editor and game. Workers use
// SOFT affinity to an ideal core (a hard pin stalled waiters), skipping core 0 and the physics core.
// config/main.json:  "jobs": { "workers": -1, "pinCores": true }   // -1 = auto
class NUKEENGINE_API Jobs
{
public:
	// workers -1 = auto (cores minus the reserved ones, at least 1). Repeat calls are ignored.
	static void Init(int workers = -1, bool pinCores = true);
	static void Shutdown();
	// True once Shutdown() started. Long-running jobs MUST poll it and bail out — Shutdown joins
	// the workers, so an oblivious job blocks process exit.
	static bool Stopping();
	static int  WorkerCount();
	static int  Pending();   // jobs queued, not yet picked up (status-bar jobs list)
	static int  Busy();      // jobs executing right now

	// Queue a job on the pool.
	static JobHandle Schedule(const boost::function<void()>& fn);

	// Data-parallel [begin, end) in `grain`-sized chunks; the calling thread also works.
	// Returns when all indices ran.
	static void ParallelFor(int begin, int end, int grain, const boost::function<void(int)>& fn);

	// Post a callback to the MAIN/game thread (for engine state that is not thread-safe).
	static void RunOnMain(const boost::function<void()>& fn);
	static void PumpMain();   // drain the RunOnMain queue; hosts call it once per frame
};

}  // namespace nuke

#endif // !NUKEE_JOBS_H
