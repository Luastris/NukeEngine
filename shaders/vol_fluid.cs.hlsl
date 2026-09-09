// Fluid fog: a small incompressible 3D fluid inside a volume's box, in the box's local frame.
// The density is the medium (shape x noise, normalised 0..1) and MOVES: advected by its own
// velocity, which the wind drives, the Force Fields push, pull and swirl (attract / repel /
// vortex / turbulence, m/s^2), curl noise stirs, and the bodies walking through shape as SOLID
// cells - the fluid parts around them and swirls in their wake, as around a real body. The box is OPEN: the wind flows in through one side and out the other (pressure 0
// outside), so the fog rolls through instead of stalling in a closed tank. Refill only tops up
// what blew away or was pushed aside, never erases a wisp that drifted. One entry, five passes
// selected by g_FlRes.w: 0 velocity advection + forces, 1 divergence (+ clear pressure),
// 2 Jacobi pressure iteration, 3 projection, 4 density advection + refill.
cbuffer FluidCB
{
    float4 g_FlRes;        // cells x, y, z; w = pass
    float4 g_FlBox;        // half extents (local); w = dt (0 on the first step)
    float4 g_FlUp;         // world up in the box frame (the vortex axis)
    float4 g_FlPos;        // x = parcel radius (m); w = shape
    float4 g_FlWind;       // wind in the local frame, m/s; w = refill rate of a hole (1/s)
    float4 g_FlParams;     // turbulence (m/s), dissipation (1/s), noise amount, noise scale
    float4 g_FlParams2;    // falloff, time, wind advection of the noise, history valid
    float4 g_FlCounts;     // bodies, force fields, height falloff (1/m above the box bottom), ledger slot (0/1)
    float4 g_FlDispPos[8]; // local body positions, w = radius
    float4 g_FlDispVel[8]; // local body velocities, w = strength
    float4 g_FlForcePos[8];  // Force Fields / wind zones: local position, w = radius
    float4 g_FlForceDir[8];  // local direction, w = strength (m/s^2)
    float4 g_FlForceMisc[8];
    float4 g_FlForceDent[8];   // vortex funnel: dent depth, sharpness, size (m, 0 = noise scale), extra density in the folds
    float4 g_FlForceDent2[8];  // x = inner radius (0 = 15% of the radius)
    float4 g_FlMisc;       // body count, cell size x, y, z
    float4 g_FlSplat;      // clumps: splat grid res x, y, z, parcel count
};
Texture3D<float4>   g_VelIn;   SamplerState g_VelIn_sampler;
Texture3D<float>    g_PrsIn;
Texture3D<float>    g_DivIn;
RWTexture3D<float4> g_VelOut;
RWTexture3D<float>  g_PrsOut;
RWTexture3D<float>  g_DivOut;
RWTexture3D<float>  g_RhoOut;   // the fog itself: what the froxels sample
RWStructuredBuffer<uint> g_Ledger;     // three slots of (resting sum x16, parcel weight sum x256): this frame's fills, last frame's is read, next frame's is cleared
// The fog as MATERIAL: parcels (clumps), each a bit of the medium with a place, a size and a
// weight. They ride the same motion as the particles - the air (wind, turbulence, the bodies'
// wakes) plus the Force Fields' drift - so a vortex turns them into arms that keep turning: a
// density on the grid in a steady flow is a still picture, a parcel is not. They die in a
// draining vortex's core (or a pushing one's rim, or leaving the box) and are born again where
// the medium enters, fading in and out over 1/Refill seconds; their number never changes, and
// the splat is normalised to the resting amount, so the total fog is what it was.
// Parcel i: [2i] = (pos.xyz local, unused), [2i+1] = (weight 0..1, seed, dying, unused).
RWStructuredBuffer<float4> g_Parcels;
RWTexture3D<uint>          g_Acc;      // the parcels' splat this step (fixed point x65536)
// Grid mode: the density itself on the air grid (MacCormack), see passes 8 / 9
Texture3D<float>    g_DensIn;  SamplerState g_DensIn_sampler;
Texture3D<float>    g_Scratch;
RWTexture3D<float>  g_DensOut;

