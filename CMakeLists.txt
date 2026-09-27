cmake_minimum_required(VERSION 3.20)
project(eruption-engine VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output the executable to the project root
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

# Options
option(ERUPTION_BUILD_TESTS "Build tests" OFF)
option(ERUPTION_BUILD_TOOLS "Build tools" OFF)
option(ERUPTION_DEBUG "Debug build with validation layers" ON)

# Demo map (release asset, downloaded on first configure if missing)
include(${CMAKE_SOURCE_DIR}/cmake/DemoMap.cmake)

# FetchContent for dependencies
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.8
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 0.9.9.8
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.3.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    shaderc
    GIT_REPOSITORY https://github.com/google/shaderc.git
    GIT_TAG v2023.7
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v2.9.3
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    draco
    GIT_REPOSITORY https://github.com/google/draco.git
    GIT_TAG 1.5.7
    GIT_SHALLOW TRUE
)

# Make available
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)

set(VMA_STATIC_VULKAN_FUNCTIONS OFF CACHE BOOL "" FORCE)
set(VMA_DYNAMIC_VULKAN_FUNCTIONS ON CACHE BOOL "" FORCE)

# Draco options: disable extras, keep only mesh compression needed by glTF.
set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
set(DRACO_GLTF ON CACHE BOOL "" FORCE)
set(DRACO_ANIMATION_ENCODING OFF CACHE BOOL "" FORCE)
set(DRACO_POINT_CLOUD_COMPRESSION OFF CACHE BOOL "" FORCE)
set(DRACO_STANDARD_EDGEBREAKER ON CACHE BOOL "" FORCE)
set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
set(DRACO_BACKWARDS_COMPATIBILITY OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw glm vma imgui nlohmann_json tinygltf draco)

# Avoid stb_image conflicts with the engine's own ImageUtils.cpp implementation.
target_compile_definitions(tinygltf PRIVATE
    TINYGLTF_NO_STB_IMAGE
    TINYGLTF_NO_STB_IMAGE_WRITE
    TINYGLTF_ENABLE_DRACO
)
target_link_libraries(tinygltf PRIVATE draco::draco)
target_include_directories(tinygltf PRIVATE
    ${draco_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}
)

# Find Vulkan
find_package(Vulkan REQUIRED)

# Find Python3 for tools
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Find shader compiler (glslc preferred, glslangValidator fallback)
find_program(GLSLC_EXECUTABLE glslc PATHS ${Vulkan_BIN_DIR} $ENV{VULKAN_SDK}/bin)
find_program(GLSLANG_VALIDATOR glslangValidator)

if(GLSLC_EXECUTABLE)
    set(SHADER_COMPILER ${GLSLC_EXECUTABLE})
    set(SHADER_FLAGS "--target-env=vulkan1.3" "-O" "-I${CMAKE_SOURCE_DIR}/shaders")
elseif(GLSLANG_VALIDATOR)
    set(SHADER_COMPILER ${GLSLANG_VALIDATOR})
    set(SHADER_FLAGS "-V" "--target-env" "vulkan1.3" "-I${CMAKE_SOURCE_DIR}/shaders" "-o")
else()
    message(FATAL_ERROR "No GLSL compiler found. Install glslc or glslangValidator.")
endif()

# Shader compilation
set(SHADER_SOURCE_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_SOURCE_DIR}/shaders)

file(GLOB_RECURSE SHADER_SOURCES
    "${SHADER_SOURCE_DIR}/*.vert"
    "${SHADER_SOURCE_DIR}/*.frag"
    "${SHADER_SOURCE_DIR}/*.comp"
    # Tesselacao (banda perto do chao): sem estas duas linhas o glslc nunca
    # compila model.tesc/model.tese e o pipeline sai NULL sem dizer por que.
    "${SHADER_SOURCE_DIR}/*.tesc"
    "${SHADER_SOURCE_DIR}/*.tese"
)

