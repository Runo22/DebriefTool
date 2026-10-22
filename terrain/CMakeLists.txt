# tdem — terrain height packs (standalone; also used via add_subdirectory).
cmake_minimum_required(VERSION 3.21)
project(tdem VERSION 1.0 LANGUAGES CXX)

option(TDEM_BUILD_TOOLS   "Build the tdem command-line tool" ${PROJECT_IS_TOP_LEVEL})
option(TDEM_BUILD_TESTS   "Build tdem tests"                 ${PROJECT_IS_TOP_LEVEL})

add_library(tdem STATIC src/tdem.cpp)
add_library(tdem::tdem ALIAS tdem)
target_include_directories(tdem PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(tdem PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(tdem PRIVATE /W4 /wd4996)
else()
    target_compile_options(tdem PRIVATE -Wall -Wextra)
endif()

# Directory with the bundled .tdem packs, for consumers that want to copy them.
set(TDEM_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data CACHE INTERNAL "")

if(TDEM_BUILD_TOOLS)
    add_executable(tdem_cli tools/tdem_cli.cpp)
    target_link_libraries(tdem_cli PRIVATE tdem)
    set_target_properties(tdem_cli PROPERTIES OUTPUT_NAME tdem)
endif()

if(TDEM_BUILD_TESTS)
    enable_testing()
    add_executable(tdem_test tests/tdem_test.cpp)
    target_link_libraries(tdem_test PRIVATE tdem)
    target_compile_definitions(tdem_test PRIVATE
        TDEM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
        TDEM_TEST_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/tests/vectors.json")
    add_test(NAME tdem_test COMMAND tdem_test)
endif()
