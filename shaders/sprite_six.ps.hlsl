// Six-way lit sprite (hero smoke): two lightmaps hold the puff lit from six directions in the
// billboard's frame — A.rgb = from +right, -right, +up; B.rgb = from -up, +front (toward the
// eye), -front; A.a = opacity. Every scene light picks the maps by its direction, the ambient
// takes their mean; then the froxel fog treatment of sprite.ps (grid light + own-column fog).
// The batch frame (right, up, toward the eye) comes from the first quad (SpriteLitCB).
#include "vol.hlsli"
Texture2D    g_Sprite;   SamplerState g_Sprite_sampler;    // lightmap A
Texture2D    g_SpriteB;  SamplerState g_SpriteB_sampler;   // lightmap B
Texture2D    g_Mask;     SamplerState g_Mask_sampler;      // alpha mask (setSpriteMask), white = none
Texture2D<float> g_SceneDepth;
Texture3D<float4> g_VolInteg; SamplerState g_VolInteg_sampler;
Texture3D<float4> g_VolLight; SamplerState g_VolLight_sampler;
cbuffer SpriteCB    { float4x4 g_VP; float4 g_Soft; float4 g_Soft2; };
cbuffer SpriteLitCB { float4 g_T; float4 g_B; float4 g_N; };   // right, up, toward the eye
#define MAX_LIGHTS 256
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
cbuffer FrameCB { float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS]; };

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 wpos : TEXCOORD1; };
float LinD(float z) { return (g_Soft.z * g_Soft.y) / max(g_Soft.z - z * (g_Soft.z - g_Soft.y), 1e-6); }

float3 SixWay(float3 A, float3 B, float3 L)   // L = direction toward the light, world
{
    float3 l = float3(dot(L, g_T.xyz), dot(L, g_B.xyz), dot(L, g_N.xyz));
    float3 p = max(l, 0.0), n = max(-l, 0.0);
    return A.r * p.x + A.g * n.x + A.b * p.y + B.r * n.y + B.g * p.z + B.b * n.z;
}

float4 main(in PSIn i) : SV_TARGET
{
    float4 A = g_Sprite.Sample(g_Sprite_sampler, i.uv);
    float3 B = g_SpriteB.Sample(g_SpriteB_sampler, i.uv).rgb;
    float3 lit = g_Ambient.rgb * g_Ambient.a * ((A.r + A.g + A.b + B.r + B.g + B.b) / 6.0);
    int cnt = (int)g_LightCount.x;
    [loop] for (int li = 0; li < cnt; ++li)
    {
        Light lt = g_Lights[li]; float type = lt.posType.w;
        float3 L; float atten = 1.0;
        if (type < 0.5) L = normalize(-lt.dirRange.xyz);
        else
        {
            float3 d = lt.posType.xyz - i.wpos; float dist = length(d); L = d / max(dist, 1e-4);
            float rng = max(lt.dirRange.w, 1e-4); float win = saturate(1.0 - pow(dist / rng, 4.0));
            atten = (win * win) / (dist * dist + 1.0);
            if (type > 1.5) { float cd = dot(normalize(-lt.dirRange.xyz), -L); float s = saturate((cd - lt.spot.y) / max(lt.spot.x - lt.spot.y, 1e-4)); atten *= s * s; }
        }
        if (atten <= 0.0) continue;
        lit += lt.colorIntensity.rgb * lt.colorIntensity.w * atten * SixWay(A.rgb, B, L);
    }
    float4 c = float4(lit * i.col.rgb, A.a * i.col.a);
    float zs = g_SceneDepth.Load(int3((int2)i.pos.xy, 0));
    if (g_Soft.w > 0.5) c *= saturate((LinD(zs) - LinD(i.pos.z)) / max(g_Soft.x, 1e-4));
    if (g_Soft2.x > 0.5)
    {
        float2 uv = i.pos.xy / g_VolScreen.xy;
        float  zp = VolLinearZ(i.pos.z);
        if (g_Soft2.y > 0.0)
        {
            // the grid's light already contains every light: blend toward it as the local-light multiplier
            float3 lg = g_VolLight.SampleLevel(g_VolLight_sampler, float3(uv, VolW(zp)), 0).rgb;
            c.rgb = lerp(c.rgb, (A.r + A.g + A.b + B.r + B.g + B.b) / 6.0 * lg * i.col.rgb, g_Soft2.y);
        }
        float zo = (zs >= 0.99999) ? g_VolRange.y : VolLinearZ(zs);
        c.rgb = VolFogTranslucent(g_VolInteg, g_VolInteg_sampler, c.rgb, uv, zp, zo);
    }
    c.a *= g_Mask.Sample(g_Mask_sampler, i.uv).a;
    return c;
}
