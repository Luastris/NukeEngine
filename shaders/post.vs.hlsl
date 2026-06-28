// Post-process — fullscreen triangle (no vertex buffer). uv covers the screen; clip-space y flipped
// so uv (0,0) = top-left (matches the scene render targets).
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
void main(uint id : SV_VertexID, out PSIn o)
{
    float2 uv = float2((id << 1) & 2, id & 2);   // (0,0) (2,0) (0,2)
    o.uv  = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
