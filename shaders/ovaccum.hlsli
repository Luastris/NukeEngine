// W5 accumulation weight of one overlay slot in a DOMAIN shader: the state's value through its
// threshold/feather, topOnly and (from-sky) the sky gate, times the slot's displacement depth.
// Pixel-only shaping (painted mask, 2D mask map) is not repeated here. Needs skyocc.hlsli + MatCB.
float OvAccum(float4 ov, float4 ovp, float d, float3 wp, float3 ng)
{
    if (d <= 0.0 || ov.x <= 0.0) return 0.0;
    float v = ov.x;
    if ((uint)(ovp.w + 0.5) & 32u) v *= SkyGate(wp, ng);
    float w = smoothstep(ov.y, ov.y + max(ov.z, 1e-3), v);
    if (ov.w > 0.0) { float up = saturate(ng.y); w *= lerp(1.0, up * up, ov.w); }
    return w * d;
}
float OvAccumAll(float3 wp, float3 ng)
{
    return OvAccum(g_Ov0, g_OvP0, g_OvD0.x, wp, ng) + OvAccum(g_Ov1, g_OvP1, g_OvD0.y, wp, ng)
         + OvAccum(g_Ov2, g_OvP2, g_OvD0.z, wp, ng) + OvAccum(g_Ov3, g_OvP3, g_OvD0.w, wp, ng)
         + OvAccum(g_Ov4, g_OvP4, g_OvD1.x, wp, ng) + OvAccum(g_Ov5, g_OvP5, g_OvD1.y, wp, ng)
         + OvAccum(g_Ov6, g_OvP6, g_OvD1.z, wp, ng) + OvAccum(g_Ov7, g_OvP7, g_OvD1.w, wp, ng);
}
