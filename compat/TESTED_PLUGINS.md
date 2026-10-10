# Plugins testados na camada de compatibilidade

A camada `compat/` executa plugins feitos para outros motores. O Eruption Engine
**não distribui** nenhum desses plugins: cada usuário traz os seus. Os plugins
abaixo são projetos de código aberto com licença permissiva, usados só como
bateria de testes. `compat/scripts/baixar_testes.sh` os baixa em
`compat/testdata/` (fora do git) fixados no commit listado; nada deles entra no
repositório nem no executável.

Licenças conferidas pela API do GitHub (`license.spdx_id`) e pelo arquivo de
licença do próprio repositório em 2026-10-10.

Os nomes Godot, Unity e Unreal são usados apenas para dizer com o que a camada
é compatível. Não há afiliação nem endosso.

## Godot (GDExtension)

| Plugin | Origem | Licença | Commit | O que exercita | Resultado |
|---|---|---|---|---|---|
| godot-cpp 4.5 (biblioteca de ligação, compila os testes) | https://github.com/godotengine/godot-cpp | MIT | `e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77` (tag godot-4.5-stable) | toda a inicialização do godot-cpp (≈160 funções da interface, Variant, StringName, ClassDB) | carrega e inicializa |
| GDExtensionSummator | https://github.com/paddy-exe/GDExtensionSummator | Unlicense (cabeçalhos citam CC-BY 3.0 da documentação) | `4f687536b5cab10c6d9bb8937566c39a04b1f915` | classe `RefCounted`, métodos com argumento padrão (`add(value = 1)`), retorno `int` | **funciona**: `add 10`, `add 20`, `get_total` → 30; `reset`, `add` → 1 |
| godot-cpp-template (`ExampleClass`) | https://github.com/godotengine/godot-cpp-template | Unlicense | `d36f9d03e9e02dad22829f1faf4e03a347616df5` | argumento `Variant`, `vformat` (`String % Array`), `print_line` | **funciona**: `print_type 42` imprime `Type: 2`, `1.5` → 3, texto → 4, `true` → 1 |
| GDExample (tutorial oficial de GDExtension em C++) | https://github.com/godotengine/godot-docs (`tutorials/scripting/cpp/gdextension_cpp_example.rst`) | CC-BY 3.0 | `33e08e14c517541fac3d9069e82fb3ee2fc99a10` (branch 4.5) | `Sprite2D` herdado, `_process(delta)` virtual, `set_position` nativo | **funciona**: após 60 quadros de 1/60 s a posição é (19.093, 10.707), igual a `10 + 10·sin(2)`, `10 + 10·cos(1.5)` |
| YParticles3D (partículas em CPU) | https://github.com/DanielSnd/YParticles3D | Unlicense | `22ee8109068d93591dc01dd7eb5d44ead8d136d0` | `Node3D`, `RenderingServer`, `RandomNumberGenerator`, curvas, gradientes | **parcial**: carrega, registra `YParticles3D`, entra na cena e roda quadros sem travar; a camada lista o que falta (`RenderingServer.mesh_create`, `Node3D.get_global_transform`, `RandomNumberGenerator.randf`...) |

## Unity (plugins nativos)

| Plugin | Origem | Licença | Commit | O que exercita | Resultado |
|---|---|---|---|---|---|
| NativeRenderingPlugin (exemplo oficial de plugin de renderização) | https://github.com/Unity-Technologies/NativeRenderingPlugin | MIT (os cabeçalhos da API de plugin dentro dele têm outra licença e **não** são usados pela camada) | `522254181faf188efa8b50c3e3bf6fce720b26e4` | `UnityPluginLoad`/`Unload`, `IUnityInterfaces`, `IUnityGraphics` (callback de dispositivo), pedido de `IUnityGraphicsVulkan`, `GetRenderEventFunc`, exportações chamadas por `DllImport` | **funciona**: com dispositivo nulo, ciclo de vida e eventos completos; com OpenGL fora de tela (EGL), o plugin desenha o triângulo (6144 pixels coloridos em 256×256) e escreve a textura "plasma" (249 tons) |

