#include "API/Model/Jobs.h"
#include "config.h"
#include <boost/atomic.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>   // helping waits: timed cv waits
#include <deque>
#include <vector>
#include <iostream>
#ifdef _WIN32
#include <Windows.h>   // SetThreadIdealProcessor (per-core load spreading)
#endif

namespace nuke {

struct JobState
{
	boost::mutex              m;
	boost::condition_variable cv;
	bool done = false;
};

// Defined below the pool; JobHandle::Then registers through it.
void Jobs_AddContinuation(const std::shared_ptr<JobState>&, const boost::function<void()>&);

bool JobHandle::Valid() const { return (bool)state; }
bool JobHandle::Done() const
{
	if (!state) return true;
	boost::mutex::scoped_lock l(state->m);
	return state->done;
}
void JobHandle::Wait()
{
	if (!state) return;
	boost::mutex::scoped_lock l(state->m);
	while (!state->done) state->cv.wait(l);
}
JobHandle& JobHandle::Then(const boost::function<void()>& onMain)
{
	if (!state) { Jobs::RunOnMain(onMain); return *this; }   // empty handle: next pump
	Jobs_AddContinuation(state, onMain);
	return *this;
}

namespace {

struct QueuedJob
{
	std::shared_ptr<JobState> state;
	boost::function<void()>   fn;
	bool helpable = false;   // a ParallelFor chunk runner: a waiting caller may run it in place
	int  level = 0;          // ParallelFor nesting level of that runner (outermost = 1)
};
// Nesting level of the ParallelFor chunk runner executing on this thread (0 = none).
thread_local int t_pfLevel = 0;

struct Pool
{
	std::vector<boost::thread*> workers;
	std::deque<QueuedJob> queue;
	boost::mutex              qm;
	boost::condition_variable qcv;
	boost::atomic<bool> stop{ false };   // atomic: Stopping() polls it lock-free from jobs
	bool  inited = false;
	boost::atomic<int> busy{ 0 };   // jobs executing right now

	// main-thread delivery queue (RunOnMain -> PumpMain)
	boost::mutex                            mm;
	std::deque<boost::function<void()>>     mainQueue;

