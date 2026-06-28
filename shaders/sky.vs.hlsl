// Procedural sky — fullscreen triangle (no vertex buffer). Outputs the pixel's NDC for ray reconstruction.
struct PSIn { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
void main(in uint vid : SV_VertexID, out PSIn o)
{
    float2 p = float2((vid << 1) & 2, vid & 2);   // (0,0) (2,0) (0,2)
    float2 c = p * 2.0 - 1.0;                      // (-1,-1) (3,-1) (-1,3) — covers the screen
    o.ndc = c;
    o.pos = float4(c, 0.0, 1.0);
}
