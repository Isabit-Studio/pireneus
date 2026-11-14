// The photorealistic renderer's shaders (renderer/photoreal), shared by both backends.
//
// The architecture is The Forge's triangle visibility buffer : a first pass writes only "which
// triangle covers this pixel", and a single full-screen pass then fetches that triangle's vertices,
// recomputes its barycentrics and derivatives, and shades. Every pixel is shaded exactly once, whatever
// the overdraw. All resources are bindless (see renderer/gpu.h). Lighting is physically based, as in The
// Forge's PBR samples (GGX, split-sum image-based lighting) ; the outdoors treatment comes from
// Inigo Quilez's articles : sun / sky / bounce light rig, analytic height fog with sun in-scattering,
// and his finishing touches (contrast curve, saturation, vignette).
//
// Matrices are float4 ROWS applied with explicit dot products, as in mesh.slang.

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[] : register(t0, space1);
[[vk::binding(1, 0)]] ByteAddressBuffer g_buffers[] : register(t0, space2);
// Separate variables, not an array : D3D12's static samplers can't back a shader array.
[[vk::binding(2, 0)]] SamplerState g_sampler_point : register(s0, space1);
[[vk::binding(3, 0)]] SamplerState g_sampler_linear : register(s1, space1);
[[vk::binding(4, 0)]] SamplerState g_sampler_anisotropic : register(s2, space1);
[[vk::binding(5, 0)]] SamplerState g_sampler_panorama : register(s3, space1);

static const float PI = 3.14159265359;
static const uint NO_TEXTURE = 0xFFFFFFFFu;
static const uint EMPTY_PIXEL = 0xFFFFFFFFu;
static const uint PRIMITIVE_BITS = 20u; // the visibility buffer's ID : 12 bits of draw, 20 bits of triangle

[vk::push_constant]
cbuffer Push
{
    uint4 ids;      // x : frame buffer ; y : frame data offset ; z : instances offset ; w : draw index
    uint4 inputs;   // the pass's textures
    float4 params;
    float4 params2;
};

// Mirrors PhotorealFrame in photoreal.c, field for field.
struct Frame
{
    float4 view_projection[4];
    float4 inverse_view_projection[4];
    float4 view[4];
    float4 inverse_projection[4];
    float4 camera_position;       // w : time
    float4 sun_direction;         // xyz : toward the sun ; w : tangent of its angular radius
    float4 sun_color;             // rgb : irradiance at normal incidence ; w : radiance of the visible disk
    float4 sh[9];                 // irradiance / PI, as spherical harmonics
    float4 environment;           // x : intensity ; y : prefiltered mips ; z : bounce light ; w : unused
    uint4 environment_textures;   // x : panorama ; y : prefiltered ; z : BRDF LUT ; w : ambient occlusion
    float4 fog_inscatter;         // rgb : color ; w : density
    float4 fog_sun;               // rgb : color toward the sun ; w : height falloff
    float4 fog_params;            // x : base height ; y : sun power
    float4 shadow_view_projection[16];
    float4 cascade_far;
    uint4 shadow_textures;
    float4 shadow_texel_world;    // world size of a texel, per cascade
    float4 shadow_depth_range;    // world depth covered by [0, 1], per cascade
    float4 shadow_params;         // x : normal offset (texels) ; y : samples ; z : cascades ; w : softness
    float4 resolution;            // xy ; zw : 1 / xy
};

// Mirrors PhotorealInstance in photoreal.c.
struct Instance
{
    float4 model[3];
    float4 normal_matrix[3];
    uint4 geometry;      // x : vertices ; y : indices ; z : first index ; w : flags (1 = alpha masked, 2 = no vertex colors)
    float4 base_color;
    float4 emissive;     // w : metallic
    float4 surface;      // x : roughness ; y : normal scale ; z : occlusion strength ; w : alpha cutoff
    uint4 textures;      // x : base color ; y : normal ; z : metallic-roughness ; w : emissive
    uint4 textures2;     // x : occlusion
    float4 uv_transform; // xy : scale ; zw : offset
};

static const uint INSTANCE_SIZE = 208u;

Frame load_frame()
{
    return g_buffers[ids.x].Load<Frame>(ids.y);
}

Instance load_instance(uint index)
{
    return g_buffers[NonUniformResourceIndex(ids.x)].Load<Instance>(ids.z + index * INSTANCE_SIZE);
}

float4 mul_rows(float4 m[4], float4 v)
{
    return float4(dot(m[0], v), dot(m[1], v), dot(m[2], v), dot(m[3], v));
}

float3 to_world(Instance instance, float3 p)
{
    float4 v = float4(p, 1.0);
    return float3(dot(instance.model[0], v), dot(instance.model[1], v), dot(instance.model[2], v));
}