// integer lattice hash (pcg3d): no grid, no rings, the same anywhere in space
float FHash(float3 p)
{
    uint3 v = (uint3)((int3)floor(p) + 0x7fffff);
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return (float)(v.x & 0x00ffffffu) / 16777216.0;
}
float FNoise(float3 x)
{
    float3 i = floor(x), f = frac(x); f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(lerp(FHash(i), FHash(i + float3(1, 0, 0)), f.x), lerp(FHash(i + float3(0, 1, 0)), FHash(i + float3(1, 1, 0)), f.x), f.y),
                lerp(lerp(FHash(i + float3(0, 0, 1)), FHash(i + float3(1, 0, 1)), f.x), lerp(FHash(i + float3(0, 1, 1)), FHash(i + float3(1, 1, 1)), f.x), f.y), f.z);
}
float FFbm(float3 p) { return 0.6 * FNoise(p) + 0.3 * FNoise(p * 2.13 + 5.1) + 0.1 * FNoise(p * 4.31 + 9.7); }
// Curl of a noise potential: a divergence-free stirring field.
float3 CurlNoise(float3 p)
{
    const float e = 0.15;
    float3 dx = float3(e, 0, 0), dy = float3(0, e, 0), dz = float3(0, 0, e);
    float3 a = float3(FNoise(p + 3.1), FNoise(p + 17.7), FNoise(p + 31.3));
    float3 px = float3(FNoise(p + dx + 3.1), FNoise(p + dx + 17.7), FNoise(p + dx + 31.3));
    float3 py = float3(FNoise(p + dy + 3.1), FNoise(p + dy + 17.7), FNoise(p + dy + 31.3));
    float3 pz = float3(FNoise(p + dz + 3.1), FNoise(p + dz + 17.7), FNoise(p + dz + 31.3));
    float3 dX = (px - a) / e, dY = (py - a) / e, dZ = (pz - a) / e;   // d(potential)/dx, dy, dz
    return float3(dY.z - dZ.y, dZ.x - dX.z, dX.y - dY.x);
}

float3 CellPos(int3 c) { return -g_FlBox.xyz + (float3(c) + 0.5) * g_FlMisc.yzw; }
// the clumps' splat grid (its own resolution: a clump spans a few of its cells whatever the air grid is)
float3 SplatCell()          { return 2.0 * g_FlBox.xyz / g_FlSplat.xyz; }
bool   InsideS(int3 c)      { return all(c >= 0) && all(c < (int3)g_FlSplat.xyz); }
float3 CellPosS(int3 c)     { return -g_FlBox.xyz + (float3(c) + 0.5) * SplatCell(); }
float3 ToUVW(float3 p) { return (p + g_FlBox.xyz) / (2.0 * g_FlBox.xyz); }
bool   Inside(int3 c) { return all(c >= 0) && all(c < (int3)g_FlRes.xyz); }
int3   Clamp3(int3 c) { return clamp(c, int3(0, 0, 0), (int3)g_FlRes.xyz - 1); }

// A displacer occupying this point: solid, moving with the body (strength scales how much of
// the body's motion the fluid takes; 1 = a rigid body).
bool Solid(float3 p, out float3 vel)
{
    vel = 0.0;
    const int n = (int)g_FlCounts.x;
    [loop] for (int i = 0; i < n; ++i)
    {
        float3 d = p - g_FlDispPos[i].xyz;
        if (dot(d, d) < g_FlDispPos[i].w * g_FlDispPos[i].w) { vel = g_FlDispVel[i].xyz * g_FlDispVel[i].w; return true; }
    }
    return false;
}
// Velocity at a cell for the differences: solid cells carry the body's velocity; outside the
// box the wind flows (open boundary).
float3 VelAt(int3 c)
{
    if (!Inside(c)) return g_FlWind.xyz;
    float3 sv; if (Solid(CellPos(c), sv)) return sv;
    return g_VelIn[c].xyz;
}
// Pressure at a neighbour: 0 outside (open), the cell's own value across a solid face (no flow into the body).
float PrsAt(int3 c, float own)
{
    if (!Inside(c)) return 0.0;
    float3 sv; if (Solid(CellPos(c), sv)) return own;
    return g_PrsIn[c];
}

// The Force Fields' acceleration at a point (m/s^2), faded by their falloff.
float3 FieldAccel(float3 p, bool withVortex)
{
    float3 a = 0.0;
    const int nf = (int)g_FlCounts.y;
    [loop] for (int fi = 0; fi < nf; ++fi)
    {
        float3 d = p - g_FlForcePos[fi].xyz; float R = g_FlForcePos[fi].w;
        float  l = length(d);
        if (l >= R) continue;   // a field acts inside its radius only
        float  w = 1.0 - g_FlForceMisc[fi].y * (l / R);
        // the grid cannot hold a step: the field's edge fades over its last two cells (or 5% of the radius),
        // or a hard edge (Falloff 0) piles the fog into a one-cell ring with a scalloped rim
        float  edge = max(0.05 * R, 2.0 * max(g_FlMisc.y, max(g_FlMisc.z, g_FlMisc.w)));
        w *= smoothstep(0.0, 1.0, (R - l) / edge);
        float3 n = (l > 1e-4) ? d / l : g_FlUp.xyz;
        float  s = g_FlForceDir[fi].w * w;
        int    mode = (int)g_FlForceMisc[fi].x;
        if      (mode == 0) a += normalize(g_FlForceDir[fi].xyz) * s;          // directional
        else if (mode == 1) a += n * s;                                         // radial: + repel, - attract
        else if (mode == 2)                                                     // vortex: turns around the axis, pulled in or pushed out
        {   // (the parcels take it exactly in VortexStep - an Euler step on a circle drifts outward - and skip it here)
            if (!withVortex) continue;
            // the grid turns as a body: one angular speed over the disc (the strength is the rim
            // speed), the falloff is the rim's width - the fog keeps turning instead of winding itself
            // into still rings within seconds
            float3 u  = normalize(g_FlForceDir[fi].xyz);
            float3 dr = d - u * dot(d, u);
            float  rh = length(dr);
            float  rin = R * (1.0 - 0.5 * g_FlForceMisc[fi].y);
            float  rim = 1.0 - saturate((rh - rin) / max(R - rin, 1e-3));
            if (rh > 1e-4) a += (cross(u, dr) / rh) * g_FlForceDir[fi].w * (rh / R) * rim;
        }
        else                a += CurlNoise(p * 0.7 + g_FlForceMisc[fi].z * 13.0 + g_FlParams2.y * 0.5) * s * 2.0;   // turbulence
    }
    return a;
}
// The fog's motion in a field: the field's acceleration x a short response time, on top of
// riding the air - a repel field blows the fog out of its centre (a hole with a rim), an
// attract field gathers it, a vortex only spins it (its clumps go round, nothing is flung out).
float3 FieldDrift(float3 p, bool withVortex)
{
    const float tau = 0.35;
    float3 v = FieldAccel(p, withVortex) * tau;
    float  m = length(v);
    return (m > 15.0) ? v * (15.0 / m) : v;
}

