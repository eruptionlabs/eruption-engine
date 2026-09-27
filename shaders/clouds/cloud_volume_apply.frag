#version 450

layout(location = 0) in vec2 fragUV;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D accum;

// Composites the half-res accumulated cloud volumes over the scene. The
// offscreen holds the premultiplied "over" composite of every cloud, so the
// pass outputs it verbatim and the pipeline blends premultiplied-over
// (ONE / ONE_MINUS_SRC_ALPHA) — associativity of "over" makes this identical
// to drawing every cloud straight into the scene at full resolution.
void main() {
    outColor = texture(accum, fragUV);
}