# Cabecalhos .glsl incluidos por outros shaders. Precisam entrar em DEPENDS:
# sem isso, editar um include NAO recompila quem o inclui, e o .spv antigo
# continua sendo usado SEM AVISO NENHUM.
#
# Isso ja' custou caro: uma sessao inteira perseguindo um "terreno vermelho
# chapado" que era o pbr_common.glsl novo convivendo com um directional.frag.spv
# velho. O bug nao existia no codigo; existia so' no binario obsoleto.
#
# A dependencia e' grossa de proposito - qualquer .glsl recompila TODOS os
# shaders. Sao poucos includes e a compilacao inteira leva segundos; um
# rastreamento fino exigiria extrair o grafo de #include e erraria em silencio
# de novo se alguem acrescentasse um include novo.
file(GLOB_RECURSE SHADER_INCLUDES "${SHADER_SOURCE_DIR}/*.glsl")

foreach(shader ${SHADER_SOURCES})
    file(RELATIVE_PATH rel_path ${SHADER_SOURCE_DIR} ${shader})
    get_filename_component(filename ${rel_path} NAME)
    get_filename_component(subdir ${rel_path} DIRECTORY)
    if(subdir)
        set(output "${SHADER_BINARY_DIR}/${subdir}/${filename}.spv")
        file(MAKE_DIRECTORY ${SHADER_BINARY_DIR}/${subdir})
    else()
        set(output "${SHADER_BINARY_DIR}/${filename}.spv")
    endif()
    if(GLSLC_EXECUTABLE)
        add_custom_command(
            OUTPUT ${output}
            COMMAND ${SHADER_COMPILER}
                    ${shader} -o ${output}
                    ${SHADER_FLAGS}
            DEPENDS ${shader} ${SHADER_INCLUDES}
            COMMENT "Compiling shader: ${rel_path}"
        )
    else()
        add_custom_command(
            OUTPUT ${output}
            COMMAND ${SHADER_COMPILER}
                    ${shader}
                    ${SHADER_FLAGS} ${output}
            DEPENDS ${shader} ${SHADER_INCLUDES}
            COMMENT "Compiling shader: ${rel_path}"
        )
    endif()
    list(APPEND SHADER_BINARIES ${output})
endforeach()

add_custom_target(shaders ALL DEPENDS ${SHADER_BINARIES})

# Shadow shader SPIR-V validation
find_program(SPIRV_VAL_EXECUTABLE spirv-val)
if(SPIRV_VAL_EXECUTABLE)
    set(SHADOW_SPV_BINARIES "")
    foreach(spv ${SHADER_BINARIES})
        if(spv MATCHES "/shadow/")
            list(APPEND SHADOW_SPV_BINARIES ${spv})
        endif()
    endforeach()

    if(SHADOW_SPV_BINARIES)
        add_custom_target(validate_shadow_shaders
            DEPENDS ${SHADOW_SPV_BINARIES}
        )
        foreach(spv ${SHADOW_SPV_BINARIES})
            get_filename_component(spv_name ${spv} NAME)
            add_custom_command(TARGET validate_shadow_shaders POST_BUILD
                COMMAND ${SPIRV_VAL_EXECUTABLE} --target-env vulkan1.3 ${spv}
                COMMENT "Validating SPIR-V: ${spv_name}"
            )
        endforeach()
    endif()
endif()

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.hpp")
list(APPEND SOURCES
    "${CMAKE_SOURCE_DIR}/src/utils/GpuAutoSelect.hpp"
)

