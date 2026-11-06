# Vendored third-party libraries

Every dependency lives here as plain source — no git submodules, no downloads
at configure time — so the repository clones and builds on an offline machine.
Each library keeps its own licence file.

| Library | Upstream | Commit | Licence |
|---|---|---|---|
| raylib (6.1-dev) | https://github.com/raysan5/raylib (`master`) | `0d78f10161f9d3df38b10bd33444451b7ee11eda` | zlib |
| flecs 4 | https://github.com/SanderMertens/flecs (`master`) | `1094ec7613a1584d2eaec0c5f18218d084e0b608` | MIT |
| Dear ImGui (1.92.9 WIP, docking) | https://github.com/ocornut/imgui (`docking`) | `5a76f2adf1b0403b86a45010121fb32a6bff8680` | MIT |
| ImPlot | https://github.com/epezent/implot (`master`) | `1351ab2c46d7a05a60f3533047bfba8a953520a1` | MIT |
| rlImGui | https://github.com/raylib-extras/rlImGui (`main`) | `552a3ad44f130d05055e00e11e1c3f7cf45e5657` | zlib |
| Assimp | https://github.com/assimp/assimp (`master`) | `0404c8758cf4a85825c241283804d764616af623` | BSD-3-Clause |
| yaml-cpp | https://github.com/jbeder/yaml-cpp | `2decf96e915d2b0c26c68c1659665789dfef2633` | MIT |
| tinyfiledialogs (v2.9.3) | https://sourceforge.net/projects/tinyfiledialogs/ | — | zlib |

## Removed from the upstream trees (not used by the build)

To keep the repository small, these were dropped; none is reachable with the
options set in the root `CMakeLists.txt`:

- `assimp/test` (212 MB of sample models; `ASSIMP_BUILD_TESTS=OFF`)
- `flecs/docs`, `flecs/test`, `flecs/examples` (`FLECS_TESTS` off)
- `raylib/examples`, `raylib/projects` (`BUILD_EXAMPLES=OFF`)
- `imgui/examples`
- `rlImGui/premake5*` (prebuilt premake binaries)
- `yaml-cpp/test` (`YAML_CPP_BUILD_TESTS=OFF`)
- every nested `.git*` file and `.github/` folder

`third_party/.gitignore` re-includes everything below this folder, so the root
ignore rules (`*.cmake`, `build/`, `*.lib`, …) can never silently drop library
files.

## Updating a library

1. On a machine with network access, check out the new upstream revision.
2. Replace the folder here with its contents (without `.git`), remove the
   folders listed above, and delete nested `.gitignore` / `.gitattributes` files.
3. Update the commit in the table, build, and commit.
