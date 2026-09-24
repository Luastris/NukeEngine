#include "rt_common.hlsl"

Texture2D g_Cover;   // sprite coverage over the reflector (sprite_cover.ps): that share of the base stays

// Ray generation: per pixel, find the primary reflector, trace one reflection ray and composite the
// returned radiance onto the base colour. Further bounces happen inside the closest-hit shader.
[shader("raygeneration")]
void main()
{
    uint2 px  = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float3 base = g_Source.Load(int3(px, 0)).rgb;
    float  depth = g_Depth.Load(int3(px, 0)).r;
    if (depth >= 0.99999) { g_Output[px] = float4(base, 1.0); return; }   // sky pixel

    float4 gb = g_GBuffer.Load(int3(px, 0));
    const bool isWater = gb.w < -0.5;   // the water's G-pass flags itself with metal -1 (not in the TLAS)
    float  rough = gb.z, metal = max(gb.w, 0.0);
    float3 N = OctDecode(gb.xy);

    float2 uv = (float2(px) + 0.5) / float2(dim);
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 vp4 = mul(g_InvProj, clip); float3 vpos = vp4.xyz / vp4.w;     // clip -> view
    float3 wpos = mul(g_InvView, float4(vpos, 1.0)).xyz;                  // view -> world

    float3 V = normalize(wpos - g_RTCam.xyz);
    if (dot(N, V) > 0.0) N = -N;
    float  NoV = saturate(dot(N, -V));
    float  F0  = lerp(0.04, 1.0, metal);
    float  roughFade = 1.0 - smoothstep(g_RTParams.w * 0.4, g_RTParams.w, rough);   // fade toward the roughness cutoff
    float  refl = (F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0)) * (1.0 - rough) * roughFade;
    float  intensity = g_RTParams.x;
    if (refl < 0.01 || intensity <= 0.0) { g_Output[px] = float4(base, 1.0); return; }

    // Re-find the exact primary surface with a camera ray: G-buffer depth drifts on curved
    // surfaces and can place the reflection origin inside the object. Never for the water: it is
    // not in the TLAS, and where its surface meets a wall the wall behind it sits within the
    // tolerance - the seam then reflected off the wall's normal (a bright torn line).
    if (!isWater)
    {
        // Refine the reflector's point/normal from the TLAS along the primary ray - but only when
        // the hit IS the visible surface (the same distance, within the depth drift). Anything
        // else keeps the G-buffer point: a surface the G-buffer holds that is not in the TLAS
        // (the water's G-pass) sits in front of whatever the ray finds behind it; and a wall the
        // camera has poked through (nearer than the near plane, so the raster never showed it)
        // would otherwise become every pixel's reflector - its inside mirrored in the wall opposite.
        const float gbufT = length(wpos - g_RTCam.xyz);
        RayDesc cray; cray.Origin = g_RTCam.xyz; cray.Direction = V; cray.TMin = 0.0; cray.TMax = 1.0e5;
        RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> cq;
        cq.TraceRayInline(g_TLAS, RAY_FLAG_NONE, 0xFF, cray); cq.Proceed();
        if (cq.CommittedStatus() == COMMITTED_TRIANGLE_HIT && abs(cq.CommittedRayT() - gbufT) <= gbufT * 0.01 + 0.05)
        {
            wpos = g_RTCam.xyz + V * cq.CommittedRayT();
            uint ci = cq.CommittedInstanceID();
            // Geometric normal only: g_MatTex is a closest-hit resource and is NOT bound in ray-gen,
            // so sampling a normal map here causes device removal.
            N = FetchWorldNormal(g_Instances[ci].nrmOffset, cq.CommittedPrimitiveIndex(),
                                 cq.CommittedTriangleBarycentrics(), cq.CommittedObjectToWorld3x4());
            if (dot(N, V) > 0.0) N = -N;
        }
    }

    float  maxD = (g_RTParams.y > 0.5) ? g_RTParams.y : 1000.0;
    float3 R = reflect(V, N);
    // The water: the reflected leg stays above the plane and the escaping ray's sky blurs with
    // distance, both as water.ps does - a leg that dives re-shades the surface at its own foot,
    // a mirror-sharp horizon row under animated wave normals is a field of flickering streaks.
    float  lobeRough = 0.0;
    if (isWater)
    {
        if (R.y < 0.02) R = normalize(float3(R.x, 0.02, R.z));
        const float dist = length(wpos - g_RTCam.xyz);
        lobeRough = clamp(0.05 + (1.0 - exp(-dist / 260.0)) * 0.13, 0.05, 0.3);   // WaterReflRough (nukewater_slope.hlsl)
    }
    RayDesc ray; ray.Origin = wpos + N * 0.08 + R * 0.05; ray.Direction = R; ray.TMin = 0.02; ray.TMax = maxD;
    RTPayload p; p.color = 0.0; p.depth = 1; p.hitT = maxD; p.rough = lobeRough; p.flags = isWater ? RT_PAY_SURFACE : 0u;
    TraceRay(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, 0, 1, 0, ray, p);   // only reflection-visible instances
    {   // the reflected leg through the froxel fog (the camera -> mirror leg comes with the fog
        // composite). Fog volumes lie in the water too (silt, sand): the whole leg, over the
        // water's own fog and never dyed by it.
        float3 amb = (g_SkyParams.y > 0.5) ? (g_SkyTop.rgb + 2.0 * g_SkyHorizon.rgb + g_SkyGround.rgb) * 0.25 * g_SkyParams.x * g_Ambient.w
                                           : g_Ambient.rgb * g_Ambient.w;
        p.color = VolFogSegment(ray.Origin, ray.Origin + R * min(p.hitT, g_VolRange.y), p.color, amb);
    }

    float k = saturate(refl * intensity);
    // Attenuate the reflection by whatever water the camera -> reflector segment crossed
    // (a point on the surface itself lies inside the water's wave band: not "through" it).
    // A submerged camera's leg is the underwater post's (it fogs the composited pixel).
    if (!RTUnderEye(g_RTCam.xyz)) k *= RTWaterTrans(g_RTCam.xyz, wpos);
    // A sprite drawn over the reflector (a puff in front of a mirror, above the water) is part
    // of the base colour, not of the reflector: it keeps its share of the pixel.
    k *= 1.0 - g_Cover.Load(int3(px, 0)).r;
    // The LDR path (g_SkyParams.z): the scene is tonemapped + sRGB; the trace is linear
    // radiance. Mix in linear and re-encode (the same extended Reinhard as world.ps).
    if (g_SkyParams.z > 0.5)
    {
        float W = (g_SkyParams.w > 1e-3) ? g_SkyParams.w : 1.0;
        float3 lin = pow(max(base, 0.0), 2.2);
        float3 y = min(lin, 0.999);
        lin = max(0.5 * W * W * ((y - 1.0) + sqrt((1.0 - y) * (1.0 - y) + 4.0 * y / (W * W))), 0.0);
        float3 mix = lerp(lin, p.color, k);
        mix = mix * (1.0 + mix / (W * W)) / (1.0 + mix);
        g_Output[px] = float4(pow(max(mix, 0.0), 1.0 / 2.2), 1.0);
        return;
    }
    g_Output[px] = float4(lerp(base, p.color, k), 1.0);
}
