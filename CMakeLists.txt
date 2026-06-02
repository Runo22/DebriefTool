cmake_minimum_required(VERSION 3.25)
project(debrief VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ────────────────────────────────────────────────────────────────────
option(DEBRIEF_TRACY  "Enable Tracy profiler instrumentation" OFF)
option(DEBRIEF_TESTS  "Build unit tests"                      OFF)

# ── Platform detection ─────────────────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(DEBRIEF_WINDOWS)
elseif(APPLE)
    add_compile_definitions(DEBRIEF_MACOS)
endif()

# ── Global flags — applied to ALL targets including submodules ─────────────────
#
#  IMPORTANT: keep this section MINIMAL and submodule-safe.
#  Strict flags (/WX, -Werror, /W4, -Wall) go on the `debrief` target below,
#  NOT here — otherwise Assimp/Raylib/Flecs warnings become build errors.
#
if(MSVC)
    add_compile_options(
        /MP              # parallel compilation
        /wd4996          # 'memcpy'/'strncpy'/etc "may be unsafe" — suppress globally
        /wd4267          # size_t->int narrowing in C library headers
        /wd4244          # double->float narrowing in C library headers
        /wd4305          # double->float truncation in third-party initializers
        /wd4100          # unreferenced formal parameter in third-party code
        /wd4127          # "conditional expression is constant" in Flecs templates (VS2022 cl)
    )
    add_compile_options(/Zc:__cplusplus /std:c++latest)
    add_compile_definitions(
        _CRT_SECURE_NO_WARNINGS   # suppress the entire memcpy/strncpy "unsafe" family
        _CRT_NONSTDC_NO_WARNINGS  # suppress _strdup/_stricmp POSIX name deprecation
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    # Suppress deprecated-declaration noise from third-party libs on macOS Clang 15+
    # (strncpy, bzero, etc. are deprecated in newer Apple SDKs)
    add_compile_options(-Wno-deprecated-declarations)
    # Suppress Clang 16+ unsafe-buffer-usage warnings in third-party C code
    add_compile_options(-Wno-unsafe-buffer-usage)
endif()

# ── Third-party: Raylib ────────────────────────────────────────────────────────
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/raylib)

# ── Third-party: Flecs ────────────────────────────────────────────────────────
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/flecs)

# ── Third-party: Assimp (only the importers we need — much faster compile) ────
set(ASSIMP_WARNINGS_AS_ERRORS         OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER   ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER   ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER  ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL              OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT            ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/assimp)

# ── Third-party: ImGui + ImPlot ───────────────────────────────────────────────
set(IMGUI_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
)
# No custom GL loader — let ImGui use its bundled GLAD loader

# ── Third-party: rlImGui ──────────────────────────────────────────────────────
set(RLIMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rlImGui)
add_library(rlimgui STATIC ${RLIMGUI_DIR}/rlImGui.cpp)
target_include_directories(rlimgui PUBLIC ${RLIMGUI_DIR})
target_link_libraries(rlimgui PUBLIC imgui raylib)

# ── Third-party: Tracy (optional) ─────────────────────────────────────────────
if(DEBRIEF_TRACY)
    add_subdirectory(third_party/tracy)
    add_compile_definitions(TRACY_ENABLE)
endif()

# ── Third-party: yaml-cpp ─────────────────────────────────────────────────────
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/yaml-cpp)

# ── Main executable ────────────────────────────────────────────────────────────
file(GLOB_RECURSE DEBRIEF_SRC "src/*.cpp" "src/*.hpp")

add_executable(debrief ${DEBRIEF_SRC})

target_include_directories(debrief PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMPLOT_DIR}
)

target_link_libraries(debrief PRIVATE
    raylib
    flecs_static
    assimp
    imgui
    rlimgui
    yaml-cpp
)

# ── Strict flags for OUR code only — NOT propagated to submodules ──────────────
if(MSVC)
    target_compile_options(debrief PRIVATE
        /W4           # high warning level
        /WX           # warnings-as-errors for our own code only
        /permissive-  # strict standards conformance
        /arch:AVX2    # AVX2 intrinsics on x64
        /wd4201       # anonymous struct/union (used in raymath.h headers we include)
        /wd4324       # structure padded due to alignas — intentional in SPSCQueue
    )
    # NOGDI/NOUSER prevent wingdi.h and winuser.h from being included via winsock2.h
    # in our TUs. Those headers declare Rectangle() and DrawText() as Win32 API
    # functions that conflict with raylib's Rectangle struct and DrawText function.
    # Applied per-target so GLFW (a separate target) still gets those headers.
    target_compile_definitions(debrief PRIVATE NOGDI NOUSER)
else()
    target_compile_options(debrief PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -ffast-math
        # -march=native is commented out for portability across machines;
        # uncomment locally for maximum performance on your build machine.
        # -march=native
    )
endif()

# ── Platform link libs ─────────────────────────────────────────────────────────
if(WIN32)
    target_link_libraries(debrief PRIVATE ws2_32 winmm)
endif()

if(APPLE)
    target_link_libraries(debrief PRIVATE
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreAudio"
        "-framework AudioToolbox"
    )
endif()

if(DEBRIEF_TRACY)
    target_link_libraries(debrief PRIVATE TracyClient)
endif()

# ── Post-build: copy assets ────────────────────────────────────────────────────
add_custom_command(TARGET debrief POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:debrief>/assets
    COMMENT "Copying assets..."
)

if(DEBRIEF_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
