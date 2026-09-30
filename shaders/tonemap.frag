#version 460
// HDR -> display. ACES filmic approximation (Narkowicz); the swapchain is
// sRGB so the hardware applies the transfer function on write.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec3 Aces(vec3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 hdr = texture(textures[nonuniformEXT(pc.hdr_texture)], in_uv).rgb;
    out_color = vec4(Aces(hdr), 1.0);
}
