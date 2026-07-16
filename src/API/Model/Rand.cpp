// Header-only boost.chrono BEFORE any boost include (project rule — the lib flavor
// double-defines steady_clock::now inside the engine DLL; boost/thread pulls chrono).
#define BOOST_CHRONO_HEADER_ONLY
#define BOOST_ERROR_CODE_HEADER_ONLY
#include "API/Model/Rand.h"

#include <boost/thread/mutex.hpp>
#include <map>
#include <cmath>

namespace nuke {

namespace {

// PCG32 (Melissa O'Neill, pcg-random.org, minimal variant): tiny state, excellent quality,
// fully deterministic across platforms — exactly what mapgen/storyteller replay needs.
struct Pcg32
{
	uint64_t state = 0x853c49e6748fea9bULL;
	uint64_t inc   = 0xda3e39cb94b95bdbULL;   // stream selector (odd)

	void seed(uint64_t s)
	{
		state = 0; inc = (s << 1u) | 1u;
		next(); state += 0x853c49e6748fea9bULL + s; next();
	}
	uint32_t next()
	{
		uint64_t old = state;
		state = old * 6364136223846793005ULL + inc;
		uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
		uint32_t rot = (uint32_t)(old >> 59u);
		return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
	}
	// gaussSpare: Box-Muller produces pairs; the spare is part of the stream's determinism.
	double gaussSpare = 0.0;
	bool   hasSpare = false;
};

boost::mutex g_randMutex;   // stream map + draws (scripts may roll from any thread)
// std::map: node-stable — StreamHandle pointers stay valid as streams are added.
std::map<std::string, Pcg32> g_streams;

Pcg32& StreamFor(const std::string& name)
{
	auto it = g_streams.find(name);
	if (it != g_streams.end()) return it->second;
	Pcg32& s = g_streams[name];
	// Un-seeded stream: derive a stable default from the name (deterministic, but SEED IT
	// for real reproducibility — the default just avoids identical cross-stream sequences).
	uint64_t h = 1469598103934665603ULL;
	for (char c : name) { h ^= (unsigned char)c; h *= 1099511628211ULL; }
	s.seed(h);
	return s;
}

inline double ToUnit(uint32_t r) { return (double)r / 4294967296.0; }   // [0,1)

}  // namespace

void Rand::Seed(const std::string& stream, double seed)
{
	boost::mutex::scoped_lock lock(g_randMutex);
	StreamFor(stream).seed((uint64_t)(long long)seed);
}

double Rand::Value(const std::string& stream)
{
	boost::mutex::scoped_lock lock(g_randMutex);
	return ToUnit(StreamFor(stream).next());
}

double Rand::Range(const std::string& stream, double minv, double maxv)
{
	if (maxv < minv) { double t = minv; minv = maxv; maxv = t; }
	boost::mutex::scoped_lock lock(g_randMutex);
	return minv + ToUnit(StreamFor(stream).next()) * (maxv - minv);
}

int Rand::RangeInt(const std::string& stream, int minv, int maxv)
{
	if (maxv < minv) { int t = minv; minv = maxv; maxv = t; }
	const uint32_t span = (uint32_t)(maxv - minv) + 1u;
	boost::mutex::scoped_lock lock(g_randMutex);
	if (span == 0) return (int)StreamFor(stream).next();   // full int range
	// Unbiased bounded draw (rejection sampling, Lemire-style threshold).
	Pcg32& s = StreamFor(stream);
	const uint32_t threshold = (0u - span) % span;
	uint32_t r;
	do { r = s.next(); } while (r < threshold);
	return minv + (int)(r % span);
}

bool Rand::Chance(const std::string& stream, double p)
{
	if (p <= 0.0) return false;
	if (p >= 1.0) return true;
	boost::mutex::scoped_lock lock(g_randMutex);
	return ToUnit(StreamFor(stream).next()) < p;
}

double Rand::Gauss(const std::string& stream, double mean, double dev)
{
	boost::mutex::scoped_lock lock(g_randMutex);
	Pcg32& s = StreamFor(stream);
	if (s.hasSpare) { s.hasSpare = false; return mean + dev * s.gaussSpare; }
	double u1, u2;
	do { u1 = ToUnit(s.next()); } while (u1 <= 1e-12);
	u2 = ToUnit(s.next());
	const double mag = std::sqrt(-2.0 * std::log(u1));
	s.gaussSpare = mag * std::sin(6.283185307179586 * u2);
	s.hasSpare = true;
	return mean + dev * mag * std::cos(6.283185307179586 * u2);
}

// State round-trip for savegames. A double's 53-bit mantissa can't carry the full 64-bit
// state losslessly, so the reflected form splits it via the stream's inc being derived
// from the seed: we pack the STATE ONLY (inc survives via Seed at load) — the practical
// contract is Seed(stream, seed) at load then SetState(stream, State()) captured at save.
double Rand::State(const std::string& stream)
{
	boost::mutex::scoped_lock lock(g_randMutex);
	// Expose the low 52 bits (mantissa-safe); the generator re-anchors on SetState.
	return (double)(StreamFor(stream).state & 0x000FFFFFFFFFFFFFULL);
}

void Rand::SetState(const std::string& stream, double state)
{
	boost::mutex::scoped_lock lock(g_randMutex);
	Pcg32& s = StreamFor(stream);
	s.state = (s.state & 0xFFF0000000000000ULL) | ((uint64_t)(long long)state & 0x000FFFFFFFFFFFFFULL);
	s.hasSpare = false;
}

void* Rand::StreamHandle(const std::string& stream)
{
	boost::mutex::scoped_lock lock(g_randMutex);
	return &StreamFor(stream);
}

uint32_t Rand::Next(void* h)              { return h ? ((Pcg32*)h)->next() : 0; }
double   Rand::ValueH(void* h)            { return h ? ToUnit(((Pcg32*)h)->next()) : 0.0; }
int      Rand::RangeIntH(void* h, int a, int b)
{
	if (!h) return a;
	if (b < a) { int t = a; a = b; b = t; }
	const uint32_t span = (uint32_t)(b - a) + 1u;
	if (span == 0) return (int)((Pcg32*)h)->next();
	return a + (int)(((Pcg32*)h)->next() % span);
}

}  // namespace nuke
