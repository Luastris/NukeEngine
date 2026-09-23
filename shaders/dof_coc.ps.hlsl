// DOF pass 1 (half res): the source colour with its signed circle of confusion in alpha.
// CoC < 0 = nearer than the focus (the near field), > 0 = farther; |CoC| in pixels of the full frame.
Texture2D        g_Source; SamplerState g_Source_sampler;   // full-res HDR chain colour
Texture2D<float> g_Depth;                                    // prepass device depth (full res, Load)
cbuffer DofCB
{
    float4 g_Dof0;   // near, far, focus distance (m), focus range (m)
    float4 g_Dof1;   // max CoC (px), near blur strength, full-res w, full-res h
    float4 g_Dof2;   // 1/half w, 1/half h, 0, 0
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float LinearDepth(float z) { float n = g_Dof0.x, f = g_Dof0.y; return n * f / max(f - z * (f - n), 1e-6); }

// Signed CoC of a linear depth: 0 inside the sharp band, thin-lens growth outside it.
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
    if (coc < 0.0) coc *= g_Dof1.y;   // the near field's strength
    return coc;
}

float4 main(in PSIn i) : SV_Target
{
    // The half-res texel covers 2x2 full-res pixels. A NEAR pixel among them wins (a thin near
    // object must keep its near CoC to bleed over what is behind it); otherwise the texel takes
    // the LARGEST far CoC, so a texel straddling a sharp object's edge blurs like the background
    // behind it - the composite picks sharp or blurred per FULL-res pixel (dof_comp), not here.
    int2 p = int2(i.uv * g_Dof1.zw);
    float c00 = CoCOf(LinearDepth(g_Depth.Load(int3(p, 0))));
    float c10 = CoCOf(LinearDepth(g_Depth.Load(int3(p + int2(1, 0), 0))));
    float c01 = CoCOf(LinearDepth(g_Depth.Load(int3(p + int2(0, 1), 0))));
    float c11 = CoCOf(LinearDepth(g_Depth.Load(int3(p + int2(1, 1), 0))));
    float nearest = min(min(c00, c10), min(c01, c11));
    float coc = (nearest < 0.0) ? nearest : max(max(c00, c10), max(c01, c11));
    float3 c = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    return float4(c, coc);
}
