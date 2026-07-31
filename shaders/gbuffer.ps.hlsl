// G-buffer prepass pixel shader: writes octahedral world normal (.xy), roughness (.z), metalness (.w),
// plus a screen-space motion vector and the per-object id. Normal mapping is applied here, as in world.ps.
cbuffer MatCB { float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; };
Texture2D    g_MetalRough;   SamplerState g_MetalRough_sampler;   // G = roughness, B = metallic (glTF)
Texture2D    g_Normal;       SamplerState g_Normal_sampler;       // tangent-space normal map

struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2;
              float4 curClip : TEXCOORD3; float4 prevClip : TEXCOORD4;
              nointerpolation float objId : TEXCOORD5; };
struct PSOut { float4 gbuf : SV_Target0; float2 velocity : SV_Target1; float objId : SV_Target2; };   // objId target is R8

// Tangent-space normal mapping without mesh tangents; must stay identical to world.ps.
// Derivatives are taken by the caller: ddx/ddy on a passed-in parameter miscompiles to zero on DXC.
float3 PerturbNormal(float3 N, float3 n, float3 dp1, float3 dp2, float2 du1, float2 du2)
{
    float3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    float3 T = dp2p * du1.x + dp1p * du2.x;
    float3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));   // div-by-zero guard only; a larger floor flattens fine frames
    return normalize(T * (inv * n.x) + B * (inv * n.y) + N * n.z);
}

// Octahedral normal encode (unit vector -> [-1,1]^2). Decoded in ssr.post.hlsl.
float2 OctEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    float2 e = (n.z >= 0.0) ? n.xy : (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return e;
}

void main(PSIn i, out PSOut o)
{
    float metallic = saturate(g_Params.z);
    float rough    = clamp(g_Params.w, 0.04, 1.0);
    if (g_Params2.x > 0.5)
    {
        float3 m = g_MetalRough.Sample(g_MetalRough_sampler, i.uv).rgb;
        rough = clamp(m.g, 0.04, 1.0); metallic = saturate(m.b);
    }
    float3 N = normalize(i.nrm);
    // g_Params.y: 0 = none, >0 = OpenGL green (flip), <0 = DirectX. Must match world.ps.
    if (abs(g_Params.y) > 0.5)
    {
        float2 nxy = g_Normal.Sample(g_Normal_sampler, i.uv).rg * 2.0 - 1.0;
        if (g_Params.y > 0.0) nxy.y = -nxy.y;
        float3 nTS = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
        N = PerturbNormal(N, nTS, ddx(i.wpos), ddy(i.wpos), ddx(i.uv), ddy(i.uv));
    }
    o.gbuf = float4(OctEncode(N), rough, metallic);

    // Motion vector in UV space, from UNjittered transforms. TAA reads it as prevUV = uv - velocity.
    float2 curNdc  = i.curClip.xy  / i.curClip.w;
    float2 prevNdc = i.prevClip.xy / i.prevClip.w;
    float2 curUV   = float2(curNdc.x  * 0.5 + 0.5, 0.5 - curNdc.y  * 0.5);
    float2 prevUV  = float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5);
    o.velocity = curUV - prevUV;
    o.objId = i.objId;
}
