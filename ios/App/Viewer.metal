// SPDX-License-Identifier: GPL-2.0-only
#include <metal_stdlib>
using namespace metal;
struct VertexOut { float4 position [[position]]; float2 uv; };
struct Parameters {
    float4 dimensions;  // drawable width/height, video width/height
    float4 lens;        // k1, k2, image scale, horizontal optical-center shift
    float4 controls;    // mode 0 flat / 1 SBS / 2 duplicate, swap, vertical shift, test 0/1/2
    float4 orientation; // headset quaternion x/y/z/w (used ONLY in local test scene)
    float4 color;       // full range, matrix (0=709, 1=601, 2=2020), demo FOV radians, spare
};
vertex VertexOut nd_vertex(uint id [[vertex_id]]) {
    const float2 pos[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    VertexOut o; o.position = float4(pos[id], 0, 1);
    o.uv = float2((pos[id].x + 1) * 0.5, (1 - pos[id].y) * 0.5);
    return o;
}
float3 rotate_q(float4 q, float3 v) { return v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }
float3 room(float2 uv, float aspect, int eye, bool stereo, constant Parameters &p) {
    float scale = tan(p.color.z * 0.5);
    float3 ray = normalize(float3((uv.x * 2 - 1) * aspect * scale, (1 - 2 * uv.y) * scale, -1));
    ray = rotate_q(p.orientation, ray);
    float3 origin = rotate_q(p.orientation, float3(stereo ? (eye == 0 ? -0.032 : 0.032) : 0, 0, 0));
    float3 rgb = mix(float3(0.025, 0.045, 0.08), float3(0.1, 0.2, 0.27), clamp(ray.y * 0.5 + 0.5, 0.0, 1.0));
    float nearest = 1000;
    if (ray.y < -0.001) {
        float t = (-1.6 - origin.y) / ray.y;
        float3 pt = origin + t * ray;
        if (t > 0 && t < 35) {
            float checker = fmod(floor(pt.x) + floor(pt.z), 2.0) == 0 ? 0.12 : 0.23;
            rgb = mix(float3(checker), rgb, clamp(t / 40, 0.0, 1.0)); nearest = t;
        }
    }
    const float3 centers[5] = {float3(-1,0,-3), float3(0,0.3,-4), float3(1,-0.1,-3), float3(-3,0,-6), float3(3,0,-6)};
    const float3 colors[5] = {float3(1,0.3,0.22), float3(0.1,0.9,0.7), float3(0.3,0.6,1), float3(0.9,0.7,0.2), float3(0.7,0.3,0.9)};
    for (int i = 0; i < 5; i++) {
        float3 oc = origin - centers[i]; float b = dot(oc, ray);
        float d = b*b - (dot(oc,oc) - 0.16);
        if (d > 0) {
            float t = -b - sqrt(d);
            if (t > 0 && t < nearest) {
                nearest = t; float3 normal = normalize(origin + ray*t - centers[i]);
                rgb = colors[i] * (0.28 + 0.72 * max(0.0, dot(normal, normalize(float3(-0.4,0.8,1)))));
            }
        }
    }
    return rgb;
}
float3 grid(float2 uv, int eye, float aspect) {
    float2 centered = (uv - 0.5) * float2(aspect, 1);
    float2 g = abs(fract(uv * 12) - 0.5);
    bool line = g.x > 0.48 || g.y > 0.48;
    bool circle = abs(length(centered) - 0.28) < 0.003;
    bool crosshair = abs(uv.x - 0.5) < 0.002 || abs(uv.y - 0.5) < 0.002;
    float3 bg = eye == 0 ? float3(0.025,0.07,0.10) : float3(0.06,0.035,0.10);
    return crosshair || circle ? float3(0.9) : (line ? float3(0.25,0.55,0.62) : bg);
}
fragment float4 nd_fragment(VertexOut in [[stage_in]],
                            texture2d<float> yTexture [[texture(0)]],
                            texture2d<float> uvTexture [[texture(1)]],
                            constant Parameters &p [[buffer(0)]]) {
    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
    bool flat = p.controls.x < 0.5;
    int eye = flat ? 0 : (in.uv.x < 0.5 ? 0 : 1);
    float2 uv = flat ? in.uv : float2(in.uv.x * 2 - eye, in.uv.y);
    float aspect = (flat ? p.dimensions.x : p.dimensions.x * 0.5) / p.dimensions.y;
    if (!flat) {
        float2 center = float2(0.5 + (eye == 0 ? p.lens.w : -p.lens.w), 0.5 + p.controls.z);
        float2 delta = (uv - center) * 2;
        delta.x *= aspect;
        float r2 = dot(delta, delta);
        delta *= (1 + p.lens.x * r2 + p.lens.y * r2*r2) / max(p.lens.z, 0.2);
        delta.x /= aspect;
        uv = 0.5 + delta * 0.5;
    }
    if (any(uv < 0) || any(uv > 1)) return float4(0,0,0,1);
    if (p.controls.w > 1.5) return float4(room(uv, aspect, eye, !flat, p), 1);
    if (p.controls.w > 0.5) return float4(grid(uv, eye, aspect), 1);
    bool sbs = p.controls.x > 0.5 && p.controls.x < 1.5;
    float sourceAspect = (sbs ? p.dimensions.z * 0.5 : p.dimensions.z) / max(p.dimensions.w, 1.0);
    // Fit, rather than stretch, each source eye into its physical viewport.
    if (aspect > sourceAspect) uv.x = (uv.x - 0.5) * aspect / sourceAspect + 0.5;
    else uv.y = (uv.y - 0.5) * sourceAspect / aspect + 0.5;
    if (any(uv < 0) || any(uv > 1)) return float4(0,0,0,1);
    if (sbs) { int sourceEye = p.controls.y > 0.5 ? 1-eye : eye; uv.x = (sourceEye + uv.x) * 0.5; }
    float y = yTexture.sample(s, uv).r;
    float2 chroma = uvTexture.sample(s, uv).rg - (128.0/255.0);
    if (p.color.x < 0.5) { y = (y - 16.0/255.0) * (255.0/219.0); chroma *= (255.0/224.0); }
    float3 rgb;
    if (p.color.y > 1.5) rgb = float3(y + 1.4746*chroma.y, y - 0.164553*chroma.x - 0.571353*chroma.y, y + 1.8814*chroma.x);
    else if (p.color.y > 0.5) rgb = float3(y + 1.402*chroma.y, y - 0.344136*chroma.x - 0.714136*chroma.y, y + 1.772*chroma.x);
    else rgb = float3(y + 1.5748*chroma.y, y - 0.187324*chroma.x - 0.468124*chroma.y, y + 1.8556*chroma.x);
    return float4(clamp(rgb, 0.0, 1.0), 1);
}
