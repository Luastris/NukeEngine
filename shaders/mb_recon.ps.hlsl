// Motion blur pass 3 (full res): McGuire-style reconstruction. Taps along the neighbourhood's
// dominant velocity; a tap counts when it is in front and moving (it smears over this pixel) or
// behind and this pixel is moving (this pixel smears over it). Depth decides front/behind.
Texture2D         g_Source;   SamplerState g_Source_sampler;   // HDR chain colour
Texture2D<float2> g_Velocity;                                    // per-pixel motion (uv units), Load
Texture2D<float>  g_Depth;                                       // prepass device depth, Load
Texture2D<float2> g_Tiles;    SamplerState g_Tiles_sampler;      // neighbour max (px), tile res
cbuffer MbCB
{
    float4 g_Mb0;   // full w, full h, tile size, max blur (px)
    float4 g_Mb1;   // shutter, samples, 1/tiles w, 1/tiles h
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float Cone(float dist, float len) { return saturate(1.0 - dist / max(len, 1e-3)); }
float Cylinder(float dist, float len) { return 1.0 - smoothstep(0.95 * len, 1.05 * len, dist); }
float SoftZ(float za, float zb) { return saturate(1.0 - (za - zb) * 400.0); }   // 1 = a is in front of b (device depth)

float4 main(in PSIn i) : SV_Target
{
    float2 px = i.pos.xy;
    float2 vmax = g_Tiles.Sample(g_Tiles_sampler, i.uv);
    float  lmax = length(vmax);
    float4 c0 = g_Source.Sample(g_Source_sampler, i.uv);
    if (lmax < 0.5) return c0;

    int2 ip = int2(px);
    float2 v0 = g_Velocity.Load(int3(ip, 0)) * g_Mb0.xy * g_Mb1.x;
    float  l0 = max(length(v0), 0.5);
    if (length(v0) > g_Mb0.w) v0 *= g_Mb0.w / length(v0);
    float  z0 = g_Depth.Load(int3(ip, 0));

    const int N = clamp((int)g_Mb1.y, 4, 32);
    // A per-pixel jitter breaks the tap pattern into noise instead of banding.
    float jit = frac(sin(dot(px, float2(12.9898, 78.233))) * 43758.5453) - 0.5;
    float2 dir = vmax / lmax;
    float3 sum = c0.rgb * (1.0 / l0); float wsum = 1.0 / l0;
    [loop] for (int k = 0; k < N; ++k)
    {
        float t = lerp(-1.0, 1.0, ((float)k + jit + 1.0) / (float)(N + 1));
        float2 sp = px + dir * (t * lmax);
        float2 suv = sp / g_Mb0.xy;
        if (any(suv < 0.0) || any(suv > 1.0)) continue;
        int2 sip = int2(sp);
        float  zs = g_Depth.Load(int3(sip, 0));
        float2 vs = g_Velocity.Load(int3(sip, 0)) * g_Mb0.xy * g_Mb1.x;
        float  ls = max(length(vs), 0.5);
        float  dist = abs(t * lmax);
        float f = SoftZ(zs, z0), b = SoftZ(z0, zs);
        float w = f * Cone(dist, ls)                 // the tap is in front and blurs over us
                + b * Cone(dist, l0)                 // we are in front and blur over the tap
                + Cylinder(dist, ls) * Cylinder(dist, l0) * 2.0;   // both moving alike
        sum += g_Source.Sample(g_Source_sampler, suv).rgb * w; wsum += w;
    }
    return float4(sum / max(wsum, 1e-4), c0.a);
}
