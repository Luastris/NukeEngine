// Displacement tessellation domain: interpolate the patch, displace along the normal by the
// height map (g_Disp = POM depth, displacement in world units, mid level, tess factor) and
// project. Outputs the exact PSIn world.ps expects, so the SAME pixel shader lights it.
// The displacement fades with the tess factor: at factor 1 it is zero, matching the plain PSO.
cbuffer CB { float4x4 g_WVP; float4x4 g_World; };
cbuffer MatCB { float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; float4 g_UVT; float4 g_UVT2; float4 g_Disp;
                float4 g_Ov0;  float4 g_Ov1;  float4 g_Ov2;  float4 g_Ov3;  float4 g_Ov4;  float4 g_Ov5;  float4 g_Ov6;  float4 g_Ov7;
                float4 g_OvT0; float4 g_OvT1; float4 g_OvT2; float4 g_OvT3; float4 g_OvT4; float4 g_OvT5; float4 g_OvT6; float4 g_OvT7;
                float4 g_OvP0; float4 g_OvP1; float4 g_OvP2; float4 g_OvP3; float4 g_OvP4; float4 g_OvP5; float4 g_OvP6; float4 g_OvP7;
                float4 g_OvM0; float4 g_OvM1; float4 g_OvM2; float4 g_OvMQ;
                float4 g_Det; float4 g_Var; };
Texture2D    g_Height;
SamplerState g_Height_sampler;

struct HSOut { float3 pos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };
struct PatchTess { float edge[3] : SV_TessFactor; float inside : SV_InsideTessFactor; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };

[domain("tri")]
PSIn main(PatchTess pt, float3 b : SV_DomainLocation, const OutputPatch<HSOut, 3> p)
{
    float3 pos = p[0].pos * b.x + p[1].pos * b.y + p[2].pos * b.z;
    float3 nrm = normalize(p[0].nrm * b.x + p[1].nrm * b.y + p[2].nrm * b.z);
    float2 uv  = p[0].uv  * b.x + p[1].uv  * b.y + p[2].uv  * b.z;

    // Sample the height where the material maps land: the same UV transform as the passes.
    float2 tl = (abs(g_UVT.x) + abs(g_UVT.y) < 1e-6) ? float2(1.0, 1.0) : g_UVT.xy;
    float2 suv = uv * tl + g_UVT.zw;
    if (abs(g_UVT2.x) > 1e-6)
    {
        float sr, cr; sincos(g_UVT2.x, sr, cr);
        suv = float2(suv.x * cr - suv.y * sr, suv.x * sr + suv.y * cr);
    }
    float h = g_Height.SampleLevel(g_Height_sampler, suv, 0).r;

    // Displacement in WORLD units mapped back to local space by the world scale ALONG THE
    // NORMAL (non-uniformly scaled objects displace correctly).
    float axis = max(length(mul((float3x3)g_World, nrm)), 1e-4);
    float fade = saturate((g_Disp.w - 1.0) * 0.5);   // factor 1 -> zero displacement (seam-free handover)
    pos += nrm * ((h - g_Disp.z) * g_Disp.y / axis) * fade;

    PSIn o;
    o.pos  = mul(g_WVP,   float4(pos, 1.0));
    o.wpos = mul(g_World, float4(pos, 1.0)).xyz;
    o.nrm  = mul((float3x3)g_World, nrm);
    o.uv   = uv;
    return o;
}
