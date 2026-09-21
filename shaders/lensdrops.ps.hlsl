// Lens film composite: refracts the scene through the wet mask (lensfilm.ps). Only PARTIAL
// wetness distorts - a dry lens and a fully sheeted one both stay clear - so the look lives on
// the front line, the streaks and the beads.
Texture2D        g_Scene; SamplerState g_Scene_sampler;
Texture2D<float> g_Wet;   SamplerState g_Wet_sampler;
cbuffer LensCB
{
    float4 g_L0;   // dt, 1/drain, time, inject on
    float4 g_L1;   // rain rate, rain amount, w, h
    float4 g_L2;   // 1/w, 1/h, composite amount, 0
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(in PSIn i) : SV_TARGET
{
    float3 col = g_Scene.Sample(g_Scene_sampler, i.uv).rgb;
    float amount = g_L2.z;
    if (amount <= 0.001) return float4(col, 1.0);
    float m = g_Wet.Sample(g_Wet_sampler, i.uv);
    if (m < 0.015) return float4(col, 1.0);

    // Film thickness profile; its gradient is the film surface normal.
    float2 px = g_L2.xy;
    float h0 = smoothstep(0.0, 1.0, m);
    float hx = smoothstep(0.0, 1.0, g_Wet.Sample(g_Wet_sampler, i.uv + float2(px.x * 3.0, 0.0)))
             - smoothstep(0.0, 1.0, g_Wet.Sample(g_Wet_sampler, i.uv - float2(px.x * 3.0, 0.0)));
    float hy = smoothstep(0.0, 1.0, g_Wet.Sample(g_Wet_sampler, i.uv + float2(0.0, px.y * 3.0)))
             - smoothstep(0.0, 1.0, g_Wet.Sample(g_Wet_sampler, i.uv - float2(0.0, px.y * 3.0)));
    float3 n = normalize(float3(hx * 3.5, hy * 3.5, 1.0));

    float edge = saturate(m * (1.0 - m) * 4.0);
    float k = saturate(edge * amount);
    float2 off = -n.xy * (0.06 + 0.14 * k) * saturate(amount) + float2(0.0, 0.02 * k);
    off += float2(sin(g_L0.z * 2.1 + i.uv.y * 30.0), cos(g_L0.z * 1.7 + i.uv.x * 26.0)) * 0.002 * h0 * saturate(amount);
    float2 suv = clamp(i.uv + off * saturate(m + edge), 0.002, 0.998);
    float3 wetC = g_Scene.Sample(g_Scene_sampler, suv).rgb;

    float3 L = normalize(float3(-0.35, -0.8, 0.55));
    float sharpEdge = saturate((length(float2(hx, hy)) * 6.0 - 0.4) * 2.0);
    float spec = pow(saturate(dot(n, L)), 10.0) * edge * 0.45 * sharpEdge;
    wetC = wetC + spec;

    col = lerp(col, wetC, saturate(edge * 1.6 + h0 * 0.3) * saturate(amount));
    return float4(col, 1.0);
}
