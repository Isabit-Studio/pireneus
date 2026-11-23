// The passes both 3D pipelines share (renderer/render3d) : shadow maps, the geometry vertex shader of
// depth-only passes, full-screen passes (ambient occlusion, sky, bloom, tone mapping, anti-aliasing).

#include "render3d_common.slang"

// --- Shadow and visibility passes -----------------------------------------------------------------

[shader("vertex")]
float4 r3ShadowVs(uint vertex_id : SV_VertexID) : SV_Position
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);
    float4 world = float4(to_world(instance, load_vertex(instance.geometry.x, vertex_id).position), 1.0);
    uint c = inputs.x * 4u;

    return float4(dot(frame.shadow_view_projection[c], world), dot(frame.shadow_view_projection[c + 1u], world),
        dot(frame.shadow_view_projection[c + 2u], world), dot(frame.shadow_view_projection[c + 3u], world));
}

// Alpha-masked surfaces cast a shadow with their holes in it.
struct ShadowVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
ShadowVaryings r3ShadowMaskedVs(uint vertex_id : SV_VertexID)
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);
    VertexData vertex = load_vertex(instance.geometry.x, vertex_id);
    float4 world = float4(to_world(instance, vertex.position), 1.0);
    uint c = inputs.x * 4u;

    ShadowVaryings output;
    output.position = float4(dot(frame.shadow_view_projection[c], world), dot(frame.shadow_view_projection[c + 1u], world),
        dot(frame.shadow_view_projection[c + 2u], world), dot(frame.shadow_view_projection[c + 3u], world));
    output.uv = vertex.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    return output;
}

[shader("fragment")]
void r3ShadowMaskedPs(ShadowVaryings input)
{
    Instance instance = load_instance(ids.w);
    float alpha = g_textures[NonUniformResourceIndex(instance.textures.x)].SampleLevel(g_sampler_anisotropic, input.uv, 0.0).a * instance.base_color.a;

    if (alpha < instance.surface.w)
    {
        discard;
    }
}

// Point and spot lights' shadow maps : one tile of the atlas each (six for a point light), its matrix
// in the frame's shadow tiles. inputs.x : the tile.
[shader("vertex")]
float4 r3LocalShadowVs(uint vertex_id : SV_VertexID) : SV_Position
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);
    ShadowTile tile = load_shadow_tile(frame, inputs.x);
    return mul_rows(tile.view_projection, float4(to_world(instance, load_vertex(instance.geometry.x, vertex_id).position), 1.0));
}

[shader("vertex")]
ShadowVaryings r3LocalShadowMaskedVs(uint vertex_id : SV_VertexID)
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);
    ShadowTile tile = load_shadow_tile(frame, inputs.x);
    VertexData vertex = load_vertex(instance.geometry.x, vertex_id);

    ShadowVaryings output;
    output.position = mul_rows(tile.view_projection, float4(to_world(instance, vertex.position), 1.0));
    output.uv = vertex.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    return output;
}

// A full-screen triangle at the far plane : clears the viewport's rectangle of a depth target.
[shader("vertex")]
float4 r3ClearDepthVs(uint vertex_id : SV_VertexID) : SV_Position
{
    float2 ndc = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);
    return float4(ndc, 1.0, 1.0);
}

// The scene seen from the camera, for Deferred+'s visibility buffer : the position, and the UV that
// alpha-masked surfaces cut their holes with.
struct GeometryVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
GeometryVaryings r3GeometryVs(uint vertex_id : SV_VertexID)
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);
    VertexData vertex = load_vertex(instance.geometry.x, vertex_id);

    GeometryVaryings output;
    output.position = clip_position(frame, instance, vertex.position);
    output.uv = vertex.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    return output;
}

// --- Full-screen passes ---------------------------------------------------------------------------

[shader("vertex")]
ScreenVaryings r3ScreenVs(uint vertex_id : SV_VertexID)
{
    float2 ndc = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);

    ScreenVaryings output;
    output.position = float4(ndc, 0.0, 1.0);
    output.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return output;
}

