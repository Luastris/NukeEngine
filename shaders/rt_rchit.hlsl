#include "rt_common.hlsl"

// Default closest hit: metallic-roughness PBR for materials without their own "<name>.surf.hlsl",
// recursing through TraceRay for the specular term.
[shader("closesthit")]
void main(inout RTPayload p, in BuiltInTriangleIntersectionAttributes attr)
{
    uint instId = InstanceID();
    RTInstanceData inst = g_Instances[instId];
    uint   prim = PrimitiveIndex();
    float2 bc   = attr.barycentrics;
    float3 wdir = WorldRayDirection();

    float2 uv     = FetchUV(inst.uvOffset, prim, bc);
    float3 geomN  = FetchWorldNormal(inst.nrmOffset, prim, bc, ObjectToWorld3x4());
    float3 hitN   = ApplyNormalMap(inst, prim, uv, geomN, ObjectToWorld3x4());
    if (dot(hitN, wdir) > 0.0) hitN = -hitN;
    float3 hitPos = WorldRayOrigin() + wdir * RayTCurrent();
    float3 albedo = pow(max(SampleAlbedo(inst, uv), 0.0), 2.2);   // sRGB -> linear
    float3 V      = -wdir;
    float  metal  = inst.albedoMetal.w, rough = inst.emissiveRough.w;
    SampleMR(inst, uv, metal, rough);
    float  ao     = SampleAO(inst, uv);
    float3 spec   = SampleSpec(inst, uv);
    float4 dc     = FetchDynColor(inst, prim, bc);
    albedo       *= dc.rgb;
    float3 emiss  = inst.emissiveRough.rgb * SampleEmissiveMap(inst, uv) * dc.rgb * dc.a;

    float3 col = ShadeSurface(hitPos, hitN, V, albedo, metal, rough, emiss, ao, spec);

    // Specular: recurse into the scene for sharp reflections, blurred env for rough ones.
    float3 R = reflect(wdir, hitN);
    float3 env = ReflEnv(R, rough), traced = env;
    if (p.depth < (uint)g_RTParams.z)
    {
        RayDesc ray; ray.Origin = hitPos + hitN * 0.08 + R * 0.05; ray.Direction = R; ray.TMin = 0.02;
        ray.TMax = (g_RTParams.y > 0.5) ? g_RTParams.y : 1000.0;
        RTPayload p2; p2.color = 0.0; p2.depth = p.depth + 1;
        TraceRay(g_TLAS, RAY_FLAG_NONE, RT_REFLECT_MASK, 0, 1, 0, ray, p2);   // only reflection-visible instances
        traced = p2.color;
    }
    col += SpecFr(hitN, V, rough, albedo, metal, spec) * lerp(traced, env, rough);
    // Attenuate by any water this segment crossed, and show the water surface itself in its place
    // (it is not in the TLAS). Each recursion level handles its own segment.
    float3 wT = RTWaterTrans3(WorldRayOrigin(), hitPos);
    p.color = col * wT + RTWaterLook(wdir) * (1.0 - dot(wT, float3(0.299, 0.587, 0.114)));
}
