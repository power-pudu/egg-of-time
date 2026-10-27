cmake_minimum_required(VERSION 3.25)
project(EggOfTime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# SDL3: an installed package (Windows SDK zip, Homebrew, distro), else built from the pinned source release
# (Ubuntu 24.04 and other distros without SDL3 packages; needs the X11/Wayland/ALSA/PulseAudio development
# packages for a usable window and sound).
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
    message(STATUS "SDL3 not installed: building SDL 3.4.18 from source")
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
        URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3)
    FetchContent_MakeAvailable(SDL3)
endif()

# FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample) plays the replacement cutscenes of movie
# mods (runtime/movie.c): Windows, a shared build's folder in CMAKE_PREFIX_PATH (its DLLs are copied next to the
# executable); macOS, Homebrew; Linux, the distro's development packages.
find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
set(FFMPEG_LIBRARIES)
foreach(lib avformat avcodec avutil swscale swresample)
    find_library(FFMPEG_${lib}_LIBRARY ${lib} REQUIRED)
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${lib}_LIBRARY})
endforeach()
set(RUNTIME_SOURCES
    runtime/main.c
    runtime/boot.c
    runtime/cpu.c
    runtime/display.c
    runtime/game_build.c
    runtime/card.c
    runtime/hw.c
    runtime/memory.c
    runtime/movie.c
    runtime/music.c
    runtime/patches.c
    runtime/pxi.c
    runtime/rt_dispatch.c
    runtime/sched.c
    runtime/snd.c
    runtime/spu.c
    runtime/state.c
    runtime/system_menu.c
    runtime/selftest.c
    runtime/single_screen.c
    runtime/thread.c
    runtime/ui_font.c
    runtime/video.c
    runtime/widescreen.c)

# Single screen mode: the boot and title screens laid out on the top screen, the top screen alone shown by
# default, Classic (one-screen) battles by default. OFF keeps the DS's two-screen presentation and defaults,
# for ports to devices with two screens.
option(EGG_SINGLE_SCREEN "Lay the game out for one screen" ON)
if(EGG_SINGLE_SCREEN)
    add_compile_definitions(EGG_SINGLE_SCREEN=1)
else()
    add_compile_definitions(EGG_SINGLE_SCREEN=0)
endif()

# EGG_RUNTIME_ONLY=ON builds the executable without the game library (no ROM needed): CI and releases, on
# machines that must not hold ROM-derived data. A release builds the game library on the player's machine
# (runtime/game_build.c, scripts/package.py).
option(EGG_RUNTIME_ONLY "Build only the runtime executable (CI, releases)" OFF)

add_executable(eggoftime ${RUNTIME_SOURCES})
# The executable exports the runtime (RT_API in runtime/recomp.h) to the game library; on Windows that also
# writes its import library, libeggoftime.a, which the game library links against.
set_target_properties(eggoftime PROPERTIES OUTPUT_NAME "Egg of Time" ENABLE_EXPORTS ON
    IMPORT_PREFIX "" IMPORT_SUFFIX ".a" ARCHIVE_OUTPUT_NAME "libeggoftime")
if(WIN32)
    target_link_options(eggoftime PRIVATE -Wl,--export-all-symbols)
    # Executable icon (also used by SDL for the window and taskbar).
    enable_language(RC)
    target_sources(eggoftime PRIVATE res/icon.rc)
endif()
target_compile_options(eggoftime PRIVATE -Wall -Wextra -fwrapv $<$<BOOL:${EGG_RUNTIME_ONLY}>:-Werror>)
target_include_directories(eggoftime PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(eggoftime PRIVATE SDL3::SDL3 ${FFMPEG_LIBRARIES})
if(UNIX)
    target_link_libraries(eggoftime PRIVATE m)
endif()
if(UNIX AND NOT APPLE)
    set_target_properties(eggoftime PROPERTIES
        BUILD_RPATH   "$ORIGIN"
        INSTALL_RPATH "$ORIGIN")
endif()

if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET eggoftime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:eggoftime>)
endif()
if(WIN32)
    get_filename_component(FFMPEG_ROOT ${FFMPEG_INCLUDE_DIR} DIRECTORY)
    file(GLOB FFMPEG_DLLS ${FFMPEG_ROOT}/bin/avformat-*.dll ${FFMPEG_ROOT}/bin/avcodec-*.dll ${FFMPEG_ROOT}/bin/avutil-*.dll
         ${FFMPEG_ROOT}/bin/swscale-*.dll ${FFMPEG_ROOT}/bin/swresample-*.dll)
    if(FFMPEG_DLLS)
        add_custom_command(TARGET eggoftime POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${FFMPEG_DLLS} $<TARGET_FILE_DIR:eggoftime>)
    endif()
endif()

# Movie and music mods: empty mods/cutscenes and mods/music folders next to the executable, each with the list of
# file names (readme.txt).
add_custom_command(TARGET eggoftime POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:eggoftime>/mods/cutscenes
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/res/mods/cutscenes/readme.txt
            $<TARGET_FILE_DIR:eggoftime>/mods/cutscenes
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:eggoftime>/mods/music
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/res/mods/music/readme.txt
            $<TARGET_FILE_DIR:eggoftime>/mods/music)

if(EGG_RUNTIME_ONLY)
    return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Recompiled game code. Needs the dsd project (extract/ + config/) from NOTES.md.
set(GEN_DIR ${CMAKE_BINARY_DIR}/gen)
set(RECOMP_DIR ${CMAKE_SOURCE_DIR}/tools/recomp)
file(GLOB RECOMP_INPUTS
    ${RECOMP_DIR}/*.py
    ${RECOMP_DIR}/*.txt
    ${CMAKE_SOURCE_DIR}/config/arm9/*.txt
    ${CMAKE_SOURCE_DIR}/config/arm9/*/*.txt
    ${CMAKE_SOURCE_DIR}/config/arm9/overlays/*/*.txt)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${RECOMP_INPUTS})
execute_process(
    COMMAND ${Python3_EXECUTABLE} -I ${RECOMP_DIR}/recomp.py ${CMAKE_SOURCE_DIR} ${GEN_DIR}
    RESULT_VARIABLE recomp_result)
if(NOT recomp_result EQUAL 0)
    message(FATAL_ERROR "recompiler failed")
endif()
include(${GEN_DIR}/sources.cmake)

# The game library: the recompiled code, in game/ next to the executable, loaded at start (rt_game_load). Release
# builds make it on the player's machine instead (runtime/game_build.c); this target is the same build.
add_library(eggoftime_game MODULE ${GEN_SOURCES})
set_target_properties(eggoftime_game PROPERTIES PREFIX "" OUTPUT_NAME "eggoftime_game"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/game LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/game)
target_include_directories(eggoftime_game PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${GEN_DIR})
target_compile_definitions(eggoftime_game PRIVATE RT_GAME_LIB)
# Generated code: huge functions with many labels; keep optimisation moderate so builds stay fast.
target_compile_options(eggoftime_game PRIVATE -O1 -w -fwrapv)
target_link_libraries(eggoftime_game PRIVATE eggoftime)
