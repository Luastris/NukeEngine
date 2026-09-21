// Ground trails - footprint stamps: one quad per imprint (SV_InstanceID -> g_TSImp) in map space.
cbuffer TrailSimCB
{
    float4 g_TS0;
    float4 g_TS1;   // origin x, origin z, 1/size, texels per side
    float4 g_TSImp[256];   // x, z, radius, weight
};
struct VSOut { float4 pos : SV_POSITION; float2 l : TEXCOORD0; float w : TEXCOORD1; };
void main(uint vid : SV_VertexID, uint iid : SV_InstanceID, out VSOut o)
{
    static const float2 kq[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(-1, 1), float2(1, -1), float2(1, 1) };
    float4 im = g_TSImp[iid];
    float2 q = kq[vid];
    float2 uv = (im.xy + q * im.z - g_TS1.xy) * g_TS1.z;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.l = q; o.w = im.w;
}
