#include "rt_common.hlsl"

// Ray generation: per pixel, find the primary reflector (G-buffer + camera-ray re-find), spawn ONE reflection
// ray through the RT pipeline, and composite the returned radiance onto the base colour. Recursion (mirror in
// mirror) happens natively inside the closest-hit shader via TraceRay.
[shader("raygeneration")]
void main()
{
    uint2 px  = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float3 base = g_Source.Load(int3(px, 0)).rgb;
    float  depth = g_Depth.Load(int3(px, 0)).r;
    if (depth >= 0.99999) { g_Output[px] = float4(base, 1.0); return; }   // sky pixel

    float4 gb = g_GBuffer.Load(int3(px, 0));
    float  rough = gb.z, metal = gb.w;
    float3 N = OctDecode(gb.xy);

    float2 uv = (float2(px) + 0.5) / float2(dim);
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 vp4 = mul(g_InvProj, clip); float3 vpos = vp4.xyz / vp4.w;     // clip -> view
    float3 wpos = mul(g_InvView, float4(vpos, 1.0)).xyz;                  // view -> world

    float3 V = normalize(wpos - g_RTCam.xyz);
    if (dot(N, V) > 0.0) N = -N;
    float  NoV = saturate(dot(N, -V));
    float  F0  = lerp(0.04, 1.0, metal);
    float  roughFade = 1.0 - smoothstep(g_RTParams.w * 0.4, g_RTParams.w, rough);   // fade out toward the configured roughness cutoff
    float  refl = (F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0)) * (1.0 - rough) * roughFade;
    float  intensity = g_RTParams.x;
    if (refl < 0.01 || intensity <= 0.0) { g_Output[px] = float4(base, 1.0); return; }

    // Exact primary surface via camera-ray trace (gbuffer depth drifts on curved surfaces -> origin inside object).
    {
        RayDesc cray; cray.Origin = g_RTCam.xyz; cray.Direction = V; cray.TMin = 0.0; cray.TMax = 1.0e5;
        RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> cq;
        cq.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, cray); cq.Proceed();
        if (cq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
        {
            wpos = g_RTCam.xyz + V * cq.CommittedRayT();
            uint ci = cq.CommittedInstanceID();
            // NOTE: primary reflector normal is geometric only. Normal-mapping the reflector here would sample the
            // bindless g_MatTex from the ray-gen stage, where it is NOT bound (bindless array is a closest-hit
            // resource) -> device removal. The reflected surfaces still get full normal mapping in the hit shaders.
            N = FetchWorldNormal(g_Instances[ci].nrmOffset, cq.CommittedPrimitiveIndex(),
                                 cq.CommittedTriangleBarycentrics(), cq.CommittedObjectToWorld3x4());
            if (dot(N, V) > 0.0) N = -N;
        }
    }

    float  maxD = (g_RTParams.y > 0.5) ? g_RTParams.y : 1000.0;
    float3 R = reflect(V, N);
    RayDesc ray; ray.Origin = wpos + N * 0.08 + R * 0.05; ray.Direction = R; ray.TMin = 0.02; ray.TMax = maxD;
    RTPayload p; p.color = 0.0; p.depth = 1;
    TraceRay(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, 0, 1, 0, ray, p);   // only reflection-visible instances

    float k = saturate(refl * intensity);
    g_Output[px] = float4(lerp(base, p.color, k), 1.0);
}