# Encoders de bloco (BC1/BC3 via rgbcx, BC7 via bc7enc). Os mesmos que
# tools/bake_assets.py usa offline, agora tambem em runtime para as texturas
# EMBUTIDAS nos GLB (que nao tem arquivo em disco pra cozinhar).
#
# Sem o encoder, o bake do .etexpack nao produz payload e as texturas embutidas
# caem no caminho RGBA8 com downscale: mais VRAM e imagem pior. Por isso ele
# e' obrigatorio de fato: usa a copia em third_party/bc7enc_rdo/ se existir,
# senao baixa a fonte (MIT / dominio publico) pelo FetchContent, como as
# demais dependencias. So' rgbcx.cpp e bc7enc.cpp entram no build.
set(ERUPTION_BC7ENC_DIR "${CMAKE_SOURCE_DIR}/third_party/bc7enc_rdo")
if(NOT EXISTS "${ERUPTION_BC7ENC_DIR}/rgbcx.cpp" OR NOT EXISTS "${ERUPTION_BC7ENC_DIR}/bc7enc.cpp")
    FetchContent_Declare(
        bc7enc_rdo
        GIT_REPOSITORY https://github.com/richgel999/bc7enc_rdo.git
        GIT_TAG b9438627eef73a1157e84201b6fa6eb2ffd6d9f0
    )
    FetchContent_GetProperties(bc7enc_rdo)
    if(NOT bc7enc_rdo_POPULATED)
        FetchContent_Populate(bc7enc_rdo)
    endif()
    set(ERUPTION_BC7ENC_DIR "${bc7enc_rdo_SOURCE_DIR}")
endif()
if(EXISTS "${ERUPTION_BC7ENC_DIR}/rgbcx.cpp" AND EXISTS "${ERUPTION_BC7ENC_DIR}/bc7enc.cpp")
    list(APPEND SOURCES
        "${ERUPTION_BC7ENC_DIR}/rgbcx.cpp"
        "${ERUPTION_BC7ENC_DIR}/bc7enc.cpp"
    )
    set_source_files_properties("${ERUPTION_BC7ENC_DIR}/rgbcx.cpp" "${ERUPTION_BC7ENC_DIR}/bc7enc.cpp"
                                PROPERTIES COMPILE_OPTIONS "-w")
    set(ERUPTION_HAVE_BC_ENCODER ON)
    message(STATUS "Encoder BC em runtime: bc7enc_rdo em ${ERUPTION_BC7ENC_DIR}")
else()
    set(ERUPTION_HAVE_BC_ENCODER OFF)
    message(WARNING "Encoder BC em runtime: bc7enc_rdo AUSENTE - texturas embutidas sobem RGBA8 com downscale")
endif()

# meshoptimizer (zeux, MIT): simplificacao de malha para LOD por distancia e
# otimizacao de ordem de vertice/indice (cache pos-transform).
#
# Motivo medido: o parana_field submete ate' 3,9 MILHOES de triangulos por
# frame (713 instancias visiveis de malhas de ate' 17k vertices). Compartilhar
# geometria entre instancias resolveu memoria, mas nao reduz o que e'
# rasterizado - so' LOD reduz. Referencias em docs/optimization_research.md.
#
# Vendorizado (biblioteca pequena, sem dependencias) para build reprodutivel
# offline. Como o bc7enc acima, a presenca e' OPCIONAL: sem ela a engine
# compila sem LOD e desenha a malha cheia. Nunca falha o build.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/meshoptimizer.h")
    file(GLOB MESHOPT_SOURCES "${CMAKE_SOURCE_DIR}/third_party/meshoptimizer/*.cpp")
    list(APPEND SOURCES ${MESHOPT_SOURCES})
    set(ERUPTION_HAVE_MESHOPT ON)
    message(STATUS "Simplificacao de malha: meshoptimizer encontrado")
else()
    set(ERUPTION_HAVE_MESHOPT OFF)
    message(STATUS "Simplificacao de malha: meshoptimizer AUSENTE - sem LOD geometrico")
endif()

# ImGuizmo (gizmo de posicao/rotacao no viewport - setinhas XYZ tipo Unity/
# Blender/Unreal). Vendorizado (MIT, Cedric Guillemet) por reprodutibilidade
# de build offline, copiado do proprio exemplo do tinygltf que ja' estava
# em _deps - mesma origem, sem dependencia nova pra baixar.
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/ImGuizmo/ImGuizmo.h")
    list(APPEND SOURCES "${CMAKE_SOURCE_DIR}/third_party/ImGuizmo/ImGuizmo.cpp")
    message(STATUS "Gizmo de viewport: ImGuizmo encontrado")