// The sky, at the far plane : depth-tested against the scene, it only covers the pixels nothing else did.
[shader("vertex")]
ScreenVaryings r3SkyVs(uint vertex_id : SV_VertexID)
{
    float2 ndc = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);

    ScreenVaryings output;
    output.position = float4(ndc, 1.0, 1.0);
    output.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return output;
}

[shader("fragment")]
float4 r3SkyPs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    float2 ndc = float2(input.position.x * frame.resolution.z * 2.0 - 1.0, 1.0 - input.position.y * frame.resolution.w * 2.0);
    return float4(min(background(frame, ndc), float3(60000.0, 60000.0, 60000.0)), 1.0);
}

// --- Volumetric lighting (both pipelines) -------------------------------------------------------------
//
// At half resolution, a ray marched from the eye to the scene through the height fog's medium (the same
// density as the analytic fog, which it then replaces) : at each step, the sun's light (its shadow
// cascades and the clouds' shadows) scattered toward the eye by a Henyey-Greenstein lobe, and optionally
// the point and spot lights of the step's cluster. The start of the ray jitters per pixel (and per frame
// with TAA) : few steps, no banding. Then upsampled with depth-aware weights and blended over the image.

// The sun's shadow at a point of the medium : one tap of the right cascade.
float sun_visibility(Frame frame, float3 world, float view_distance)
{
    int cascade_count = int(frame.shadow_params.z);
    int cascade = 0;

    for (int i = 0; i < cascade_count - 1; i++)
    {
        if (view_distance > frame.cascade_far[i])
        {
            cascade = i + 1;
        }
    }

    if (cascade_count == 0 || view_distance > frame.cascade_far[cascade_count - 1])
    {
        return 1.0;
    }

    uint c = uint(cascade) * 4u;
    float4 p = float4(world, 1.0);
    float4 clip = float4(dot(frame.shadow_view_projection[c], p), dot(frame.shadow_view_projection[c + 1u], p),
        dot(frame.shadow_view_projection[c + 2u], p), dot(frame.shadow_view_projection[c + 3u], p));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
    {
        return 1.0;
    }

    float occluder = g_textures[NonUniformResourceIndex(frame.shadow_textures[cascade])].SampleLevel(g_sampler_point, uv, 0.0).r;
    return ndc.z <= occluder + 1e-4 ? 1.0 : 0.0;
}

// inputs.x : the scene's depth. params.x : steps ; y : the farthest distance, in meters ; z : 1 to add the
// point and spot lights ; w : this pass' pixels per full-resolution pixel (0.5).
[shader("fragment")]
float4 r3VolumetricsPs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    float depth = g_textures[inputs.x].SampleLevel(g_sampler_point, input.uv, 0.0).r;
    float3 eye = frame.camera_position.xyz;

    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    float4 p = mul_rows(frame.inverse_view_projection, float4(ndc, depth >= 1.0 ? 0.5 : depth, 1.0));
    float3 target = p.xyz / p.w;
    float3 direction = normalize(target - eye);
    float distance = depth >= 1.0 ? params.y : min(length(target - eye), params.y);

    int steps = int(params.x);
    float step_length = distance / float(steps);
    float offset = frame_noise(frame, input.position.xy);

    float density_base = frame.fog_inscatter.w;
    float falloff = max(frame.fog_sun.w, 1e-4);
    float base_height = frame.fog_params.x;
    float g = frame.volumetrics.y;
    float sun_phase = henyey_greenstein(dot(direction, frame.sun_direction.xyz), g);
    float3 ambient = max(frame.sh[0].xyz * 0.282095, float3(0.0, 0.0, 0.0)) * frame.environment.x;
    float3 albedo = frame.fog_inscatter.xyz * frame.volumetrics.z;
    float2 full_pixel = input.position.xy / params.w;

    float3 scattered = float3(0.0, 0.0, 0.0);
    float transmittance = 1.0;

    for (int i = 0; i < steps; i++)
    {
        float t = (float(i) + offset) * step_length;
        float3 x = eye + direction * t;
        float density = density_base * exp(-(x.y - base_height) * falloff);

        if (density < 1e-6)
        {
            continue;
        }

        float3 light = ambient + frame.sun_color.rgb * (sun_visibility(frame, x, t) * cloud_shadow(frame, x) * sun_phase);

        if (params.z > 0.0)
        {
            uint2 cluster = cluster_lights(frame, x, full_pixel);

            for (uint l = 0u; l < cluster.y; l++)
            {
                Light local = load_light(frame, g_buffers[ids.x].Load(frame.light_data.z + (cluster.x + l) * 4u));
                float3 to_light = local.position_range.xyz - x;
                float d2 = dot(to_light, to_light);
                float window = saturate(1.0 - d2 * d2 * local.color.w * local.color.w);

                if (window <= 0.0)
                {
                    continue;
                }

                float3 Ll = to_light * rsqrt(max(d2, 1e-6));
                float attenuation = window * window / max(d2, 1e-2);

                if (local.direction_type.w > 0.5)
                {
                    float cone = saturate(dot(-Ll, local.direction_type.xyz) * local.spot_shadow.x + local.spot_shadow.y);
                    attenuation *= cone * cone;
                }

                if (attenuation > 0.0 && local.spot_shadow.z >= 0.0)
                {
                    attenuation *= local_shadow(frame, local, x, float3(0.0, 0.0, 0.0), sqrt(d2), input.position.xy);
                }

                light += local.color.rgb * (attenuation * henyey_greenstein(dot(direction, -Ll), g));
            }
        }

        // Energy-conserving integration over the step (Hillaire).
        float extinction = density;
        float step_transmittance = exp(-extinction * step_length);
        scattered += transmittance * (light * albedo) * ((1.0 - step_transmittance) / extinction) * density;
        transmittance *= step_transmittance;
    }

    return float4(min(scattered, float3(60000.0, 60000.0, 60000.0)), transmittance);
}

