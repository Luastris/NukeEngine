// Standard Nuke material constant block — THE single source of truth. Every material-capable
// shader (world/gbuffer/custom) declares its MatCB cbuffer as just this include; custom
// shaders append their own props AFTER it. Packing docs live in world.ps.hlsl.
// NOTE: comments here must stay BRACE-FREE — the engine prop parser scans the cbuffer body.
float4 g_Color; float4 g_Params; float4 g_Params2; float4 g_Emissive2; float4 g_UVT; float4 g_UVT2; float4 g_Disp;
float4 g_Ov0;  float4 g_Ov1;  float4 g_Ov2;  float4 g_Ov3;  float4 g_Ov4;  float4 g_Ov5;  float4 g_Ov6;  float4 g_Ov7;
float4 g_OvT0; float4 g_OvT1; float4 g_OvT2; float4 g_OvT3; float4 g_OvT4; float4 g_OvT5; float4 g_OvT6; float4 g_OvT7;
float4 g_OvP0; float4 g_OvP1; float4 g_OvP2; float4 g_OvP3; float4 g_OvP4; float4 g_OvP5; float4 g_OvP6; float4 g_OvP7;
float4 g_OvM0; float4 g_OvM1; float4 g_OvM2; float4 g_OvMQ;
float4 g_Det; float4 g_Var;
float4 g_Brdf1; float4 g_Brdf2; float4 g_Brdf3; float4 g_Brdf4;
// Masked-tween twins: xyz = tweened value, w = maskSlot+1 (0 = off). The base prop keeps its
// untweened value; the shader lerps base -> twin per pixel by the mask weight.
// g_DispT = (pomDepth, dispScale, dispMid); g_ColorT = base rgb; g_EmisT = final emissive
// rgb (intensity premultiplied); g_ParamsT = (metallic, roughness, wipeThreshold).
float4 g_DispT; float4 g_ColorT; float4 g_EmisT; float4 g_ParamsT;
// Spatial mask slots (LiveMask): A = (cx, cy, cz, shape + space*4)  shape 0 circle / 1 ring /
// 2 stamp, space 0 UV / 1 world; B = (scale, ringRepeat, rotationRad, fade);
// C = (softness, strength, hasStamp, 0). strength 0 = slot off.
float4 g_MskA0; float4 g_MskB0; float4 g_MskC0;
float4 g_MskA1; float4 g_MskB1; float4 g_MskC1;
float4 g_MskA2; float4 g_MskB2; float4 g_MskC2;
float4 g_MskA3; float4 g_MskB3; float4 g_MskC3;
float4 g_MskA4; float4 g_MskB4; float4 g_MskC4;
float4 g_MskA5; float4 g_MskB5; float4 g_MskC5;
// Last hit that fired a material event: (impulse, hit normal xyz) — custom shaders react
// to touches (water drops etc.) by reading this next to the mask weights.
float4 g_Hit;
// Toon cel band: g_Toon = (band threshold, edge softness, 0, on) and the shade-side tint.
float4 g_Toon; float4 g_ToonShade;
