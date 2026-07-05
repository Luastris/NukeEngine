#include "API/Model/Jobs.h"
#include "config.h"
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
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

namespace {

struct Pool
{
	std::vector<boost::thread*> workers;
	std::deque<std::pair<std::shared_ptr<JobState>, boost::function<void()>>> queue;
	boost::mutex              qm;
	boost::condition_variable qcv;
	bool  stop = false;
	bool  inited = false;

	// main-thread delivery queue (RunOnMain -> PumpMain)
	boost::mutex                            mm;
	std::deque<boost::function<void()>>     mainQueue;
};
Pool g_pool;

void WorkerLoop(int core)
{
#ifdef _WIN32
	// SOFT affinity: spread the workers one-per-core but let the scheduler migrate a
	// worker whose core got taken (render/driver threads). A hard SetThreadAffinityMask
	// here stalled the whole pool: a preempted worker held its job for an OS quantum
	// while every waiter (e.g. the physics barrier) sat blocked — verified by the
	// kinematic-lift regression (fixed ticks dropped 30 -> 17 per 0.5 s, riders kicked).
	if (core >= 0 && core < 64)
		SetThreadIdealProcessor(GetCurrentThread(), (DWORD)core);
#endif
	for (;;)
	{
		std::pair<std::shared_ptr<JobState>, boost::function<void()>> job;
		{
			boost::mutex::scoped_lock l(g_pool.qm);
			while (!g_pool.stop && g_pool.queue.empty()) g_pool.qcv.wait(l);
			if (g_pool.stop) return;
			job = g_pool.queue.front();
			g_pool.queue.pop_front();
		}
		try { if (job.second) job.second(); }
		catch (const std::exception& e) { std::cout << "[Jobs]\t\tjob threw: " << e.what() << std::endl; }
		catch (...)                     { std::cout << "[Jobs]\t\tjob threw (unknown)" << std::endl; }
		if (job.first)
		{
			boost::mutex::scoped_lock l(job.first->m);
			job.first->done = true;
			job.first->cv.notify_all();
		}
	}
}

}  // namespace

void Jobs::Init(int workers, bool pinCores)
{
	if (g_pool.inited) return;
	g_pool.inited = true;

	const int cores = (int)boost::thread::hardware_concurrency();

	// Reserved cores: 0 (OS + the main/render thread) and the physics core (the fixed
	// thread is pinned there — see AppInstance::FixedThread / config "physicsCore").
	int physCore = -1;
	if (Config* cfg = Config::getSingleton())
	{
		physCore = cfg->physicsCore;
		if (physCore == -1) physCore = cores - 1;   // its auto rule: the last core
	}

	// The allowed core set for workers, round-robined when workers > set size.
	std::vector<int> allowed;
	for (int c = 1; c < cores; ++c)
		if (c != physCore) allowed.push_back(c);
	if (allowed.empty()) allowed.push_back(cores > 1 ? 1 : 0);

	int n = workers;
	if (n <= 0) n = (int)allowed.size();   // auto: one worker per free core
	if (n < 1) n = 1;

	for (int i = 0; i < n; ++i)
	{
		const int core = pinCores ? allowed[i % allowed.size()] : -1;
		g_pool.workers.push_back(new boost::thread(boost::bind(&WorkerLoop, core)));
	}
	std::cout << "[Jobs]\t\t" << n << " worker(s)";
	if (pinCores)
	{
		std::cout << " spread over cores [";
		for (int i = 0; i < n; ++i) std::cout << (i ? "," : "") << allowed[i % allowed.size()];
		std::cout << "]";
	}
	std::cout << " (physics core " << physCore << " reserved)" << std::endl;
}

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
		g_pool.queue.emplace_back(h.state, fn);
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
	auto runChunks = [sh, begin, end, grain, fn]()
	{
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
		handles.push_back(Schedule(runChunks));
	runChunks();                       // the caller crunches too
	for (JobHandle& h : handles) h.Wait();
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
	for (auto& fn : batch)
	{
		try { fn(); }
		catch (const std::exception& e) { std::cout << "[Jobs]\t\tmain-thread callback threw: " << e.what() << std::endl; }
	}
}

}  // namespace nuke
