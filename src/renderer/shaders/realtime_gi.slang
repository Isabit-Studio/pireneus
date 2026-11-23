// Realtime global illumination (renderer/render3d, both pipelines, where the GPU has ray tracing), after
// DDGI (Majercik et al.) : a grid of probes over the scene, each tracing rays every frame (in a new random
// rotation) with inline ray queries. What a ray finds (the sun, the sky, emission, and the probes' own light
// at the hit : infinite bounces, a frame at a time) is integrated into L1 spherical harmonics of
// irradiance / PI, blended over the previous frames'. The shading then reads them as it reads the baked
// probe volume (render3d_common.slang).
//
// The probes live in textures (fragment passes can't write buffers) : probe p at texel (p % width,
// p / width) of four RGBA16F targets, the red, green and blue coefficients and (validity, unused...).

#include "render3d_common.slang"

[[vk::binding(6, 0)]] RaytracingAccelerationStructure g_tlas[2] : register(t0, space3);

// The same rotation for a frame's two passes, a different one every frame : params.x counts frames.
float3x3 frame_rotation(uint frame)
{
    float a = float(frame) * 0.7548776;
    float b = float(frame) * 0.5698403;
    float u1 = frac(a), u2 = frac(b), u3 = frac(a * 1.618034 + b);

    // A uniformly distributed rotation (Shoemake), from three numbers.
    float s1 = sqrt(1.0 - u1), s2 = sqrt(u1);
    float4 q = float4(s1 * sin(2.0 * PI * u2), s1 * cos(2.0 * PI * u2), s2 * sin(2.0 * PI * u3), s2 * cos(2.0 * PI * u3));
    float x = q.x, y = q.y, z = q.z, w = q.w;

    return float3x3(
        1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w),
        2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w),
        2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y));
}

float3 ray_direction(uint ray, uint rays, float3x3 rotation)
{
    float z = 1.0 - (2.0 * float(ray) + 1.0) / float(rays);
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = float(ray) * 2.39996323;
    return mul(rotation, float3(r * cos(phi), z, r * sin(phi)));
}

float3 probe_position(Frame frame, uint probe)
{
    uint3 grid = frame.gi_grid.xyz;
    uint3 p = uint3(probe % grid.x, (probe / grid.x) % grid.y, probe / (grid.x * grid.y));
    return frame.gi_origin.xyz + float3(p) * frame.gi_origin.w;
}

struct GiHit
{
    bool hit;
    uint draw;
    uint primitive;
    float2 barycentrics;
    float t;
};

GiHit trace_gi(uint tlas, float3 origin, float3 direction, float max_t, bool any_hit)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 1e-3;
    ray.TMax = max_t;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;

    // Constant indices : indexing an array of acceleration structures dynamically isn't guaranteed everywhere.
    if (tlas == 0u)
    {
        query.TraceRayInline(g_tlas[0], any_hit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : 0, 0xFF, ray);
    }
    else
    {
        query.TraceRayInline(g_tlas[1], any_hit ? RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH : 0, 0xFF, ray);
    }

    query.Proceed();

    GiHit result;
    result.hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    result.draw = 0u;
    result.primitive = 0u;
    result.barycentrics = float2(0.0, 0.0);
    result.t = max_t;

    if (result.hit)
    {
        result.draw = query.CommittedInstanceID();
        result.primitive = query.CommittedPrimitiveIndex();
        result.barycentrics = query.CommittedTriangleBarycentrics();
        result.t = query.CommittedRayT();
    }

    return result;
}