// The resting shape at a cell (mask x height profile, no clumps): where spawned mass lands.
float RestShape(float3 p)
{
    float3 q = p / g_FlBox.xyz;
    float  m = (g_FlPos.w < 0.5) ? max(abs(q.x), max(abs(q.y), abs(q.z))) : length(q);
    if (m >= 1.0) return 0.0;
    return (1.0 - smoothstep(1.0 - g_FlParams2.x, 1.0, m)) * exp(-g_FlCounts.z * max(p.y + g_FlBox.y, 0.0));
}
// The funnel: a vortex field does not move the grid's fog, it SHAPES it. The clump pattern at a
// point is read from where the field's own motion would have brought it from - turned back by the
// local angular speed (the field's profile: (R/r)^(1+pull) outside the inner radius, solid-body
// inside it), moved along the radial flow (pull > 0 out of the centre, < 0 into it; a
// multiplicative shift, so nothing ever collapses to a point) - so the speed differences dent
// the pattern into the funnel's spiral dents, and the funnel is seen turning at each radius at
// its own speed. A feature lives one revolution at half the radius: its age runs 0..T, and two
// such patterns half a period apart are crossfaded by a triangle weight (a pattern weighs nothing
// when it renews) - the dents wind by one turn at most and nothing ever pops. Where a period
// winds one dent past a full turn against its neighbour, the pattern smooths out (the core).
struct Funnel { float3 xA, xB; float mixA, weight, contrast, size, depth, sharp, density; };
Funnel FunnelOrigin(float3 p)
{
    Funnel f; f.xA = p; f.xB = p; f.mixA = 1.0; f.weight = 0.0; f.contrast = 1.0;
    f.size = g_FlParams.w; f.depth = 0.0; f.sharp = 0.5; f.density = 0.0;
    const int nf = (int)g_FlCounts.y;
    [loop] for (int fi = 0; fi < nf; ++fi)
    {
        if ((int)g_FlForceMisc[fi].x != 2) continue;
        float3 d = p - g_FlForcePos[fi].xyz; float R = g_FlForcePos[fi].w;
        float  l = length(d);
        if (l >= R) continue;
        // the funnel owns the inside and lets go over the outer 30% of the radius; the falloff fades it further inward on top
        float  w  = (1.0 - smoothstep(0.7, 1.0, l / R)) * smoothstep(0.0, 0.6, 1.0 - g_FlForceMisc[fi].y * (l / R));
        if (w <= 0.0) continue;
        float  s  = g_FlForceDir[fi].w * 0.35;            // the rim speed, m/s (signed)
        float  pull = g_FlForceMisc[fi].w;
        float  inner = (g_FlForceDent2[fi].x > 0.0) ? min(g_FlForceDent2[fi].x, 0.9 * R) : 0.15 * R;
        float3 u  = normalize(g_FlForceDir[fi].xyz);
        float3 dr = d - u * dot(d, u);
        float  rh = max(length(dr), 1e-3);
        // the field's own speed, as the particles take it: strength x falloff along the radius (omega ~ 1/r);
        // solid body inside the inner radius. (A steeper profile wound the pattern into rings by the core.)
        float  rc = max(rh, inner);
        float  wf = 1.0 - g_FlForceMisc[fi].y * (l / R);
        float  om = s * wf / rc;                           // the angular speed here (signed)
        float  rm = max(0.5 * R, inner);
        float  T  = 6.2832 / max(abs(s * (1.0 - 0.5 * g_FlForceMisc[fi].y) / rm), 1e-3);   // one revolution at half the radius
        float  ph = frac(g_FlParams2.y / T), phB = frac(ph + 0.5);
        float  aA = ph * T, aB = phB * T;
        float3 e  = dr / rh;
        float3 c0 = g_FlForcePos[fi].xyz + u * dot(d, u);
        // the radial flow. Into the centre (pull > 0) at the full pull speed at every radius: the pattern
        // came from pull*s*age further out (an r-proportional pull hardly moved the middle and the turning
        // spiral read as expanding - the barber-pole illusion). Out of it (pull < 0) multiplicatively: the
        // pattern came from further in, never collapsing to a point.
        // Into the centre the pattern may come from beyond the rim (the undisturbed, two-dimensional fog
        // out there), but it only TURNED while inside the field: the turn counts for the time from the
        // rim to here at the pull speed, no longer. (Turning the far origin by the full age spread
        // neighbouring radii over huge arcs and printed concentric rings; capping the origin at the rim
        // made a one-dimensional ribbon of it - arcs again.)
        float  rA = (pull >= 0.0) ? min(rh + pull * s * aA, rh + 4.0 * R) : rh * exp(max(pull * s * aA / R, -1.0));
        float  rB = (pull >= 0.0) ? min(rh + pull * s * aB, rh + 4.0 * R) : rh * exp(max(pull * s * aB / R, -1.0));
        // The turn a parcel took on its way here: the angular speed integrated along its radial path.
        // Pulled in at v_r = pull*s with omega = s*wf/r: theta = (wf/pull) ln(r0/r) - it keeps growing with
        // age (the funnel is seen turning) yet stays bounded (no far origin flung round huge arcs =
        // rings); pushed out at v_r = pull*s*r/R: theta = (R wf/|pull|)(1/r0 - 1/r). At pull -> 0 both
        // become omega*age. (Cutting the turn at the rim froze the spiral and left a concentric
        // pattern there that the grid printed as arcs.)
        float  rcA = max(rA, inner), rcB = max(rB, inner);
        float  thA, thB;
        // pushed out: the turn at the local speed for the whole age (the 1/r closed form met the solid
        // core, where the parcel came from, and stopped growing with age - the spiral froze at Pull -1)
        if (pull > 0.02) { thA = -(wf / pull) * log(rcA / rc); thB = -(wf / pull) * log(rcB / rc); }
        else             { thA = -om * aA;                     thB = -om * aB; }
        float3 bA = c0 + (e * cos(thA) + cross(u, e) * sin(thA)) * rA;
        float3 bB = c0 + (e * cos(thB) + cross(u, e) * sin(thB)) * rB + u * (0.37 * g_FlParams.w);   // its own clumps
        float  size = (g_FlForceDent[fi].z > 0.0) ? g_FlForceDent[fi].z : g_FlParams.w;
        // no smoothing: the core is the Inner Radius and nothing else (at 0 the funnel winds down to a point)
        f.xA = lerp(f.xA, bA, w); f.xB = lerp(f.xB, bB, w);
        f.mixA = 1.0 - abs(2.0 * ph - 1.0);
        if (w > f.weight)
        {
            f.weight = w;
            f.size = lerp(g_FlParams.w, size, w); f.depth = g_FlForceDent[fi].x * w; f.sharp = g_FlForceDent[fi].y; f.density = g_FlForceDent[fi].w * w;
        }
    }
    return f;
}
float FunnelWeight(float3 p) { return FunnelOrigin(p).weight; }

