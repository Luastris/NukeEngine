// World (3D) pass — pixel shader. Metallic-roughness PBR (Cook-Torrance), multiple scene lights
// (directional / point / spot), base-color + normal maps. The "universal" lit shader.
//
// MatCB:  g_Color    = base color (rgba)
//         g_Params   = (hasBaseTex, hasNormalTex, metallic, roughness)
//         g_Params2  = (hasMetalRoughTex, hasOcclusionTex, hasEmissiveTex, occlusionStrength)
//         g_Emissive2 = (emissive rgb, emissive intensity)  (texture g_Emissive is separate)
cbuffer MatCB { float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; };

#define MAX_LIGHTS 16
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
// FrameCB: per-camera lighting. g_CamPos.xyz; g_Ambient = (rgb, intensity); g_LightCount.x = count.
cbuffer FrameCB { float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS]; };

// One shared sampler for every map (g_Tex_sampler). FXC merges identical samplers anyway, and a single
// combined sampler keeps Diligent's combined-texture-sampler reflection happy (no "unassigned" warnings).
SamplerState g_Tex_sampler;
Texture2D    g_Tex;          // base color
Texture2D    g_Normal;       // tangent-space normal map
Texture2D    g_MetalRough;   // G = roughness, B = metallic (glTF)
Texture2D    g_Occlusion;    // R = ambient occlusion
Texture2D    g_Emissive;     // emissive color

struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD2; };

static const float PI = 3.14159265359;

float3 FresnelSchlick(float cosT, float3 F0) { return F0 + (1.0 - F0) * pow(saturate(1.0 - cosT), 5.0); }
float DistributionGGX(float3 N, float3 H, float rough)
{
    float a = rough * rough; float a2 = a * a;
    float ndh = max(dot(N, H), 0.0);
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-5);
}
float GeometrySchlick(float ndv, float k) { return ndv / (ndv * (1.0 - k) + k); }
float GeometrySmith(float3 N, float3 V, float3 L, float rough)
{
    float k = (rough + 1.0); k = k * k / 8.0;
    return GeometrySchlick(max(dot(N, V), 0.0), k) * GeometrySchlick(max(dot(N, L), 0.0), k);
}
// Tangent-space normal mapping WITHOUT mesh tangents — derive a cotangent frame from screen-space derivatives.
float3 PerturbNormal(float3 N, float3 V, float2 uv)
{
    float3 n = g_Normal.Sample(g_Tex_sampler, uv).xyz * 2.0 - 1.0;
    float3 dp1 = ddx(-V), dp2 = ddy(-V);
    float2 du1 = ddx(uv), du2 = ddy(uv);
    float3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    float3 T = dp2p * du1.x + dp1p * du2.x;
    float3 B = dp2p * du1.y + dp1p * du2.y;
    float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-8));
    return normalize(T * (inv * n.x) + B * (inv * n.y) + N * n.z);
}

float4 main(in PSIn i) : SV_Target
{
    float4 base = g_Color;
    if (g_Params.x > 0.5) base *= g_Tex.Sample(g_Tex_sampler, i.uv);
    float3 albedo = pow(max(base.rgb, 0.0), 2.2);   // sRGB -> linear

    float metallic = saturate(g_Params.z);
    float rough    = clamp(g_Params.w, 0.04, 1.0);
    if (g_Params2.x > 0.5)                                  // metallic-roughness map wins (glTF G/B)
    {
        float3 m = g_MetalRough.Sample(g_Tex_sampler, i.uv).rgb;
        rough = clamp(m.g, 0.04, 1.0); metallic = saturate(m.b);
    }

    float3 V = normalize(g_CamPos.xyz - i.wpos);
    float3 N = normalize(i.nrm);
    if (g_Params.y > 0.5) N = PerturbNormal(N, V, i.uv);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 Lo = 0.0;

    int cnt = (int)g_LightCount.x;
    [loop] for (int li = 0; li < cnt; ++li)
    {
        Light lt = g_Lights[li];
        float  type = lt.posType.w;
        float3 L; float atten = 1.0;
        if (type < 0.5)                       // directional
        {
            L = normalize(-lt.dirRange.xyz);
        }
        else                                  // point / spot
        {
            float3 d = lt.posType.xyz - i.wpos;
            float  dist = length(d);
            L = d / max(dist, 1e-4);
            float rng = max(lt.dirRange.w, 1e-4);
            float win = saturate(1.0 - pow(dist / rng, 4.0));
            atten = (win * win) / (dist * dist + 1.0);
            if (type > 1.5)                   // spot cone
            {
                float cd = dot(normalize(-lt.dirRange.xyz), -L);
                float s  = saturate((cd - lt.spot.y) / max(lt.spot.x - lt.spot.y, 1e-4));
                atten *= s * s;
            }
        }
        float ndl = max(dot(N, L), 0.0);
        if (ndl <= 0.0) continue;
        float3 H = normalize(V + L);
        float3 radiance = lt.colorIntensity.rgb * lt.colorIntensity.w * atten;

        float  D = DistributionGGX(N, H, rough);
        float  G = GeometrySmith(N, V, L, rough);
        float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
        float3 spec = (D * G) * F / max(4.0 * max(dot(N, V), 0.0) * ndl, 1e-4);
        float3 kd = (1.0 - F) * (1.0 - metallic);
        Lo += (kd * albedo / PI + spec) * radiance * ndl;
    }

    float ao = (g_Params2.y > 0.5) ? g_Occlusion.Sample(g_Tex_sampler, i.uv).r : 1.0;
    float3 ambient = g_Ambient.rgb * g_Ambient.w * albedo * ao;
    float3 emissive = g_Emissive2.rgb * g_Emissive2.w;
    if (g_Params2.z > 0.5) emissive *= g_Emissive.Sample(g_Tex_sampler, i.uv).rgb;
    float3 color = ambient + Lo + emissive;

    color = color / (color + 1.0);          // Reinhard tonemap
    color = pow(max(color, 0.0), 1.0 / 2.2); // linear -> sRGB
    return float4(color, base.a);
}
