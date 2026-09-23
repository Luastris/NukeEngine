// DOF pass 3 (full res): sharp -> far blur by THIS pixel's own far CoC (from the full-res depth,
// so a sharp object keeps a crisp edge against the blurred background and the background never
// takes the object's CoC), then the near blur over it by its coverage.
Texture2D        g_Source; SamplerState g_Source_sampler;   // sharp full-res colour
Texture2D<float> g_Depth;                                    // prepass device depth (full res, Load)
Texture2D        g_Far;    SamplerState g_Far_sampler;      // half-res far gather
Texture2D        g_Near;   SamplerState g_Near_sampler;     // half-res near gather (a = near coverage)
cbuffer DofCB
{
    float4 g_Dof0;   // near, far, focus distance, focus range
    float4 g_Dof1;   // max CoC (px), near strength, full w, full h
    float4 g_Dof2;
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float LinearDepth(float z) { float n = g_Dof0.x, f = g_Dof0.y; return n * f / max(f - z * (f - n), 1e-6); }
float CoCOf(float d)
{
    float focus = g_Dof0.z, range = max(g_Dof0.w, 0.01), maxCoC = g_Dof1.x;
    // Thin lens: the circle grows with the distance OUTSIDE the sharp band relative to the point
    // itself (1 - focus/d far, focus/d - 1 near), reaching max CoC only far out - not a wall of
    // mush a few metres past the focus.
    float edge = (d > focus) ? focus + range : max(focus - range, 0.05);
    float coc = (d > focus) ? saturate(1.0 - edge / max(d, 1e-3)) : -saturate(edge / max(d, 1e-3) - 1.0);
    if (abs(d - focus) <= range) coc = 0.0;
    coc *= maxCoC;
    if (coc < 0.0) coc *= g_Dof1.y;
    return coc;
}

float4 main(in PSIn i) : SV_Target
{
    float4 sharp = g_Source.Sample(g_Source_sampler, i.uv);
    float4 far   = g_Far.Sample(g_Far_sampler, i.uv);
    float4 nr    = g_Near.Sample(g_Near_sampler, i.uv);
    float  coc   = CoCOf(LinearDepth(g_Depth.Load(int3(int2(i.pos.xy), 0))));
    // Sharp -> far over the first 3 px of CoC: the half-res gather is ~2 px soft even at its
    // smallest, so a shorter ramp swapped the sharp image for it in one step - a line at the
    // sharp band's edge.
    float fb = saturate(max(coc, 0.0) / 3.0);
    float3 c = lerp(sharp.rgb, far.rgb, fb);
    // Near: a pixel INSIDE a near object blurs by its OWN CoC on the same ramp (the coverage alone
    // weighted it ~30 % at a 4 px circle, so the blur snapped in late); outside a near object the
    // neighbours' coverage carries the halo over the background.
    float nb = max(saturate(-coc / 3.0), nr.a);
    c = lerp(c, nr.rgb, nb);
    return float4(c, sharp.a);
}
