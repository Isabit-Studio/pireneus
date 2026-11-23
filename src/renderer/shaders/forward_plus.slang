// Forward+ (renderer/render3d) : after a depth prepass, each object is shaded where it's drawn, with the
// depth test rejecting every hidden pixel before its shader runs : one shading per pixel too, without
// the visibility buffer's reconstruction.
//
// Both passes run the same vertex shader, fpForwardVs : the same program on the same vertices lands on
// exactly the same depth, which the shading pass' depth test relies on (two different shaders computing
// the same position may round it differently, and leave holes along the silhouettes).

#include "render3d_common.slang"

// Centroid : with MSAA, a pixel on a silhouette is shaded once for the samples its triangle covers, but
// at its center, which may lie outside the triangle ; plain interpolation would extrapolate the normal and
// UV there (bright specks along the edges). Centroid sampling keeps them inside.
struct ForwardVaryings
{
    float4 position : SV_Position;
    centroid float3 world : TEXCOORD0;
    centroid float3 normal : TEXCOORD1;
    centroid float4 tangent : TEXCOORD2;
    centroid float4 color : TEXCOORD3;
    centroid float2 uv : TEXCOORD4;
    centroid float2 lightmap_uv : TEXCOORD5;
};

[shader("vertex")]
ForwardVaryings fpForwardVs(uint vertex_id : SV_VertexID)
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);
    VertexData vertex = load_vertex(instance.geometry.x, vertex_id);

    ForwardVaryings output;
    output.position = clip_position(frame, instance, vertex.position);
    output.world = to_world(instance, vertex.position);
    output.normal = normal_to_world(instance, vertex.normal);
    output.tangent = float4(normal_to_world(instance, vertex.tangent.xyz), vertex.tangent.w);
    output.color = vertex.color;
    output.uv = vertex.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    output.lightmap_uv = instance.lightmap.z != 0u ? load_lightmap_uv(instance, vertex_id) : float2(0.0, 0.0);
    return output;
}

// The prepass : alpha-masked surfaces cut their holes.
bool masked_out(Instance instance, float2 uv)
{
    if ((instance.geometry.w & 1u) == 0u || instance.textures.x == NO_TEXTURE)
    {
        return false;
    }

    float alpha = g_textures[NonUniformResourceIndex(instance.textures.x)].Sample(g_sampler_anisotropic, uv).a * instance.base_color.a;
    return alpha < instance.surface.w;
}

[shader("fragment")]
void fpDepthMaskedPs(ForwardVaryings input)
{
    if (masked_out(load_instance(ids.w), input.uv))
    {
        discard;
    }
}

[shader("fragment")]
float4 fpForwardPs(ForwardVaryings input) : SV_Target
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);

    // Alpha-masked surfaces cut the same holes as in the prepass, or they'd draw over what's behind them.
    if (masked_out(instance, input.uv))
    {
        discard;
    }

    SurfaceInput surface_input;
    surface_input.position = input.world;
    surface_input.geometric_normal = cross(ddy(input.world), ddx(input.world));
    surface_input.normal = input.normal;
    surface_input.tangent = input.tangent;
    surface_input.vertex_color = input.color;
    surface_input.uv = input.uv;
    surface_input.uv_ddx = ddx(input.uv);
    surface_input.uv_ddy = ddy(input.uv);
    surface_input.lightmap_uv = input.lightmap_uv;

    float3 view_vector = frame.camera_position.xyz - input.world;
    float view_length = length(view_vector);
    float3 V = view_vector / view_length;

    Surface surface = evaluate_surface(instance, surface_input, V);

    float ao = min(surface.occlusion, screen_occlusion(frame, int2(input.position.xy)));
    float3 color = shade_surface(frame, surface, ao, V, view_length, surface.position, input.position.xy);

    return float4(color, 1.0);
}
