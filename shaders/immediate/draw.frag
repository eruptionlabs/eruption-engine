#version 450
// Textura (PNG sRGB, amostrada em linear) vezes a cor do vertice. A cor sai em
// linear/HDR para a imagem iluminada (pode passar de 1); o blend (aditivo ou
// alfa) e' do pipeline.
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() {
    outColor = texture(uTex, vUV) * vColor;
}