Observação: o plugin chama `GetBackBufferWidth()` sem checar se há API gráfica
ativa; com dispositivo nulo isso derruba o próprio plugin (bug dele, não da camada).

## Unreal Engine (plugins em código-fonte)

Compatibilidade de **fonte**: o plugin é recompilado por
`compat/unreal/ferramentas/compilar_plugin.py` contra cabeçalhos próprios
(`compat/unreal/include`, subconjunto de CoreMinimal escrito do zero). A
ferramenta faz o papel mínimo do gerador de reflexão: escreve os
`*.generated.h` (Super/ThisClass por arquivo e linha) e "thunks" para as
`UFUNCTION` estáticas. Arquivos gerados pela ferramenta oficial que venham no
repositório do plugin (`Intermediate/`) são ignorados.

| Plugin | Origem | Licença | Commit | O que exercita | Resultado |
|---|---|---|---|---|---|
| AdvancedEases | https://github.com/DKesserich/AdvancedEasesPlugin | MIT | `4fe1c6d00d0eb3b2b9a4dfdd65075f80d7348855` | `.uplugin` (LoadingPhase), `IModuleInterface`/`IMPLEMENT_MODULE`, `UBlueprintFunctionLibrary`, `UENUM`/`UMETA`, `UFUNCTION` estática com argumento padrão, `FVector`, `PI` | **funciona**: `StartupModule`/`ShutdownModule`; `AdvancedEase_Float(AE_OUTBOUNCE, 0.5, 0, 10)` → 7.65625; `AdvancedEase_Vector(AE_LERP, 0, (10,20,30), 0.5)` → (5, 10, 15) |
| Serial COM | https://github.com/videofeedback/Unreal_Engine_SerialCOM_Plugin | MIT | `bd3b2ee44cec7016fe64975c2d03ffd7fda20a2d` | `DECLARE_LOG_CATEGORY_CLASS`, `UE_LOG` com `TEXT()`, módulo com `LoadingPhase` PreDefault | **parcial**: o plugin é só Win64 (porta serial via Win32), a ferramenta respeita `PlatformAllowList`; compilando só o arquivo do módulo (`--forcar-plataforma --so-fontes SERIALCOMModule.cpp`), o `UE_LOG` dele aparece no log do host |

## Como rodar

```sh
bash compat/scripts/baixar_testes.sh            # tudo (ou: godot | unity | unreal)
cmake -S compat/tests/unity -B compat/build/unity-tests -DCMAKE_BUILD_TYPE=Release
nice cmake --build compat/build/unity-tests -j4
nice cmake --build compat/build/sozinho -j4 --target eruption-compat-unity-gl
bash compat/tests/unreal/compilar.sh
bash compat/scripts/testar_compat.sh            # bateria completa, sem o motor
bash compat/scripts/testar_motor.sh             # dentro do motor (xvfb)
```

Passo a passo só do Godot:

```sh
bash compat/scripts/baixar_testes.sh godot
cmake -S compat/tests/godot -B compat/build/godot-tests -DCMAKE_BUILD_TYPE=Release
nice cmake --build compat/build/godot-tests -j4
cmake -S compat -B compat/build/sozinho -DCMAKE_BUILD_TYPE=Release
nice cmake --build compat/build/sozinho -j4
B=compat/build/godot-tests/bin
compat/build/sozinho/eruption-compat-run --godot $B/libgd_summator.so:summator_library_init \
    --novo Summator --chamar add 10 --chamar add 20 --chamar get_total
compat/build/sozinho/eruption-compat-run --godot $B/libgd_gdexample.so:example_library_init \
    --novo GDExample --cena --quadros 60 --descrever
```
