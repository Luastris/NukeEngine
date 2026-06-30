// Ray-traced reflections — built-in post effect (renderer special-cases it: isRTRef). D3D12 + DXR only; DXC SM6.5.
// Per pixel: reflect the view ray off the G-buffer surface, ray-query the scene TLAS, and SHADE the hit from real
// geometry (interpolated normal/uv + bindless material) under ALL scene lights (FrameCB) with an RT shadow ray
// each. Miss / off-screen -> reflection probe or sky. Layers onto the base colour (lerp, faded by roughness).
Texture2D    g_Source;   SamplerState g_Source_sampler;    // current HDR chain colour
Texture2D    g_GBuffer;  SamplerState g_GBuffer_sampler;   // (octN.xy world normal, roughness, metalness)
Texture2D    g_Depth;    SamplerState g_Depth_sampler;     // device depth
TextureCube  g_Probe;    SamplerState g_Probe_sampler;     // probe cubemap (miss / ambient fallback)

RaytracingAccelerationStructure g_TLAS;
ByteAddressBuffer               g_AllNrm;       // concatenated mesh normals (float3 per vertex)
ByteAddressBuffer               g_AllUV;        // concatenated mesh uvs (float2 per vertex)
Texture2D                       g_MatTex[64];   // bindless material albedo textures
SamplerState                    g_MatTex_sampler;
struct RTInstanceData { uint nrmOffset; uint uvOffset; uint texIndex; uint pad; float4 albedoMetal; float4 emissiveRough; };
StructuredBuffer<RTInstanceData> g_Instances;

cbuffer PostParams { float g_Intensity = 1.0; float g_MaxDist = 100.0; };
cbuffer RTRefCB { float4x4 g_InvProj; float4x4 g_InvView; };   // clip->view, view->world (two-step = numerically stable)

// FrameCB — identical layout to world.ps (bound to the same worldFrameCB): all scene lights + ambient + sky + probe.
#define MAX_LIGHTS 16
#define MAX_SHADOWS 4
struct Light { float4 posType; float4 dirRange; float4 colorIntensity; float4 spot; };
cbuffer FrameCB
{
    float4 g_CamPos; float4 g_Ambient; float4 g_LightCount; Light g_Lights[MAX_LIGHTS];
    float4x4 g_ShadowVP[MAX_SHADOWS]; float4 g_ShadowParams;
    float4 g_SkyTop; float4 g_SkyHorizon; float4 g_SkyGround; float4 g_SkyParams;
    float4 g_ProbePos; float4 g_ProbeParams; float4 g_ProbeBox;
};

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 OctDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float3 SkyColor(float3 d)
{
    float up = d.y;
    float3 c = (up >= 0.0) ? lerp(g_SkyHorizon.rgb, g_SkyTop.rgb, pow(saturate(up), 0.5))
                           : lerp(g_SkyHorizon.rgb, g_SkyGround.rgb, saturate(-up));
    return c * g_SkyParams.x;
}
float3 EnvFallback(float3 dir, float rough)
{
    if (g_ProbePos.w > 0.5) return g_Probe.SampleLevel(g_Probe_sampler, dir, saturate(rough) * g_ProbeParams.y).rgb * g_ProbeParams.x;
    return SkyColor(dir);
}
// Shadow ray (1 = lit, 0 = occluded) from a hit point toward a light, up to maxD.
float RTShadowRay(float3 origin, float3 L, float maxD)
{
    RayDesc r; r.Origin = origin; r.Direction = L; r.TMin = 0.02; r.TMax = maxD;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}

