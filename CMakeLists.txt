# NukeEngine core — CMake build for NON-Windows platforms (Deuterium).
# On Windows the hand-maintained NukeEngine.vcxproj/.sln stays authoritative (nukegen
# prebuild, vcpkg autolink, dist deploy all live there); this file exists so macOS/Linux
# can build the same sources as a shared library. Dependencies come from vcpkg through
# the toolchain file (classic mode, triplet e.g. arm64-osx).
cmake_minimum_required(VERSION 3.20)
project(NukeEngineCore CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/NukeRunDir.cmake")   # NUKE_RUN_SUBDIR: x64 / macos / linux

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Windows build exports the annotated API surface only; on Mach-O/ELF we keep default
# visibility so the whole API is linkable — matches how the modules resolve engine symbols.

file(GLOB_RECURSE NUKE_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(APPEND NUKE_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/NukeEngine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/dllmain.cpp"     # no-op outside _WIN32
    "${CMAKE_CURRENT_SOURCE_DIR}/pch.cpp")

add_library(NukeEngine SHARED ${NUKE_CORE_SOURCES})

target_include_directories(NukeEngine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"                 # pch.h / framework.h / NukeEngine.h
    "${CMAKE_CURRENT_SOURCE_DIR}/deps")

target_compile_definitions(NukeEngine PRIVATE
    NUKEENGINE_EXPORTS
    GLM_ENABLE_EXPERIMENTAL)

if(NOT MSVC)
    target_compile_options(NukeEngine PRIVATE
        -Wno-c++11-narrowing     # MSVC compiles double->float in init-lists as a warning; the tree relies on it
        -Wno-unknown-attributes) # [[nuke::prop]]/[[nuke::func]] are nukegen reflection markers, not compiler attributes
endif()

target_precompile_headers(NukeEngine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")

# ---- dependencies (vcpkg) ----------------------------------------------------------------
find_package(Boost REQUIRED COMPONENTS atomic chrono container filesystem system thread)
find_package(assimp CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)

target_include_directories(NukeEngine PRIVATE ${STB_INCLUDE_DIRS})
# PUBLIC where the engine's own headers expose the dependency (boost::function/container,
# glm, nlohmann in the API surface) — modules linking the NukeEngine target inherit them.
target_link_libraries(NukeEngine PUBLIC
    Boost::atomic Boost::chrono Boost::container Boost::filesystem Boost::system Boost::thread
    glm::glm
    nlohmann_json::nlohmann_json
    PRIVATE
    assimp::assimp
    meshoptimizer::meshoptimizer
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
    ZLIB::ZLIB
    ${CMAKE_DL_LIBS})

# Same run-dir layout as Windows (x64/<Config>): the editor, player and every module's
# deploy step navigate relative to it, so the tree stays uniform across platforms.
set_target_properties(NukeEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${NUKE_RUN_SUBDIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${NUKE_RUN_SUBDIR}/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${NUKE_RUN_SUBDIR}/$<CONFIG>"
    # Hosts and modules load siblings from the run dir: resolve @rpath there.
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_NAME_DIR TRUE)
