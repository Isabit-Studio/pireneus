// Deferred+ (renderer/render3d) : The Forge's triangle visibility buffer. A first pass writes only
// "which triangle covers this pixel" ; a single full-screen pass then fetches that triangle's vertices,
// recomputes its barycentrics and derivatives, and shades. Every pixel is shaded exactly once, whatever
// the overdraw.

#include "render3d_common.slang"

static const uint EMPTY_PIXEL = 0xFFFFFFFFu;
static const uint PRIMITIVE_BITS = 20u; // the visibility buffer's ID : 12 bits of draw, 20 bits of triangle

// Drawn with the shared r3GeometryVs (render3d.slang).
struct GeometryVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("fragment")]
float4 dpVisibilityPs(GeometryVaryings input, uint primitive : SV_PrimitiveID) : SV_Target
{
    Instance instance = load_instance(ids.w);

    if ((instance.geometry.w & 1u) != 0u && instance.textures.x != NO_TEXTURE)
    {
        float alpha = g_textures[NonUniformResourceIndex(instance.textures.x)].Sample(g_sampler_anisotropic, input.uv).a * instance.base_color.a;

        if (alpha < instance.surface.w)
        {
            discard;
        }
    }

    // An RGBA8 target holds the 32 bits exactly, as in The Forge.
    uint id = (ids.w << PRIMITIVE_BITS) | (primitive & ((1u << PRIMITIVE_BITS) - 1u));
    return float4(float(id & 255u), float((id >> 8u) & 255u), float((id >> 16u) & 255u), float((id >> 24u) & 255u)) / 255.0;
}

// --- Visibility buffer shading ---------------------------------------------------------------------

struct Barycentrics
{
    float3 lambda;
    float3 ddx;
    float3 ddy;
};

// The triangle's barycentrics at this pixel and their screen derivatives, perspective-correct : what the
// rasterizer would have interpolated (after The Forge / Stachowiak's deferred material rendering).
Barycentrics compute_barycentrics(float4 p0, float4 p1, float4 p2, float2 pixel_ndc, float2 resolution)
{
    Barycentrics result;

    float3 inv_w = 1.0 / float3(p0.w, p1.w, p2.w);
    float2 ndc0 = p0.xy * inv_w.x;
    float2 ndc1 = p1.xy * inv_w.y;
    float2 ndc2 = p2.xy * inv_w.z;

    float2 a = ndc2 - ndc1;
    float2 b = ndc0 - ndc1;
    float inv_det = 1.0 / (a.x * b.y - a.y * b.x);

    result.ddx = float3(ndc1.y - ndc2.y, ndc2.y - ndc0.y, ndc0.y - ndc1.y) * inv_det * inv_w;
    result.ddy = float3(ndc2.x - ndc1.x, ndc0.x - ndc2.x, ndc1.x - ndc0.x) * inv_det * inv_w;

    float ddx_sum = dot(result.ddx, float3(1.0, 1.0, 1.0));
    float ddy_sum = dot(result.ddy, float3(1.0, 1.0, 1.0));

    float2 delta = pixel_ndc - ndc0;
    float interp_inv_w = inv_w.x + delta.x * ddx_sum + delta.y * ddy_sum;
    float interp_w = 1.0 / interp_inv_w;

    result.lambda.x = interp_w * (inv_w.x + delta.x * result.ddx.x + delta.y * result.ddy.x);
    result.lambda.y = interp_w * (delta.x * result.ddx.y + delta.y * result.ddy.y);
    result.lambda.z = interp_w * (delta.x * result.ddx.z + delta.y * result.ddy.z);

    result.ddx *= 2.0 / resolution.x;
    result.ddy *= -2.0 / resolution.y;
    ddx_sum *= 2.0 / resolution.x;
    ddy_sum *= -2.0 / resolution.y;

    float interp_w_ddx = 1.0 / (interp_inv_w + ddx_sum);
    float interp_w_ddy = 1.0 / (interp_inv_w + ddy_sum);

    result.ddx = interp_w_ddx * (result.lambda * interp_inv_w + result.ddx) - result.lambda;
    result.ddy = interp_w_ddy * (result.lambda * interp_inv_w + result.ddy) - result.lambda;

    return result;
}

