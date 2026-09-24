// Super resolution: a built-in chain stage run by the renderer (NukeDiligent_Upscale.cpp). The
// scene renders at a lower internal resolution and this stage reconstructs the output size.
// Temporal modes (DLSS) replace TAA and run in HDR at this slot; the spatial FSR 1 runs after
// the tonemap. This file only declares the params and registers the stage; main() below is an
// unused passthrough. Needs the depth prepass (depth + velocity). g_FrameGen adds the vendor's
// frame generation on top (NukeDiligent_FrameGen.cpp): its swap chain presents a generated frame
// between every two rendered ones.
Texture2D    g_Source;
SamplerState g_Source_sampler;
cbuffer PostParams
{
    float g_Mode      = 0.0;   // 0 = Auto (best this GPU offers), 1 = DLSS, 2 = FSR, 3 = XeSS, 4 = FSR 1 (spatial, any GPU)
    float g_Quality   = 1.0;   // 0 = Native (DLAA), 1 = Quality, 2 = Balanced, 3 = Performance, 4 = Ultra Performance
    float g_Sharpness = 0.0;   // 0..1 sharpening where the mode supports it
    float g_FrameGen  = 0.0;   // 1 = frame generation: DLSS-G / FSR 3.1 FG / XeSS-FG after the mode's vendor (D3D12 + Vulkan)
    float g_FrameGenFrames = 1.0;   // generated frames per rendered one (the GPU caps it: 1 on FSR / XeSS, up to 5 on DLSS-G)
};
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(in PSIn i) : SV_Target { return g_Source.Sample(g_Source_sampler, i.uv); }
