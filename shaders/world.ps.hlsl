// World (3D) pass — pixel shader. Simple directional + ambient, diffuse texture * material color.
cbuffer MatCB { float4 g_Color; float4 g_Params; };   // g_Params.x = hasTexture
Texture2D    g_Tex;
SamplerState g_Tex_sampler;
struct PSIn { float4 pos : SV_POSITION; float3 nrm : TEXCOORD0; float2 uv : TEXCOORD1; };
float4 main(in PSIn i) : SV_Target
{
    float3 N = normalize(i.nrm);
    float3 L = normalize(float3(0.4, 0.85, 0.35));
    float  d = max(dot(N, L), 0.0) * 0.8 + 0.2;   // simple directional + ambient
    float4 base = (g_Params.x > 0.5) ? g_Tex.Sample(g_Tex_sampler, i.uv) : float4(1, 1, 1, 1);
    base *= g_Color;
    return float4(d * base.rgb, base.a);
}