// The half-resolution medium, upsampled : of its four nearest texels, those at the pixel's depth count
// most (no fog bleeding across edges). Blended with GPU_BLEND_ATTENUATE : image * transmittance + light.
// inputs.x : the volumetrics ; y : the scene's depth.
[shader("fragment")]
float4 r3VolumetricsCompositePs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    int2 pixel = int2(input.position.xy);
    float depth = g_textures[inputs.y].Load(int3(pixel, 0)).r;
    float z = -view_position(frame, input.uv, depth).z;

    uint w = 0u;
    uint h = 0u;
    g_textures[inputs.x].GetDimensions(w, h);
    float2 low = input.uv * float2(w, h) - 0.5;
    int2 base = int2(floor(low));
    float2 f = low - float2(base);

    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weight_sum = 0.0;

    for (int y = 0; y <= 1; y++)
    {
        for (int x = 0; x <= 1; x++)
        {
            int2 q = clamp(base + int2(x, y), int2(0, 0), int2(w, h) - 1);
            float2 q_uv = (float2(q) + 0.5) / float2(w, h);
            float q_depth = g_textures[inputs.y].SampleLevel(g_sampler_point, q_uv, 0.0).r;
            float q_z = -view_position(frame, q_uv, q_depth).z;

            float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
            float weight = bilinear * (1.0 / (1e-3 + abs(q_z - z) / max(z, 1e-3) * 50.0)) + 1e-5;

            sum += g_textures[inputs.x].Load(int3(q, 0)) * weight;
            weight_sum += weight;
        }
    }

    return sum / weight_sum;
}

// --- Temporal anti-aliasing (both pipelines) ---------------------------------------------------------
//
// Every frame is rendered with a different sub-pixel offset (Halton 2, 3) and blended into the history
// of the previous ones, found where the camera saw this point last frame (from depth : static geometry
// reprojects exactly). The history is sampled with a Catmull-Rom filter (Jimenez's 5 taps) rather than
// bilinear, which keeps it sharp, and clipped to the variance of the current neighborhood in YCoCg
// (Salvi), which keeps it from ghosting.

