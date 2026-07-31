// Surface for the "unlit" shader, shared by the raster pass and the auto-generated RT closest-hit.
// MatCB fields and MAT_BASE_TEX are supplied by whichever harness includes this.
void Surface(SurfaceIn IN, inout SurfaceOut O)
{
    float4 base = (g_Params.x > 0.5) ? MAT_BASE_TEX(IN.uv) : float4(1, 1, 1, 1);
    O.emissive = base.rgb * g_Color.rgb * g_Tint * g_Brightness;
    O.alpha    = base.a * g_Color.a;
    O.unlit    = true;
}
