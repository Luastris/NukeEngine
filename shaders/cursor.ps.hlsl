// Software cursor PS: straight textured quad, alpha-blended over the finished frame.
Texture2D    g_Tex;
SamplerState g_Tex_sampler;

struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 main(in PSIn i) : SV_Target
{
    return g_Tex.Sample(g_Tex_sampler, i.uv);
}
