// Selection outline pixel shader: flat editor-highlight color.
struct PSIn { float4 pos : SV_POSITION; };
float4 main(in PSIn i) : SV_Target
{
    return float4(1.0, 0.6, 0.1, 1.0);
}
