# Camada de compatibilidade de plugins (Godot GDExtension, Unity nativo,
# modulos Unreal). Fica fora de src/ de proposito: o glob do motor nao a ve,
# e ela so' entra no build com -DERUPTION_MODULE_COMPAT=ON.
#
# Tambem compila sozinha, sem o motor, para testes rapidos:
#   cmake -S compat -B compat/build/sozinho -DCMAKE_BUILD_TYPE=Release
#   nice cmake --build compat/build/sozinho -j4
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(eruption-compat LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

file(GLOB COMPAT_FONTES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/host/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/godot/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/unity/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/unreal/*.cpp"
)

add_library(eruption-compat STATIC ${COMPAT_FONTES})
target_include_directories(eruption-compat PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(eruption-compat PUBLIC ${CMAKE_DL_LIBS})
set_target_properties(eruption-compat PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eruption-compat PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Executavel de teste: carrega plugins e roda quadros sem janela.
add_executable(eruption-compat-run "${CMAKE_CURRENT_SOURCE_DIR}/tools/compat_run.cpp")
target_link_libraries(eruption-compat-run PRIVATE eruption-compat)
set_target_properties(eruption-compat-run PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(eruption-compat-run PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Prova de render de plugin nativo Unity em OpenGL fora de tela (so' teste).
find_package(OpenGL QUIET COMPONENTS OpenGL EGL)
if(TARGET OpenGL::EGL AND TARGET OpenGL::GL)
    add_executable(eruption-compat-unity-gl EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tools/unity_gl_prova.cpp")
    target_link_libraries(eruption-compat-unity-gl PRIVATE eruption-compat OpenGL::EGL OpenGL::GL)
    set_target_properties(eruption-compat-unity-gl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(eruption-compat-unity-gl PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter)
    endif()
endif()