float3 normal_to_world(Instance instance, float3 n)
{
    return normalize(float3(dot(instance.normal_matrix[0].xyz, n), dot(instance.normal_matrix[1].xyz, n), dot(instance.normal_matrix[2].xyz, n)));
}

struct VertexData
{
    float3 position;
    float3 normal;
    float4 color;
    float2 uv;
    float4 tangent;
};

// A MeshVertex (renderer.h) : 64 bytes.
VertexData load_vertex(uint buffer, uint index)
{
    uint base = index * 64u;
    float4 a = asfloat(g_buffers[NonUniformResourceIndex(buffer)].Load4(base));
    float4 b = asfloat(g_buffers[NonUniformResourceIndex(buffer)].Load4(base + 16u));
    float4 c = asfloat(g_buffers[NonUniformResourceIndex(buffer)].Load4(base + 32u));
    float4 d = asfloat(g_buffers[NonUniformResourceIndex(buffer)].Load4(base + 48u));

    VertexData vertex;
    vertex.position = a.xyz;
    vertex.normal = float3(a.w, b.x, b.y);
    vertex.color = float4(b.z, b.w, c.x, c.y);
    vertex.uv = c.zw;
    vertex.tangent = d;
    return vertex;
}

// --- Shadow and visibility passes -----------------------------------------------------------------

[shader("vertex")]
float4 prShadowVs(uint vertex_id : SV_VertexID) : SV_Position
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
ShadowVaryings prShadowMaskedVs(uint vertex_id : SV_VertexID)
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
void prShadowMaskedPs(ShadowVaryings input)
{
    Instance instance = load_instance(ids.w);
    float alpha = g_textures[NonUniformResourceIndex(instance.textures.x)].SampleLevel(g_sampler_anisotropic, input.uv, 0.0).a * instance.base_color.a;

    if (alpha < instance.surface.w)
    {
        discard;
    }
}

struct VisibilityVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
VisibilityVaryings prVisibilityVs(uint vertex_id : SV_VertexID)
{
    Frame frame = load_frame();
    Instance instance = load_instance(ids.w);
    VertexData vertex = load_vertex(instance.geometry.x, vertex_id);

    VisibilityVaryings output;
    output.position = mul_rows(frame.view_projection, float4(to_world(instance, vertex.position), 1.0));
    output.uv = vertex.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    return output;
}

[shader("fragment")]
float4 prVisibilityPs(VisibilityVaryings input, uint primitive : SV_PrimitiveID) : SV_Target
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

// --- Full-screen passes ---------------------------------------------------------------------------

struct ScreenVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
ScreenVaryings prScreenVs(uint vertex_id : SV_VertexID)
{
    float2 ndc = float2(vertex_id == 1u ? 3.0 : -1.0, vertex_id == 2u ? 3.0 : -1.0);

    ScreenVaryings output;
    output.position = float4(ndc, 0.0, 1.0);
    output.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return output;
}

float interleaved_gradient_noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// --- Ambient occlusion : GTAO (Jimenez et al.) from depth alone ------------------------------------

