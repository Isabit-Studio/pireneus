// What every shader of the 3D renderers (renderer/render3d) shares, Forward+ and Deferred+ alike : the
// bindless tables (see renderer/gpu.h), the per-frame and per-object data, vertex fetching, and the
// whole lighting model, so both pipelines shade a surface identically.
//
// Lighting is physically based (GGX, split-sum image-based lighting, as in The Forge's PBR samples) ;
// the outdoors treatment comes from Inigo Quilez's articles : sun / sky / bounce light rig, analytic
// height fog with sun in-scattering.
//
// Matrices are float4 ROWS applied with explicit dot products.

#ifndef RENDER3D_COMMON_SLANG
#define RENDER3D_COMMON_SLANG

[[vk::binding(0, 0)]] Texture2D<float4> g_textures[] : register(t0, space1);
[[vk::binding(1, 0)]] ByteAddressBuffer g_buffers[] : register(t0, space2);
[[vk::binding(7, 0)]] Texture2DMS<float4> g_textures_ms[] : register(t0, space4); // multisampled targets
// Separate variables, not an array : D3D12's static samplers can't back a shader array.
[[vk::binding(2, 0)]] SamplerState g_sampler_point : register(s0, space1);
[[vk::binding(3, 0)]] SamplerState g_sampler_linear : register(s1, space1);
[[vk::binding(4, 0)]] SamplerState g_sampler_anisotropic : register(s2, space1);
[[vk::binding(5, 0)]] SamplerState g_sampler_panorama : register(s3, space1);

static const float PI = 3.14159265359;
static const uint NO_TEXTURE = 0xFFFFFFFFu;

[vk::push_constant]
cbuffer Push
{
    uint4 ids;      // x : frame buffer ; y : frame data offset ; z : instances offset ; w : draw index
    uint4 inputs;   // the pass's textures
    float4 params;
    float4 params2;
};

// Mirrors Render3DFrame in render3d_internal.h, field for field.
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
    float4 cluster_params;        // x, y : clusters per pixel across, down ; z, w : depth slice = log(depth) * z + w
    uint4 cluster_grid;           // x, y, z : clusters across, down, deep ; w : light count
    uint4 light_data;             // offsets in g_buffers[ids.x] : x lights ; y clusters ; z light indices ; w shadow tiles
    uint4 local_shadows;          // x : the atlas ; y : filter taps
    float4 reprojection[4];       // this frame's unjittered NDC and depth -> the previous frame's clip space
    float4 jitter;                // xy : this frame's sub-pixel offset, in NDC ; z : frame number
    float4 clouds;                // x : coverage (0 = none) ; y : density ; z : altitude ; w : 1 / scale
    float4 clouds_motion;         // xy : the wind, in m/s ; z : how dark their shadows are ; w : seconds
    float4 volumetrics;           // x : 1 if the volumetric pass replaces the analytic fog ; y : anisotropy ; z : intensity
    float4 probe_origin;          // xyz : the probe volume's corner ; w : its cells' size (0 : no volume)
    uint4 probe_grid;             // xyz : cells across X, Y, Z ; w : its buffer
    uint4 probe_layout;           // offsets in that buffer : x cell table ; y coarse probes ; z bricks
    uint4 reflection_probes;      // x : how many ; y : their offset in g_buffers[ids.x]
    float4 gi_origin;             // realtime GI : xyz the probe grid's corner ; w its spacing (0 : off)
    uint4 gi_grid;                // xyz : probes across X, Y, Z ; w : rays per probe
    uint4 gi_textures;            // the probes : red, green, blue coefficients, state
};

// Mirrors Render3DInstance in render3d_internal.h.
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
    uint4 lightmap;      // x : its UVs (a float2 per vertex) ; y : its texture ; z : 1 if lit by it
    float4 lightmap_transform; // xy : scale ; zw : offset, into its page
};

static const uint INSTANCE_SIZE = 240u;

// A vertex's lightmap UV, in the object's page.
float2 load_lightmap_uv(Instance instance, uint index)
{
    float2 uv = asfloat(g_buffers[NonUniformResourceIndex(instance.lightmap.x)].Load2(index * 8u));
    return uv * instance.lightmap_transform.xy + instance.lightmap_transform.zw;
}

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

// The clip-space position of a vertex, seen from the camera.
float4 clip_position(Frame frame, Instance instance, float3 position)
{
    return mul_rows(frame.view_projection, float4(to_world(instance, position), 1.0));
}

