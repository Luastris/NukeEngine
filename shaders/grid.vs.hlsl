// Editor infinite-grid VS: one ground-plane quad (y=0) centered on the camera, built from
// SV_VertexID — no vertex buffer. The PS draws the grid analytically and occludes itself.
cbuffer GridCB { float4x4 g_VP; float4x4 g_InvVP; float4 g_CamStep; float4 g_GridFade; float4 g_Res; };

static const float2 kCorners[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1),
    float2(-1, -1), float2(1, 1),  float2(-1, 1)
};

struct PSIn { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; };

void main(uint vid : SV_VertexID, out PSIn o)
{
    // Quad radius: just past the fade reach — a huge quad ruins interpolated depth precision.
    const float R = g_GridFade.x * 2.0;
    float2 c = kCorners[vid];
    o.wpos = float3(g_CamStep.x + c.x * R, 0.0, g_CamStep.z + c.y * R);
    o.pos  = mul(g_VP, float4(o.wpos, 1.0));
}
