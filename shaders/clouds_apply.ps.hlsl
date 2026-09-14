// Volumetric clouds, composite: scene * transmittance + in-scatter wherever the surface lies
// beyond the clouds' entry (the sky, and anything farther than the layer's near edge). Runs
// before the fog composite, so the atmosphere's own scattering is applied over the clouds. On
// the LDR path the scene is un-tonemapped around the mix, like the fog.
#include "clouds.hlsli"

Texture2D          g_Source;    SamplerState g_Source_sampler;    // scene colour
Texture2D          g_Depth;     SamplerState g_Depth_sampler;     // prepass device depth (point)
Texture2D          g_Clouds;    SamplerState g_Clouds_sampler;    // resolved clouds (linear)
Texture2D<float2>  g_CloudDist;                                   // entry / mean distance (Load)

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float LinearZ(float d) { float n = g_ClMisc.x, f = g_ClMisc.y; return n * f / max(f - d * (f - n), 1e-6); }
float3 FromLDR(float3 c)
{
    c = pow(max(c, 0.0), 2.2);
    float W = max(g_ClMisc.w, 1e-3);
    float3 y = min(c, 0.999);
    return max(0.5 * W * W * ((y - 1.0) + sqrt((1.0 - y) * (1.0 - y) + 4.0 * y / (W * W))), 0.0);
}
float3 ToLDR(float3 c)
{
    float W = max(g_ClMisc.w, 1e-3);
    c = c * (1.0 + c / (W * W)) / (1.0 + c);
    return pow(max(c, 0.0), 1.0 / 2.2);
}

float4 main(in PSIn i) : SV_Target
{
    float4 src = g_Source.Sample(g_Source_sampler, i.uv);
    float4 cl  = g_Clouds.Sample(g_Clouds_sampler, i.uv);
    if (cl.a >= 0.999 && all(cl.rgb <= 0.0)) return src;
    float  dev = g_Depth.Sample(g_Depth_sampler, i.uv).r;
    if (dev < 0.99999)
    {   // a surface: the clouds only lie behind it if it is farther than their entry along this ray
        float4 wp = mul(g_ClInvVP, float4(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, 1.0, 1.0));
        float3 d  = normalize(wp.xyz / wp.w);   // the direction matrix has no translation (precision far from the origin): no camera subtraction
        float4 fc = mul(g_ClInvVP, float4(0.0, 0.0, 1.0, 1.0));
        float3 fwd = normalize(fc.xyz / fc.w);
        float  dist = LinearZ(dev) / max(dot(d, fwd), 1e-4);
        int2   mp = clamp((int2)(i.uv * g_ClScreen.xy), int2(0, 0), (int2)g_ClScreen.xy - 1);
        float  entry = g_CloudDist.Load(int3(mp, 0)).x;
        if (dist < entry) return src;
    }
    float3 c = src.rgb;
    if (g_ClMisc.z > 0.5) c = ToLDR(FromLDR(c) * cl.a + cl.rgb);
    else                  c = c * cl.a + cl.rgb;
    return float4(c, src.a);
}
