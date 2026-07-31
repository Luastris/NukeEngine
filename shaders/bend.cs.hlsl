// Bends the merged per-chunk RT foliage meshes into the BLAS vertex buffer using the same
// NukeBend the raster passes use. Dispatched once per bend-mesh per frame, then the BLAS is refit.
#include "nukebend.hlsl"

cbuffer BendCSParams
{
    // std140: the float3 must lead so it sits at a 16-byte boundary and the uint packs into the tail.
    // Mirrors the MapHelper write in NukeDiligent_RT.cpp BendRTMeshes.
    float3 g_AtomOffset;   // chunk verts are atom-local; wind waves key on world position
    uint   g_VertCount;
};
StructuredBuffer<float>    g_SrcPos;    // static baked positions, 3 floats per vertex
StructuredBuffer<float4>   g_BendData;  // (source-mesh local Y, custom.z, custom.w, 0)
StructuredBuffer<float4>   g_BendPivot; // (instance origin in atom space, 0), hashed for the sway phase
RWStructuredBuffer<float>  g_DstPos;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint v = id.x;
    if (v >= g_VertCount) return;
    float3 p  = float3(g_SrcPos[v * 3], g_SrcPos[v * 3 + 1], g_SrcPos[v * 3 + 2]);
    float4 bd = g_BendData[v];
    float3 pv = g_BendPivot[v].xyz + g_AtomOffset;
    // NukeBend expects (world pos, blade-local pos [y drives the gate], custom [z/w gates], pivot).
    float3 bent = NukeBend(p + g_AtomOffset, float3(0.0, bd.x, 0.0), float4(0.0, 0.0, bd.y, bd.z), pv)
                - g_AtomOffset;
    g_DstPos[v * 3]     = bent.x;
    g_DstPos[v * 3 + 1] = bent.y;
    g_DstPos[v * 3 + 2] = bent.z;
}