float3 view_position(Frame frame, float2 uv, float depth)
{
    float4 p = mul_rows(frame.inverse_projection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return p.xyz / p.w;
}

float load_depth(uint texture, int2 pixel)
{
    return g_textures[texture].Load(int3(pixel, 0)).r;
}

[shader("fragment")]
float4 prAoPs(ScreenVaryings input) : SV_Target
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
    float noise = interleaved_gradient_noise(input.position.xy);
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
float4 prAoBlurPs(ScreenVaryings input) : SV_Target
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

float2 panorama_uv(float3 d)
{
    return float2(0.5 + atan2(d.x, -d.z) / (2.0 * PI), acos(clamp(d.y, -1.0, 1.0)) / PI);
}

float3 sh_irradiance(Frame frame, float3 n)
{
    return max(float3(0.0, 0.0, 0.0),
        frame.sh[0].xyz * 0.282095 +
        frame.sh[1].xyz * (0.488603 * n.y) +
        frame.sh[2].xyz * (0.488603 * n.z) +
        frame.sh[3].xyz * (0.488603 * n.x) +
        frame.sh[4].xyz * (1.092548 * n.x * n.y) +
        frame.sh[5].xyz * (1.092548 * n.y * n.z) +
        frame.sh[6].xyz * (0.315392 * (3.0 * n.z * n.z - 1.0)) +
        frame.sh[7].xyz * (1.092548 * n.x * n.z) +
        frame.sh[8].xyz * (0.546274 * (n.x * n.x - n.y * n.y)));
}

float d_ggx(float n_dot_h, float a)
{
    float a2 = a * a;
    float f = (n_dot_h * a2 - n_dot_h) * n_dot_h + 1.0;
    return a2 / (PI * f * f);
}

float v_smith_ggx(float n_dot_v, float n_dot_l, float a)
{
    float a2 = a * a;
    float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

float3 f_schlick(float3 f0, float v_dot_h)
{
    float f = pow(1.0 - v_dot_h, 5.0);
    return f0 + (1.0 - f0) * f;
}

float2 vogel_disk(int index, int count, float phi)
{
    float r = sqrt((float(index) + 0.5) / float(count));
    float angle = float(index) * 2.39996323 + phi;
    return r * float2(cos(angle), sin(angle));
}

// 1 = lit, 0 = in shadow : cascaded shadow maps with percentage-closer soft shadows, the penumbra widening
// with the distance to the occluder (contact hardening).
float sun_shadow(Frame frame, float3 world, float3 normal, float view_distance, float2 pixel)
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

    float texel_world = frame.shadow_texel_world[cascade];
    float depth_range = frame.shadow_depth_range[cascade];
    float3 offset_world = world + normal * (texel_world * frame.shadow_params.x);
    float4 p = float4(offset_world, 1.0);
    uint c = uint(cascade) * 4u;
    float4 clip = float4(dot(frame.shadow_view_projection[c], p), dot(frame.shadow_view_projection[c + 1u], p),
        dot(frame.shadow_view_projection[c + 2u], p), dot(frame.shadow_view_projection[c + 3u], p));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0)
    {
        return 1.0;
    }

    uint map = frame.shadow_textures[cascade];
    float receiver = ndc.z - (texel_world * 0.5) / depth_range;
    float texel_uv = texel_world == 0.0 ? 0.0 : 1.0 / frame.resolution.x;
    int samples = int(frame.shadow_params.y);
    float phi = interleaved_gradient_noise(pixel) * 2.0 * PI;
    float light_tan = frame.sun_direction.w * frame.shadow_params.w;

    // Texels per world unit, across the map : the map covers 2 * radius = texels * texel_world.
    uint map_w = 0u;
    uint map_h = 0u;
    g_textures[NonUniformResourceIndex(map)].GetDimensions(map_w, map_h);
    float texel = 1.0 / float(map_w);

    // Blocker search : how far is what shadows this point ?
    float search_world = light_tan * 20.0;
    float search_radius = clamp(search_world / texel_world, 2.0, 48.0) * texel;
    float blocker_sum = 0.0;
    float blocker_count = 0.0;

    for (int s = 0; s < 12; s++)
    {
        float2 offset = vogel_disk(s, 12, phi) * search_radius;
        float occluder = g_textures[NonUniformResourceIndex(map)].SampleLevel(g_sampler_point, uv + offset, 0.0).r;

        if (occluder < receiver)
        {
            blocker_sum += occluder;
            blocker_count += 1.0;
        }
    }

    if (blocker_count == 0.0)
    {
        return 1.0;
    }

    float distance_world = (receiver - blocker_sum / blocker_count) * depth_range;
    float penumbra_world = max(distance_world, 0.0) * light_tan * 2.0;
    float radius = clamp(penumbra_world / texel_world, 1.0, 48.0) * texel;

    float lit = 0.0;

    for (int s = 0; s < samples; s++)
    {
        float2 offset = vogel_disk(s, samples, phi) * radius;
        float occluder = g_textures[NonUniformResourceIndex(map)].SampleLevel(g_sampler_point, uv + offset, 0.0).r;
        lit += receiver <= occluder ? 1.0 : 0.0;
    }

    return lit / float(samples);
}

// Inigo Quilez, "better fog" : density falling off exponentially with height, integrated analytically
// along the view ray ; the fog takes the sun's color when looking toward it.
float3 apply_fog(Frame frame, float3 color, float3 origin, float3 direction, float distance)
{
    float density = frame.fog_inscatter.w;

    if (density <= 0.0)
    {
        return color;
    }

    float falloff = max(frame.fog_sun.w, 1e-4);
    float height = origin.y - frame.fog_params.x;
    float amount;

    if (abs(direction.y) < 1e-4)
    {
        amount = density * exp(-height * falloff) * distance;
    }
    else
    {
        amount = density * exp(-height * falloff) * (1.0 - exp(-distance * direction.y * falloff)) / (direction.y * falloff);
    }

    float sun_amount = pow(max(dot(direction, frame.sun_direction.xyz), 0.0), frame.fog_params.y);
    float3 fog_color = lerp(frame.fog_inscatter.xyz, frame.fog_sun.xyz, sun_amount);

    return lerp(color, fog_color, 1.0 - exp(-max(amount, 0.0)));
}

float3 sky_radiance(Frame frame, float3 direction)
{
    float2 uv = panorama_uv(direction);
    float3 color = g_textures[frame.environment_textures.x].SampleLevel(g_sampler_panorama, uv, 0.0).rgb * frame.environment.x;

    // The sun's disk, when the environment doesn't already contain it.
    float cos_angle = dot(direction, frame.sun_direction.xyz);
    float cos_radius = 1.0 / sqrt(1.0 + frame.sun_direction.w * frame.sun_direction.w);

    if (frame.sun_color.w > 0.0 && cos_angle > cos_radius)
    {
        color += frame.sun_color.rgb * frame.sun_color.w;
    }

    return color;
}

