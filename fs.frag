#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    float sharpen;
    float contrast;
    float saturation;
    float vignette;
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() {
    vec2 px = 1.0 / vec2(textureSize(src, 0));
    vec4 c4 = texture(src, uv);
    vec3 c = c4.rgb;
    vec3 n = texture(src, uv + vec2(0.0, -px.y)).rgb;
    vec3 s = texture(src, uv + vec2(0.0,  px.y)).rgb;
    vec3 e = texture(src, uv + vec2( px.x, 0.0)).rgb;
    vec3 w = texture(src, uv + vec2(-px.x, 0.0)).rgb;
    vec3 blur = (n + s + e + w) * 0.25;
    c += (c - blur) * p.sharpen;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, p.saturation);
    c = (c - 0.5) * p.contrast + 0.5;
    vec2 d = uv - 0.5;
    c *= 1.0 - p.vignette * dot(d, d) * 2.0;
    o = vec4(clamp(c, 0.0, 1.0), c4.a);
}
