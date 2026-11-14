#version 450
// Ambient occlusion, second step: the raw occlusion (half resolution, each
// texel taken at the top left pixel of its 2x2 block) is smoothed over the
// 5x5 texels around, without mixing across depth edges, and multiplied into
// the image of the game world (the colour target blends with dst * src), and
// with it the bounced light of the global illumination, smoothed the same way.
#extension GL_GOOGLE_include_directive : require
#include "ao_common.glsl"

layout(location = 0) out vec4 out_factor;

void main() {
    vec2 frag = gl_FragCoord.xy;
    float d = texelFetch(depth_tex, ivec2(frag), 0).r;
    if (d <= 0.0) {
        out_factor = vec4(1.0);
        return;
    }
    float z = AO_NEAR / d;
    ivec2 size = textureSize(ao_tex, 0), full = textureSize(depth_tex, 0);
    ivec2 centre = ivec2(frag) / 2;
    float sum = 0.0, weight = 0.0;
    // The bounced light (ao.frag traced_bounce): every pixel of a block of
    // 4x4 texels holds its own part of the rays, so each of the four columns
    // and rows of the block counts the same (the outer two of the five, half)
    vec3 bounce = vec3(0.0);
    float bounce_weight = 0.0;
    // Wheelkit (a modified version's addition): with three views (rect: one screen's third) the texels around stay on
    // this pixel's screen, whose occlusion was traced with its own projection
    ivec2 lo = ivec2(0), hi = size - 1;
    if (ao.rect.z * 2.5 < float(full.x)) {
        int s = int(frag.x / ao.rect.z);
        lo.x = int(ceil(float(s) * ao.rect.z / 2.0)), hi.x = min(hi.x, int(ceil(float(s + 1) * ao.rect.z / 2.0)) - 1);
    }
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++) {
            ivec2 q = clamp(centre + ivec2(x, y), lo, hi);
            float dq = texelFetch(depth_tex, min(q * 2, full - 1), 0).r;   // where that texel was taken
            if (dq <= 0.0) continue;
            // a surface seen at a slant (a board along the road) changes depth
            // fast from texel to texel: a tight tolerance left its rays' pattern
            // unsmoothed, a grey hatch over it
            float w = exp(-abs(AO_NEAR / dq - z) / (0.08 * z));
            vec4 t = texelFetch(ao_tex, q, 0);
            sum += t.a * w;
            weight += w;
            float wb = w * (abs(x) == 2 ? 0.5 : 1.0) * (abs(y) == 2 ? 0.5 : 1.0);
            bounce += t.rgb * wb;
            bounce_weight += wb;
        }
    float open = weight > 0.0 ? sum / weight : 1.0;
    vec3 factor = vec3(mix(1.0, open, ao.params.y));
    // The colour so far is the surface's own times its light: the bounced
    // light, as a share more of it
    if (ao.gi.x != 0.0 && bounce_weight > 0.0) {
        bounce /= bounce_weight;
        factor = ao.gi.x < 0.0 ? bounce * -ao.gi.x : factor * (1.0 + ao.gi.x * bounce);
    }
    out_factor = vec4(factor, 1.0);
}