else()
    message(STATUS "Gizmo de viewport: ImGuizmo AUSENTE")
endif()

# Exclude legacy parsers from the main engine build (active versions live in src/formats/)
file(GLOB_RECURSE LEGACY_SOURCES "${CMAKE_SOURCE_DIR}/src/legacy/*.cpp" "${CMAKE_SOURCE_DIR}/src/legacy/*.hpp")
foreach(legacy_src ${LEGACY_SOURCES})
    list(REMOVE_ITEM SOURCES ${legacy_src})
endforeach()

# Generate Icons
set(ICONS_HEADER "${CMAKE_SOURCE_DIR}/src/generated/Icons.hpp")
file(GLOB ICON_FILES "${CMAKE_SOURCE_DIR}/assets/icon/*.png")
add_custom_command(
    OUTPUT ${ICONS_HEADER}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/embed_icons.py ${ICONS_HEADER} ${CMAKE_SOURCE_DIR}/assets/icon
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/embed_icons.py ${ICON_FILES}
    COMMENT "Generating Icons.hpp from assets/icon..."
)
add_custom_target(generate_icons DEPENDS ${ICONS_HEADER})

# Install the icon so file managers can associate it with the binary
install(FILES "${CMAKE_SOURCE_DIR}/assets/icon/eruption_v3_512.png"
        DESTINATION .local/share/icons/hicolor/512x512/apps
        RENAME "eruption-engine.png")

# ImGui sources
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)


# Main executable
add_executable(eruption-engine ${SOURCES} ${IMGUI_SOURCES} ${ICONS_HEADER})
add_dependencies(eruption-engine generate_icons)
if(ERUPTION_HAVE_BC_ENCODER)
    target_compile_definitions(eruption-engine PRIVATE ERUPTION_HAVE_BC_ENCODER=1)
endif()
if(ERUPTION_HAVE_MESHOPT)
    target_compile_definitions(eruption-engine PRIVATE ERUPTION_HAVE_MESHOPT=1)
endif()

