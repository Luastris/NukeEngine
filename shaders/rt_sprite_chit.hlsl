#include "rt_common.hlsl"
#include "rt_water_shade.hlsli"

// Closest hit for sprites: lit like the triangle path with the quad facing the ray. No specular
// recursion: particles are matte (black albedo, the colour rides the emissive).
[shader("closesthit")]
void main(inout RTPayload p, in SpriteAttr attr)
{
    p.hitT = RayTCurrent();
    RTInstanceData inst = g_Instances[InstanceID()];
    float3 wdir   = WorldRayDirection();
    float3 hitN   = -wdir;
    float3 hitPos = WorldRayOrigin() + wdir * RayTCurrent();
    float2 uv     = attr.uv;
    float3 albedo = pow(max(SampleAlbedo(inst, uv), 0.0), 2.2);   // sRGB -> linear
    float  metal  = inst.albedoMetal.w, rough = inst.emissiveRough.w;
    SampleMR(inst, uv, metal, rough);
    float  ao     = SampleAO(inst, uv);
    float3 spec   = SampleSpec(inst, uv);
    float4 dc     = FetchSpriteColor(inst, PrimitiveIndex(), attr.along);
    albedo       *= dc.rgb;
    float3 emiss  = inst.emissiveRough.rgb * SampleEmissiveMap(inst, uv) * dc.rgb * dc.a;
    float3 col    = ShadeSurface(hitPos, hitN, -wdir, albedo, metal, rough, emiss, ao, spec);
    p.color = RTWaterFinish(WorldRayOrigin(), wdir, hitPos, col, p.depth, p.flags);
}
