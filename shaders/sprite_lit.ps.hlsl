// Lit textured sprite (tilemap layers with a normal map): diffuse * tint, Lambert-lit by the scene lights.
// The tangent basis is constant per batch, since all tiles of one map share a plane.
Texture2D    g_Sprite;  SamplerState g_Sprite_sampler;
Texture2D    g_Normal;  SamplerState g_Normal_sampler;

// g_T/g_B/g_N = the batch plane's world tangent/bitangent/normal.
// g_N.w = green convention: >0 = OpenGL (flip), <0 = DirectX.
cbuffer SpriteLitCB { float4 g_T; float4 g_B; float4 g_N; };

// FrameCB below declares only the prefix this shader reads; the layout must match world.ps.
#define MAX_LIGHTS 256
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
cbuffer FrameCB { float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS]; };

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 wpos : TEXCOORD1; };

float4 main(in PSIn i) : SV_TARGET
{
    float4 albedo = g_Sprite.Sample(g_Sprite_sampler, i.uv) * i.col;

    float3 nTS = g_Normal.Sample(g_Normal_sampler, i.uv).xyz * 2.0 - 1.0;
    if (g_N.w > 0.0) nTS.y = -nTS.y;   // OpenGL-authored map: flip green
    float3 N = normalize(g_T.xyz * nTS.x + g_B.xyz * nTS.y + g_N.xyz * nTS.z);

    float3 lit = g_Ambient.rgb * g_Ambient.a;
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
        lit += lt.colorIntensity.rgb * lt.colorIntensity.w * atten * ndl;
    }
    return float4(albedo.rgb * lit, albedo.a);
}