float3 rgb_to_ycocg(float3 c)
{
    return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 ycocg_to_rgb(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

float3 history_catmull_rom(uint texture, float2 uv, float2 resolution)
{
    float2 position = uv * resolution;
    float2 center = floor(position - 0.5) + 0.5;
    float2 f = position - center;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = -0.5 * f3 + f2 - 0.5 * f;
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w2 = -1.5 * f3 + 2.0 * f2 + 0.5 * f;
    float2 w3 = 0.5 * f3 - 0.5 * f2;
    float2 w12 = w1 + w2;

    float2 texel = 1.0 / resolution;
    float2 uv0 = (center - 1.0) * texel;
    float2 uv12 = (center + w2 / w12) * texel;
    float2 uv3 = (center + 2.0) * texel;

    SamplerState s = g_sampler_linear;
    float3 result =
        g_textures[texture].SampleLevel(s, float2(uv12.x, uv0.y), 0.0).rgb * (w12.x * w0.y) +
        g_textures[texture].SampleLevel(s, float2(uv0.x, uv12.y), 0.0).rgb * (w0.x * w12.y) +
        g_textures[texture].SampleLevel(s, float2(uv12.x, uv12.y), 0.0).rgb * (w12.x * w12.y) +
        g_textures[texture].SampleLevel(s, float2(uv3.x, uv12.y), 0.0).rgb * (w3.x * w12.y) +
        g_textures[texture].SampleLevel(s, float2(uv12.x, uv3.y), 0.0).rgb * (w12.x * w3.y);
    float weight = (w12.x * w0.y) + (w0.x * w12.y) + (w12.x * w12.y) + (w3.x * w12.y) + (w12.x * w3.y);

    return max(result / weight, float3(0.0, 0.0, 0.0));
}

// inputs.x : this frame (HDR) ; y : the history ; z : the scene's depth.
// params.x : 1 if the history is valid ; y : how much of this frame goes in (0.05 to 0.2).
[shader("fragment")]
float4 r3TaaPs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    int2 pixel = int2(input.position.xy);
    float3 current = g_textures[inputs.x].Load(int3(pixel, 0)).rgb;

    // The neighborhood's statistics, and its closest depth (moving edges keep their own history).
    float3 m1 = float3(0.0, 0.0, 0.0);
    float3 m2 = float3(0.0, 0.0, 0.0);
    float closest = 1.0;
    int2 closest_pixel = pixel;

    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            int2 p = clamp(pixel + int2(x, y), int2(0, 0), int2(frame.resolution.xy) - 1);
            float3 c = rgb_to_ycocg(g_textures[inputs.x].Load(int3(p, 0)).rgb);
            m1 += c;
            m2 += c * c;

            float d = g_textures[inputs.z].Load(int3(p, 0)).r;

            if (d < closest)
            {
                closest = d;
                closest_pixel = p;
            }
        }
    }

    if (params.x == 0.0)
    {
        return float4(current, 1.0);
    }

    // Where this point was in the previous frame.
    float2 uv = (float2(closest_pixel) + 0.5) * frame.resolution.zw;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 previous = mul_rows(frame.reprojection, float4(ndc, closest, 1.0));
    float2 previous_ndc = previous.xy / previous.w;
    float2 motion = float2(ndc.x - previous_ndc.x, previous_ndc.y - ndc.y) * 0.5; // in UV
    float2 history_uv = input.uv - motion;

    if (any(history_uv < 0.0) || any(history_uv > 1.0))
    {
        return float4(current, 1.0);
    }

    float3 history = rgb_to_ycocg(history_catmull_rom(inputs.y, history_uv, frame.resolution.xy));

    // Variance clipping : the history pulled toward the neighborhood's box, along the line to its center.
    float3 mean = m1 / 9.0;
    float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, float3(0.0, 0.0, 0.0)));
    float3 extent = sigma * 1.25 + 1e-4;
    float3 offset = history - mean;
    float3 units = abs(offset / extent);
    float largest = max(units.x, max(units.y, units.z));

    if (largest > 1.0)
    {
        history = mean + offset / largest;
    }

    history = ycocg_to_rgb(history);

    // Blended with weights against brightness (Karis), so lone bright samples don't flicker.
    float alpha = params.y;
    float wc = alpha / (1.0 + max(current.r, max(current.g, current.b)));
    float wh = (1.0 - alpha) / (1.0 + max(history.r, max(history.g, history.b)));

    return float4((current * wc + history * wh) / (wc + wh), 1.0);
}