// The resting medium at a cell: the shape's soft mask, the height profile (the fog thins
// exponentially above the shape's bottom) and the wind-drifting clump noise, 0..1.
float RestDensity(float3 p)
{
    float3 q = p / g_FlBox.xyz;
    float  m = (g_FlPos.w < 0.5) ? max(abs(q.x), max(abs(q.y), abs(q.z))) : length(q);
    if (m >= 1.0) return 0.0;
    float w = 1.0 - smoothstep(1.0 - g_FlParams2.x, 1.0, m);
    w *= exp(-g_FlCounts.z * max(p.y + g_FlBox.y, 0.0));
    Funnel f = FunnelOrigin(p);
    // A vortex dents even a "uniform" fog: real fog is never uniform, the funnel reveals its
    // structure - inside a field the clump contrast is at least the Dent Depth whatever the Noise
    const float amount = max(g_FlParams.z, f.depth);
    if (amount > 0.0)
    {
        float3 drift = g_FlWind.xyz * g_FlParams2.z * g_FlParams2.y;
        float  nA = FFbm((f.xA - drift) / f.size);
        float  n  = nA;
        if (f.weight > 0.0)
        {   // the two layers crossfaded, the variance put back (a mix of two noises is flatter than either)
            float nB = FFbm((f.xB - drift) / f.size);
            float m  = f.mixA;
            n = 0.5 + (lerp(nB, nA, m) - 0.5) / sqrt(max(1.0 - 2.0 * m * (1.0 - m), 0.5));
        }
        // the clumps: cut and steepened; the funnel's sharpness sets the cut (0.3..0.5) and the steepness (2..8)
        float cut = lerp(0.4, lerp(0.3, 0.5, f.sharp), f.weight), steep = lerp(3.0, lerp(2.0, 8.0, f.sharp), f.weight);
        float clumps = saturate((n - cut) * steep);
        w *= lerp(1.0, clumps, amount) * (1.0 + f.density * clumps);   // the folds denser than the fog around
    }
    return w;
}