	// job continuations (JobHandle::Then): PumpMain polls Done and runs them on the main thread
	boost::mutex cm;
	std::vector<std::pair<std::shared_ptr<JobState>, boost::function<void()>>> conts;
};
Pool g_pool;

// Run one dequeued job to completion (worker threads and helping waiters share this).
void RunJob(QueuedJob& job)
{
	++g_pool.busy;
	try { if (job.fn) job.fn(); }
	catch (const std::exception& e) { std::cout << "[Jobs]\t\tjob threw: " << e.what() << std::endl; }
	catch (...)                     { std::cout << "[Jobs]\t\tjob threw (unknown)" << std::endl; }
	--g_pool.busy;
	if (job.state)
	{
		boost::mutex::scoped_lock l(job.state->m);
		job.state->done = true;
		job.state->cv.notify_all();
	}
}

// A waiter that would otherwise block runs one queued HELPABLE job instead. False = none queued.
// This is what keeps nested ParallelFor (chunks that fan out again) from deadlocking: every
// worker blocked in an inner wait would otherwise leave the inner chunk jobs unclaimed forever.
// Only runners at a DEEPER level than the waiter qualify — taking an outer runner from an inner
// wait would recurse without bound (outer chunk -> inner fan-out -> outer chunk -> ...).
bool HelpOne(int minLevel)
{
	QueuedJob job;
	{
		boost::mutex::scoped_lock l(g_pool.qm);
		for (auto it = g_pool.queue.begin(); it != g_pool.queue.end(); ++it)
			if (it->helpable && it->level >= minLevel) { job = *it; g_pool.queue.erase(it); break; }
		if (!job.fn) return false;
	}
	RunJob(job);
	return true;
}

void WorkerLoop(int core)
{
#ifdef _WIN32
	// SOFT affinity only: a hard SetThreadAffinityMask lets a preempted worker hold its job
	// for an OS quantum and stall every waiter (e.g. the physics barrier).
	if (core >= 0 && core < 64)
		SetThreadIdealProcessor(GetCurrentThread(), (DWORD)core);
#endif
	for (;;)
	{
		QueuedJob job;
		{
			boost::mutex::scoped_lock l(g_pool.qm);
			while (!g_pool.stop && g_pool.queue.empty()) g_pool.qcv.wait(l);
			if (g_pool.stop) return;
			job = g_pool.queue.front();
			g_pool.queue.pop_front();
		}
		RunJob(job);
	}
}

}  // namespace

// JobHandle::Then registers here (defined above the pool — hence the seam function).
void Jobs_AddContinuation(const std::shared_ptr<JobState>& s, const boost::function<void()>& fn)
{
	boost::mutex::scoped_lock l(g_pool.cm);
	g_pool.conts.emplace_back(s, fn);
}

void Jobs::Init(int workers, bool pinCores)
{
	if (g_pool.inited) return;
	g_pool.inited = true;

	const int cores = (int)boost::thread::hardware_concurrency();

	// Core budget: the engine confines itself to the first N cores (main on 0, physics on
	// the budget's last, workers on the rest); 0 = the whole machine.
	int physCore = -1, budget = 0;
	if (Config* cfg = Config::getSingleton())
	{
		physCore = cfg->effectivePhysicsCore();   // -1 auto resolves INSIDE the budget
		budget   = cfg->jobCoreBudget;
	}
	if (budget > cores) budget = cores;
	const int top = budget > 0 ? budget : cores;   // worker-eligible cores live below this
	if (physCore == -1) physCore = top - 1;        // no config at all: mirror the auto rule

	// The allowed core set for workers, round-robined when workers > set size.
	std::vector<int> allowed;
	for (int c = 1; c < top; ++c)
		if (c != physCore) allowed.push_back(c);
	if (allowed.empty()) allowed.push_back(top > 1 ? 1 : 0);

	int n = workers;
	if (n <= 0) n = (int)allowed.size();   // auto: one worker per free core (inside the budget)
	if (n < 1) n = 1;

	// A budget is only real when the workers actually stay inside it: pin whenever it is set.
	const bool pin = pinCores || budget > 0;
	for (int i = 0; i < n; ++i)
	{
		const int core = pin ? allowed[i % allowed.size()] : -1;
		g_pool.workers.push_back(new boost::thread(boost::bind(&WorkerLoop, core)));
	}
	std::cout << "[Jobs]\t\t" << n << " worker(s)";
	if (pin)
	{
		std::cout << " spread over cores [";
		for (int i = 0; i < n; ++i) std::cout << (i ? "," : "") << allowed[i % allowed.size()];
		std::cout << "]";
	}
	if (budget > 0) std::cout << ", core budget " << budget << "/" << cores;
	std::cout << " (physics core " << physCore << " reserved)" << std::endl;
}

bool Jobs::Stopping() { return g_pool.stop; }

void Jobs::Shutdown()
{
	if (!g_pool.inited) return;
	{
		boost::mutex::scoped_lock l(g_pool.qm);
		g_pool.stop = true;
		g_pool.qcv.notify_all();
	}
	for (boost::thread* t : g_pool.workers)
	{
		if (t->joinable()) t->join();
		delete t;
	}
	g_pool.workers.clear();
	g_pool.queue.clear();
	g_pool.stop = false;
	g_pool.inited = false;
}

int Jobs::WorkerCount()
{
	return (int)g_pool.workers.size();
}

int Jobs::Pending()
{
	boost::mutex::scoped_lock l(g_pool.qm);
	return (int)g_pool.queue.size();
}

int Jobs::Busy()
{
	return g_pool.busy.load(boost::memory_order_relaxed);
}

JobHandle Jobs::Schedule(const boost::function<void()>& fn)
{
	if (!g_pool.inited)
	{
		Config* cfg = Config::getSingleton();
		Init(cfg ? cfg->jobWorkers : -1, cfg ? cfg->jobPinCores : true);
	}
	JobHandle h;
	h.state = std::make_shared<JobState>();
	{
		boost::mutex::scoped_lock l(g_pool.qm);
		QueuedJob q; q.state = h.state; q.fn = fn;
		g_pool.queue.push_back(q);
		g_pool.qcv.notify_one();
	}
	return h;
}

void Jobs::ParallelFor(int begin, int end, int grain, const boost::function<void(int)>& fn)
{
	if (end <= begin) return;
	if (grain < 1) grain = 1;
	const int count = end - begin;

	// Small ranges: not worth the fan-out.
	if (!g_pool.inited || count <= grain || WorkerCount() == 0)
	{
		for (int i = begin; i < end; ++i) fn(i);
		return;
	}

	// Shared chunk cursor; workers AND the calling thread pull chunks until dry.
	struct Shared
	{
		boost::mutex m;
		int next;
	};
	auto sh = std::make_shared<Shared>();
	sh->next = begin;
	const int level = t_pfLevel + 1;
	auto runChunks = [sh, begin, end, grain, fn, level]()
	{
		const int outer = t_pfLevel;
		t_pfLevel = level;
		struct Restore { int v; ~Restore() { t_pfLevel = v; } } restore{ outer };
		for (;;)
		{
			int s;
			{
				boost::mutex::scoped_lock l(sh->m);
				s = sh->next;
				if (s >= end) return;
				sh->next = s + grain;
			}
			const int e = (s + grain < end) ? s + grain : end;
			for (int i = s; i < e; ++i) fn(i);
		}
	};

	const int chunks = (count + grain - 1) / grain;
	const int fanout = (chunks - 1 < WorkerCount()) ? chunks - 1 : WorkerCount();
	std::vector<JobHandle> handles;
	handles.reserve(fanout);
	for (int i = 0; i < fanout; ++i)
	{
		JobHandle h;
		h.state = std::make_shared<JobState>();
		boost::mutex::scoped_lock l(g_pool.qm);
		QueuedJob q; q.state = h.state; q.fn = runChunks; q.helpable = true; q.level = level;
		g_pool.queue.push_back(q);
		g_pool.qcv.notify_one();
		handles.push_back(h);
	}
	runChunks();                       // the caller crunches too
	// Wait by HELPING: while a chunk runner is still queued or running, run other queued chunk
	// runners (ours or a nested ParallelFor's) instead of sleeping — nested fan-out from inside
	// a worker would otherwise starve with every worker parked in a wait.
	for (JobHandle& h : handles)
	{
		for (;;)
		{
			{
				boost::mutex::scoped_lock l(h.state->m);
				if (h.state->done) break;
			}
			if (HelpOne(level)) continue;
			boost::mutex::scoped_lock l(h.state->m);
			if (!h.state->done) h.state->cv.timed_wait(l, boost::posix_time::milliseconds(1));
		}
	}
}

void Jobs::RunOnMain(const boost::function<void()>& fn)
{
	boost::mutex::scoped_lock l(g_pool.mm);
	g_pool.mainQueue.push_back(fn);
}

void Jobs::PumpMain()
{
	// Drain a snapshot: callbacks may RunOnMain again (next frame then).
	std::deque<boost::function<void()>> batch;
	{
		boost::mutex::scoped_lock l(g_pool.mm);
		batch.swap(g_pool.mainQueue);
	}
	// Then-continuations of completed jobs join this frame's batch (swap-remove; order
	// across DIFFERENT jobs is not promised, per-job registration order is).
	{
		boost::mutex::scoped_lock l(g_pool.cm);
		for (size_t i = 0; i < g_pool.conts.size();)
		{
			bool done;
			{ boost::mutex::scoped_lock jl(g_pool.conts[i].first->m); done = g_pool.conts[i].first->done; }
			if (!done) { ++i; continue; }
			batch.push_back(g_pool.conts[i].second);
			g_pool.conts[i] = g_pool.conts.back();
			g_pool.conts.pop_back();
		}
	}
	for (auto& fn : batch)
	{
		try { fn(); }
		catch (const std::exception& e) { std::cout << "[Jobs]\t\tmain-thread callback threw: " << e.what() << std::endl; }
	}
}

}  // namespace nuke
