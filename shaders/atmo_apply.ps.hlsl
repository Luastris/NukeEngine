// Physical atmosphere, aerial perspective on the geometry: scene * transmittance + in-scatter
// from the camera's AP froxel volume at the surface's distance. Sky pixels pass through (the
// sky-view LUT already holds the whole path). Runs before the clouds / fog composites. The LDR
// path un/re-tonemaps around the mix, like the fog.
#include "atmosphere.hlsli"

Texture2D g_Source; SamplerState g_Source_sampler;   // scene colour
Texture2D g_Depth;  SamplerState g_Depth_sampler;    // prepass device depth (point)

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float LinearZ(float d) { float n = g_AtMisc.x, f = g_AtMisc.y; return n * f / max(f - d * (f - n), 1e-6); }
float3 FromLDR(float3 c)
{
    c = pow(max(c, 0.0), 2.2);
    float W = max(g_AtMisc.w, 1e-3);
    float3 y = min(c, 0.999);
    return max(0.5 * W * W * ((y - 1.0) + sqrt((1.0 - y) * (1.0 - y) + 4.0 * y / (W * W))), 0.0);
}
float3 ToLDR(float3 c)
{
    float W = max(g_AtMisc.w, 1e-3);
    c = c * (1.0 + c / (W * W)) / (1.0 + c);
    return pow(max(c, 0.0), 1.0 / 2.2);
}

float4 main(in PSIn i) : SV_Target
{
    float4 src = g_Source.Sample(g_Source_sampler, i.uv);
    float  dev = g_Depth.Sample(g_Depth_sampler, i.uv).r;
    if (dev >= 0.99999) return src;   // the sky
    float4 wp = mul(g_AtInvVP, float4(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, 1.0, 1.0));
    float3 d  = normalize(wp.xyz / wp.w);   // the direction matrix has no translation (precision far from the origin): no camera subtraction
    float4 fc = mul(g_AtInvVP, float4(0.0, 0.0, 1.0, 1.0));
    float3 fwd = normalize(fc.xyz / fc.w);
    float  dist = LinearZ(dev) / max(dot(d, fwd), 1e-4);
    float4 ap = AtmoAerial(i.uv, dist);
    float3 c = src.rgb;
    if (g_AtMisc.z > 0.5) c = ToLDR(FromLDR(c) * ap.a + ap.rgb);
    else                  c = c * ap.a + ap.rgb;
    return float4(c, src.a);
}