// What a ray sees at a hit : the surface's albedo, emission and normal (toward the ray), a coarse texture
// level. `backface` : the ray hit it from inside (geometry winds counterclockwise seen from outside).
void hit_surface(GiHit hit, float3 direction, out float3 position, out float3 normal, out float3 albedo, out float3 emissive, out bool backface)
{
    Instance instance = load_instance(hit.draw);

    uint first = instance.geometry.z + hit.primitive * 3u;
    uint index_buffer = instance.geometry.y;
    uint i0 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u);
    uint i1 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 4u);
    uint i2 = g_buffers[NonUniformResourceIndex(index_buffer)].Load(first * 4u + 8u);

    VertexData v0 = load_vertex(instance.geometry.x, i0);
    VertexData v1 = load_vertex(instance.geometry.x, i1);
    VertexData v2 = load_vertex(instance.geometry.x, i2);

    float3 l = float3(1.0 - hit.barycentrics.x - hit.barycentrics.y, hit.barycentrics.x, hit.barycentrics.y);
    float3 w0 = to_world(instance, v0.position);
    float3 w1 = to_world(instance, v1.position);
    float3 w2 = to_world(instance, v2.position);

    position = w0 * l.x + w1 * l.y + w2 * l.z;
    normal = normalize(cross(w1 - w0, w2 - w0));
    backface = dot(normal, direction) > 0.0;

    if (backface)
    {
        normal = -normal;
    }

    float2 uv = (v0.uv * l.x + v1.uv * l.y + v2.uv * l.z) * instance.uv_transform.xy + instance.uv_transform.zw;
    float4 vertex_color = v0.color * l.x + v1.color * l.y + v2.color * l.z;
    albedo = ((instance.geometry.w & 2u) != 0u ? float3(1.0, 1.0, 1.0) : vertex_color.rgb) * instance.base_color.rgb;

    if (instance.textures.x != NO_TEXTURE)
    {
        albedo *= g_textures[NonUniformResourceIndex(instance.textures.x)].SampleLevel(g_sampler_anisotropic, uv, 3.0).rgb;
    }

    albedo *= 1.0 - instance.emissive.w; // metals have no diffuse light to bounce
    emissive = instance.emissive.xyz;

    if (instance.textures.w != NO_TEXTURE)
    {
        emissive *= g_textures[NonUniformResourceIndex(instance.textures.w)].SampleLevel(g_sampler_anisotropic, uv, 3.0).rgb;
    }
}

// The grid moves by whole cells (params2.xyz this frame) : probe p's history is at p + shift, if that was
// in the grid ; else ~0u.
uint history_probe(Frame frame, uint probe)
{
    int3 grid = int3(frame.gi_grid.xyz);
    int3 p = int3(int(probe) % grid.x, (int(probe) / grid.x) % grid.y, int(probe) / (grid.x * grid.y)) + int3(params2.xyz);

    if (any(p < int3(0, 0, 0)) || any(p >= grid))
    {
        return ~0u;
    }

    return uint((p.z * grid.y + p.y) * grid.x + p.x);
}

// The previous frames' probes, at a point : the bounces. inputs : those four textures, where the grid was.
float3 history_irradiance(Frame frame, float3 world, float3 n)
{
    Frame previous = frame;
    previous.gi_origin.xyz -= params2.xyz * frame.gi_origin.w;
    uint4 textures = inputs;
    return gi_irradiance_from(previous, textures, world, n);
}

float gi_luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// What one ray may bring back : a few times the probe's average so far. A small bright thing (a lamp, a
// window of sky) that one ray finds and the next frame's miss would make the probe flicker ; this way it
// only shows once enough rays have seen it. params.z : 0 when there's no history to compare with.
float3 clamp_ray(Frame frame, uint probe, float3 radiance)
{
    uint previous = history_probe(frame, probe);

    if (params.z <= 0.0 || previous == ~0u)
    {
        return radiance;
    }

    // The DC coefficient of irradiance / PI : its average over directions.
    float3 average = float3(load_gi(inputs.x, previous).x, load_gi(inputs.y, previous).x, load_gi(inputs.z, previous).x) * 0.282095;
    float limit = max(gi_luminance(max(average, float3(0.0, 0.0, 0.0))) * 12.0, gi_luminance(frame.sun_color.rgb) * (0.05 / PI) + 0.02);
    float l = gi_luminance(radiance);
    return l > limit ? radiance * (limit / l) : radiance;
}

