#include "rt_common.hlsl"

// Any-hit for sprites: the quad's colour x fade x texture alpha goes into the payload's layer
// list (premultiplied; additive sprites cover nothing) and the ray goes on - the caller
// composites the layers over whatever the ray finally hits (RTCompose). Shadow rays keep
// their alpha test (RTShadow, RayQuery).
[shader("anyhit")]
void main(inout RTPayload p, in SpriteAttr attr)
{
    RTInstanceData inst = g_Instances[InstanceID()];
    float4 dc = FetchSpriteColor(inst, PrimitiveIndex(), attr.along);
    float  a  = dc.a * SampleAlphaMask(inst, attr.uv);
    if (a > 0.004)
    {
        float3 c = inst.emissiveRough.rgb * SampleEmissiveMap(inst, attr.uv) * dc.rgb;
        if (g_SkyParams.z > 0.5) c = RTDisplayToLinear(c);   // the raster writes this colour raw: LDR = displayed value
        const bool additive = inst.pad1 != 0u;
        RTPushSprite(p, c * a, additive ? 0.0 : a, RayTCurrent());
    }
    IgnoreHit();
}
