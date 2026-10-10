# Compila as extensoes GDExtension de teste (codigo de terceiros, licencas
# permissivas, baixado por compat/scripts/baixar_testes.sh em compat/testdata/).
# Nada daqui entra no motor: so' gera os .so que o host de compatibilidade
# carrega nos testes. Ver compat/TESTED_PLUGINS.md.
#
#   cmake -S compat/tests/godot -B compat/build/godot-tests -DCMAKE_BUILD_TYPE=Release
#   nice cmake --build compat/build/godot-tests -j4
cmake_minimum_required(VERSION 3.17)
project(eruption-compat-godot-tests LANGUAGES C CXX)

set(TESTDATA "${CMAKE_CURRENT_SOURCE_DIR}/../../testdata/godot")
if(NOT EXISTS "${TESTDATA}/godot-cpp/CMakeLists.txt")
    message(FATAL_ERROR "godot-cpp ausente: rode compat/scripts/baixar_testes.sh")
endif()

add_subdirectory("${TESTDATA}/godot-cpp" godot-cpp)

set(SAIDA "${CMAKE_CURRENT_BINARY_DIR}/bin")

# Cada extensao vira um .so com o nome da pasta de origem.
function(extensao_teste nome)
    set(fontes ${ARGN})
    add_library(${nome} SHARED ${fontes})
    target_link_libraries(${nome} PRIVATE godot-cpp)
    set_target_properties(${nome} PROPERTIES
        CXX_STANDARD 17
        CXX_VISIBILITY_PRESET hidden
        POSITION_INDEPENDENT_CODE ON
        LIBRARY_OUTPUT_DIRECTORY "${SAIDA}"
        PREFIX "lib")
    # Codigo de terceiros: avisos dele nao sao nossos para consertar.
    target_compile_options(${nome} PRIVATE -w)
endfunction()

# godot-cpp-template (Unlicense): ExampleClass (RefCounted) com print_type().
if(EXISTS "${TESTDATA}/godot-cpp-template/src/register_types.cpp")
    file(GLOB fontes "${TESTDATA}/godot-cpp-template/src/*.cpp")
    extensao_teste(gd_template ${fontes})
endif()

# GDExtensionSummator (Unlicense): Summator (RefCounted) add/reset/get_total.
if(EXISTS "${TESTDATA}/GDExtensionSummator/src/register_types.cpp")
    file(GLOB fontes "${TESTDATA}/GDExtensionSummator/src/*.cpp")
    extensao_teste(gd_summator ${fontes})
endif()

# GDExample da documentacao oficial (CC-BY 3.0): Sprite2D com _process().
if(EXISTS "${TESTDATA}/gdexample/src/register_types.cpp")
    file(GLOB fontes "${TESTDATA}/gdexample/src/*.cpp")
    extensao_teste(gd_gdexample ${fontes})
endif()

# YParticles3D (Unlicense): sistema de particulas em CPU (Node3D + RenderingServer).
if(EXISTS "${TESTDATA}/YParticles3D/src/register_types.cpp")
    file(GLOB_RECURSE fontes "${TESTDATA}/YParticles3D/src/*.cpp")
    extensao_teste(gd_yparticles3d ${fontes})
    target_include_directories(gd_yparticles3d PRIVATE "${TESTDATA}/YParticles3D/src")
endif()
