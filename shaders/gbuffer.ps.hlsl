// G-buffer prepass pixel shader: writes octahedral world normal (.xy), roughness (.z), metalness (.w),
// plus a screen-space motion vector and the per-object id. Normal mapping is applied here, as in world.ps.
// LiveMaterial: g_UVT = (uvTiling.xy; 0,0 = identity, uvOffset.xy + tween scroll);
// g_UVT2.x = uvRotation rad — the SSR/TAA normals must sample where the color pass does.
cbuffer MatCB { float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; float4 g_UVT; float4 g_UVT2; float4 g_Disp;
                float4 g_Ov0;  float4 g_Ov1;  float4 g_Ov2;  float4 g_Ov3;  float4 g_Ov4;  float4 g_Ov5;  float4 g_Ov6;  float4 g_Ov7;
                float4 g_OvT0; float4 g_OvT1; float4 g_OvT2; float4 g_OvT3; float4 g_OvT4; float4 g_OvT5; float4 g_OvT6; float4 g_OvT7;
                float4 g_OvP0; float4 g_OvP1; float4 g_OvP2; float4 g_OvP3; float4 g_OvP4; float4 g_OvP5; float4 g_OvP6; float4 g_OvP7;
                float4 g_OvM0; float4 g_OvM1; float4 g_OvM2; float4 g_OvMQ;
                float4 g_Det; float4 g_Var; };
Texture2D    g_MetalRough;   SamplerState g_MetalRough_sampler;   // G = roughness, B = metallic (glTF)
Texture2D    g_Normal;       SamplerState g_Normal_sampler;       // tangent-space normal map
Texture2D    g_Tex;          SamplerState g_Tex_sampler;          // base color (alpha for cutout)
Texture2D    g_WipeMask;     SamplerState g_WipeMask_sampler;     // luma-wipe mask

// Overlay slots shape the G-buffer normal + rough/metal too (SSR must see wet gloss / snow
// bumps). Albedo maps are not needed here; ONE shared sampler (g_Ov0Nrm_sampler) serves the
// block — same D3D11 16-samplers/stage budget as world.ps.
#define OVG_DECL(N) Texture2D g_Ov##N##Nrm; Texture2D g_Ov##N##MR; Texture2D g_Ov##N##Mask;
Texture2D    g_Ov0Nrm;       SamplerState g_Ov0Nrm_sampler;
Texture2D    g_Ov0MR; Texture2D g_Ov0Mask;
OVG_DECL(1) OVG_DECL(2) OVG_DECL(3) OVG_DECL(4) OVG_DECL(5) OVG_DECL(6) OVG_DECL(7)
Texture2D    g_Mask3D;       // painted SurfaceMask flipbook (see world.ps)
Texture2D    g_DetailNrm;    // high-frequency detail normal (shares g_Ov0Nrm_sampler)

// The material UV transform, shared by every projection plane (identical to world.ps).
float2 ApplyUVT(float2 uv)
{
    float2 tl = (abs(g_UVT.x) + abs(g_UVT.y) < 1e-6) ? float2(1.0, 1.0) : g_UVT.xy;
    uv = uv * tl + g_UVT.zw;
    if (abs(g_UVT2.x) > 1e-6)
    {
        float sr, cr; sincos(g_UVT2.x, sr, cr);
        uv = float2(uv.x * cr - uv.y * sr, uv.x * sr + uv.y * cr);
    }
    return uv;
}

// Painted-mask channel at a world position — identical to world.ps's OvMask3D.
float OvMask3D(float3 wpos, float chan)
{
    float4 hp = float4(wpos, 1.0);
    float3 c = float3(dot(g_OvM0, hp), dot(g_OvM1, hp), dot(g_OvM2, hp));
    if (any(c < 0.0) || any(c > 1.0)) return 0.0;
    float res  = g_OvMQ.x;
    float3 cell = c * res;
    float sx = clamp(cell.x, 0.5, res - 0.5);
    float sv = clamp(cell.y, 0.5, res - 0.5) / res;
    float fz = clamp(cell.z - 0.5, 0.0, res - 1.001);
    float z0 = floor(fz), tz = fz - z0;
    float u0 = (z0 * res + sx) / (res * res);
    float u1 = (min(z0 + 1.0, res - 1.0) * res + sx) / (res * res);
    float4 a = g_Mask3D.SampleLevel(g_Ov0Nrm_sampler, float2(u0, sv), 0);
    float4 b = g_Mask3D.SampleLevel(g_Ov0Nrm_sampler, float2(u1, sv), 0);
    float4 s = lerp(a, b, tz);
    return (chan < 0.5) ? s.r : (chan < 1.5) ? s.g : (chan < 2.5) ? s.b : s.a;
}