// The shading pass : the lit color, and for the screen-space reflections the surface's normal,
// roughness and specular terms.
struct ShadeOutput
{
    float4 color : SV_Target0;
    float4 surface : SV_Target1;  // xy : octahedral normal ; z : roughness ; w : specular occlusion
    float4 specular : SV_Target2; // rgb : the reflection's weight
};

ShadeOutput shade(ScreenVaryings input)
{
    ShadeOutput output;
    output.surface = float4(0.0, 0.0, 1.0, 0.0);
    output.specular = float4(0.0, 0.0, 0.0, 0.0);

    Frame frame = load_frame();
    uint visibility_texture = inputs.x;
    int2 pixel = int2(input.position.xy);

    float2 ndc = float2(input.position.x * frame.resolution.z * 2.0 - 1.0, 1.0 - input.position.y * frame.resolution.w * 2.0);
    float3 eye = frame.camera_position.xyz;

    float4 packed = g_textures[visibility_texture].Load(int3(pixel, 0));
    uint4 bytes = uint4(packed * 255.0 + 0.5);
    uint id = bytes.x | (bytes.y << 8u) | (bytes.z << 16u) | (bytes.w << 24u);

    if (id == EMPTY_PIXEL)
    {
        output.color = float4(min(background(frame, ndc), float3(60000.0, 60000.0, 60000.0)), 1.0);
        return output;
    }

    uint draw = id >> PRIMITIVE_BITS;
    uint primitive = id & ((1u << PRIMITIVE_BITS) - 1u);
    Instance instance = load_instance(draw);

    uint first = instance.geometry.z + primitive * 3u;
    uint index_buffer = instance.geometry.y;
    uint i0 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u);
    uint i1 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 4u);
    uint i2 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 8u);

    VertexData v0 = load_vertex(instance.geometry.x, i0);
    VertexData v1 = load_vertex(instance.geometry.x, i1);
    VertexData v2 = load_vertex(instance.geometry.x, i2);

    float3 w0 = to_world(instance, v0.position);
    float3 w1 = to_world(instance, v1.position);
    float3 w2 = to_world(instance, v2.position);

    Barycentrics bary = compute_barycentrics(
        mul_rows(frame.view_projection, float4(w0, 1.0)),
        mul_rows(frame.view_projection, float4(w1, 1.0)),
        mul_rows(frame.view_projection, float4(w2, 1.0)),
        ndc, frame.resolution.xy);

    float3 l = bary.lambda;

    float2 uv0 = v0.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    float2 uv1 = v1.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    float2 uv2 = v2.uv * instance.uv_transform.xy + instance.uv_transform.zw;

    SurfaceInput surface_input;
    surface_input.position = w0 * l.x + w1 * l.y + w2 * l.z;
    surface_input.geometric_normal = cross(w1 - w0, w2 - w0);
    surface_input.normal = normal_to_world(instance, v0.normal * l.x + v1.normal * l.y + v2.normal * l.z);
    float4 tangent = v0.tangent * l.x + v1.tangent * l.y + v2.tangent * l.z;
    surface_input.tangent = float4(normal_to_world(instance, tangent.xyz), tangent.w);
    surface_input.vertex_color = v0.color * l.x + v1.color * l.y + v2.color * l.z;
    surface_input.uv = uv0 * l.x + uv1 * l.y + uv2 * l.z;
    surface_input.uv_ddx = uv0 * bary.ddx.x + uv1 * bary.ddx.y + uv2 * bary.ddx.z;
    surface_input.uv_ddy = uv0 * bary.ddy.x + uv1 * bary.ddy.y + uv2 * bary.ddy.z;
    surface_input.lightmap_uv = float2(0.0, 0.0);

    if (instance.lightmap.z != 0u)
    {
        surface_input.lightmap_uv = load_lightmap_uv(instance, i0) * l.x + load_lightmap_uv(instance, i1) * l.y + load_lightmap_uv(instance, i2) * l.z;
    }

    float3 view_vector = eye - surface_input.position;
    float view_length = length(view_vector);
    float3 V = view_vector / view_length;

    Surface surface = evaluate_surface(instance, surface_input, V);

    // Hanika, "Hacking the Shadow Terminator" : on a coarse smooth-shaded mesh, look the shadow up from
    // the point the vertex normals' tangent planes predict, not from the flat triangle. The visibility
    // buffer has the triangle at hand.
    float3 world = surface.position;
    float3 t0 = world - w0;
    float3 t1 = world - w1;
    float3 t2 = world - w2;
    float3 n0 = normal_to_world(instance, v0.normal);
    float3 n1 = normal_to_world(instance, v1.normal);
    float3 n2 = normal_to_world(instance, v2.normal);
    t0 -= min(0.0, dot(t0, n0)) * n0;
    t1 -= min(0.0, dot(t1, n1)) * n1;
    t2 -= min(0.0, dot(t2, n2)) * n2;
    float3 shadow_position = world + t0 * l.x + t1 * l.y + t2 * l.z;

    float ao = min(surface.occlusion, screen_occlusion(frame, pixel));
    SpecularTerms terms;
    float3 color = shade_surface_terms(frame, surface, ao, V, view_length, shadow_position, input.position.xy, terms);

    output.color = float4(color, 1.0);
    output.surface = float4(encode_octahedron(surface.normal), surface.roughness, terms.occlusion);
    output.specular = float4(terms.weight, 1.0);
    return output;
}