float4 main(in PSIn i) : SV_Target
{
    float3 base = g_Source.Sample(g_Source_sampler, i.uv).rgb;
    float depth = g_Depth.Sample(g_Depth_sampler, i.uv).r;
    if (depth >= 0.99999) return float4(base, 1.0);            // sky pixel

    float4 gb = g_GBuffer.Sample(g_GBuffer_sampler, i.uv);
    float rough = gb.z, metal = gb.w;
    float3 N = OctDecode(gb.xy);

    float4 clip = float4(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0, depth, 1.0);
    float4 vp4 = mul(g_InvProj, clip); float3 vpos = vp4.xyz / vp4.w;   // clip -> view
    float3 wpos = mul(g_InvView, float4(vpos, 1.0)).xyz;                // view -> world

    float3 V = normalize(wpos - g_CamPos.xyz);                 // eye -> pixel
    if (dot(N, V) > 0.0) N = -N;                               // keep the reflector normal facing the camera (no x-ray)
    float  NoV = saturate(dot(N, -V));
    float  F0  = lerp(0.04, 1.0, metal);
    // RT reflections are sharp (1 ray) -> valid only on smooth surfaces; fade out with roughness.
    float  roughFade = 1.0 - smoothstep(0.25, 0.6, rough);
    float  refl = (F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0)) * (1.0 - rough) * roughFade;
    if (refl < 0.01 || g_Intensity <= 0.0) return float4(base, 1.0);

    // Re-find the EXACT primary surface by casting the camera ray into the TLAS (the same path that works for
    // surfaces seen in the mirror). The gbuffer depth reconstruction drifts on curved surfaces and puts the
    // origin INSIDE the sphere -> self-intersection. The view direction V is reliable, so trace along it.
    {
        RayDesc cray; cray.Origin = g_CamPos.xyz; cray.Direction = V; cray.TMin = 0.0; cray.TMax = 1.0e5;
        RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> cq;
        cq.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, cray); cq.Proceed();
        if (cq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            wpos = g_CamPos.xyz + V * cq.CommittedRayT();
            RTInstanceData ci = g_Instances[cq.CommittedInstanceID()];
            uint cprim = cq.CommittedPrimitiveIndex(); float2 cbc = cq.CommittedTriangleBarycentrics();
            uint cnb = ci.nrmOffset + cprim * 36u;
            float3 cN = asfloat(g_AllNrm.Load3(cnb)) * (1.0 - cbc.x - cbc.y) + asfloat(g_AllNrm.Load3(cnb + 12u)) * cbc.x + asfloat(g_AllNrm.Load3(cnb + 24u)) * cbc.y;
            N = normalize(mul((float3x3)cq.CommittedObjectToWorld3x4(), cN));
            if (dot(N, V) > 0.0) N = -N;
        }
    }

    const float PI = 3.14159265;
    const int   MAX_BOUNCES = 4;                              // mirror-in-mirror: trace reflections of reflections
    float  maxD = (g_MaxDist > 0.5) ? g_MaxDist : 1000.0;

    float3 accum = 0.0;          // accumulated reflected radiance
    float3 thru  = 1.0;          // remaining reflective throughput
    float3 rayD  = reflect(V, N);
    // Bias off the surface BOTH along the normal and along the ray — on a curved (sphere) surface a grazing
    // reflection runs nearly tangent and would otherwise re-intersect the surface itself (black rim).
    float3 rayO  = wpos + N * 0.08 + rayD * 0.05;

    [loop] for (int bounce = 0; bounce < MAX_BOUNCES; ++bounce)
    {
        RayDesc ray; ray.Origin = rayO; ray.Direction = rayD; ray.TMin = 0.02; ray.TMax = maxD;
        RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, ray);
        q.Proceed();
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            accum += thru * EnvFallback(rayD, 0.0);           // ray escaped -> sky / probe
            thru = 0.0;                                       // consumed -> no double-add after the loop
            break;
        }

        RTInstanceData inst = g_Instances[q.CommittedInstanceID()];
        uint prim = q.CommittedPrimitiveIndex();
        float2 bc = q.CommittedTriangleBarycentrics();
        float w0 = 1.0 - bc.x - bc.y;
        uint nb = inst.nrmOffset + prim * 36u;
        float3 objN = asfloat(g_AllNrm.Load3(nb)) * w0 + asfloat(g_AllNrm.Load3(nb + 12u)) * bc.x + asfloat(g_AllNrm.Load3(nb + 24u)) * bc.y;
        float3 hitN = normalize(mul((float3x3)q.CommittedObjectToWorld3x4(), objN));
        if (dot(hitN, rayD) > 0.0) hitN = -hitN;              // face the incoming ray
        float3 hitPos = rayO + rayD * q.CommittedRayT();

        float3 albedo = inst.albedoMetal.rgb;
        float  hitMetal = inst.albedoMetal.w;
        float  hitRough = inst.emissiveRough.w;
        if (inst.texIndex != 0xFFFFFFFFu)
        {
            uint ub = inst.uvOffset + prim * 24u;
            float2 uv = asfloat(g_AllUV.Load2(ub)) * w0 + asfloat(g_AllUV.Load2(ub + 8u)) * bc.x + asfloat(g_AllUV.Load2(ub + 16u)) * bc.y;
            albedo *= g_MatTex[NonUniformResourceIndex(inst.texIndex)].SampleLevel(g_MatTex_sampler, uv, 0).rgb;
        }
        if (inst.pad == 1u) { accum += thru * (albedo + inst.emissiveRough.rgb); break; }   // unlit

        // Local shading: all scene lights (diffuse, kd/PI) + RT shadow each, + diffuse IBL ambient + emissive.
        float3 kd = albedo * (1.0 - hitMetal);
        float3 Lo = 0.0;
        int cnt = (int)g_LightCount.x;
        [loop] for (int li = 0; li < cnt; ++li)
        {
            Light lt = g_Lights[li]; float type = lt.posType.w;
            float3 L; float atten = 1.0; float lmax = 1e4;
            if (type < 0.5) L = normalize(-lt.dirRange.xyz);
            else
            {
                float3 d = lt.posType.xyz - hitPos; float dist = length(d); L = d / max(dist, 1e-4); lmax = dist;
                float rng = max(lt.dirRange.w, 1e-4); float win = saturate(1.0 - pow(dist / rng, 4.0));
                atten = (win * win) / (dist * dist + 1.0);
                if (type > 1.5) { float cd = dot(normalize(-lt.dirRange.xyz), -L); float s = saturate((cd - lt.spot.y) / max(lt.spot.x - lt.spot.y, 1e-4)); atten *= s * s; }
            }
            float ndl = saturate(dot(hitN, L));
            if (ndl <= 0.0 || atten <= 0.0) continue;
            float sh = RTShadowRay(hitPos + L * 0.05 + hitN * 0.02, L, lmax);
            Lo += (kd / PI) * lt.colorIntensity.rgb * lt.colorIntensity.w * (atten * ndl * sh);
        }
        accum += thru * (Lo + kd * EnvFallback(hitN, 1.0) * g_Ambient.w + inst.emissiveRough.rgb);

        // Reflective surfaces (metal / glossy) continue the path -> reflections of reflections.
        float3 specW = lerp(float3(0.04, 0.04, 0.04), albedo, hitMetal) * (1.0 - hitRough);
        if (max(max(specW.r, specW.g), specW.b) < 0.05) break;   // matte -> stop
        thru  *= specW;
        rayD   = reflect(rayD, hitN);
        rayO   = hitPos + hitN * 0.08 + rayD * 0.05;
    }
    accum += thru * EnvFallback(rayD, 0.0);   // path still reflective at the bounce limit -> sample the environment

    float k = saturate(refl * g_Intensity);
    return float4(lerp(base, accum, k), 1.0);
}