struct ScreenVaryings
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float interleaved_gradient_noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// The noise sampling patterns rotate with : a different one every frame with TAA (frame.jitter.z counts
// frames), which averages them away ; the same one otherwise, which doesn't flicker.
float frame_noise(Frame frame, float2 pixel)
{
    return interleaved_gradient_noise(pixel + frame.jitter.z * 5.588238);
}

float3 view_position(Frame frame, float2 uv, float depth)
{
    float4 p = mul_rows(frame.inverse_projection, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return p.xyz / p.w;
}

float load_depth(uint texture, int2 pixel)
{
    return g_textures[texture].Load(int3(pixel, 0)).r;
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
    float phi = frame_noise(frame, pixel) * 2.0 * PI;
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

// --- Procedural clouds ------------------------------------------------------------------------------
//
// A thin layer at a fixed altitude : two-dimensional fractal noise, drifting with the wind. Lit by looking
// a little toward the sun through the same noise (the denser it is there, the darker), with a forward
// scattering lobe for the silver lining. Cheap enough for every sky pixel, and the same density darkens
// the ground below : cloud shadows.

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float value_noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i);
    float b = hash12(i + float2(1.0, 0.0));
    float c = hash12(i + float2(0.0, 1.0));
    float d = hash12(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float cloud_fbm(float2 p)
{
    float value = 0.0;
    float amplitude = 0.5;

    for (int i = 0; i < 5; i++)
    {
        value += value_noise(p) * amplitude;
        p = float2(p.x * 1.6 - p.y * 1.2, p.x * 1.2 + p.y * 1.6) + 17.3; // rotated, so the octaves don't align
        amplitude *= 0.5;
    }

    return value;
}

// The layer's opacity at a point of its plane (world XZ).
float cloud_opacity(Frame frame, float2 xz)
{
    float coverage = frame.clouds.x;

    if (coverage <= 0.0)
    {
        return 0.0;
    }

    float2 p = (xz - frame.clouds_motion.xy * frame.clouds_motion.w) * frame.clouds.w;
    float n = cloud_fbm(p + cloud_fbm(p * 0.5) * 0.6); // a little domain warping : billows rather than blobs
    float d = saturate((n - (1.0 - coverage)) / max(coverage, 0.05));
    return 1.0 - exp(-d * frame.clouds.y * 4.0);
}

// How much the clouds let the sun through, down to a point.
float cloud_shadow(Frame frame, float3 world)
{
    float3 L = frame.sun_direction.xyz;

    if (frame.clouds.x <= 0.0 || frame.clouds_motion.z <= 0.0 || L.y <= 0.02 || world.y >= frame.clouds.z)
    {
        return 1.0;
    }

    float2 hit = world.xz + L.xz * ((frame.clouds.z - world.y) / L.y);
    return 1.0 - cloud_opacity(frame, hit) * frame.clouds_motion.z;
}

float henyey_greenstein(float cos_angle, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * cos_angle, 1e-4), 1.5));
}

// with_sun : false for what already has the sun as a light (realtime GI's rays), which would otherwise
// count it twice, and in a disk so small that a ray finding it now and then makes the light flicker.
float3 sky_radiance(Frame frame, float3 direction, bool with_sun = true)
{
    float2 uv = panorama_uv(direction);
    float3 color = g_textures[frame.environment_textures.x].SampleLevel(g_sampler_panorama, uv, 0.0).rgb * frame.environment.x;

    // The sun's disk, when the environment doesn't already contain it.
    float cos_angle = dot(direction, frame.sun_direction.xyz);
    float cos_radius = 1.0 / sqrt(1.0 + frame.sun_direction.w * frame.sun_direction.w);

    if (with_sun && frame.sun_color.w > 0.0 && cos_angle > cos_radius)
    {
        color += frame.sun_color.rgb * frame.sun_color.w;
    }

    float3 eye = frame.camera_position.xyz;

    if (frame.clouds.x > 0.0 && direction.y > 0.01 && eye.y < frame.clouds.z)
    {
        float t = (frame.clouds.z - eye.y) / direction.y;
        float2 hit = eye.xz + direction.xz * t;
        float opacity = cloud_opacity(frame, hit);

        if (opacity > 0.0)
        {
            // Self-shadowing : the layer's own opacity a little toward the sun.
            float3 L = frame.sun_direction.xyz;
            float toward_sun = cloud_opacity(frame, hit + L.xz * (120.0 / max(L.y, 0.2)));
            float lit = exp(-toward_sun * 2.5) * saturate(L.y * 4.0 + 0.2);

            float3 ambient = max(frame.sh[0].xyz * 0.282095, float3(0.0, 0.0, 0.0)) * frame.environment.x * 1.6;
            float phase = 0.6 + henyey_greenstein(cos_angle, 0.6) * 4.0;
            float3 cloud = ambient + frame.sun_color.rgb * (lit * phase * 0.08);

            // Fading into the haze toward the horizon.
            float fade = saturate(direction.y * 6.0) * exp(-t * 0.00002);
            color = lerp(color, cloud, opacity * fade);
        }
    }

    return color;
}