// Blend weight of one overlay slot — identical to world.ps's OvWeight.
float OvWeight(float4 ov, float4 ovp, uint flags, float mask2d, float3 wpos, float3 ng)
{
    float v = ov.x;
    if (g_OvMQ.y > 0.5 && ovp.z >= 0.0) v = max(v, OvMask3D(wpos, ovp.z));
    if (flags & 8u) v *= mask2d;
    float w = smoothstep(ov.y, ov.y + max(ov.z, 1e-3), v);
    if (ov.w > 0.0) { float up = saturate(ng.y); w *= lerp(1.0, up * up, ov.w); }
    return w;
}

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
    // Triplanar: the dominant plane's uv, exactly as world.ps derives it (SSR normals in step).
    [branch] if ((uint)(g_Var.w + 0.5) & 1u)
    {
        float3 an = abs(normalize(i.nrm));
        i.uv = ApplyUVT((an.x >= an.y && an.x >= an.z) ? i.wpos.zy : (an.y >= an.z ? i.wpos.xz : i.wpos.xy));
    }
    else
        // LiveMaterial UV transform (identical to world.ps so all passes sample in step).
        i.uv = ApplyUVT(i.uv);
    // Cutout + luma-wipe holes must not leave SSR normals or TAA velocities behind.
    if (g_UVT2.z > 0.0)
        clip(g_WipeMask.Sample(g_WipeMask_sampler, i.uv).r - g_UVT2.z);
    if (g_UVT2.y > 0.0)
    {
        float a = g_Color.a;
        if (g_Params.x > 0.5) a *= g_Tex.Sample(g_Tex_sampler, i.uv).a;
        clip(a - g_UVT2.y);
    }
    // Overlay slot weights — identical math to world.ps so SSR normals/roughness stay in step.
    float3 ovNg = normalize(i.nrm);
    uint  ovF[8];
    float ovW[8];
#define OVG_WEIGHT(N) \
    ovF[N] = (uint)(g_OvP##N.w + 0.5); ovW[N] = 0.0; \
    [branch] if (g_Ov##N.x > 0.0 || g_OvP##N.z >= 0.0) \
        ovW[N] = OvWeight(g_Ov##N, g_OvP##N, ovF[N], (ovF[N] & 8u) ? g_Ov##N##Mask.Sample(g_Ov0Nrm_sampler, i.uv).r : 1.0, i.wpos, ovNg);
    OVG_WEIGHT(0) OVG_WEIGHT(1) OVG_WEIGHT(2) OVG_WEIGHT(3)
    OVG_WEIGHT(4) OVG_WEIGHT(5) OVG_WEIGHT(6) OVG_WEIGHT(7)

    float metallic = saturate(g_Params.z);
    float rough    = clamp(g_Params.w, 0.04, 1.0);
    if (g_Params2.x > 0.5)
    {
        float3 m = g_MetalRough.Sample(g_MetalRough_sampler, i.uv).rgb;
        rough = clamp(m.g, 0.04, 1.0); metallic = saturate(m.b);
    }
#define OVG_MR(N) \
    [branch] if (ovW[N] > 0.001) \
    { \
        if (ovF[N] & 4u) { float3 m = g_Ov##N##MR.Sample(g_Ov0Nrm_sampler, i.uv).rgb; rough = lerp(rough, clamp(m.g, 0.04, 1.0), ovW[N]); metallic = lerp(metallic, saturate(m.b), ovW[N]); } \
        else { if (g_OvP##N.x >= 0.0) metallic = lerp(metallic, saturate(g_OvP##N.x), ovW[N]); \
               if (g_OvP##N.y >= 0.0) rough    = lerp(rough, clamp(g_OvP##N.y, 0.04, 1.0), ovW[N]); } \
    }
    OVG_MR(0) OVG_MR(1) OVG_MR(2) OVG_MR(3) OVG_MR(4) OVG_MR(5) OVG_MR(6) OVG_MR(7)

    float3 N = normalize(i.nrm);
    // g_Params.y: 0 = none, >0 = OpenGL green (flip), <0 = DirectX. Must match world.ps.
    {
        float3 nTS = float3(0.0, 0.0, 1.0);
        bool anyN = false;
        if (abs(g_Params.y) > 0.5)
        {
            float2 nxy = g_Normal.Sample(g_Normal_sampler, i.uv).rg * 2.0 - 1.0;
            if (g_Params.y > 0.0) nxy.y = -nxy.y;
            nTS = float3(nxy, sqrt(saturate(1.0 - dot(nxy, nxy))));
            anyN = true;
        }
        // Detail normal (identical to world.ps).
        [branch] if (g_Det.y > 0.0 && ((uint)(g_Det.z + 0.5) & 2u))
        {
            float2 dxy = g_DetailNrm.Sample(g_Ov0Nrm_sampler, i.uv * g_Det.x).rg * 2.0 - 1.0;
            if ((uint)(g_Det.z + 0.5) & 4u) dxy.y = -dxy.y;
            nTS = normalize(float3(nTS.xy + dxy * g_Det.y, nTS.z)); anyN = true;
        }
#define OVG_NRM(N) \
        [branch] if (ovW[N] > 0.001 && (ovF[N] & 2u)) \
        { \
            float2 oxy = g_Ov##N##Nrm.Sample(g_Ov0Nrm_sampler, i.uv).rg * 2.0 - 1.0; \
            if (ovF[N] & 16u) oxy.y = -oxy.y; \
            nTS = lerp(nTS, float3(oxy, sqrt(saturate(1.0 - dot(oxy, oxy)))), ovW[N]); anyN = true; \
        }
        OVG_NRM(0) OVG_NRM(1) OVG_NRM(2) OVG_NRM(3) OVG_NRM(4) OVG_NRM(5) OVG_NRM(6) OVG_NRM(7)
        if (anyN)
            N = PerturbNormal(N, normalize(nTS), ddx(i.wpos), ddy(i.wpos), ddx(i.uv), ddy(i.uv));
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
