// G-buffer prepass — pixel shader. Single-sample pass that captures surface data for screen-space reflections:
// octahedral world-space normal (.xy), roughness (.z), metalness (.w). Reuses world.vs (PSIn below). Vertex
// normals only (no normal-map perturbation) — plenty for SSR ray reflection, and avoids needing the view vector.
cbuffer MatCB { float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; };
SamplerState g_Tex_sampler;
Texture2D    g_MetalRough;   // G = roughness, B = metallic (glTF)

struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };

// Octahedral normal encode (unit vector -> [-1,1]^2). Decoded in ssr.post.hlsl.
float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}

float4 main(PSIn i) : SV_TARGET
{
    float metallic = saturate(g_Params.z);
    float rough    = clamp(g_Params.w, 0.04, 1.0);
    if (g_Params2.x > 0.5)
    {
        float3 m = g_MetalRough.Sample(g_Tex_sampler, i.uv).rgb;
        rough = clamp(m.g, 0.04, 1.0); metallic = saturate(m.b);
    }
    float3 N = normalize(i.nrm);
    return float4(OctEncode(N), rough, metallic);
}
