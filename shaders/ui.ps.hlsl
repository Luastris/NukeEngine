// Generic textured 2D pass — pixel shader. SRGBA_TO_LINEAR is injected as a macro by the host.
struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; float2 uv : TEXCOORD; };
Texture2D    Texture;
SamplerState Texture_sampler;
float4 main(in PSInput PSIn) : SV_Target
{
    float4 col = Texture.Sample(Texture_sampler, PSIn.uv) * PSIn.col;
    col.rgb *= col.a;            // premultiplied alpha
    SRGBA_TO_LINEAR(col)
    return col;
}
