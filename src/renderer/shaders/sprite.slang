// The engine's single sprite shader (a textured, tinted quad), shared by both rendering
// backends : compiled directly to SPIR-V for Vulkan (Slang's own backend, see
// cmake/pireneus_shaders.cmake), and transpiled to HLSL text then compiled to DXIL by dxc for
// DX12 (Slang doesn't bundle dxcompiler.dll, so its own DXIL backend isn't used here ; going
// through HLSL text keeps using the same dxc.exe the project already locates from the Windows
// SDK). One source instead of a hand-written GLSL/HLSL pair per backend, so there's no risk of
// the two backends' shading logic drifting apart.

[vk::push_constant]
cbuffer PushConstants
{
    float2 scale;
    float2 offset;
};

Sampler2D tex;

struct VSInput
{
    float2 position : POSITION0;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VSOutput vertexMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position * scale + offset, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VSOutput input) : SV_Target
{
    return tex.Sample(input.uv) * input.color;
}
