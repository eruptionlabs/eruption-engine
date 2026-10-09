// Comum aos shaders do executor de redes (src/ml/NnVulkan.cpp): todos os
// tensores moram num buffer float so' (a arena); offsets e passos chegam em
// elementos pelas push constants. Despacho 1D quebrado em 2D quando passa do
// limite de grupos por eixo.
layout(std430, set = 0, binding = 0) buffer Arena { float data[]; } arena;

uint flatIndex() {
    return gl_GlobalInvocationID.x + gl_GlobalInvocationID.y * gl_NumWorkGroups.x * gl_WorkGroupSize.x;
}
