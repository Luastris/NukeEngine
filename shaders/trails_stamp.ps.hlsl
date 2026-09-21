// Ground trails - footprint stamp: a soft disc, MAX-blended into the carve map.
struct VSOut { float4 pos : SV_POSITION; float2 l : TEXCOORD0; float w : TEXCOORD1; };
float main(in VSOut i) : SV_TARGET
{
    return smoothstep(1.0, 0.65, length(i.l)) * i.w;
}
