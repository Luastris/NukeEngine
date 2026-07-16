#include "API/Model/Noise.h"

#include <cmath>
#include <cstdint>

namespace nuke {

namespace {

// Seed-mixed integer hash (splitmix64 finalizer) — the permutation source. Stateless:
// every call derives gradients from (seed, lattice coords) directly, so there is no
// permutation table to build or store per seed.
inline uint64_t HashU64(uint64_t x)
{
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}
inline uint64_t HashCoords(uint64_t seed, int64_t x, int64_t y, int64_t z = 0)
{
	uint64_t h = HashU64(seed ^ 0x8f3af1cb92e470a1ULL);
	h = HashU64(h ^ (uint64_t)x);
	h = HashU64(h ^ (uint64_t)y);
	h = HashU64(h ^ (uint64_t)z);
	return h;
}

inline double Fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }
inline double LerpD(double a, double b, double t) { return a + t * (b - a); }

// 2D gradient from the hash: one of 8 unit-ish directions (Perlin's improved set).
inline double Grad2(uint64_t h, double dx, double dy)
{
	switch (h & 7u)
	{
	case 0: return  dx + dy;
	case 1: return  dx - dy;
	case 2: return -dx + dy;
	case 3: return -dx - dy;
	case 4: return  dx;
	case 5: return -dx;
	case 6: return  dy;
	default: return -dy;
	}
}
// 3D gradient (12 edge directions of a cube, Perlin's improved set).
inline double Grad3(uint64_t h, double x, double y, double z)
{
	switch (h % 12u)
	{
	case 0:  return  x + y; case 1:  return -x + y; case 2:  return  x - y; case 3:  return -x - y;
	case 4:  return  x + z; case 5:  return -x + z; case 6:  return  x - z; case 7:  return -x - z;
	case 8:  return  y + z; case 9:  return -y + z; case 10: return  y - z; default: return -y - z;
	}
}

inline int64_t FloorI(double v) { return (int64_t)std::floor(v); }

double Perlin2Impl(uint64_t seed, double x, double y)
{
	const int64_t x0 = FloorI(x), y0 = FloorI(y);
	const double dx = x - x0, dy = y - y0;
	const double u = Fade(dx), v = Fade(dy);
	const double n00 = Grad2(HashCoords(seed, x0,     y0),     dx,       dy);
	const double n10 = Grad2(HashCoords(seed, x0 + 1, y0),     dx - 1.0, dy);
	const double n01 = Grad2(HashCoords(seed, x0,     y0 + 1), dx,       dy - 1.0);
	const double n11 = Grad2(HashCoords(seed, x0 + 1, y0 + 1), dx - 1.0, dy - 1.0);
	// ×√2: gradient-pair dot products land in [-1/√2, 1/√2] — normalize to [-1, 1].
	return LerpD(LerpD(n00, n10, u), LerpD(n01, n11, u), v) * 1.4142135623730951;
}

}  // namespace

double Noise::Perlin2(double seed, double x, double y)
{
	return Perlin2Impl((uint64_t)(long long)seed, x, y);
}

double Noise::Perlin3(double seed, double x, double y, double z)
{
	const uint64_t s = (uint64_t)(long long)seed;
	const int64_t x0 = FloorI(x), y0 = FloorI(y), z0 = FloorI(z);
	const double dx = x - x0, dy = y - y0, dz = z - z0;
	const double u = Fade(dx), v = Fade(dy), w = Fade(dz);
	auto g = [&](int ix, int iy, int iz, double ox, double oy, double oz)
	{ return Grad3(HashCoords(s, x0 + ix, y0 + iy, z0 + iz), dx - ox, dy - oy, dz - oz); };
	const double n000 = g(0,0,0, 0,0,0), n100 = g(1,0,0, 1,0,0);
	const double n010 = g(0,1,0, 0,1,0), n110 = g(1,1,0, 1,1,0);
	const double n001 = g(0,0,1, 0,0,1), n101 = g(1,0,1, 1,0,1);
	const double n011 = g(0,1,1, 0,1,1), n111 = g(1,1,1, 1,1,1);
	const double nx00 = LerpD(n000, n100, u), nx10 = LerpD(n010, n110, u);
	const double nx01 = LerpD(n001, n101, u), nx11 = LerpD(n011, n111, u);
	return LerpD(LerpD(nx00, nx10, v), LerpD(nx01, nx11, v), w) * 1.1547005383792515;   // ×2/√3
}

double Noise::Fbm(double seed, double x, double y, int octaves, double lacunarity, double gain)
{
	if (octaves < 1) octaves = 1;
	if (octaves > 12) octaves = 12;
	const uint64_t s = (uint64_t)(long long)seed;
	double sum = 0.0, amp = 1.0, norm = 0.0, freq = 1.0;
	for (int i = 0; i < octaves; ++i)
	{
		// Each octave gets its own derived seed — layers decorrelate instead of echoing.
		sum  += amp * Perlin2Impl(HashU64(s + (uint64_t)i * 0x9e3779b9ULL), x * freq, y * freq);
		norm += amp;
		amp  *= gain;
		freq *= lacunarity;
	}
	return norm > 0.0 ? sum / norm : 0.0;
}

double Noise::Voronoi2(double seed, double x, double y)
{
	const uint64_t s = (uint64_t)(long long)seed;
	const int64_t cx = FloorI(x), cy = FloorI(y);
	double best = 1e9;
	for (int64_t oy = -1; oy <= 1; ++oy)
		for (int64_t ox = -1; ox <= 1; ++ox)
		{
			const uint64_t h = HashCoords(s, cx + ox, cy + oy);
			// Jittered feature point inside the neighbor cell.
			const double fx = (double)(cx + ox) + (double)((h >> 16) & 0xFFFF) / 65536.0;
			const double fy = (double)(cy + oy) + (double)((h >> 32) & 0xFFFF) / 65536.0;
			const double dx = fx - x, dy = fy - y;
			const double d = std::sqrt(dx * dx + dy * dy);
			if (d < best) best = d;
		}
	return best;
}

double Noise::CellId2(double seed, double x, double y)
{
	const uint64_t s = (uint64_t)(long long)seed;
	const int64_t cx = FloorI(x), cy = FloorI(y);
	double best = 1e9; uint64_t bestId = 0;
	for (int64_t oy = -1; oy <= 1; ++oy)
		for (int64_t ox = -1; ox <= 1; ++ox)
		{
			const uint64_t h = HashCoords(s, cx + ox, cy + oy);
			const double fx = (double)(cx + ox) + (double)((h >> 16) & 0xFFFF) / 65536.0;
			const double fy = (double)(cy + oy) + (double)((h >> 32) & 0xFFFF) / 65536.0;
			const double dx = fx - x, dy = fy - y;
			const double d = dx * dx + dy * dy;
			if (d < best) { best = d; bestId = h; }
		}
	// Mantissa-safe stable id (52 bits) of the owning cell.
	return (double)(bestId & 0x000FFFFFFFFFFFFFULL);
}

double Noise::WarpX(double seed, double x, double y, double amp)
{
	return x + amp * Perlin2Impl(HashU64((uint64_t)(long long)seed ^ 0xa5a5a5a5ULL), x, y);
}

double Noise::WarpY(double seed, double x, double y, double amp)
{
	return y + amp * Perlin2Impl(HashU64((uint64_t)(long long)seed ^ 0x5a5a5a5aULL), x, y);
}

}  // namespace nuke
