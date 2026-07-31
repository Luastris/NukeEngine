// Debug/gizmo lines: flat vertex color.
struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR0; };
float4 main(in PSIn i) : SV_Target { return i.col; }