// One texel per (ray, probe). inputs : the history ; ids.w : the TLAS ; params.x : the frame ;
// params.y : how many lights to bounce (the first ones) ; params.z : 1 with a history ; params2.xyz : the
// grid's move.
[shader("fragment")]
float4 r3GiTracePs(ScreenVaryings input) : SV_Target
{
    Frame frame = load_frame();
    uint ray = uint(input.position.x);
    uint probe = uint(input.position.y);
    uint rays = frame.gi_grid.w;
    float3 origin = probe_position(frame, probe);
    float3 direction = ray_direction(ray, rays, frame_rotation(uint(params.x)));

    GiHit hit = trace_gi(ids.w, origin, direction, 1e5, false);

    if (!hit.hit)
    {
        return float4(clamp_ray(frame, probe, min(sky_radiance(frame, direction, false), float3(60000.0, 60000.0, 60000.0))), 1e5);
    }

    float3 position, normal, albedo, emissive;
    bool backface;
    hit_surface(hit, direction, position, normal, albedo, emissive, backface);

    // Inside something : no light, and a negative distance marks it.
    if (backface)
    {
        return float4(0.0, 0.0, 0.0, -hit.t);
    }
    float3 offset = position + normal * 0.01;

    // Irradiance / PI there : the sun (shadowed by another ray), the lights, the probes' bounces.
    float3 light = float3(0.0, 0.0, 0.0);
    float3 L = frame.sun_direction.xyz;
    float n_dot_l = dot(normal, L);

    if (n_dot_l > 0.0 && !trace_gi(ids.w, offset, L, 1e5, true).hit)
    {
        light += frame.sun_color.rgb * (n_dot_l * cloud_shadow(frame, position) / PI);
    }

    uint light_count = min(frame.cluster_grid.w, uint(params.y));

    for (uint i = 0u; i < light_count; i++)
    {
        Light local = load_light(frame, i);
        float3 to_light = local.position_range.xyz - position;
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

        light += local.color.rgb * (attenuation * saturate(dot(normal, Ll)) / PI);
    }

    light += history_irradiance(frame, position, normal);

    float3 radiance = emissive + albedo * light;
    return float4(clamp_ray(frame, probe, min(radiance, float3(60000.0, 60000.0, 60000.0))), hit.t);
}

struct GiProbeOutput
{
    float4 red : SV_Target0;
    float4 green : SV_Target1;
    float4 blue : SV_Target2;
    float4 state : SV_Target3; // x : validity
};

// One texel per probe : its rays, projected onto L1 spherical harmonics, blended over the history.
// inputs : the history ; params.x : the frame ; params.y : how much of the history stays (0 : none) ;
// params.z : the rays' texture ; params2.xyz : the grid's move.
[shader("fragment")]
GiProbeOutput r3GiUpdatePs(ScreenVaryings input)
{
    Frame frame = load_frame();
    uint width = 0u, height = 0u;
    g_textures[inputs.x].GetDimensions(width, height);
    uint probe = uint(input.position.y) * width + uint(input.position.x);
    uint probe_count = frame.gi_grid.x * frame.gi_grid.y * frame.gi_grid.z;

    GiProbeOutput output;
    output.red = output.green = output.blue = float4(0.0, 0.0, 0.0, 0.0);
    output.state = float4(0.0, 0.0, 0.0, 0.0);

    if (probe >= probe_count)
    {
        return output;
    }

    uint rays = frame.gi_grid.w;
    float3x3 rotation = frame_rotation(uint(params.x));
    uint ray_texture = asuint(params.z);
    float4 r = float4(0.0, 0.0, 0.0, 0.0), g = r, b = r;
    float backfaces = 0.0;

    for (uint i = 0u; i < rays; i++)
    {
        float4 sample = g_textures[ray_texture].Load(int3(i, probe, 0));
        float3 d = ray_direction(i, rays, rotation);
        float4 basis = float4(0.282095, 0.488603 * d.y, 0.488603 * d.z, 0.488603 * d.x);

        backfaces += sample.a < 0.0 ? 1.0 : 0.0;
        r += sample.r * basis;
        g += sample.g * basis;
        b += sample.b * basis;
    }

    // Monte Carlo weight, then the cosine lobe's convolution over PI (Ramamoorthi & Hanrahan).
    float4 convolution = float4(1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0) * (4.0 * PI / float(rays));
    r *= convolution;
    g *= convolution;
    b *= convolution;
    float validity = backfaces < float(rays) * 0.2 ? 1.0 : 0.0;

    // A probe new to the grid (it moved) starts from its rays alone.
    uint previous = history_probe(frame, probe);
    float keep = previous == ~0u ? 0.0 : params.y;
    previous = previous == ~0u ? probe : previous;
    output.red = lerp(r, load_gi(inputs.x, previous), keep);
    output.green = lerp(g, load_gi(inputs.y, previous), keep);
    output.blue = lerp(b, load_gi(inputs.z, previous), keep);
    output.state = float4(lerp(validity, load_gi(inputs.w, previous).x, keep), 0.0, 0.0, 1.0);
    return output;
}
