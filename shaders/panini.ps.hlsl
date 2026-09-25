// Panini remap: the over-scanned rectilinear LDR frame -> the cylindrical Panini view (paired
// with post.vs). Output pixel -> Panini image point (x, y) -> azimuth/elevation (phi, theta) ->
// the rectilinear source point (tan phi, tan theta / cos phi) -> Catmull-Rom sample. d = 0 is
// plain rectilinear; d = 1 the classic Panini (the projection centre one sphere radius back).
// Camera::ScreenRayDir (engine) inverts the same mapping for screen rays.
Texture2D g_Source; SamplerState g_Source_sampler;
cbuffer PaniniCB
{
    float4 g_P0;   // d, xMax (half-width of the Panini image at the edge angle), tanV (output half-height), s (vertical term)
    float4 g_P1;   // source tanH, source tanV (over-scanned), 1/w, 1/h
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// 9-tap Catmull-Rom (bilinear-assisted): keeps the remapped image sharp.
float4 SampleCR(float2 uv, float2 invTex)
{
    const float2 texSize = 1.0 / invTex;
    float2 samplePos = uv * texSize;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / w12;
    float2 texPos0  = (texPos1 - 1.0) * invTex;
    float2 texPos3  = (texPos1 + 2.0) * invTex;
    float2 texPos12 = (texPos1 + offset12) * invTex;
    float4 r = 0;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos0.x,  texPos0.y),  0) * w0.x  * w0.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos12.x, texPos0.y),  0) * w12.x * w0.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos3.x,  texPos0.y),  0) * w3.x  * w0.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos0.x,  texPos12.y), 0) * w0.x  * w12.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos12.x, texPos12.y), 0) * w12.x * w12.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos3.x,  texPos12.y), 0) * w3.x  * w12.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos0.x,  texPos3.y),  0) * w0.x  * w3.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos12.x, texPos3.y),  0) * w12.x * w3.y;
    r += g_Source.SampleLevel(g_Source_sampler, float2(texPos3.x,  texPos3.y),  0) * w3.x  * w3.y;
    return r;
}

float4 main(in PSIn i) : SV_TARGET
{
    const float d = g_P0.x;
    // The Panini image point of this pixel (x right, y up).
    const float x = (i.uv.x * 2.0 - 1.0) * g_P0.y;
    const float y = (1.0 - i.uv.y * 2.0) * g_P0.z;
    // Inverse Panini: x = S sin(phi), S = (d + 1) / (d + cos(phi))  ->  cos(phi) in closed form.
    const float a  = x / (d + 1.0);
    const float a2 = a * a;
    const float cosPhi = (-a2 * d + sqrt(max(a2 * (1.0 - d * d) + 1.0, 0.0))) / (a2 + 1.0);
    const float sinPhi = a * (d + cosPhi);
    const float S = (d + 1.0) / (d + cosPhi);
    // y = S tan(theta) (1 + s (1/cos(phi) - 1)): s = 0 cylindrical, s = d/(d+1) rectilinear vertical
    // (lines at one depth stay straight - no "smile").
    const float tanTheta = y / (S * (1.0 + g_P0.w * (1.0 / cosPhi - 1.0)));
    // The same direction in the rectilinear source: (tan phi, tan theta / cos phi).
    const float xr = sinPhi / cosPhi;
    const float yr = tanTheta / cosPhi;
    // The over-scan covers the corners exactly, except when the renderer had to cap it (a source
    // FOV past ~166 deg): those last corner pixels hold the edge instead of going black.
    const float2 suv = saturate(float2(0.5 + 0.5 * xr / g_P1.x, 0.5 - 0.5 * yr / g_P1.y));
    return SampleCR(suv, g_P1.zw);
}
