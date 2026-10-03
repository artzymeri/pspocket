// Synthetic load for the Metal tab (LoadRenderer.swift): a cheap procedural pattern drawn as fullscreen passes into
// the 1152x640 scene, then a composite that copies or scales the result into the drawable.

#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv; // (0,0) top left
};

// One triangle that covers the viewport: no vertex buffer.
vertex VertexOut fullscreen_vs(uint vid [[vertex_id]])
{
    const float2 p = float2((vid << 1) & 2, vid & 2); // (0,0) (2,0) (0,2)
    VertexOut out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

// Matches LoadRenderer.PatternParams.
struct PatternParams {
    float time;
    uint pass;
    uint passes;
    float aspect;
};

fragment float4 pattern_fs(VertexOut in [[stage_in]], constant PatternParams& p [[buffer(0)]])
{
    const float2 uv = in.uv;
    const float t = p.time;
    if (p.pass == 0) {
        // Base: a slow gradient and a white bar sweeping across at constant speed, so uneven pacing is visible.
        const float3 base = float3(0.15 + 0.35 * uv.x, 0.12 + 0.30 * uv.y, 0.35 + 0.25 * sin(t * 0.5));
        const float bar = 1.0 - smoothstep(0.004, 0.008, abs(fract(uv.x - t * 0.2) - 0.5));
        return float4(mix(base, float3(1.0), bar), 1.0);
    }
    // Load pass k: rings around a moving centre, blended in at low alpha so every pass changes the image.
    const float k = float(p.pass);
    float2 d = uv - float2(0.5 + 0.35 * sin(t * 0.7 + k * 1.7), 0.5 + 0.35 * cos(t * 0.9 + k * 2.3));
    d.x *= p.aspect;
    const float rings = 0.5 + 0.5 * sin(length(d) * 60.0 - t * 3.0 + k);
    const float3 tint = 0.5 + 0.5 * cos(float3(0.0, 2.1, 4.2) + k * 0.37 + t * 0.3);
    return float4(tint * rings, 0.12);
}

fragment float4 composite_fs(VertexOut in [[stage_in]], texture2d<float> source [[texture(0)]],
                             sampler smp [[sampler(0)]])
{
    return source.sample(smp, in.uv);
}
