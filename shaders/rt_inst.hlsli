// BYTE-MIRROR of the renderer's RTInstanceData (112 bytes) for the raster RayQuery consumers
// (world.ps shadows, ao.ps RT-AO): only the particle fields are read, the rest is typed loosely
// to keep the stride. Change with NukeDiligentImpl.h + rt_common.hlsl.
#ifndef RT_INST_HLSLI
#define RT_INST_HLSLI
struct RTInstInfo
{
    uint4  offs;          // nrmOffset, uvOffset, posOffset, matByteOffset
    uint4  texA;          // texIndex, nrmTexIndex, mrTexIndex, aoTexIndex
    uint4  texB;          // emTexIndex, specTexIndex, specularFactor(asfloat), nrmFlipG
    float4 albedoMetal;
    float4 emissiveRough;
    uint   colOffset; uint shadowShape; float shadowAlpha; uint dynPosOffset;   // dynPosOffset: g_DynPos byte offset of a sprite mesh (0xFFFFFFFF = none)
    uint   maskTexIndex; uint pad1, pad2, pad3;
};
StructuredBuffer<RTInstInfo> g_RTInst;
#endif