// --- Contrast adaptive sharpening (after AMD FidelityFX CAS) --------------------------------------------
//
// Sharpens each pixel against its four neighbors, less where they already differ a lot : details come
// back after TAA's blending without halos around edges. inputs.x : the LDR image ; params.xy : its texel ;
// params.z : sharpness, 0 to 1.
[shader("fragment")]
float4 r3SharpenPs(ScreenVaryings input) : SV_Target
{
    SamplerState s = g_sampler_linear;
    float2 t = params.xy;
    float3 e = g_textures[inputs.x].SampleLevel(s, input.uv, 0.0).rgb;
    float3 b = g_textures[inputs.x].SampleLevel(s, input.uv + float2(0.0, -t.y), 0.0).rgb;
    float3 d = g_textures[inputs.x].SampleLevel(s, input.uv + float2(-t.x, 0.0), 0.0).rgb;
    float3 f = g_textures[inputs.x].SampleLevel(s, input.uv + float2(t.x, 0.0), 0.0).rgb;
    float3 h = g_textures[inputs.x].SampleLevel(s, input.uv + float2(0.0, t.y), 0.0).rgb;

    float3 lo = min(e, min(min(b, d), min(f, h)));
    float3 hi = max(e, max(max(b, d), max(f, h)));
    float3 amount = sqrt(saturate(min(lo, 2.0 - hi) / max(hi, 1e-4)));
    float3 w = amount * (-1.0 / lerp(8.0, 5.0, saturate(params.z)));

    float3 color = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);
    return float4(saturate(color), 1.0);
}

// --- MSAA resolves (Forward+) ---------------------------------------------------------------------

// The scene's depth for the screen-space passes : the closest of each pixel's samples, a depth that
// exists, where averaging them would float halfway between an edge and what's behind it.
// inputs.x : the multisampled depth ; params.x : its sample count.
[shader("fragment")]
float4 r3ResolveDepthPs(ScreenVaryings input) : SV_Target
{
    int2 pixel = int2(input.position.xy);
    int samples = int(params.x);
    float depth = 1.0;

    for (int s = 0; s < samples; s++)
    {
        depth = min(depth, g_textures_ms[inputs.x].Load(pixel, s).r);
    }

    return float4(depth, 0.0, 0.0, 1.0);
}

// The HDR image : samples averaged in a tone-mapped space (Karis), so that one very bright sample on an
// edge doesn't swallow the others and bring the aliasing back. inputs.x : the multisampled color.
[shader("fragment")]
float4 r3ResolveColorPs(ScreenVaryings input) : SV_Target
{
    int2 pixel = int2(input.position.xy);
    int samples = int(params.x);
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight_sum = 0.0;

    for (int s = 0; s < samples; s++)
    {
        float3 c = g_textures_ms[inputs.x].Load(pixel, s).rgb;
        float weight = 1.0 / (1.0 + max(c.r, max(c.g, c.b)));
        sum += c * weight;
        weight_sum += weight;
    }

    return float4(sum / max(weight_sum, 1e-6), 1.0);
}

// --- Ambient occlusion : GTAO (Jimenez et al.) from depth alone ------------------------------------

