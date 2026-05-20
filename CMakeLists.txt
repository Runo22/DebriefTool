cmake_minimum_required(VERSION 3.25)
project(debrief VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ────────────────────────────────────────────────────────────────────
option(DEBRIEF_TRACY   "Enable Tracy profiler instrumentation" OFF)
option(DEBRIEF_TESTS   "Build unit tests"                      OFF)

# ── Platform detection ─────────────────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(DEBRIEF_WINDOWS NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    add_compile_definitions(DEBRIEF_MACOS)
endif()

# ── Compiler flags ─────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(
        /W4 /WX /MP /permissive-
        /arch:AVX2
        /wd4201   # anonymous struct/union
        /wd4996   # deprecated POSIX names
    )
    # Enable C++23 std::expected and std::print
    add_compile_options(/Zc:__cplusplus /std:c++latest)
else()
    add_compile_options(
        -Wall -Wextra -Wno-unused-parameter
        -march=native -ffast-math
    )
endif()

# ── Third-party: Raylib ────────────────────────────────────────────────────────
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES    OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/raylib)

# ── Third-party: Flecs ────────────────────────────────────────────────────────
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/flecs)

# ── Third-party: Assimp (minimal importers for speed) ─────────────────────────
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER   ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_IMPORTER   ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER  ON  CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS          OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL              OFF CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT            ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/assimp)

# ── Third-party: ImGui (docking branch) ───────────────────────────────────────
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
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_CUSTOM)

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

# ── Main executable ────────────────────────────────────────────────────────────
file(GLOB_RECURSE DEBRIEF_SRC
    "src/*.cpp"
    "src/*.hpp"
)

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
)

if(DEBRIEF_TRACY)
    target_link_libraries(debrief PRIVATE TracyClient)
endif()

# Windows: Winsock + multimedia timer
if(WIN32)
    target_link_libraries(debrief PRIVATE ws2_32 winmm)
endif()

# macOS: system frameworks
if(APPLE)
    target_link_libraries(debrief PRIVATE
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreAudio"
        "-framework AudioToolbox"
    )
endif()

# ── Post-build: copy assets ────────────────────────────────────────────────────
add_custom_command(TARGET debrief POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:debrief>/assets
    COMMENT "Copying assets..."
)

# ── Tests ─────────────────────────────────────────────────────────────────────
if(DEBRIEF_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