// Octahedral unit vectors (Cigolle et al.) : a normal in two numbers.
float2 encode_octahedron(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;

    if (n.z < 0.0)
    {
        e = (1.0 - abs(e.yx)) * float2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
    }

    return e;
}

float3 decode_octahedron(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = saturate(-n.z);
    n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

// --- Point and spot lights -------------------------------------------------------------------------
//
// Sorted on the CPU into clusters : the view cut into tiles across, and exponential slices in depth
// (Olsson's clustered shading, as id Tech 6 assigns them). Each cluster lists the lights reaching it.

// Mirrors R3Light in render3d_internal.h.
struct Light
{
    float4 position_range;  // w : range
    float4 color;           // rgb : color * intensity ; w : 1 / range^2
    float4 direction_type;  // xyz : where a spot points ; w : 0 point, 1 spot
    float4 spot_shadow;     // x, y : the cone's falloff, scale and offset on cos ; z : first shadow tile, < 0 : none
};

// Mirrors R3ShadowTile.
struct ShadowTile
{
    float4 view_projection[4];
    float4 rect;            // the tile in the atlas, in UV : offset xy, size zw
    float4 params;          // x : world size of a texel at 1 m from the light
};

static const uint LIGHT_SIZE = 64u;
static const uint SHADOW_TILE_SIZE = 96u;

Light load_light(Frame frame, uint index)
{
    return g_buffers[ids.x].Load<Light>(frame.light_data.x + index * LIGHT_SIZE);
}

ShadowTile load_shadow_tile(Frame frame, uint index)
{
    return g_buffers[ids.x].Load<ShadowTile>(frame.light_data.w + index * SHADOW_TILE_SIZE);
}

// The lights reaching this pixel's cluster : x first index, y count.
uint2 cluster_lights(Frame frame, float3 world, float2 pixel)
{
    float view_depth = -dot(frame.view[2], float4(world, 1.0));
    uint x = min(uint(pixel.x * frame.cluster_params.x), frame.cluster_grid.x - 1u);
    uint y = min(uint(pixel.y * frame.cluster_params.y), frame.cluster_grid.y - 1u);
    float slice = log(max(view_depth, 1e-4)) * frame.cluster_params.z + frame.cluster_params.w;
    uint z = uint(clamp(slice, 0.0, float(frame.cluster_grid.z - 1u)));
    uint cluster = (z * frame.cluster_grid.y + y) * frame.cluster_grid.x + x;
    return g_buffers[ids.x].Load2(frame.light_data.y + cluster * 8u);
}

// 1 = lit : the light's shadow tile (the cube face toward the point, for a point light), filtered.
float local_shadow(Frame frame, Light light, float3 world, float3 normal, float distance, float2 pixel)
{
    uint tile_index = uint(light.spot_shadow.z);

    if (light.direction_type.w < 0.5)
    {
        // The cube face : +X, -X, +Y, -Y, +Z, -Z.
        float3 d = world - light.position_range.xyz;
        float3 a = abs(d);
        uint face = a.x >= a.y && a.x >= a.z ? (d.x >= 0.0 ? 0u : 1u) : a.y >= a.z ? (d.y >= 0.0 ? 2u : 3u) : (d.z >= 0.0 ? 4u : 5u);
        tile_index += face;
    }

    ShadowTile tile = load_shadow_tile(frame, tile_index);

    // Pushed off the surface by a few texels of the tile, at this distance from the light.
    float4 p = float4(world + normal * (tile.params.x * distance * 2.0), 1.0);
    float4 clip = mul_rows(tile.view_projection, p);

    if (clip.w <= 0.0)
    {
        return 1.0;
    }

    float3 ndc = clip.xyz / clip.w;
    float2 local_uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

    if (any(local_uv < 0.0) || any(local_uv > 1.0) || ndc.z > 1.0)
    {
        return 1.0;
    }

    uint atlas = frame.local_shadows.x;
    uint atlas_w = 0u;
    uint atlas_h = 0u;
    g_textures[NonUniformResourceIndex(atlas)].GetDimensions(atlas_w, atlas_h);
    float texel = 1.0 / float(atlas_w);

    // Inside the tile, by half a texel : the filter never reads a neighbor's depth.
    float2 lo = tile.rect.xy + texel * 0.5;
    float2 hi = tile.rect.xy + tile.rect.zw - texel * 0.5;
    float2 uv = tile.rect.xy + local_uv * tile.rect.zw;

    int taps = int(frame.local_shadows.y);
    float phi = frame_noise(frame, pixel) * 2.0 * PI;
    float lit = 0.0;

    for (int s = 0; s < taps; s++)
    {
        float2 offset = vogel_disk(s, taps, phi) * (texel * 1.75);
        float occluder = g_textures[NonUniformResourceIndex(atlas)].SampleLevel(g_sampler_point, clamp(uv + offset, lo, hi), 0.0).r;
        lit += ndc.z <= occluder ? 1.0 : 0.0;
    }

    return lit / float(taps);
}

// --- Baked probe volume ---------------------------------------------------------------------------------
//
// An adaptive grid of irradiance probes (after Unity's adaptive probe volumes), baked with the lightmaps :
// coarse cells everywhere, and where there's geometry, a brick of 5 x 5 x 5 probes four times denser in
// the cell. Each probe is L1 spherical harmonics of irradiance / PI, already convolved with the cosine
// lobe, and a validity : probes buried in geometry don't count. What lights the objects that have no
// lightmap (models, anything moving), in both pipelines.

static const uint PROBE_SIZE = 64u;         // 3 float4 of coefficients (r, g, b), 1 float4 : validity in x
static const uint NO_BRICK = 0xFFFFFFFFu;

struct ProbeSample
{
    float3 irradiance;
    float weight;
};

void add_probe(Frame frame, uint offset, float3 n, float weight, inout ProbeSample s)
{
    uint buffer = frame.probe_grid.w;
    float4 r = asfloat(g_buffers[buffer].Load4(offset));
    float4 g = asfloat(g_buffers[buffer].Load4(offset + 16u));
    float4 b = asfloat(g_buffers[buffer].Load4(offset + 32u));
    float validity = asfloat(g_buffers[buffer].Load(offset + 48u));
    float4 basis = float4(0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x);

    float w = weight * validity;
    s.irradiance += max(float3(dot(r, basis), dot(g, basis), dot(b, basis)), float3(0.0, 0.0, 0.0)) * w;
    s.weight += w;
}

// Irradiance / PI at a point, facing n ; `found` stays false outside the volume (or among buried probes).
float3 probe_irradiance(Frame frame, float3 world, float3 n, out bool found)
{
    found = false;
    float cell_size = frame.probe_origin.w;

    if (cell_size <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    // Pushed along the normal a little : lookups don't reach into the wall behind a surface.
    float3 local = (world + n * (cell_size * 0.08) - frame.probe_origin.xyz) / cell_size;
    int3 cells = int3(frame.probe_grid.xyz);

    if (any(local < 0.0) || any(local >= float3(cells)))
    {
        return float3(0.0, 0.0, 0.0);
    }

    int3 cell = min(int3(floor(local)), cells - 1);
    float3 f = local - float3(cell);
    uint buffer = frame.probe_grid.w;
    uint cell_index = uint((cell.z * cells.y + cell.y) * cells.x + cell.x);
    uint brick = g_buffers[buffer].Load(frame.probe_layout.x + cell_index * 4u);

    ProbeSample s;
    s.irradiance = float3(0.0, 0.0, 0.0);
    s.weight = 0.0;

    if (brick != NO_BRICK)
    {
        // Inside the cell's brick : 4 fine intervals across.
        float3 fine = f * 4.0;
        int3 base = min(int3(floor(fine)), int3(3, 3, 3));
        float3 t = fine - float3(base);
        uint brick_offset = frame.probe_layout.z + brick * 125u * PROBE_SIZE;

        for (int i = 0; i < 8; i++)
        {
            int3 corner = base + int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            float3 w3 = lerp(1.0 - t, t, float3(corner - base));
            uint probe = uint((corner.z * 5 + corner.y) * 5 + corner.x);
            add_probe(frame, brick_offset + probe * PROBE_SIZE, n, w3.x * w3.y * w3.z, s);
        }
    }
    else
    {
        int3 corners = cells + 1;

        for (int i = 0; i < 8; i++)
        {
            int3 corner = cell + int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            float3 w3 = lerp(1.0 - f, f, float3(corner - cell));
            uint probe = uint((corner.z * corners.y + corner.y) * corners.x + corner.x);
            add_probe(frame, frame.probe_layout.y + probe * PROBE_SIZE, n, w3.x * w3.y * w3.z, s);
        }
    }

    if (s.weight <= 1e-4)
    {
        return float3(0.0, 0.0, 0.0);
    }

    found = true;
    return s.irradiance / s.weight;
}

// --- Realtime GI's probes (realtime_gi.slang updates them) --------------------------------------------

static const uint GI_TEXTURE_WIDTH = 128u; // probe p at texel (p % width, p / width)

float4 load_gi(uint texture, uint probe)
{
    return g_textures[NonUniformResourceIndex(texture)].Load(int3(probe % GI_TEXTURE_WIDTH, probe / GI_TEXTURE_WIDTH, 0));
}

// Irradiance / PI at a point, facing n : the eight probes around it, weighted by distance, validity, and
// how much they face the point's side (DDGI's wrap weight, against light leaking through thin walls).
float3 gi_irradiance_from(Frame frame, uint4 textures, float3 world, float3 n)
{
    float spacing = frame.gi_origin.w;
    int3 grid = int3(frame.gi_grid.xyz);
    float3 local = (world + n * (spacing * 0.2) - frame.gi_origin.xyz) / spacing;
    int3 base = clamp(int3(floor(local)), int3(0, 0, 0), grid - 2);
    float3 f = saturate(local - float3(base));

    float3 sum = float3(0.0, 0.0, 0.0);
    float weight_sum = 0.0;
    float4 basis = float4(0.282095, 0.488603 * n.y, 0.488603 * n.z, 0.488603 * n.x);

    for (int i = 0; i < 8; i++)
    {
        int3 corner = base + int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        float3 w3 = lerp(1.0 - f, f, float3(corner - base));
        uint probe = uint((corner.z * grid.y + corner.y) * grid.x + corner.x);
        float3 probe_position = frame.gi_origin.xyz + float3(corner) * spacing;
        float3 to_probe = normalize(probe_position - world + 1e-4);
        float wrap = (dot(to_probe, n) + 1.0) * 0.5;
        float w = w3.x * w3.y * w3.z * (wrap * wrap + 0.05) * load_gi(textures.w, probe).x;

        float3 value = float3(dot(load_gi(textures.x, probe), basis), dot(load_gi(textures.y, probe), basis), dot(load_gi(textures.z, probe), basis));
        sum += max(value, float3(0.0, 0.0, 0.0)) * w;
        weight_sum += w;
    }

    return weight_sum > 1e-4 ? sum / weight_sum : float3(0.0, 0.0, 0.0);
}

// --- Materials and lighting -----------------------------------------------------------------------

// What a pass knows of the surface at a pixel, before the material is applied : interpolated by the
// rasterizer (Forward+) or rebuilt from the visibility buffer's triangle (Deferred+).
struct SurfaceInput
{
    float3 position;
    float3 geometric_normal; // either side
    float3 normal;           // the interpolated vertex normal, world space
    float4 tangent;          // world space ; w : bitangent sign
    float4 vertex_color;
    float2 uv;
    float2 uv_ddx;           // the UV's screen derivatives, for texture filtering
    float2 uv_ddy;
    float2 lightmap_uv;      // if the instance has a lightmap
};

struct Surface
{
    float3 position;
    float3 geometric_normal; // toward the viewer
    float3 normal;           // shading normal : normal-mapped
    float3 albedo;
    float metallic;
    float roughness;
    float occlusion;         // the material's own
    float3 emissive;
    float3 baked;            // with a lightmap : its irradiance / PI, else x < 0
};

// The material, applied to a surface seen along V (from the surface toward the eye).
Surface evaluate_surface(Instance instance, SurfaceInput input, float3 V)
{
    Surface s;
    s.position = input.position;
    s.geometric_normal = normalize(input.geometric_normal);
    float3 normal = normalize(input.normal);

    // Single-sided geometry seen from behind : shade its other side.
    if (dot(s.geometric_normal, V) < 0.0)
    {
        s.geometric_normal = -s.geometric_normal;
    }

    if (dot(normal, s.geometric_normal) < 0.0)
    {
        normal = -normal;
    }

    SamplerState material_sampler = g_sampler_anisotropic;
    float2 uv = input.uv;
    float2 uv_ddx = input.uv_ddx;
    float2 uv_ddy = input.uv_ddy;

    // Vertex colors already include the imported material's base color factor.
    float3 albedo = (instance.geometry.w & 2u) != 0u ? float3(1.0, 1.0, 1.0) : input.vertex_color.rgb;

    if (instance.textures.x != NO_TEXTURE)
    {
        albedo *= g_textures[NonUniformResourceIndex(instance.textures.x)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).rgb;
    }

    s.albedo = albedo * instance.base_color.rgb;
    s.metallic = instance.emissive.w;
    s.roughness = instance.surface.x;

    if (instance.textures.z != NO_TEXTURE)
    {
        float4 mr = g_textures[NonUniformResourceIndex(instance.textures.z)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy);
        s.roughness *= mr.g;
        s.metallic *= mr.b;
    }

    s.normal = normal;

    if (instance.textures.y != NO_TEXTURE)
    {
        float3 T = normalize(input.tangent.xyz - normal * dot(input.tangent.xyz, normal));
        float3 B = cross(normal, T) * (input.tangent.w < 0.0 ? -1.0 : 1.0);
        // X and Y only, Z rebuilt : two-channel maps (BC5) hold no Z. geometry.w & 4 : DirectX's convention,
        // green pointing down.
        float3 tn;
        tn.xy = g_textures[NonUniformResourceIndex(instance.textures.y)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).xy * 2.0 - 1.0;
        tn.y = (instance.geometry.w & 4u) != 0u ? -tn.y : tn.y;
        tn.z = sqrt(saturate(1.0 - dot(tn.xy, tn.xy)));
        tn.xy *= instance.surface.y;
        s.normal = normalize(T * tn.x + B * tn.y + normal * tn.z);
    }

    s.emissive = instance.emissive.xyz;

    if (instance.textures.w != NO_TEXTURE)
    {
        s.emissive *= g_textures[NonUniformResourceIndex(instance.textures.w)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).rgb;
    }

    s.occlusion = 1.0;

    if (instance.textures2.x != NO_TEXTURE)
    {
        float o = g_textures[NonUniformResourceIndex(instance.textures2.x)].SampleGrad(material_sampler, uv, uv_ddx, uv_ddy).r;
        s.occlusion = lerp(1.0, o, instance.surface.z);
    }

    s.roughness = clamp(s.roughness, 0.045, 1.0);

    s.baked = float3(-1.0, 0.0, 0.0);

    if (instance.lightmap.z != 0u)
    {
        s.baked = g_textures[NonUniformResourceIndex(instance.lightmap.y)].SampleLevel(g_sampler_linear, input.lightmap_uv, 0.0).rgb;
    }

    return s;
}

// A light from direction L (toward it), of unit irradiance at normal incidence : GGX specular and
// Lambert diffuse, with the energy single scattering loses put back (Fdez-Aguera).
float3 direct_light(float3 N, float3 V, float3 L, float n_dot_v, float a, float3 F0, float3 diffuse_color, float ess)
{
    float n_dot_l = saturate(dot(N, L));
    float3 H = normalize(V + L);
    float n_dot_h = saturate(dot(N, H));
    float v_dot_h = saturate(dot(V, H));
    float3 F = f_schlick(F0, v_dot_h);
    float3 specular = F * (d_ggx(n_dot_h, a) * v_smith_ggx(n_dot_v, n_dot_l, a));
    specular *= 1.0 + F0 * (1.0 / max(ess, 1e-3) - 1.0);
    float3 diffuse = diffuse_color * (1.0 - F) / PI;

    return (diffuse + specular) * n_dot_l;
}

// The screen-space ambient occlusion at this pixel, if that pass ran.
float screen_occlusion(Frame frame, int2 pixel)
{
    return frame.environment_textures.w != NO_TEXTURE ? g_textures[frame.environment_textures.w].Load(int3(pixel, 0)).r : 1.0;
}

// What the screen-space reflections need to replace the environment's reflection by their own : the
// reflection's weight (Fresnel and the BRDF's scale and bias), and the specular occlusion dimming it.
struct SpecularTerms
{
    float3 weight;
    float occlusion;
};

// --- Reflection probes ----------------------------------------------------------------------------
//
// Baked captures of the scene (environment probes) : each a prefiltered panorama like the sky's, with a
// box. A point inside a box reflects its probe, the reflection ray first moved onto the box's walls
// (box projection, Lagarde) : a room's reflections stay put as the camera moves. The two strongest
// probes blend at their boxes' edges, and over the sky where none reaches.

// Mirrors R3ReflectionProbe in render3d_internal.h.
struct ReflectionProbe
{
    float4 position;     // the capture point ; w : how far inside the box the fade ends, meters
    float4 box_min;      // w : its prefiltered panorama
    float4 box_max;      // w : that panorama's mips
};

static const uint REFLECTION_PROBE_SIZE = 48u;

float3 sample_panorama(uint texture, float mips, float3 direction, float roughness)
{
    return g_textures[NonUniformResourceIndex(texture)].SampleLevel(g_sampler_panorama, panorama_uv(direction), roughness * (mips - 1.0)).rgb;
}

// The environment's reflection along the dominant specular direction, at a point : what shade_surface()
// adds, and what the screen-space reflections replace where they find a hit.
float3 environment_reflection(Frame frame, float3 position, float3 N, float3 V, float roughness)
{
    float a = roughness * roughness;
    float3 R = normalize(lerp(reflect(-V, N), N, a)); // shifts toward the normal on rough surfaces
    float3 sky = sample_panorama(frame.environment_textures.y, frame.environment.y, R, roughness) * frame.environment.x;

    float3 sum = float3(0.0, 0.0, 0.0);
    float weight_sum = 0.0;

    for (uint i = 0u; i < frame.reflection_probes.x && weight_sum < 1.0; i++)
    {
        ReflectionProbe probe = g_buffers[ids.x].Load<ReflectionProbe>(frame.reflection_probes.y + i * REFLECTION_PROBE_SIZE);
        float3 inside = min(position - probe.box_min.xyz, probe.box_max.xyz - position);
        float edge = min(inside.x, min(inside.y, inside.z));

        if (edge <= 0.0)
        {
            continue;
        }

        // The ray from the point, to where it leaves the box : seen from the capture point.
        float3 far_planes = max((probe.box_max.xyz - position) / R, (probe.box_min.xyz - position) / R);
        float t = min(far_planes.x, min(far_planes.y, far_planes.z));
        float3 direction = normalize(position + R * t - probe.position.xyz);

        float w = saturate(edge / max(probe.position.w, 1e-3)) * (1.0 - weight_sum);
        sum += sample_panorama(asuint(probe.box_min.w), probe.box_max.w, direction, roughness) * w;
        weight_sum += w;
    }

    return sum + sky * (1.0 - weight_sum);
}

// Everything that lights a surface : image-based lighting, the bounce light, the sun, emission, then fog.
// V : toward the eye, `view_length` away. `shadow_position` : where the sun's shadow is looked up
// (Deferred+ moves it off the flat triangles of coarse smooth-shaded meshes).
float3 shade_surface_terms(Frame frame, Surface s, float ao, float3 V, float view_length, float3 shadow_position, float2 pixel, out SpecularTerms terms)
{
    float3 N = s.normal;
    float roughness = s.roughness;
    float a = roughness * roughness;
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), s.albedo, s.metallic);
    float3 diffuse_color = s.albedo * (1.0 - s.metallic);
    float n_dot_v = clamp(dot(N, V), 1e-3, 1.0);

    // Split-sum image-based lighting, with Fdez-Aguera's multi-scattering compensation.
    float2 dfg = g_textures[frame.environment_textures.z].SampleLevel(g_sampler_linear, float2(n_dot_v, roughness), 0.0).rg;
    float3 prefiltered = environment_reflection(frame, s.position, N, V, roughness);

    // Diffuse light from afar : realtime GI's probes, else baked into the lightmap, or the probe volume,
    // else the environment's.
    bool baked = s.baked.x >= 0.0 || frame.gi_origin.w > 0.0;
    bool from_probes = false;
    float3 irradiance;

    if (frame.gi_origin.w > 0.0)
    {
        irradiance = gi_irradiance_from(frame, frame.gi_textures, s.position, N);
    }
    else if (baked)
    {
        irradiance = s.baked;
    }
    else
    {
        irradiance = probe_irradiance(frame, s.position, N, from_probes);

        if (!from_probes)
        {
            irradiance = sh_irradiance(frame, N) * frame.environment.x;
        }
    }

    float3 fss_ess = F0 * dfg.x + dfg.y;
    float ess = dfg.x + dfg.y;
    float ems = 1.0 - ess;
    float3 f_avg = F0 + (1.0 - F0) / 21.0;
    float3 fms_ems = fss_ess * f_avg * ems / (1.0 - f_avg * ems);
    float3 k_d = diffuse_color * (1.0 - fss_ess - fms_ems);

    // Specular occlusion (Lagarde) : AO barely dims what a smooth surface reflects at grazing angles.
    float specular_occlusion = saturate(pow(n_dot_v + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);

    float3 color = (fss_ess * prefiltered) * specular_occlusion + (fms_ems + k_d) * irradiance * ao;
    terms.weight = fss_ess;
    terms.occlusion = specular_occlusion;

    // Quilez's outdoors rig : what the sun bounces back from the ground, from the side opposite to it. A
    // stand-in for the indirect light that the lightmaps and probes, when there are some, have for real.
    if (!baked && !from_probes)
    {
        float3 bounce_dir = normalize(frame.sun_direction.xyz * float3(-1.0, 0.0, -1.0) + float3(0.0, 1e-3, 0.0));
        float bounce = saturate(0.5 + 0.5 * dot(N, bounce_dir)) * saturate(1.0 - max(N.y, 0.0));
        color += diffuse_color * frame.sun_color.rgb * (frame.environment.z * bounce * ao / PI);
    }

    // The sun.
    float3 L = frame.sun_direction.xyz;

    if (dot(N, L) > 0.0)
    {
        float shadow = sun_shadow(frame, shadow_position, s.geometric_normal, view_length, pixel) * cloud_shadow(frame, s.position);

        if (shadow > 0.0)
        {
            color += direct_light(N, V, L, n_dot_v, a, F0, diffuse_color, ess) * frame.sun_color.rgb * shadow;
        }
    }

    // Point and spot lights : those of this pixel's cluster.
    uint2 cluster = cluster_lights(frame, s.position, pixel);

    for (uint i = 0u; i < cluster.y; i++)
    {
        uint index = g_buffers[ids.x].Load(frame.light_data.z + (cluster.x + i) * 4u);
        Light light = load_light(frame, index);

        float3 to_light = light.position_range.xyz - s.position;
        float d2 = dot(to_light, to_light);
        float window = saturate(1.0 - d2 * d2 * light.color.w * light.color.w); // Karis : 1 - (d / range)^4

        if (window <= 0.0)
        {
            continue;
        }

        float distance = sqrt(d2);
        float3 Ll = to_light / max(distance, 1e-4);
        float attenuation = window * window / max(d2, 1e-4);

        if (light.direction_type.w > 0.5)
        {
            float cone = saturate(dot(-Ll, light.direction_type.xyz) * light.spot_shadow.x + light.spot_shadow.y);
            attenuation *= cone * cone;
        }

        if (attenuation <= 0.0 || dot(N, Ll) <= 0.0)
        {
            continue;
        }

        if (light.spot_shadow.z >= 0.0)
        {
            attenuation *= local_shadow(frame, light, shadow_position, s.geometric_normal, distance, pixel);
        }

        color += direct_light(N, V, Ll, n_dot_v, a, F0, diffuse_color, ess) * light.color.rgb * attenuation;
    }

    color += s.emissive;

    if (frame.volumetrics.x == 0.0)
    {
        color = apply_fog(frame, color, frame.camera_position.xyz, -V, view_length);
    }

    return min(color, float3(60000.0, 60000.0, 60000.0));
}

float3 shade_surface(Frame frame, Surface s, float ao, float3 V, float view_length, float3 shadow_position, float2 pixel)
{
    SpecularTerms terms;
    return shade_surface_terms(frame, s, ao, V, view_length, shadow_position, pixel, terms);
}

// The sky behind the pixel at this NDC position.
float3 background(Frame frame, float2 ndc)
{
    float4 far_point = mul_rows(frame.inverse_view_projection, float4(ndc, 1.0, 1.0));
    float3 direction = normalize(far_point.xyz / far_point.w - frame.camera_position.xyz);
    return sky_radiance(frame, direction);
}

#endif // RENDER3D_COMMON_SLANG