[shader("fragment")]
float4 dpShadePs(ScreenVaryings input) : SV_Target
{
    return shade(input).color;
}

[shader("fragment")]
ShadeOutput dpShadeSurfacePs(ScreenVaryings input)
{
    return shade(input);
}

// --- Screen-space reflections -------------------------------------------------------------------------
//
// One ray per pixel, its direction drawn from the GGX lobe of the surface (stochastic, as in Frostbite's
// SSR) : rough surfaces get blurry reflections once TAA averages the frames. The ray is marched in world
// space against the depth buffer, refined by bisection where it crosses it, and the lit image is read
// there. Where it finds nothing (off screen, behind something), the environment's reflection stays.

// A GGX-distributed microfacet normal around N (Walter et al.).
float3 sample_ggx(float3 N, float roughness, float2 xi)
{
    float a = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cos_theta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sin_theta = sqrt(max(0.0, 1.0 - cos_theta * cos_theta));

    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 T = normalize(cross(up, N));
    float3 B = cross(N, T);

    return normalize(T * (sin_theta * cos(phi)) + B * (sin_theta * sin(phi)) + N * cos_theta);
}

// Where a world point is on screen, and its view depth.
float3 project_point(Frame frame, float3 world)
{
    float4 clip = mul_rows(frame.view_projection, float4(world, 1.0));
    float2 ndc = clip.xy / clip.w;
    return float3(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, -dot(frame.view[2], float4(world, 1.0)));
}

float scene_view_depth(Frame frame, uint depth_texture, float2 uv)
{
    int2 pixel = int2(uv * frame.resolution.xy);
    float depth = g_textures[depth_texture].Load(int3(pixel, 0)).r;
    return depth >= 1.0 ? 1e30 : -view_position(frame, uv, depth).z;
}

