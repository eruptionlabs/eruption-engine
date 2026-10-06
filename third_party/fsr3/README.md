# FSR 3.1 upscaler (GPU)

Arquivos do AMD FidelityFX SDK **v1.1.4** (commit em `.sdk_commit`), licenca MIT
(`LICENSE.txt`). Sem modificacao.

- `gpu/`: headers de GPU (`ffx_core*`, `fsr3upscaler/`, `spd/`, `fsr1/`).
- `shaders/`: os passes GLSL do backend Vulkan do SDK.

So' a parte de GPU foi trazida. O SDK monta os shaders com uma ferramenta so'
de Windows (FidelityFX_SC.exe) e o lado de CPU (recursos, constantes,
despacho) e' escrito no motor. A partir da v2.0 o SDK virou DLL assinada so'
de DX12, entao a v1.1.4 e' a ultima com backend Vulkan e fonte.

Compilacao (todos os 10 passes compilam com glslangValidator, vulkan1.3, sem
fp16): `-S comp -Igpu -Igpu/fsr3upscaler -DFFX_GPU=1 -DFFX_GLSL=1` mais as
opcoes `FFX_FSR3UPSCALER_OPTION_*` de permutacao.