[shader("fragment")]
float4 r3AoPs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    uint depth_texture = inputs.x;
    int2 pixel = int2(input.position.xy);
    float depth = load_depth(depth_texture, pixel);

    if (depth >= 1.0)
    {
        return float4(1.0, 1.0, 1.0, 1.0);
    }

    float2 texel = frame.resolution.zw;
    float3 center = view_position(frame, input.uv, depth);

    // Normal from depth, picking on each axis the neighbor on the same surface.
    float depth_l = load_depth(depth_texture, pixel + int2(-1, 0));
    float depth_r = load_depth(depth_texture, pixel + int2(1, 0));
    float depth_t = load_depth(depth_texture, pixel + int2(0, -1));
    float depth_b = load_depth(depth_texture, pixel + int2(0, 1));

    float3 dx = abs(depth_l - depth) < abs(depth_r - depth)
        ? center - view_position(frame, input.uv + float2(-texel.x, 0.0), depth_l)
        : view_position(frame, input.uv + float2(texel.x, 0.0), depth_r) - center;
    float3 dy = abs(depth_t - depth) < abs(depth_b - depth)
        ? center - view_position(frame, input.uv + float2(0.0, -texel.y), depth_t)
        : view_position(frame, input.uv + float2(0.0, texel.y), depth_b) - center;

    float3 normal = normalize(cross(dy, dx));
    float3 view_dir = normalize(-center);

    if (dot(normal, view_dir) < 0.0)
    {
        normal = -normal;
    }

    float radius = params.x;
    int slices = int(params.y);
    int steps = int(params.z);
    float noise = frame_noise(frame, input.position.xy);
    float noise2 = frac(noise * 7.3 + 0.37);

    // The radius projected on screen, in UV units along Y.
    float4 projected = mul_rows(frame.inverse_projection, float4(0.0, 1.0, 0.5, 1.0));
    float tan_half_fov = abs(projected.y / max(1e-5, abs(projected.z)));
    float screen_radius = 0.5 * radius / (max(0.05, -center.z) * tan_half_fov);
    screen_radius = min(screen_radius, 0.25);

    float visibility = 0.0;

    for (int s = 0; s < slices; s++)
    {
        float phi = PI * (float(s) + noise) / float(slices);
        float2 omega = float2(cos(phi), sin(phi));
        float3 slice_dir = float3(omega.x, -omega.y, 0.0); // v goes down, view-space Y goes up
        float3 ortho_dir = slice_dir - dot(slice_dir, view_dir) * view_dir;
        float3 axis = normalize(cross(ortho_dir, view_dir));
        float3 projected_normal = normal - axis * dot(normal, axis);
        float projected_length = length(projected_normal);
        float cos_n = clamp(dot(projected_normal / max(projected_length, 1e-5), view_dir), -1.0, 1.0);
        float n = (dot(projected_normal, ortho_dir) < 0.0 ? 1.0 : -1.0) * acos(cos_n);

        float horizon1 = -1.0;
        float horizon2 = -1.0;

        for (int k = 0; k < steps; k++)
        {
            float t = (float(k) + noise2 + 0.5) / float(steps);
            float2 offset = omega * screen_radius * t * t * float2(frame.resolution.y * frame.resolution.z, 1.0);

            float2 uv1 = input.uv + offset;
            float2 uv2 = input.uv - offset;
            float3 d1 = view_position(frame, uv1, load_depth(depth_texture, int2(uv1 * frame.resolution.xy))) - center;
            float3 d2 = view_position(frame, uv2, load_depth(depth_texture, int2(uv2 * frame.resolution.xy))) - center;

            float l1 = length(d1);
            float l2 = length(d2);
            float falloff1 = saturate(l1 / radius - 0.6) * 2.5; // beyond the radius, a sample stops occluding
            float falloff2 = saturate(l2 / radius - 0.6) * 2.5;

            horizon1 = max(horizon1, lerp(dot(d1 / max(l1, 1e-4), view_dir), -1.0, saturate(falloff1)));
            horizon2 = max(horizon2, lerp(dot(d2 / max(l2, 1e-4), view_dir), -1.0, saturate(falloff2)));
        }

        float h1 = -acos(clamp(horizon2, -1.0, 1.0));
        float h2 = acos(clamp(horizon1, -1.0, 1.0));
        h1 = n + clamp(h1 - n, -PI * 0.5, PI * 0.5);
        h2 = n + clamp(h2 - n, -PI * 0.5, PI * 0.5);

        float sin_n = sin(n);
        visibility += projected_length * 0.25 * ((cos_n + 2.0 * h1 * sin_n - cos(2.0 * h1 - n)) + (cos_n + 2.0 * h2 * sin_n - cos(2.0 * h2 - n)));
    }

    float ao = saturate(visibility / float(slices));
    ao = pow(ao, params.w);

    return float4(ao, ao, ao, 1.0);
}

