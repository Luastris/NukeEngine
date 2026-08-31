# NukeEngine core — the ONE build of the engine DLL, every platform (the historical
# hand-maintained NukeEngine.vcxproj/.sln are retired). Dependencies come from the shared
# classic vcpkg pool via CMAKE_PREFIX_PATH (the superbuild root sets it; a standalone
# configure inherits the same via VCPKG_ROOT).
cmake_minimum_required(VERSION 3.21)   # TARGET_RUNTIME_DLLS (dependency-DLL deploy)
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

if(WIN32)
    # The defines the retired vcxproj carried (WIN32 spelled out: parts of the tree test the
    # plain macro, which only the IDE projects used to define).
    target_compile_definitions(NukeEngine PRIVATE
        WIN32 _WINDOWS _USRDLL _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

if(MSVC)
    set_property(TARGET NukeEngine PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(NukeEngine PRIVATE /MP /bigobj)   # parallel compile; huge TUs (World, Reflect.gen)
    # The historical engine Debug config is /O1 with RTC OFF (the retired vcxproj shipped
    # Optimization=MinSpace + BasicRuntimeChecks=Default): the engine is the hot half of a
    # Debug session, and the default /Od+/RTC1 doubles the DLL and drags the frame. Directory
    # scope — this file owns only the engine target.
    string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/Od" "/O1" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    # Release linked with symbols + dead-code stripping, as the vcxproj did (PDB stays in the
    # run dir as symbols; it never ships).
    target_link_options(NukeEngine PRIVATE $<$<NOT:$<CONFIG:Debug>>:/DEBUG /OPT:REF /OPT:ICF>)
else()
    target_compile_options(NukeEngine PRIVATE
        -Wno-c++11-narrowing     # MSVC compiles double->float in init-lists as a warning; the tree relies on it
        -Wno-unknown-attributes) # [[nuke::prop]]/[[nuke::func]] are nukegen reflection markers, not compiler attributes
endif()

# Reflection codegen BEFORE every compile, exactly as the retired vcxproj prebuild did:
# NukeGen rewrites src/reflect/Reflect.gen.cpp + the SDK doc when annotations changed (the
# superbuild orders NukeGen before this target, so the deployed exe exists).
set(_nukegen "${CMAKE_CURRENT_SOURCE_DIR}/../NukeUtils/bin/NukeGen${NUKE_HOSTTOOL_SUFFIX}")
if(WIN32 AND (EXISTS "${_nukegen}" OR TARGET NukeGen))
    add_custom_command(TARGET NukeEngine PRE_BUILD
        COMMAND "${_nukegen}" --module "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "NukeEngine reflection codegen (NukeGen)"
        VERBATIM)
endif()

target_precompile_headers(NukeEngine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pch.h")

# ---- dependencies (vcpkg) ----------------------------------------------------------------
# Windows links ONLY the boost components the engine references out-of-line (filesystem,
# thread — the historical import set), and by LIBRARY PATH, not imported target: the
# Boost::thread target's INTERFACE chain (chrono/date_time/atomic) would otherwise drag
# their unimported DLLs through $<TARGET_RUNTIME_DLLS> into the run dir and every dist.
# The two DLLs that ARE imported deploy explicitly below. atomic/chrono/container are
# header-only uses here (autolink pragmas resolve via the pool LIBPATH).
# ELF/Mach-O resolve at link time, so the other platforms keep the full component list.
if(WIN32)
    find_package(Boost REQUIRED COMPONENTS filesystem thread)
    set(_nuke_boost Boost::headers)
    set(_nuke_boost_dll_rel "")
    set(_nuke_boost_dll_dbg "")
    foreach(_c filesystem thread)
        get_target_property(_ir Boost::${_c} IMPORTED_IMPLIB_RELEASE)
        get_target_property(_id Boost::${_c} IMPORTED_IMPLIB_DEBUG)
        list(APPEND _nuke_boost optimized "${_ir}" debug "${_id}")
        get_target_property(_lr Boost::${_c} IMPORTED_LOCATION_RELEASE)
        get_target_property(_ld Boost::${_c} IMPORTED_LOCATION_DEBUG)
        list(APPEND _nuke_boost_dll_rel "${_lr}")
        list(APPEND _nuke_boost_dll_dbg "${_ld}")
    endforeach()
else()
    find_package(Boost REQUIRED COMPONENTS atomic chrono container filesystem system thread)
    set(_nuke_boost Boost::atomic Boost::chrono Boost::container Boost::filesystem Boost::system Boost::thread)
endif()
find_package(assimp CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(gdeflate CONFIG REQUIRED)   # overlay port (vcpkg-overlays/gdeflate): pak GDeflate codec
find_path(STB_INCLUDE_DIRS "stb_image.h" REQUIRED)

target_include_directories(NukeEngine PRIVATE ${STB_INCLUDE_DIRS})
# DiligentTools (same superbuild configure) vendors its own "ZLib" target and points
# ZLIB::ZLIB at it — linking the namespaced target would then emit the bare vendored name
# into the link line. Use the pool's library files directly on Windows.
if(WIN32)
    target_include_directories(NukeEngine PRIVATE ${ZLIB_INCLUDE_DIRS})
    set(_nuke_zlib optimized ${ZLIB_LIBRARY_RELEASE} debug ${ZLIB_LIBRARY_DEBUG})
else()
    set(_nuke_zlib ZLIB::ZLIB)
endif()
# PUBLIC where the engine's own headers expose the dependency (boost::function/container,
# glm, nlohmann in the API surface) — modules linking the NukeEngine target inherit them.
target_link_libraries(NukeEngine PUBLIC
    ${_nuke_boost}
    glm::glm
    nlohmann_json::nlohmann_json
    PRIVATE
    assimp::assimp
    meshoptimizer::meshoptimizer
    $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
    ${_nuke_zlib}
    gdeflate::gdeflate
    ${CMAKE_DL_LIBS})

# The historical run-dir layout (x64/<Config> on Windows): the editor, player and every
# module's deploy step navigate relative to it, so the tree stays uniform across platforms.
set_target_properties(NukeEngine PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${NUKE_RUN_SUBDIR}/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${NUKE_RUN_SUBDIR}/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${NUKE_RUN_SUBDIR}/$<CONFIG>"
    # Hosts and modules load siblings from the run dir: resolve @rpath there.
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_NAME_DIR TRUE)

# Windows: the dependency DLLs (boost, assimp + its transitives, zstd, zlib...) land beside
# the engine — the msbuild-era vcpkg applocal step did this; with the shared pool the copy
# is explicit. copy_if_different: cheap on incremental builds.
if(WIN32)
    # The boost DLL pair rides per config ($<SEMICOLON>: a raw ';' would split the genex).
    string(REPLACE ";" "$<SEMICOLON>" _nuke_boost_dbd "${_nuke_boost_dll_dbg}")
    string(REPLACE ";" "$<SEMICOLON>" _nuke_boost_dbr "${_nuke_boost_dll_rel}")
    add_custom_command(TARGET NukeEngine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:NukeEngine>
                $<IF:$<CONFIG:Debug>,${_nuke_boost_dbd},${_nuke_boost_dbr}>
                "${CMAKE_CURRENT_SOURCE_DIR}/${NUKE_RUN_SUBDIR}/$<CONFIG>"
        COMMAND_EXPAND_LISTS
        COMMENT "Deploying dependency DLLs beside NukeEngine.dll")
endif()