// inputs.x : the surface target ; y : the scene's depth ; z : the lit image.
// params.x : steps ; y : the longest ray, in meters ; z : the thickness surfaces are given, in meters.
[shader("fragment")]
float4 dpReflectionsPs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    int2 pixel = int2(input.position.xy);
    float depth = g_textures[inputs.y].Load(int3(pixel, 0)).r;
    float4 surface = g_textures[inputs.x].Load(int3(pixel, 0));
    float roughness = surface.z;

    // Rough surfaces keep the environment's reflection : a blurry one, which the screen can't give cheaply.
    if (depth >= 1.0 || roughness > 0.5)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    float4 p = mul_rows(frame.inverse_view_projection, float4(ndc, depth, 1.0));
    float3 world = p.xyz / p.w;
    float3 N = decode_octahedron(surface.xy);
    float3 V = normalize(frame.camera_position.xyz - world);

    float noise = frame_noise(frame, input.position.xy);
    float2 xi = frac(float2(noise, noise * 7.3 + 0.37) + frame.jitter.z * float2(0.7548776, 0.5698403));
    float3 H = roughness < 0.05 ? N : sample_ggx(N, roughness, xi);
    float3 R = reflect(-V, H);

    if (dot(R, N) <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    int steps = int(params.x);
    float max_distance = params.y;
    float thickness = params.z;
    float3 origin = world + N * (0.002 * length(frame.camera_position.xyz - world));
    float previous_t = 0.0;
    float hit_t = -1.0;

    for (int i = 1; i <= steps; i++)
    {
        // Denser near the surface, where contact matters most.
        float s = (float(i) - 0.5 + noise) / float(steps);
        float t = max_distance * s * s;
        float3 q = project_point(frame, origin + R * t);

        if (any(q.xy < 0.0) || any(q.xy > 1.0) || q.z <= 0.0)
        {
            break;
        }

        float scene = scene_view_depth(frame, inputs.y, q.xy);

        if (q.z > scene && q.z - scene < thickness + t * 0.05)
        {
            // Bisection between the last point in front and this one.
            float lo = previous_t;
            float hi = t;

            for (int b = 0; b < 5; b++)
            {
                float mid = (lo + hi) * 0.5;
                float3 m = project_point(frame, origin + R * mid);

                if (m.z > scene_view_depth(frame, inputs.y, m.xy))
                {
                    hi = mid;
                }
                else
                {
                    lo = mid;
                }
            }

            hit_t = hi;
            break;
        }

        previous_t = t;
    }

    if (hit_t < 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    float3 hit = project_point(frame, origin + R * hit_t);
    float3 color = g_textures[inputs.z].SampleLevel(g_sampler_point, hit.xy, 0.0).rgb;

    // Trust fades toward the screen's edges, for long rays, rays coming back at the camera, rough surfaces.
    float2 edge = saturate(min(hit.xy, 1.0 - hit.xy) * 12.0);
    float confidence = edge.x * edge.y;
    confidence *= 1.0 - saturate(hit_t / max_distance);
    confidence *= saturate(1.0 + dot(R, -V) * 2.0) * 0.5 + 0.5;
    confidence *= 1.0 - smoothstep(0.3, 0.5, roughness);

    return float4(min(color, float3(60000.0, 60000.0, 60000.0)), confidence);
}

// The lit image, its environment reflection replaced by the screen-space one where that one was found.
// inputs.x : the lit image ; y : reflections ; z : the surface target ; w : the specular target.
// params.x : the scene's depth.
[shader("fragment")]
float4 dpCompositePs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    int2 pixel = int2(input.position.xy);
    float3 color = g_textures[inputs.x].Load(int3(pixel, 0)).rgb;
    float4 reflection = g_textures[inputs.y].Load(int3(pixel, 0));

    if (reflection.a <= 0.0)
    {
        return float4(color, 1.0);
    }

    float4 surface = g_textures[inputs.z].Load(int3(pixel, 0));
    float3 weight = g_textures[inputs.w].Load(int3(pixel, 0)).rgb * surface.w;

    float depth = g_textures[asuint(params.x)].Load(int3(pixel, 0)).r;
    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    float4 p = mul_rows(frame.inverse_view_projection, float4(ndc, depth, 1.0));
    float3 world = p.xyz / p.w;
    float3 V = normalize(frame.camera_position.xyz - world);
    float3 environment = environment_reflection(frame, world, decode_octahedron(surface.xy), V, surface.z);

    color += reflection.a * (reflection.rgb - environment) * weight;

    return float4(max(color, float3(0.0, 0.0, 0.0)), 1.0);
}