// Separable, depth-aware blur of the noisy AO. params.xy : the pass's direction, one texel long.
[shader("fragment")]
float4 r3AoBlurPs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    uint ao_texture = inputs.x;
    uint depth_texture = inputs.y;
    int2 pixel = int2(input.position.xy);
    int2 direction = int2(params.xy);
    float depth = load_depth(depth_texture, pixel);
    float center_z = view_position(frame, input.uv, depth).z;

    float total = 0.0;
    float weight_sum = 0.0;

    for (int i = -3; i <= 3; i++)
    {
        int2 p = pixel + direction * i;
        float d = load_depth(depth_texture, p);
        float2 uv = (float2(p) + 0.5) * frame.resolution.zw;
        float z = view_position(frame, uv, d).z;
        float weight = exp(-abs(z - center_z) * 4.0 / max(0.1, abs(center_z) * 0.05)) * (1.0 - abs(float(i)) * 0.15);

        total += g_textures[ao_texture].Load(int3(p, 0)).r * weight;
        weight_sum += weight;
    }

    float ao = total / max(weight_sum, 1e-4);
    return float4(ao, ao, ao, 1.0);
}

// --- Bloom (Jimenez, "Next generation post processing in Call of Duty") ----------------------------

float3 bloom_sample(uint texture, float2 uv)
{
    return g_textures[texture].SampleLevel(g_sampler_linear, uv, 0.0).rgb;
}

[shader("fragment")]
float4 r3BloomDownPs(ScreenVaryings input) : SV_Target
{
    uint source = inputs.x;
    float2 t = params.xy; // the source's texel
    float2 uv = input.uv;

    float3 a = bloom_sample(source, uv + t * float2(-2.0, -2.0));
    float3 b = bloom_sample(source, uv + t * float2(0.0, -2.0));
    float3 c = bloom_sample(source, uv + t * float2(2.0, -2.0));
    float3 d = bloom_sample(source, uv + t * float2(-2.0, 0.0));
    float3 e = bloom_sample(source, uv);
    float3 f = bloom_sample(source, uv + t * float2(2.0, 0.0));
    float3 g = bloom_sample(source, uv + t * float2(-2.0, 2.0));
    float3 h = bloom_sample(source, uv + t * float2(0.0, 2.0));
    float3 i = bloom_sample(source, uv + t * float2(2.0, 2.0));
    float3 j = bloom_sample(source, uv + t * float2(-1.0, -1.0));
    float3 k = bloom_sample(source, uv + t * float2(1.0, -1.0));
    float3 l = bloom_sample(source, uv + t * float2(-1.0, 1.0));
    float3 m = bloom_sample(source, uv + t * float2(1.0, 1.0));

    float3 color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;

    // params.z : the brightest a source pixel may be, against fireflies flickering in the bloom.
    return float4(min(color, float3(params.z, params.z, params.z)), 1.0);
}

[shader("fragment")]
float4 r3BloomUpPs(ScreenVaryings input) : SV_Target
{
    uint source = inputs.x;
    float2 t = params.xy; // the source's texel, scaled by the filter's radius
    float2 uv = input.uv;

    float3 color = bloom_sample(source, uv) * 4.0;
    color += (bloom_sample(source, uv + t * float2(-1.0, 0.0)) + bloom_sample(source, uv + t * float2(1.0, 0.0)) +
        bloom_sample(source, uv + t * float2(0.0, -1.0)) + bloom_sample(source, uv + t * float2(0.0, 1.0))) * 2.0;
    color += bloom_sample(source, uv + t * float2(-1.0, -1.0)) + bloom_sample(source, uv + t * float2(1.0, -1.0)) +
        bloom_sample(source, uv + t * float2(-1.0, 1.0)) + bloom_sample(source, uv + t * float2(1.0, 1.0));

    return float4(color / 16.0, 1.0);
}

// --- Tone mapping and finishing --------------------------------------------------------------------

// Stephen Hill's fit of the ACES reference rendering transform.
float3 aces_fitted(float3 c)
{
    float3 v = float3(0.59719 * c.r + 0.35458 * c.g + 0.04823 * c.b,
                      0.07600 * c.r + 0.90834 * c.g + 0.01566 * c.b,
                      0.02840 * c.r + 0.13383 * c.g + 0.83777 * c.b);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
    float3 r = a / b;
    return float3(1.60475 * r.r - 0.53108 * r.g - 0.07367 * r.b,
                  -0.10256 * r.r + 1.10813 * r.g - 0.00605 * r.b,
                  -0.00327 * r.r - 0.07276 * r.g + 1.07602 * r.b);
}