// ---- parcels ---------------------------------------------------------------------------------
float PHash(uint i, float k) { return FHash(float3((float)(i % 8191u) * 0.173, k * 1.317 + (float)(i / 8191u), g_FlParams2.y * 0.37 + k)); }
// Under the vortex fields: is this point a drain (a draining core, a pushing rim)? And where
// does the medium enter: 1 = the box's shell (something drains), 2 = a pushing core, 0 = anywhere.
float VortexDrain(float3 p, out int spawnKind, out int pushField)
{
    float drain = 0.0; spawnKind = 0; pushField = -1;
    const int nf = (int)g_FlCounts.y;
    [loop] for (int fi = 0; fi < nf; ++fi)
    {
        if ((int)g_FlForceMisc[fi].x != 2) continue;
        float pull = g_FlForceMisc[fi].w;
        if (pull > 0.0 && spawnKind == 0) spawnKind = 1;
        if (pull < 0.0) { spawnKind = 2; pushField = fi; }
        float3 d = p - g_FlForcePos[fi].xyz; float R = g_FlForcePos[fi].w;
        float  l = length(d);
        float3 u = normalize(g_FlForceDir[fi].xyz);
        float  rh = length(d - u * dot(d, u));
        // swallowed by the core: melting from 30% of the radius in, the more the closer (a hard
        // edge piled the dying parcels into a ball); pushed past the rim: gone
        if (pull > 0.0 && l < R) drain = max(drain, saturate(1.0 - rh / (0.4 * R)));
        if (pull < 0.0 && l >= R && l < R + 0.5 * g_FlPos.x) drain = 1.0;
    }
    return drain;
}
// One step of the vortex fields on a parcel, exact: a turn about the axis by omega dt (an Euler
// step on a circle always drifts outward, r (omega dt)^2 / 2 a step - near a fast core that beat
// the pull and the parcels hung in a ring for ever) and the radial pull along the radius. The
// angular speed is the field's profile: strength x falloff, (R/r)^(1+pull), a rim speed of
// 0.35 s x acceleration like everything else the fields move.
float3 VortexStep(float3 p, float dt)
{
    const int nf = (int)g_FlCounts.y;
    [loop] for (int fi = 0; fi < nf; ++fi)
    {
        if ((int)g_FlForceMisc[fi].x != 2) continue;
        float3 d = p - g_FlForcePos[fi].xyz; float R = g_FlForcePos[fi].w;
        float  l = length(d);
        if (l >= R) continue;
        float  w = 1.0 - g_FlForceMisc[fi].y * (l / R);
        float  s = g_FlForceDir[fi].w * w * 0.35;
        float  pull = g_FlForceMisc[fi].w;
        float3 u  = normalize(g_FlForceDir[fi].xyz);
        float3 dr = d - u * dot(d, u);
        float  rh = length(dr);
        if (rh < 1e-4) continue;
        float  prof = min(pow(R / max(rh, 0.1 * R), 1.0 + pull), 10.0);
        float  om   = s * prof / max(rh, 0.1 * R);
        float  th   = om * dt;
        float3 e    = dr / rh;
        float  rh2  = max(rh - pull * s * dt, 0.0);
        p = g_FlForcePos[fi].xyz + u * dot(d, u) + (e * cos(th) + cross(u, e) * sin(th)) * rh2;
    }
    return p;
}
// A birthplace: on the shell / in a pushing core / anywhere, the best of a few tries by the
// resting shape (so the fog is born where the fog lives).
float3 Spawn(uint i, float k, int spawnKind, int pushField)
{
    float3 best = 0.0; float bestW = -1.0;
    [unroll] for (int t = 0; t < 4; ++t)
    {
        float kk = k + (float)t * 2.7;
        float3 r = float3(PHash(i, kk), PHash(i, kk + 0.5), PHash(i, kk + 1.0)) * 2.0 - 1.0;
        float3 q;
        if (spawnKind == 2)
        {
            float R = g_FlForcePos[pushField].w; float3 u = normalize(g_FlForceDir[pushField].xyz);
            float3 rr = r - u * dot(r, u);
            q = g_FlForcePos[pushField].xyz + rr * (0.15 * R) + u * (r.y * 0.3 * R);
        }
        else
        {
            q = r * g_FlBox.xyz;
            if (spawnKind == 1)
            {   // the shell: one face, a cell in
                int f = (int)(PHash(i, kk + 1.5) * 5.999);
                float sgn = (f & 1) ? 1.0 : -1.0;
                if (f < 2) q.x = sgn * (g_FlBox.x - g_FlMisc.y); else if (f < 4) q.y = sgn * (g_FlBox.y - g_FlMisc.z); else q.z = sgn * (g_FlBox.z - g_FlMisc.w);
            }
        }
        float w = RestShape(q) * (0.5 + 0.5 * PHash(i, kk + 2.0));
        if (w > bestW) { bestW = w; best = q; }
    }
    return best;
}

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID)
{
    const int   pass = (int)g_FlRes.w;
    const float dt   = g_FlBox.w;
    const float3 cs  = g_FlMisc.yzw;
    const uint  N    = (uint)g_FlSplat.w;
    if (pass == 4)   // parcels move (one thread per parcel)
    {
        const uint i = gid.x * 256u + gi;
        if (i >= N) return;
        float4 P0 = g_Parcels[2u * i], P1 = g_Parcels[2u * i + 1u];
        const float ramp = max(g_FlWind.w, 0.05), fadeOut = max(g_FlParams.y, 0.5);   // clumps: born at the Refill rate, dissolve at the Dissipation rate (at least 0.5/s)
        int sk, pf; float drain = VortexDrain(P0.xyz, sk, pf);
        if (g_FlParams2.w < 0.5)
        {   // a fresh volume: everything born at once, fading in
            P0.xyz = Spawn(i, 0.0, 0, -1); P1 = float4(0.0, PHash(i, 9.0), 0.0, 0.0);
        }
        else
        {
            float3 sv;
            float3 v = g_VelIn.SampleLevel(g_VelIn_sampler, ToUVW(P0.xyz), 0).xyz + FieldDrift(P0.xyz, false);   // the air and the other fields
            P0.xyz = VortexStep(P0.xyz + v * dt, dt);                                                           // the vortices, exactly
            if (any(abs(P0.xyz) > g_FlBox.xyz) || Solid(P0.xyz, sv)) P1.z = 1.0;   // out of the box / in a body: dying
            P1.w = drain;
            if (P1.z > 0.5)      P1.x -= dt * fadeOut;                          // out of the box / in a body
            else if (drain > 0.0) P1.x -= dt * fadeOut * 40.0 * drain * drain;   // melting into the drain (Fade Out 1 = swallowed well before the axis, where the flux focuses to a point), shrinking as it goes
            else                  P1.x = min(1.0, P1.x + dt * ramp);
            if (P1.x <= 0.0) { P0.xyz = Spawn(i, P1.y * 13.0 + g_FlParams2.y, sk, pf); P1 = float4(0.0, PHash(i, P1.y * 31.0), 0.0, 0.0); }
        }
        g_Parcels[2u * i] = P0; g_Parcels[2u * i + 1u] = P1;
        if (P1.x > 0.0) InterlockedAdd(g_Ledger[(uint)g_FlCounts.w * 2 + 1], (uint)(P1.x * 256.0 + 0.5));   // the weights' sum: the splat is normalised to it
        return;
    }
    if (pass == 6)   // parcels splat: a soft clump, its shape a piece of the clump noise drifting in time (one thread per parcel)
    {
        const uint i = gid.x * 256u + gi;
        if (i >= N) return;
        float4 P0 = g_Parcels[2u * i], P1 = g_Parcels[2u * i + 1u];
        if (P1.x <= 0.001) return;
        const float rm = g_FlPos.x * ((P1.z > 0.5 || P1.w > 0.0) ? (0.3 + 0.7 * P1.x) : 1.0);   // a dying parcel shrinks into the drain
        const float3 css = SplatCell();
        const int3  rc = min((int3)ceil(rm / css), int3(6, 6, 6));
        const int3  cc = (int3)floor(ToUVW(P0.xyz) * g_FlSplat.xyz);
        const float3 nofs = P1.y * 37.0 + g_FlParams2.y * 0.2;
        float wsum = 0.0;
        [loop] for (int dz = -rc.z; dz <= rc.z; ++dz) [loop] for (int dy = -rc.y; dy <= rc.y; ++dy) [loop] for (int dx = -rc.x; dx <= rc.x; ++dx)
        {
            int3 c = cc + int3(dx, dy, dz); if (!InsideS(c)) continue;
            float3 o = CellPosS(c) - P0.xyz; float q = length(o) / rm; if (q >= 1.0) continue;
            float w = (1.0 - q * q); w *= w;
            w *= saturate(FFbm(o / (rm * 0.6) + nofs) * 1.8 - 0.1);
            wsum += w;
        }
        if (wsum <= 1e-4) return;
        const float scale = P1.x * 65536.0 / wsum;
        [loop] for (int dz2 = -rc.z; dz2 <= rc.z; ++dz2) [loop] for (int dy2 = -rc.y; dy2 <= rc.y; ++dy2) [loop] for (int dx2 = -rc.x; dx2 <= rc.x; ++dx2)
        {
            int3 c = cc + int3(dx2, dy2, dz2); if (!InsideS(c)) continue;
            float3 o = CellPosS(c) - P0.xyz; float q = length(o) / rm; if (q >= 1.0) continue;
            float w = (1.0 - q * q); w *= w;
            w *= saturate(FFbm(o / (rm * 0.6) + nofs) * 1.8 - 0.1);
            uint add = (uint)(w * scale + 0.5);
            if (add > 0u) InterlockedAdd(g_Acc[c], add);
        }
        return;
    }

    const int3 c = (int3)id;
    const uint cur = (uint)g_FlCounts.w, prv = (cur + 2u) % 3u, nxt = (cur + 1u) % 3u;
    if (pass == 5)   // splat grid: clear; the resting amount of this step (what the parcels are normalised to)
    {
        if (!InsideS(c)) return;
        g_Acc[c] = 0u;
        if (all(c == 0)) { g_Ledger[nxt * 2] = 0; g_Ledger[nxt * 2 + 1] = 0; }   // next frame's slot; nobody touches it this frame
        float3 ps = CellPosS(c); float3 sv;
        float rest = Solid(ps, sv) ? 0.0 : RestDensity(ps);
        if (rest > 0.0) InterlockedAdd(g_Ledger[cur * 2], (uint)(rest * 16.0 + 0.5));
        return;
    }
    if (pass == 7)   // splat grid: resolve -> the fog the froxels sample (each parcel a fixed share, no pumping while some fade)
    {
        if (!InsideS(c)) return;
        float restSum = (float)g_Ledger[prv * 2] / 16.0;    // last frame's (one frame behind, changes slowly)
        float M = restSum / (float)max(N, 1u);
        float3 ps = CellPosS(c); float3 sv;
        g_RhoOut[c] = Solid(ps, sv) ? 0.0 : saturate((float)g_Acc[c] / 65536.0 * M);
        return;
    }
    if (!Inside(c)) return;
    const float3 p   = CellPos(c);
    float3 solidVel;
    const bool solid = Solid(p, solidVel);
    if (pass == 0)   // velocity: advect, then the forces
    {
        if (solid) { g_VelOut[c] = float4(solidVel, 0.0); return; }
        float3 v;
        if (g_FlParams2.w > 0.5)
        {
            float3 v0   = g_VelIn.SampleLevel(g_VelIn_sampler, ToUVW(p), 0).xyz;
            float3 back = p - v0 * dt;
            v = all(abs(back) < g_FlBox.xyz) ? g_VelIn.SampleLevel(g_VelIn_sampler, ToUVW(back), 0).xyz : g_FlWind.xyz;   // from outside: the wind flows in
        }
        else v = g_FlWind.xyz;
        // the wind carries the fluid (couples in over ~1 s); a little damping bleeds old swirls
        v += (g_FlWind.xyz - v) * (1.0 - exp(-dt * 1.0));
        v *= exp(-dt * 0.15);
        // stirring
        if (g_FlParams.x > 0.0) v += CurlNoise(p / max(g_FlParams.w, 0.1) + g_FlParams2.y * 0.15) * g_FlParams.x * (1.0 - exp(-dt * 2.0));
        // (the Force Fields act on the medium directly - FieldDrift - not through the air: driven
        // through the pressure solve they left a drift that carried the fog out of a vortex)
        // Air drag, quadratic in the speed relative to the wind: a field of 12 m/s^2 settles at
        // ~8 m/s instead of accelerating without bound and flushing the box with fresh medium.
        {
            float3 rel = v - g_FlWind.xyz;
            float  sp  = length(rel);
            rel *= 1.0 / (1.0 + dt * 0.2 * sp);
            v = g_FlWind.xyz + rel;
            float vm = length(v);
            if (vm > 30.0) v *= 30.0 / vm;
        }
        g_VelOut[c] = float4(v, 0.0);
    }
    else if (pass == 1)   // divergence of the advected velocity; pressure starts at zero
    {
        if (solid) { g_DivOut[c] = 0.0; g_PrsOut[c] = 0.0; return; }
        float3 vxp = VelAt(c + int3(1, 0, 0)), vxm = VelAt(c - int3(1, 0, 0));
        float3 vyp = VelAt(c + int3(0, 1, 0)), vym = VelAt(c - int3(0, 1, 0));
        float3 vzp = VelAt(c + int3(0, 0, 1)), vzm = VelAt(c - int3(0, 0, 1));
        float div = (vxp.x - vxm.x) / (2.0 * cs.x) + (vyp.y - vym.y) / (2.0 * cs.y) + (vzp.z - vzm.z) / (2.0 * cs.z);
        g_DivOut[c] = div;
        g_PrsOut[c] = 0.0;
    }
    else if (pass == 2)   // Jacobi: anisotropic Poisson step, open outside, solid faces closed
    {
        if (solid) { g_PrsOut[c] = 0.0; return; }
        float own = g_PrsIn[c];
        float ix = 1.0 / (cs.x * cs.x), iy = 1.0 / (cs.y * cs.y), iz = 1.0 / (cs.z * cs.z);
        float sum = (PrsAt(c + int3(1, 0, 0), own) + PrsAt(c - int3(1, 0, 0), own)) * ix
                  + (PrsAt(c + int3(0, 1, 0), own) + PrsAt(c - int3(0, 1, 0), own)) * iy
                  + (PrsAt(c + int3(0, 0, 1), own) + PrsAt(c - int3(0, 0, 1), own)) * iz;
        g_PrsOut[c] = (sum - g_DivIn[c]) / (2.0 * (ix + iy + iz));
    }
    else if (pass == 3)   // project: subtract the pressure gradient (solids keep the body's velocity)
    {
        if (solid) { g_VelOut[c] = float4(solidVel, 0.0); return; }
        float3 v = g_VelIn[c].xyz;
        float own = g_PrsIn[c];
        float3 grad = float3((PrsAt(c + int3(1, 0, 0), own) - PrsAt(c - int3(1, 0, 0), own)) / (2.0 * cs.x),
                             (PrsAt(c + int3(0, 1, 0), own) - PrsAt(c - int3(0, 1, 0), own)) / (2.0 * cs.y),
                             (PrsAt(c + int3(0, 0, 1), own) - PrsAt(c - int3(0, 0, 1), own)) / (2.0 * cs.z));
        g_VelOut[c] = float4(v - grad, 0.0);
    }
    // ---- grid mode: the fog density rides the motion vectors - the air (wind, turbulence, the
    // bodies' wakes) plus the Force Fields' drift - by MacCormack advection; a diverging drift
    // thins it, a converging one packs it; it relaxes toward the resting medium (Refill fills
    // a hole, Dissipation thins packed fog). Steps at the air's rate.
    else if (pass == 8)   // MacCormack step 1: the plain forward advection -> g_DensOut (the scratch)
    {
        float fwd = 0.0;
        float3 sv;
        if (!solid)
        {
            float3 v = g_VelIn.SampleLevel(g_VelIn_sampler, ToUVW(p), 0).xyz + FieldDrift(p, false);
            float3 back = p - v * dt;
            if (Solid(back, sv)) fwd = 0.0;
            else if (all(abs(back) < g_FlBox.xyz)) fwd = g_DensIn.SampleLevel(g_DensIn_sampler, ToUVW(back), 0);
            else fwd = RestDensity(back);   // from outside: the undisturbed medium
        }
        g_DensOut[c] = fwd;
    }
    else if (pass == 9)   // MacCormack step 2, continuity, relaxation
    {
        if (solid) { g_DensOut[c] = 0.0; g_RhoOut[c] = 0.0; return; }   // the body occupies the cell
        const float rest = RestDensity(p);
        float dens = rest;
        if (g_FlParams2.w > 0.5)
        {
            float3 v = g_VelIn.SampleLevel(g_VelIn_sampler, ToUVW(p), 0).xyz + FieldDrift(p, false);
            float3 back = p - v * dt, fore = p + v * dt;
            float  fwd = g_Scratch[c];
            float3 sv;
            if (Solid(back, sv)) dens = 0.0;
            else
            {
                float bwd = all(abs(fore) < g_FlBox.xyz) ? g_Scratch.SampleLevel(g_DensIn_sampler, ToUVW(fore), 0) : fwd;
                dens = fwd + 0.5 * (g_DensIn[c] - bwd);
                int3 cb = Clamp3((int3)floor(ToUVW(back) * g_FlRes.xyz - 0.5));
                float mn = 1e9, mx = -1e9;
                [unroll] for (int dz = 0; dz <= 1; ++dz) [unroll] for (int dy = 0; dy <= 1; ++dy) [unroll] for (int dx = 0; dx <= 1; ++dx)
                { float dd = g_DensIn[Clamp3(cb + int3(dx, dy, dz))]; mn = min(mn, dd); mx = max(mx, dd); }
                if (dens < mn || dens > mx) dens = fwd;   // an overshoot: back to the first-order sample (clamping it scalloped every curved front)
                // the fog diffuses (turbulent mixing at the cell scale): a front is never sharper than a
                // couple of cells, so the grid's lattice cannot print teeth along a curved edge
                {
                    float nb = g_DensIn[Clamp3(c + int3(1, 0, 0))] + g_DensIn[Clamp3(c - int3(1, 0, 0))]
                             + g_DensIn[Clamp3(c + int3(0, 1, 0))] + g_DensIn[Clamp3(c - int3(0, 1, 0))]
                             + g_DensIn[Clamp3(c + int3(0, 0, 1))] + g_DensIn[Clamp3(c - int3(0, 0, 1))];
                    dens = lerp(dens, nb / 6.0, 1.0 - exp(-dt * 6.0));
                }
                float3 ex = float3(cs.x, 0, 0), ey = float3(0, cs.y, 0), ez = float3(0, 0, cs.z);
                float  divF = (FieldDrift(p + ex, false).x - FieldDrift(p - ex, false).x) / (2.0 * cs.x)
                            + (FieldDrift(p + ey, false).y - FieldDrift(p - ey, false).y) / (2.0 * cs.y)
                            + (FieldDrift(p + ez, false).z - FieldDrift(p - ez, false).z) / (2.0 * cs.z);
                dens *= exp(-divF * dt);
                // relaxation toward the resting medium BOTH ways at the Refill rate (packed fog thins faster
                // still by Dissipation): a stirred patch settles to the mean of the pattern - one-sided
                // (holes only) it ratcheted up to the clumps' peaks and a vortex stood denser than its
                // surroundings
                dens = rest + (dens - rest) * exp(-dt * (g_FlWind.w + ((dens > rest) ? g_FlParams.y : 0.0)));
            }
        }
        dens = saturate(lerp(dens, rest, FunnelWeight(p)));   // inside a vortex the fog IS the funnel
        g_DensOut[c] = dens;
        g_RhoOut[c]  = dens;
    }
}
