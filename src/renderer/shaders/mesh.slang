// Forward-shaded 3D meshes with a directional light and a shadow map, shared by both rendering
// backends (compiled like sprite.slang, see cmake/pireneus_shaders.cmake). Three entry points :
// - shadowVertexMain  : depth-only pass into the shadow map (no fragment shader).
// - sceneVertexMain / sceneFragmentMain : the forward pass (Lambert + hemisphere ambient, 3x3 PCF shadows).
// - skyVertexMain / skyFragmentMain : the skybox, an equirectangular texture on a full-screen triangle.
//
// Matrices arrive as four float4 ROWS (row-major, see math/math3d.h) and are applied with explicit
// dot products : the memory layout is then unambiguous on every target, with no row/column-major
// packing rule involved (the same reasoning that keeps ortho.h matrix-free).

// Mirrors MeshConstants in src/renderer/mesh_constants.h, field for field (252 bytes). The sky pass
// reuses the same block with other meanings, see skyVertexMain(). A model matrix being affine, its
// bottom row would always be (0, 0, 0, 1) : model[3] carries the object's color instead (xyz), and how
// much the mesh's vertex colors count (w), see draw_mesh_colored().
[vk::push_constant]
cbuffer MeshConstants
{
    float4 model[4];
    float4 view_projection[4];
    float4 light_view_projection[4];
    float4 light_direction; // xyz : direction the light travels (normalized) ; w : shadow map texel size
    float4 light_color;     // xyz : color ; w : shadow depth bias
    float4 ambient_color;   // xyz : ambient light from above (sky) ; w : shadow normal offset (world units)
    float3 ground_color;    // ambient light from below (ground bounce)
};

// The pass's one texture, sampled with the engine's usual point/clamp sampler : the shadow map's depth
// in the scene pass (depth comparison and 3x3 PCF done by hand below, so it binds exactly like any
// other texture), the sky's equirectangular image in the sky pass.
Sampler2D pass_texture;

struct MeshInput
{
    float3 position : POSITION0;
    float3 normal : NORMAL0;
    float4 color : COLOR0;
};

struct SceneVaryings
{
    float4 position : SV_Position;
    float3 normal : NORMAL0;
    float4 color : COLOR0;
    float4 light_position : TEXCOORD0; // position in the light's clip space
};

float4 transform_rows(float4 r0, float4 r1, float4 r2, float4 r3, float4 v)
{
    return float4(dot(r0, v), dot(r1, v), dot(r2, v), dot(r3, v));
}

float4 to_world(float3 position)
{
    float4 p = float4(position, 1.0);
    return float4(dot(model[0], p), dot(model[1], p), dot(model[2], p), 1.0);
}

float4 to_light_clip(float4 world)
{
    return transform_rows(light_view_projection[0], light_view_projection[1], light_view_projection[2], light_view_projection[3], world);
}

[shader("vertex")]
float4 shadowVertexMain(MeshInput input) : SV_Position
{
    return to_light_clip(to_world(input.position));
}

[shader("vertex")]
SceneVaryings sceneVertexMain(MeshInput input)
{
    float4 world = to_world(input.position);

    // Normals use the model matrix's upper 3x3 : correct for rotations and uniform scales.
    float3 normal = normalize(float3(dot(model[0].xyz, input.normal), dot(model[1].xyz, input.normal), dot(model[2].xyz, input.normal)));

    SceneVaryings output;
    output.position = transform_rows(view_projection[0], view_projection[1], view_projection[2], view_projection[3], world);
    output.normal = normal;
    output.color = float4(lerp(float3(1.0, 1.0, 1.0), input.color.rgb, model[3].w) * model[3].xyz, input.color.a);

    // Normal offset : look the shadow map up from a point pushed slightly off the surface, which
    // removes self-shadowing acne on surfaces at grazing angles to the light far better than a
    // depth bias alone could.
    output.light_position = to_light_clip(world + float4(normal * ambient_color.w, 0.0));

    return output;
}

// 1 = fully lit, 0 = fully in shadow.
float shadow_factor(float4 light_position)
{
    float3 ndc = light_position.xyz / light_position.w;

    // Clip space +Y is up, texture V is down (the same on both backends, see math3d.h).
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
    {
        return 1.0; // outside the light's frustum : treated as lit
    }

    float texel = light_direction.w;
    float depth = ndc.z - light_color.w;
    float lit = 0.0;

    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            float occluder = pass_texture.SampleLevel(uv + float2(x, y) * texel, 0.0).r;
            lit += depth <= occluder ? 1.0 : 0.0;
        }
    }

    return lit / 9.0;
}

[shader("fragment")]
float4 sceneFragmentMain(SceneVaryings input) : SV_Target
{
    float3 normal = normalize(input.normal);
    float diffuse = saturate(dot(normal, -light_direction.xyz));
    float shadow = diffuse > 0.0 ? shadow_factor(input.light_position) : 0.0;

    // Hemisphere ambient : the simplest sky-based global illumination. Faces pointing up get the
    // sky's irradiance, faces pointing down the ground's, everything in between a blend of both
    // (both computed from the sky's image on the CPU, by the caller).
    float3 ambient = lerp(ground_color, ambient_color.xyz, normal.y * 0.5 + 0.5);
    float3 lighting = ambient + light_color.xyz * (diffuse * shadow);

    return float4(input.color.rgb * lighting, input.color.a);
}

// --- Sky ----------------------------------------------------------------------------------------
// Constants, sky pass : model = INVERSE view-projection (rows), light_direction.w = sky intensity.

struct SkyVaryings
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD0;
};

// A single triangle covering the whole screen, from the vertex index alone (no vertex buffer), at the
// far plane (z = w : depth 1), so it's only drawn where no mesh wrote a nearer depth (LESS_EQUAL).
[shader("vertex")]
SkyVaryings skyVertexMain(uint vertex_id : SV_VertexID)
{
    float2 ndc = float2(vertex_id == 1 ? 3.0 : -1.0, vertex_id == 2 ? 3.0 : -1.0);

    SkyVaryings output;
    output.position = float4(ndc, 1.0, 1.0);
    output.ndc = ndc;
    return output;
}

float3 unproject(float2 ndc, float depth)
{
    float4 world = transform_rows(model[0], model[1], model[2], model[3], float4(ndc, depth, 1.0));
    return world.xyz / world.w;
}

[shader("fragment")]
float4 skyFragmentMain(SkyVaryings input) : SV_Target
{
    // The view ray through this pixel : from its point on the near plane to its point on the far plane.
    float3 direction = normalize(unproject(input.ndc, 1.0) - unproject(input.ndc, 0.0));

    // Equirectangular (latitude/longitude) lookup : U wraps around the horizon (-Z at the center,
    // +X to its right), V goes from straight up (0) to straight down (1). Must match panorama_uv() in photoreal.slang.
    float u = 0.5 + atan2(direction.x, -direction.z) / (2.0 * 3.14159265);
    float v = acos(clamp(direction.y, -1.0, 1.0)) / 3.14159265;

    return float4(pass_texture.SampleLevel(float2(u, v), 0.0).rgb * light_direction.w, 1.0);
}