# Include directories
target_include_directories(eruption-engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/generated
    ${Vulkan_INCLUDE_DIRS}
    ${glfw_SOURCE_DIR}/include
    ${glm_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${ERUPTION_BC7ENC_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/meshoptimizer
    ${CMAKE_SOURCE_DIR}/third_party/ImGuizmo
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${nlohmann_json_SOURCE_DIR}/include
    ${tinygltf_SOURCE_DIR}
)

# Third-party SOURCES tambem: bc7enc_rdo dispara dezenas de -Wextra que nao
# sao nossos para consertar.
file(GLOB_RECURSE THIRD_PARTY_SRC ${CMAKE_SOURCE_DIR}/third_party/*.cpp ${CMAKE_SOURCE_DIR}/third_party/*.c)
if(THIRD_PARTY_SRC)
    set_source_files_properties(${THIRD_PARTY_SRC} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# Third-party headers are included as system headers so their warnings don't
# pollute our build output.
target_include_directories(eruption-engine SYSTEM PRIVATE
    ${vma_SOURCE_DIR}/include
    ${glm_SOURCE_DIR}
    ${tinygltf_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/ImGuizmo
)

# Link libraries
find_package(Iconv REQUIRED)

target_link_libraries(eruption-engine PRIVATE
    Vulkan::Vulkan
    glfw
    glm
    GPUOpen::VulkanMemoryAllocator
    nlohmann_json::nlohmann_json
    tinygltf
    ${CMAKE_DL_LIBS}
    Threads::Threads
    z
    Iconv::Iconv
)

# Threads
find_package(Threads REQUIRED)

# Preprocessor defines
target_compile_definitions(eruption-engine PRIVATE
    $<$<CONFIG:Debug>:ERUPTION_DEBUG>
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_FORCE_LEFT_HANDED
    GLM_FORCE_INTRINSICS
    GLM_FORCE_AVX2
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    TINYGLTF_NO_STB_IMAGE
    TINYGLTF_NO_STB_IMAGE_WRITE
)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eruption-engine PRIVATE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter -mavx2 -mfma
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O3 -march=native>
    )
elseif(MSVC)
    target_compile_options(eruption-engine PRIVATE
        /W4 /permissive-
        $<$<CONFIG:Debug>:/Zi /Od>
        $<$<CONFIG:Release>:/O2 /GL>
    )
endif()

# Assets, data and shaders are expected to be in the project root at runtime
add_dependencies(eruption-engine shaders)

# Tools
if(ERUPTION_BUILD_TOOLS)
    add_executable(eruption-normalgen
        tools/normalgen/main.cpp
    )
    target_include_directories(eruption-normalgen PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/stb
    )
    target_compile_definitions(eruption-normalgen PRIVATE)

    add_executable(pbr_cooker
        tools/pbr_cooker/main.cpp
        src/utils/PbrMapGen.cpp
        src/utils/NormalMapGen.cpp
    )
    target_include_directories(pbr_cooker PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${glm_SOURCE_DIR}
    )
    target_link_libraries(pbr_cooker PRIVATE glm)
    target_compile_definitions(pbr_cooker PRIVATE)

endif()

# Tests
enable_testing()

if(ERUPTION_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SOURCES "tests/*.cpp")
    # Sprite shadow harness is a standalone benchmark with its own main().
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/shadows/sprite_shadow_harness.cpp")
    # Map bundle test has its own target with a custom main().
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/maps/map_bundle_test.cpp")
    # PBR tests have their own target with a custom main().
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/pbr/pbr_tests.cpp")
    # GPU tests have their own target (eruption-gpu-tests) and require GLFW/Vulkan.
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/test_gpu_main.cpp")
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/test_vulkan_context.cpp")
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/test_shadow_gpu.cpp")
    list(REMOVE_ITEM TEST_SOURCES "${CMAKE_SOURCE_DIR}/tests/test_cloudfluff_gpu.cpp")
    # Standalone sprite shadow harness (has its own main()).
    add_executable(eruption-sprite-shadow-harness tests/shadows/sprite_shadow_harness.cpp)
    target_include_directories(eruption-sprite-shadow-harness PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${glm_SOURCE_DIR}
    )
    target_link_libraries(eruption-sprite-shadow-harness PRIVATE glm)
    target_include_directories(eruption-sprite-shadow-harness SYSTEM PRIVATE ${glm_SOURCE_DIR})
    target_compile_definitions(eruption-sprite-shadow-harness PRIVATE
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_LEFT_HANDED
        GLM_FORCE_AVX2
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(eruption-sprite-shadow-harness PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    # PBR cooker unit tests
    add_executable(eruption-pbr-tests tests/pbr/pbr_tests.cpp
        src/utils/PbrMapGen.cpp
        src/utils/NormalMapGen.cpp
    )
    target_include_directories(eruption-pbr-tests PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${glm_SOURCE_DIR}
    )
    target_link_libraries(eruption-pbr-tests PRIVATE glm)
    target_include_directories(eruption-pbr-tests SYSTEM PRIVATE ${glm_SOURCE_DIR})
    target_compile_definitions(eruption-pbr-tests PRIVATE
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_FORCE_LEFT_HANDED
        GLM_FORCE_AVX2
    )
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(eruption-pbr-tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME PBRMapGenTests
             COMMAND eruption-pbr-tests)
endif()

# Generate Linux .desktop shortcut
if(UNIX AND NOT APPLE)
    configure_file(
        "${CMAKE_SOURCE_DIR}/eruption-engine.desktop.in"
        "${CMAKE_BINARY_DIR}/eruption-engine.desktop"
        @ONLY
    )
    
    # Optional: Install it to user applications folder if 'make install' is run
    install(FILES "${CMAKE_BINARY_DIR}/eruption-engine.desktop"
            DESTINATION "$ENV{HOME}/.local/share/applications")
endif()
