# Compila os plugins nativos de TESTE do Unity (codigo de terceiros com
# licenca permissiva, baixado por compat/scripts/baixar_testes.sh). Cada
# plugin compila contra os cabecalhos que ele mesmo traz; a camada compat
# nao usa esses cabecalhos. Ver compat/TESTED_PLUGINS.md.
#
#   cmake -S compat/tests/unity -B compat/build/unity-tests -DCMAKE_BUILD_TYPE=Release
#   nice cmake --build compat/build/unity-tests -j4
cmake_minimum_required(VERSION 3.17)
project(eruption-compat-unity-tests LANGUAGES C CXX)

set(TESTDATA "${CMAKE_CURRENT_SOURCE_DIR}/../../testdata/unity")
set(SAIDA "${CMAKE_CURRENT_BINARY_DIR}/bin")

# NativeRenderingPlugin (MIT): mesmas opcoes do Makefile GNU do proprio projeto.
set(NRP "${TESTDATA}/NativeRenderingPlugin/PluginSource/source")
if(EXISTS "${NRP}/RenderingPlugin.cpp")
    find_package(OpenGL REQUIRED)
    find_package(Vulkan REQUIRED)
    add_library(RenderingPlugin SHARED
        "${NRP}/RenderingPlugin.cpp"
        "${NRP}/RenderAPI.cpp"
        "${NRP}/RenderAPI_OpenGLCoreES.cpp"
        "${NRP}/RenderAPI_Vulkan.cpp")
    target_compile_definitions(RenderingPlugin PRIVATE SUPPORT_OPENGL_UNIFIED=1 SUPPORT_VULKAN=1 UNITY_LINUX=1)
    target_include_directories(RenderingPlugin PRIVATE "${NRP}" ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(RenderingPlugin PRIVATE OpenGL::GL)
    target_compile_options(RenderingPlugin PRIVATE -w)
    set_target_properties(RenderingPlugin PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${SAIDA}")
endif()
