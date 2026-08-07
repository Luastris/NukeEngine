# The run-dir subfolder under NukeEngine/ that holds the per-config runtime tree
# (<subdir>/<Config>: editor, player, engine lib, modules/, shaders/). Windows keeps the
# historical "x64" — the hand-maintained .sln owns that layout. Other platforms name it
# honestly (an arm64 Mac building into "x64" is a lie).
if(NOT DEFINED NUKE_RUN_SUBDIR)
    if(WIN32)
        set(NUKE_RUN_SUBDIR "x64")
    elseif(APPLE)
        set(NUKE_RUN_SUBDIR "macos")
    else()
        set(NUKE_RUN_SUBDIR "linux")
    endif()
endif()

# Host-tool executable suffix (NukeUtils/bin/NukeGen${NUKE_HOSTTOOL_SUFFIX}): ".exe" is a
# Windows-ism — POSIX tools carry no extension.
if(NOT DEFINED NUKE_HOSTTOOL_SUFFIX)
    if(WIN32)
        set(NUKE_HOSTTOOL_SUFFIX ".exe")
    else()
        set(NUKE_HOSTTOOL_SUFFIX "")
    endif()
endif()