// Khronos PBR Neutral : keeps base colors faithful.
float3 khronos_neutral(float3 color)
{
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));

    if (peak < 0.76)
    {
        return color;
    }

    float d = 1.0 - 0.76;
    float new_peak = 1.0 - d * d / (peak + d - 0.76);
    color *= new_peak / peak;

    float g = 1.0 - 1.0 / (0.15 * (peak - new_peak) + 1.0);
    return lerp(color, float3(new_peak, new_peak, new_peak), g);
}

float3 linear_to_srgb(float3 c)
{
    float3 low = c * 12.92;
    float3 high = 1.055 * pow(max(c, float3(1e-6, 1e-6, 1e-6)), float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return float3(c.r <= 0.0031308 ? low.r : high.r, c.g <= 0.0031308 ? low.g : high.g, c.b <= 0.0031308 ? low.b : high.b);
}

// params : x exposure ; y bloom strength ; z tone mapper ; w contrast
// params2 : x saturation ; y vignette
[shader("fragment")]
float4 r3TonemapPs(ScreenVaryings input) : SV_Target
{
    float3 color = g_textures[inputs.x].SampleLevel(g_sampler_point, input.uv, 0.0).rgb;

    if (inputs.y != NO_TEXTURE)
    {
        float3 bloom = g_textures[inputs.y].SampleLevel(g_sampler_linear, input.uv, 0.0).rgb;
        color = lerp(color, bloom, params.y);
    }

    color *= params.x;

    int tonemapper = int(params.z);

    if (tonemapper == 0)
    {
        color = aces_fitted(color);
    }
    else if (tonemapper == 1)
    {
        color = khronos_neutral(color);
    }
    else if (tonemapper == 2)
    {
        color = color / (1.0 + color);
    }

    color = linear_to_srgb(saturate(color));

    // Inigo Quilez's finishing : a soft S-curve, saturation, and a vignette.
    color = lerp(color, color * color * (3.0 - 2.0 * color), params.w);
    float luma = dot(color, float3(0.2126, 0.7152, 0.0722));
    color = lerp(float3(luma, luma, luma), color, params2.x);

    float2 q = input.uv;
    float vignette = pow(16.0 * q.x * q.y * (1.0 - q.x) * (1.0 - q.y), 0.25);
    color *= lerp(1.0, vignette, params2.y);

    color = saturate(color);

    return float4(color, dot(color, float3(0.299, 0.587, 0.114)));
}

// Timothy Lottes' FXAA : the tone-mapped image, its luma in alpha. inputs.x : image ; params.xy : its texel ;
// params.z : 0 to pass through.
[shader("fragment")]
float4 r3FxaaPs(ScreenVaryings input) : SV_Target
{
    uint source = inputs.x;
    SamplerState s = g_sampler_linear;
    float2 uv = input.uv;
    float2 t = params.xy;

    float4 center = g_textures[source].SampleLevel(s, uv, 0.0);

    if (params.z == 0.0)
    {
        return float4(center.rgb, 1.0);
    }

    float luma_nw = g_textures[source].SampleLevel(s, uv + float2(-t.x, -t.y), 0.0).a;
    float luma_ne = g_textures[source].SampleLevel(s, uv + float2(t.x, -t.y), 0.0).a;
    float luma_sw = g_textures[source].SampleLevel(s, uv + float2(-t.x, t.y), 0.0).a;
    float luma_se = g_textures[source].SampleLevel(s, uv + float2(t.x, t.y), 0.0).a;
    float luma_m = center.a;

    float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));

    float2 dir = float2(-((luma_nw + luma_ne) - (luma_sw + luma_se)), (luma_nw + luma_sw) - (luma_ne + luma_se));
    float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);
    float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, float2(-8.0, -8.0), float2(8.0, 8.0)) * t;

    float3 rgb_a = 0.5 * (g_textures[source].SampleLevel(s, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
        g_textures[source].SampleLevel(s, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    float3 rgb_b = rgb_a * 0.5 + 0.25 * (g_textures[source].SampleLevel(s, uv + dir * -0.5, 0.0).rgb +
        g_textures[source].SampleLevel(s, uv + dir * 0.5, 0.0).rgb);
    float luma_b = dot(rgb_b, float3(0.299, 0.587, 0.114));

    return float4((luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b, 1.0);
}

