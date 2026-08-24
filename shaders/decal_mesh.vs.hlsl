// Target-filtered decal VS: re-draw the TARGET mesh itself — only its surface can catch the
// projection. g_WVP = the MESH's world*view*proj; g_MeshWorld carries the mesh world matrix
// (the slot screen-space decals use for the inverse view-proj — unused on this path).
cbuffer DecalCB
{
    float4x4 g_WVP;
    float4x4 g_InvWorld;     // world -> decal-box local ([-0.5,0.5]^3)
    float4x4 g_MeshWorld;    // mesh local -> world (wpos for the PS projection)
    float4   g_Tint;
    float4   g_Params;
    float4   g_ProjAxis;
    float4   g_Res;
};
struct VSIn { float3 pos : ATTRIB0; };
struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; };
void main(in VSIn i, out PSIn o)
{
    o.wpos = mul(g_MeshWorld, float4(i.pos, 1.0)).xyz;
    o.pos  = mul(g_WVP, float4(i.pos, 1.0));
    // Sit just on top of the surface this pass re-draws (same geometry -> equal depth).
    o.pos.z -= 2e-5 * o.pos.w;
}
