#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D logoTex;
layout(set = 0, binding = 1) uniform sampler2D silhouetteTex;

layout(push_constant) uniform Push {
    float reveal; // 0.0 to 1.0
    float time;
    float aspect;
    float pad;
} push;

// Simple hash for noise
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    // 1. Center and Scale Logic (Robust)
    float logoSize = 0.5; // Scale: 0.5 means logo takes half the screen height
    vec2 centeredUV = fragUV - 0.5;
    
    // Correct aspect ratio: logo is square, screen is wide
    centeredUV.x *= push.aspect;
    
    vec2 logoUV = centeredUV / logoSize + 0.5;
    
    vec3 color = vec3(0.0);
    
    // 2. Texture Sampling with Edge Clamping
    if (logoUV.x >= 0.0 && logoUV.x <= 1.0 && logoUV.y >= 0.0 && logoUV.y <= 1.0) {
        vec4 logo = texture(logoTex, logoUV);
        float silhouette = texture(silhouetteTex, logoUV).a;
        
        // Eruption Noise
        float n = noise(logoUV * 10.0 + push.time * 0.5);
        
        // REVEAL LOGIC - FAST AND VISIBLE
        // push.reveal goes 0.0 -> 1.0 in 2 seconds
        float revealVal = push.reveal * 1.5; 
        
        // The Mask (Silhueta ou Alpha)
        float mask = max(silhouette, logo.a);
        
        // Step 1: Draw the gray stroke (always visible first)
        float stroke = smoothstep(revealVal - 0.2, revealVal, mask + n * 0.05);
        color = mix(color, vec3(0.5), stroke);
        
        // Step 2: Burning Edge (Eruption touch)
        float edge = smoothstep(revealVal - 0.1, revealVal, mask + n * 0.05);
        float burn = edge * (1.0 - smoothstep(revealVal, revealVal + 0.05, mask + n * 0.05));
        color += vec3(1.0, 0.3, 0.0) * burn * 20.0;
        
        // Step 3: Reveal the real logo colors
        float logoReveal = smoothstep(0.1, 0.6, push.reveal);
        color = mix(color, logo.rgb, logoReveal * logo.a);
        
        // Step 4: Bloom Trigger (High HDR intensity)
        float glowTrigger = smoothstep(0.5, 1.0, push.reveal);
        if (logo.a > 0.1) {
            color += logo.rgb * glowTrigger * 15.0; // Pushes into Bloom
        }
    }
    
    outColor = vec4(color, 1.0);
}