[shader("fragment")]
float4 prShadePs(ScreenVaryings input) : SV_Target
{
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
        float4 far_point = mul_rows(frame.inverse_view_projection, float4(ndc, 1.0, 1.0));
        float3 direction = normalize(far_point.xyz / far_point.w - eye);
        return float4(sky_radiance(frame, direction), 1.0);
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
    float3 world = w0 * l.x + w1 * l.y + w2 * l.z;
    float3 geometric_normal = normalize(cross(w1 - w0, w2 - w0));
    float3 normal = normal_to_world(instance, v0.normal * l.x + v1.normal * l.y + v2.normal * l.z);
    float4 vertex_color = v0.color * l.x + v1.color * l.y + v2.color * l.z;

    float2 uv0 = v0.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    float2 uv1 = v1.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    float2 uv2 = v2.uv * instance.uv_transform.xy + instance.uv_transform.zw;
    float2 uv = uv0 * l.x + uv1 * l.y + uv2 * l.z;
    float2 uv_ddx = uv0 * bary.ddx.x + uv1 * bary.ddx.y + uv2 * bary.ddx.z;
    float2 uv_ddy = uv0 * bary.ddy.x + uv1 * bary.ddy.y + uv2 * bary.ddy.z;

    float3 view_vector = eye - world;
    float view_length = length(view_vector);
    float3 V = view_vector / view_length;

    // Single-sided geometry seen from behind : shade its other side.
    if (dot(geometric_normal, V) < 0.0)
    {
        geometric_normal = -geometric_normal;
    }

    if (dot(normal, geometric_normal) < 0.0)
    {
        normal = -normal;
    }

    SamplerState material_sampler = g_sampler_anisotropic;

    // Vertex colors already include the imported material's base color factor.
    float3 albedo = (instance.geometry.w & 2u) != 0u ? float3(1.0, 1.0, 1.0) : vertex_color.rgb;

    if (instance.textures.x != NO_TEXTURE)
    {
        albedo *= g_textures[NonUniformResourceIndex(instance.textures.x)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).rgb;
    }

    albedo *= instance.base_color.rgb;

    float metallic = instance.emissive.w;
    float roughness = instance.surface.x;

    if (instance.textures.z != NO_TEXTURE)
    {
        float4 mr = g_textures[NonUniformResourceIndex(instance.textures.z)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy);
        roughness *= mr.g;
        metallic *= mr.b;
    }

    float3 N = normal;

    if (instance.textures.y != NO_TEXTURE)
    {
        float4 tangent = v0.tangent * l.x + v1.tangent * l.y + v2.tangent * l.z;
        float3 T = normal_to_world(instance, tangent.xyz);
        T = normalize(T - N * dot(T, N));
        float3 B = cross(N, T) * (tangent.w < 0.0 ? -1.0 : 1.0);
        float3 tn = g_textures[NonUniformResourceIndex(instance.textures.y)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).xyz * 2.0 - 1.0;
        tn.xy *= instance.surface.y;
        N = normalize(T * tn.x + B * tn.y + N * tn.z);
    }

    float3 emissive = instance.emissive.xyz;

    if (instance.textures.w != NO_TEXTURE)
    {
        emissive *= g_textures[NonUniformResourceIndex(instance.textures.w)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).rgb;
    }

    float occlusion = 1.0;

    if (instance.textures2.x != NO_TEXTURE)
    {
        float o = g_textures[NonUniformResourceIndex(instance.textures2.x)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).r;
        occlusion = lerp(1.0, o, instance.surface.z);
    }

    float screen_ao = frame.environment_textures.w != NO_TEXTURE ? g_textures[frame.environment_textures.w].Load(int3(pixel, 0)).r : 1.0;
    float ao = min(occlusion, screen_ao);

    roughness = clamp(roughness, 0.045, 1.0);
    float a = roughness * roughness;
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuse_color = albedo * (1.0 - metallic);
    float n_dot_v = clamp(dot(N, V), 1e-3, 1.0);

    // Split-sum image-based lighting, with Fdez-Aguera's multi-scattering compensation.
    float2 dfg = g_textures[frame.environment_textures.z].SampleLevel(g_sampler_linear, float2(n_dot_v, roughness), 0.0).rg;
    float3 R = reflect(-V, N);
    R = normalize(lerp(R, N, a)); // the dominant specular direction shifts toward the normal on rough surfaces

    float3 prefiltered = g_textures[frame.environment_textures.y].SampleLevel(g_sampler_panorama, panorama_uv(R), roughness * (frame.environment.y - 1.0)).rgb * frame.environment.x;
    float3 irradiance = sh_irradiance(frame, N) * frame.environment.x;

    float3 fss_ess = F0 * dfg.x + dfg.y;
    float ess = dfg.x + dfg.y;
    float ems = 1.0 - ess;
    float3 f_avg = F0 + (1.0 - F0) / 21.0;
    float3 fms_ems = fss_ess * f_avg * ems / (1.0 - f_avg * ems);
    float3 k_d = diffuse_color * (1.0 - fss_ess - fms_ems);

    // Specular occlusion (Lagarde) : AO barely dims what a smooth surface reflects at grazing angles.
    float specular_occlusion = saturate(pow(n_dot_v + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);

    float3 color = (fss_ess * prefiltered) * specular_occlusion + (fms_ems + k_d) * irradiance * ao;

    // Quilez's outdoors rig : what the sun bounces back from the ground, from the side opposite to it.
    float3 bounce_dir = normalize(frame.sun_direction.xyz * float3(-1.0, 0.0, -1.0) + float3(0.0, 1e-3, 0.0));
    float bounce = saturate(0.5 + 0.5 * dot(N, bounce_dir)) * saturate(1.0 - max(N.y, 0.0));
    color += diffuse_color * frame.sun_color.rgb * (frame.environment.z * bounce * ao / PI);

    // The sun.
    float3 L = frame.sun_direction.xyz;
    float n_dot_l = saturate(dot(N, L));

    if (n_dot_l > 0.0)
    {
        // Hanika, "Hacking the Shadow Terminator" : on a coarse smooth-shaded mesh, look the shadow up from
        // the point the vertex normals' tangent planes predict, not from the flat triangle. The
        // visibility buffer has the triangle at hand.
        float3 t0 = world - w0;
        float3 t1 = world - w1;
        float3 t2 = world - w2;
        float3 n0 = normal_to_world(instance, v0.normal);
        float3 n1 = normal_to_world(instance, v1.normal);
        float3 n2 = normal_to_world(instance, v2.normal);
        t0 -= min(0.0, dot(t0, n0)) * n0;
        t1 -= min(0.0, dot(t1, n1)) * n1;
        t2 -= min(0.0, dot(t2, n2)) * n2;
        float3 shadow_point = world + t0 * l.x + t1 * l.y + t2 * l.z;

        float shadow = sun_shadow(frame, shadow_point, geometric_normal, view_length, input.position.xy);

        if (shadow > 0.0)
        {
            float3 H = normalize(V + L);
            float n_dot_h = saturate(dot(N, H));
            float v_dot_h = saturate(dot(V, H));
            float3 F = f_schlick(F0, v_dot_h);
            float3 specular = F * (d_ggx(n_dot_h, a) * v_smith_ggx(n_dot_v, n_dot_l, a));
            specular *= 1.0 + F0 * (1.0 / max(ess, 1e-3) - 1.0); // energy lost to single scattering
            float3 diffuse = diffuse_color * (1.0 - F) / PI;

            color += (diffuse + specular) * frame.sun_color.rgb * (n_dot_l * shadow);
        }
    }

    color += emissive;
    color = apply_fog(frame, color, eye, -V, view_length);

    return float4(min(color, float3(60000.0, 60000.0, 60000.0)), 1.0);
}

// --- Bloom (Jimenez, "Next generation post processing in Call of Duty") ----------------------------

float3 bloom_sample(uint texture, float2 uv)
{
    return g_textures[texture].SampleLevel(g_sampler_linear, uv, 0.0).rgb;
}

[shader("fragment")]
float4 prBloomDownPs(ScreenVaryings input) : SV_Target
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
float4 prBloomUpPs(ScreenVaryings input) : SV_Target
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
float4 prTonemapPs(ScreenVaryings input) : SV_Target
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
float4 prFxaaPs(ScreenVaryings input) : SV_Target
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

// --- Path tracing -----------------------------------------------------------------------------------
// The reference mode, where the GPU has ray queries (The Forge's ray tracing samples ; Inigo Quilez's
// "simple path tracing" article) : every pixel accumulates full light paths, converging to the real
// solution of the rendering equation : soft shadows, reflections and bounced light come from the same rays.

[[vk::binding(6, 0)]] RaytracingAccelerationStructure g_tlas[2] : register(t0, space3);

struct Surface
{
    float3 position;
    float3 geometric_normal;
    float3 normal;
    float3 albedo;
    float3 emissive;
    float metallic;
    float roughness;
};

// What's at this point of this triangle : the shading pass's material fetch, at the top texture level.
Surface surface_at(uint draw, uint primitive, float2 barycentrics, float3 incoming)
{
    Instance instance = load_instance(draw);

    uint first = instance.geometry.z + primitive * 3u;
    uint index_buffer = instance.geometry.y;
    uint i0 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u);
    uint i1 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 4u);
    uint i2 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 8u);

    VertexData v0 = load_vertex(instance.geometry.x, i0);
    VertexData v1 = load_vertex(instance.geometry.x, i1);
    VertexData v2 = load_vertex(instance.geometry.x, i2);

    float3 l = float3(1.0 - barycentrics.x - barycentrics.y, barycentrics.x, barycentrics.y);

    float3 w0 = to_world(instance, v0.position);
    float3 w1 = to_world(instance, v1.position);
    float3 w2 = to_world(instance, v2.position);

    Surface surface;
    surface.position = w0 * l.x + w1 * l.y + w2 * l.z;
    surface.geometric_normal = normalize(cross(w1 - w0, w2 - w0));
    surface.normal = normal_to_world(instance, v0.normal * l.x + v1.normal * l.y + v2.normal * l.z);

    if (dot(surface.geometric_normal, incoming) > 0.0)
    {
        surface.geometric_normal = -surface.geometric_normal;
    }

    if (dot(surface.normal, surface.geometric_normal) < 0.0)
    {
        surface.normal = -surface.normal;
    }

    float2 uv = (v0.uv * l.x + v1.uv * l.y + v2.uv * l.z) * instance.uv_transform.xy + instance.uv_transform.zw;
    float4 vertex_color = v0.color * l.x + v1.color * l.y + v2.color * l.z;

    surface.albedo = ((instance.geometry.w & 2u) != 0u ? float3(1.0, 1.0, 1.0) : vertex_color.rgb) * instance.base_color.rgb;

    if (instance.textures.x != NO_TEXTURE)
    {
        surface.albedo *= g_textures[NonUniformResourceIndex(instance.textures.x)].SampleLevel(g_sampler_anisotropic, uv, 0.0).rgb;
    }

    surface.metallic = instance.emissive.w;
    surface.roughness = instance.surface.x;

    if (instance.textures.z != NO_TEXTURE)
    {
        float4 mr = g_textures[NonUniformResourceIndex(instance.textures.z)].SampleLevel(g_sampler_anisotropic, uv, 0.0);
        surface.roughness *= mr.g;
        surface.metallic *= mr.b;
    }

    if (instance.textures.y != NO_TEXTURE)
    {
        float4 tangent = v0.tangent * l.x + v1.tangent * l.y + v2.tangent * l.z;
        float3 T = normal_to_world(instance, tangent.xyz);
        T = normalize(T - surface.normal * dot(T, surface.normal));
        float3 B = cross(surface.normal, T) * (tangent.w < 0.0 ? -1.0 : 1.0);
        float3 tn = g_textures[NonUniformResourceIndex(instance.textures.y)].SampleLevel(g_sampler_anisotropic, uv, 0.0).xyz * 2.0 - 1.0;
        tn.xy *= instance.surface.y;
        surface.normal = normalize(T * tn.x + B * tn.y + surface.normal * tn.z);
    }

    surface.emissive = instance.emissive.xyz;

    if (instance.textures.w != NO_TEXTURE)
    {
        surface.emissive *= g_textures[NonUniformResourceIndex(instance.textures.w)].SampleLevel(g_sampler_anisotropic, uv, 0.0).rgb;
    }

    surface.roughness = clamp(surface.roughness, 0.045, 1.0);

    return surface;
}

