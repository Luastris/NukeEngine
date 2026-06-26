// Generic textured 2D pass (ImGui-style draw lists).
cbuffer Constants { float4x4 ProjectionMatrix; }
struct VSInput { float2 pos : ATTRIB0; float2 uv : ATTRIB1; float4 col : ATTRIB2; };
struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
void main(in VSInput VSIn, out PSInput PSIn)
{
    PSIn.pos = mul(ProjectionMatrix, float4(VSIn.pos.xy, 0.0, 1.0));
    PSIn.col = VSIn.col;
    PSIn.uv  = VSIn.uv;
}