struct RayHit
{
    bool hit;
    uint draw;
    uint primitive;
    float2 barycentrics;
};

// Alpha-masked materials : a triangle only stops a ray where its texture is solid, as in the visibility pass.
bool alpha_passes(uint draw, uint primitive, float2 barycentrics)
{
    Instance instance = load_instance(draw);

    uint first = instance.geometry.z + primitive * 3u;
    uint index_buffer = instance.geometry.y;
    uint i0 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u);
    uint i1 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 4u);
    uint i2 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 8u);

    float2 uv0 = load_vertex(instance.geometry.x, i0).uv;
    float2 uv1 = load_vertex(instance.geometry.x, i1).uv;
    float2 uv2 = load_vertex(instance.geometry.x, i2).uv;

    float2 uv = (uv0 * (1.0 - barycentrics.x - barycentrics.y) + uv1 * barycentrics.x + uv2 * barycentrics.y) * instance.uv_transform.xy + instance.uv_transform.zw;
    float alpha = g_textures[NonUniformResourceIndex(instance.textures.x)].SampleLevel(g_sampler_anisotropic, uv, 0.0).a * instance.base_color.a;

    return alpha >= instance.surface.w;
}

RayHit trace_closest(uint tlas, float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 1e-3;
    ray.TMax = 1e6;

    RayQuery<RAY_FLAG_NONE> query;

    // Constant indices : indexing an array of acceleration structures dynamically isn't guaranteed everywhere.
    if (tlas == 0u)
    {
        query.TraceRayInline(g_tlas[0], 0, 0xFF, ray);
    }
    else
    {
        query.TraceRayInline(g_tlas[1], 0, 0xFF, ray);
    }

    while (query.Proceed())
    {
        if (alpha_passes(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    RayHit result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.draw = 0u;
    result.primitive = 0u;
    result.barycentrics = float2(0.0, 0.0);

    if (result.hit)
    {
        result.draw = query.CommittedInstanceID();
        result.primitive = query.CommittedPrimitiveIndex();
        result.barycentrics = query.CommittedTriangleBarycentrics();
    }

    return result;
}

bool is_occluded(uint tlas, float3 origin, float3 direction)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 1e-3;
    ray.TMax = 1e6;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;

    if (tlas == 0u)
    {
        query.TraceRayInline(g_tlas[0], 0, 0xFF, ray);
    }
    else
    {
        query.TraceRayInline(g_tlas[1], 0, 0xFF, ray);
    }

    while (query.Proceed())
    {
        if (alpha_passes(query.CandidateInstanceID(), query.CandidatePrimitiveIndex(), query.CandidateTriangleBarycentrics()))
        {
            query.CommitNonOpaqueTriangleHit();
        }
    }

    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

uint pcg_hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float random(inout uint state)
{
    state = pcg_hash(state);
    return float(state) * (1.0 / 4294967296.0);
}

void orthonormal_basis(float3 n, out float3 t, out float3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// Heitz, "Sampling the GGX Distribution of Visible Normals" : a half vector, in the normal's local frame.
float3 sample_vndf(float3 v, float a, float u1, float u2)
{
    float3 vh = normalize(float3(a * v.x, a * v.y, v.z));
    float length_squared = vh.x * vh.x + vh.y * vh.y;
    float3 t1 = length_squared > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(length_squared) : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(vh, t1);

    float r = sqrt(u1);
    float phi = 2.0 * PI * u2;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;

    float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(float3(a * nh.x, a * nh.y, max(0.0, nh.z)));
}

float smith_g1(float n_dot_x, float a)
{
    float a2 = a * a;
    return 2.0 * n_dot_x / (n_dot_x + sqrt(a2 + (1.0 - a2) * n_dot_x * n_dot_x));
}

// What the environment sends back along a ray that escaped : without the sun's disk, which the direct
// light sampling already accounts for.
float3 environment_radiance(Frame frame, float3 direction)
{
    return g_textures[frame.environment_textures.y].SampleLevel(g_sampler_panorama, panorama_uv(direction), 0.0).rgb * frame.environment.x;
}

// inputs.x : the previous accumulation ; inputs.y : which acceleration structure.
// params : x sample index ; y bounces ; z samples per frame ; w the brightest a single path may be.
[shader("fragment")]
float4 prPathTracePs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    int2 pixel = int2(input.position.xy);
    uint tlas = inputs.y;
    int sample_index = int(params.x);
    int bounces = int(params.y);
    int samples = int(params.z);
    float firefly_limit = params.w;
    float3 eye = frame.camera_position.xyz;

    uint state = pcg_hash(uint(pixel.x) + uint(pixel.y) * 65536u + uint(sample_index) * 2654435761u);

    float3 total = float3(0.0, 0.0, 0.0);
    int valid_samples = 0;

    for (int s = 0; s < samples; s++)
    {
        // Jittered inside the pixel : the accumulation antialiases for free.
        float2 sub_pixel = input.position.xy + float2(random(state), random(state)) - 0.5;
        float2 ndc = float2(sub_pixel.x * frame.resolution.z * 2.0 - 1.0, 1.0 - sub_pixel.y * frame.resolution.w * 2.0);
        float4 far_point = mul_rows(frame.inverse_view_projection, float4(ndc, 1.0, 1.0));

        float3 origin = eye;
        float3 direction = normalize(far_point.xyz / far_point.w - eye);
        float3 primary_direction = direction;
        float3 radiance = float3(0.0, 0.0, 0.0);
        float3 visible_sky = float3(0.0, 0.0, 0.0);
        float3 throughput = float3(1.0, 1.0, 1.0);
        float primary_distance = 0.0;

        for (int bounce = 0; bounce <= bounces; bounce++)
        {
            RayHit hit = trace_closest(tlas, origin, direction);

            if (!hit.hit)
            {
                if (bounce == 0)
                {
                    visible_sky = sky_radiance(frame, direction); // with the sun's disk ; never a firefly
                }
                else
                {
                    radiance += throughput * environment_radiance(frame, direction);
                }

                break;
            }

            Surface surface = surface_at(hit.draw, hit.primitive, hit.barycentrics, direction);
            float3 N = surface.normal;
            float3 V = -direction;
            float n_dot_v = max(dot(N, V), 1e-4);
            float a = surface.roughness * surface.roughness;
            float3 F0 = lerp(float3(0.04, 0.04, 0.04), surface.albedo, surface.metallic);
            float3 diffuse_color = surface.albedo * (1.0 - surface.metallic);
            float3 offset_origin = surface.position + surface.geometric_normal * 2e-3;

            if (bounce == 0)
            {
                primary_distance = length(surface.position - eye);
            }

            radiance += throughput * surface.emissive;

            // Direct light : a ray toward a random point of the sun's disk.
            {
                float3 st;
                float3 sb;
                orthonormal_basis(frame.sun_direction.xyz, st, sb);

                float disk_r = sqrt(random(state)) * frame.sun_direction.w * frame.shadow_params.w;
                float disk_phi = 2.0 * PI * random(state);
                float3 L = normalize(frame.sun_direction.xyz + st * (disk_r * cos(disk_phi)) + sb * (disk_r * sin(disk_phi)));
                float n_dot_l = dot(N, L);

                if (n_dot_l > 0.0 && dot(surface.geometric_normal, L) > 0.0 && !is_occluded(tlas, offset_origin, L))
                {
                    float3 H = normalize(V + L);
                    float n_dot_h = saturate(dot(N, H));
                    float v_dot_h = saturate(dot(V, H));
                    float3 F = f_schlick(F0, v_dot_h);
                    float3 specular = F * (d_ggx(n_dot_h, a) * v_smith_ggx(n_dot_v, n_dot_l, a));
                    float3 diffuse = diffuse_color * (1.0 - F) / PI;

                    radiance += throughput * (diffuse + specular) * frame.sun_color.rgb * n_dot_l;
                }
            }

            if (bounce == bounces)
            {
                break;
            }

            // The next direction : the specular lobe or the diffuse one, chosen by their likely energy.
            float3 F_view = f_schlick(F0, n_dot_v);
            float specular_energy = dot(F_view, float3(0.333, 0.333, 0.334));
            float diffuse_energy = dot(diffuse_color * (1.0 - F_view), float3(0.333, 0.333, 0.334));
            float specular_probability = clamp(specular_energy / max(specular_energy + diffuse_energy, 1e-4), 0.05, 0.95);

            float3 tangent;
            float3 bitangent;
            orthonormal_basis(N, tangent, bitangent);

            if (random(state) < specular_probability)
            {
                float3 local_v = float3(dot(V, tangent), dot(V, bitangent), n_dot_v);
                float3 local_h = sample_vndf(local_v, a, random(state), random(state));
                float3 H = tangent * local_h.x + bitangent * local_h.y + N * local_h.z;
                float3 L = reflect(-V, H);
                float n_dot_l = dot(N, L);

                if (n_dot_l <= 0.0)
                {
                    break;
                }

                float3 F = f_schlick(F0, saturate(dot(V, H)));
                throughput *= F * (smith_g1(n_dot_l, a) / specular_probability);
                direction = L;
            }
            else
            {
                // Cosine-weighted, as in Quilez's article : the cosine and the PDF cancel out.
                float r = sqrt(random(state));
                float phi = 2.0 * PI * random(state);
                float3 L = normalize(tangent * (r * cos(phi)) + bitangent * (r * sin(phi)) + N * sqrt(max(0.0, 1.0 - r * r)));

                throughput *= diffuse_color * (1.0 - F_view) / (1.0 - specular_probability);
                direction = L;
            }

            if (dot(direction, surface.geometric_normal) <= 0.0)
            {
                break;
            }

            origin = offset_origin;

            // Russian roulette, past the first bounces.
            if (bounce >= 2)
            {
                float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 1.0);

                if (random(state) > survive)
                {
                    break;
                }

                throughput /= survive;
            }
        }

        // A NaN or an infinity would stay in the running average forever : such a path is thrown away.
        if (any(isnan(radiance)) || any(isinf(radiance)))
        {
            continue;
        }

        // Against fireflies : a rare path carrying huge energy would take thousands of samples to average out.
        float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
        radiance = luminance > firefly_limit ? radiance * (firefly_limit / luminance) : radiance;

        if (primary_distance > 0.0)
        {
            radiance = apply_fog(frame, radiance, eye, primary_direction, primary_distance);
        }

        total += radiance + visible_sky;
        valid_samples++;
    }

    float3 previous = sample_index > 0 ? g_textures[inputs.x].Load(int3(pixel, 0)).rgb : float3(0.0, 0.0, 0.0);

    if (valid_samples == 0)
    {
        return float4(previous, 1.0);
    }

    float3 color = total / float(valid_samples);
    float weight = float(samples) / float(sample_index + samples);

    return float4(min(lerp(previous, color, weight), float3(60000.0, 60000.0, 60000.0)), 1.0);
}
