cmake_minimum_required(VERSION 3.16)
project(arcade_foundry_cores_lindbergh C)

# The core interface header, shared with the other core repos (submodule
# common/, Arcade-Foundry-Cores-Common).
set(AFCORE_INCLUDE ${CMAKE_SOURCE_DIR}/common/include)
if(NOT EXISTS ${AFCORE_INCLUDE}/afcore.h)
  message(FATAL_ERROR "common/ is empty: run git submodule update --init")
endif()

# The game's ELF is translated at build time; nothing derived from it is committed.
set(GAME "hotd4a" CACHE STRING "game directory under games/")
# per-game defaults (games/<game>/game.cmake): HotD4's when a game has none
set(GAME_GEN_COUNT 54)
set(GAME_TITLE "The House of the Dead 4")
set(GAME_WIDTH 1280)
set(GAME_HEIGHT 768)
set(GAME_CONTROLS gun)            # gun or driving: what the core maps the front end's controls to
set(GAME_DRIVE_BOARD position)    # the steering drive board on /dev/tts/0: position (Race TV) or status (OutRun 2 SP)
set(GAME_CORE_VERSION 1.0.0)      # the game's core version (afcore_api.core_version)
if(EXISTS ${CMAKE_SOURCE_DIR}/games/${GAME}/game.cmake)
  include(${CMAKE_SOURCE_DIR}/games/${GAME}/game.cmake)
endif()
# games.toml's games (tools/gen_games.py writes cmake/games.cmake): their
# title, controls and core version, which is bumped there with a change to
# how the game plays or saves
include(${CMAKE_SOURCE_DIR}/cmake/games.cmake)
if(DEFINED LB_TITLE_${GAME})
  set(GAME_TITLE "${LB_TITLE_${GAME}}")
  set(GAME_CONTROLS ${LB_CONTROLS_${GAME}})
  set(GAME_CORE_VERSION ${LB_CORE_VERSION_${GAME}})
endif()
set(GAME_ELF "" CACHE FILEPATH "path to the game's main ELF (e.g. disk0/elf/hod4M.elf)")
set(GAME_TEST_ELF "" CACHE FILEPATH "optional: the cabinet's test-menu ELF (e.g. disk0/elf/hod4testM.elf)")
set(GL_XML "${CMAKE_SOURCE_DIR}/extern/gl.xml" CACHE FILEPATH "Khronos gl.xml registry")
if(NOT EXISTS ${GL_XML})
  message(FATAL_ERROR "missing ${GL_XML}: curl -L -o extern/gl.xml "
    "https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml")
endif()
if(NOT GAME_ELF)
  message(FATAL_ERROR "set -DGAME_ELF=/path/to/game.elf")
endif()

option(LB_RUN "build lindbergh-run (SDL window, keyboard and mouse)" ON)
option(LB_CORE "build the game's core library for Arcade Foundry: Insert Coin (afcore_<game>)" OFF)
set(CORE_ROMSET "${GAME}" CACHE STRING "the core's romset name: afcore_api_<romset>")
set(CORE_TITLE "${GAME_TITLE}" CACHE STRING "the core's game title")
set(CORE_WIDTH ${GAME_WIDTH} CACHE STRING "the game's screen width")
set(CORE_HEIGHT ${GAME_HEIGHT} CACHE STRING "the game's screen height")

# Python: the one shared venv (Cores-Common tools/afic-venv: AFIC_VENV,
# default ~/venvs/afic) when there is one and no Python3_EXECUTABLE is given
if(NOT Python3_EXECUTABLE)
  if(DEFINED ENV{AFIC_VENV})
    set(AFIC_VENV "$ENV{AFIC_VENV}")
  else()
    set(AFIC_VENV "$ENV{HOME}/venvs/afic")
  endif()
  if(EXISTS "${AFIC_VENV}/bin/python3")
    set(Python3_EXECUTABLE "${AFIC_VENV}/bin/python3" CACHE FILEPATH "Python (the shared afic venv)")
  endif()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# mtlgles (Cores-Common's lib/mtlgles): the iOS path's gl4es drawing through our
# own OpenGL ES 3.0 subset on Metal instead of Apple's OpenGL ES, on macOS or iOS.
# The default for every iOS build and every macOS core; -DLB_MTLGLES=OFF keeps
# the old renderers (Apple's OpenGL ES on iOS, desktop OpenGL on macOS) for one
# release. lindbergh-run (LB_CORE OFF) stays on desktop OpenGL through SDL.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR (APPLE AND LB_CORE))
  set(LB_MTLGLES_DEFAULT ON)
else()
  set(LB_MTLGLES_DEFAULT OFF)
endif()
option(LB_MTLGLES "macOS or iOS: draw through gl4es and mtlgles on Metal" ${LB_MTLGLES_DEFAULT})
if(LB_MTLGLES AND NOT APPLE)
  message(FATAL_ERROR "LB_MTLGLES needs Metal (macOS or iOS)")
endif()
# iOS (iPadOS): the core only, on OpenGL ES through gl4es (desktop GL 2.x, fixed
# function and ARB programs translated to GLES); see runtime/host_core_ios.m
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR (APPLE AND LB_MTLGLES))
  set(LB_IOS ON)
  set(LB_RUN OFF)
  set(LB_CORE ON)
  enable_language(OBJC)
  add_compile_definitions(LB_IOS GLES_SILENCE_DEPRECATION GL_SILENCE_DEPRECATION)
  if(LB_MTLGLES)
    add_compile_definitions(LB_MTLGLES)
    # only mtlgles from Common: the rest of it is not built (EXCLUDE_FROM_ALL)
    set(AFC_MTLGLES ON CACHE BOOL "" FORCE)
    set(AFC_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(common ${CMAKE_BINARY_DIR}/common EXCLUDE_FROM_ALL)
  endif()
  include(FetchContent)
  # gl4es (MIT, ptitSeb/gl4es), fetched at a fixed commit at build time
  set(STATICLIB ON CACHE BOOL "" FORCE)
  set(NOEGL ON CACHE BOOL "" FORCE)
  set(NOX11 ON CACHE BOOL "" FORCE)
  set(NO_LOADER ON CACHE BOOL "" FORCE)
  set(NO_INIT_CONSTRUCTOR ON CACHE BOOL "" FORCE)
  # The archive is downloaded once per machine into a shared cache and every build
  # folder (each set, platform, clean rebuild) extracts its own copy from there: the
  # build patches gl4es in place, so only the download is shared, never the source.
  # AF_DOWNLOAD_CACHE (cache variable or environment) moves the cache; GL4ES_ARCHIVE
  # names an archive directly. The SHA-256 is checked either way.
  set(GL4ES_COMMIT 2037865590d410bcd7b8d1f22b841472ed6d39ac)
  set(GL4ES_SHA256 eaee3043a95eab40570539592f3f85f23d855286418dfacd961fa91d5211560d)
  if(DEFINED ENV{AF_DOWNLOAD_CACHE})
    set(af_cache_default "$ENV{AF_DOWNLOAD_CACHE}")
  else()
    set(af_cache_default "$ENV{HOME}/.cache/arcade-foundry/downloads")
  endif()
  set(AF_DOWNLOAD_CACHE "${af_cache_default}" CACHE PATH
    "Folder for downloads shared by every build folder on this machine")
  set(GL4ES_ARCHIVE "${AF_DOWNLOAD_CACHE}/gl4es-${GL4ES_COMMIT}.tar.gz" CACHE FILEPATH
    "gl4es source archive at the pinned commit (downloaded here when missing)")
  if(NOT FETCHCONTENT_SOURCE_DIR_GL4ES)
    if(EXISTS "${GL4ES_ARCHIVE}")
      file(SHA256 "${GL4ES_ARCHIVE}" have)
      if(NOT have STREQUAL GL4ES_SHA256)
        message(STATUS "gl4es: ${GL4ES_ARCHIVE} fails its SHA-256, downloading it again")
        file(REMOVE "${GL4ES_ARCHIVE}")
      endif()
    endif()
    if(NOT EXISTS "${GL4ES_ARCHIVE}")
      get_filename_component(dir "${GL4ES_ARCHIVE}" DIRECTORY)
      file(MAKE_DIRECTORY "${dir}")
      string(RANDOM LENGTH 8 tag)
      set(part "${GL4ES_ARCHIVE}.${tag}.part")
      message(STATUS "gl4es: downloading ${GL4ES_COMMIT} once into ${dir}")
      file(DOWNLOAD
        "https://codeload.github.com/ptitSeb/gl4es/tar.gz/${GL4ES_COMMIT}" "${part}"
        SHOW_PROGRESS STATUS st)
      list(GET st 0 code)
      list(GET st 1 why)
      if(code EQUAL 0)
        file(SHA256 "${part}" have)
        if(NOT have STREQUAL GL4ES_SHA256)
          set(code 1)
          set(why "SHA-256 ${have}, expected ${GL4ES_SHA256}")
        endif()
      endif()
      if(NOT code EQUAL 0)
        file(REMOVE "${part}")
        message(FATAL_ERROR "gl4es download failed: ${why}")
      endif()
      # a rename, so a stopped download never leaves a half archive under the real name
      file(RENAME "${part}" "${GL4ES_ARCHIVE}")
    endif()
  endif()
  FetchContent_Declare(gl4es
    URL "${GL4ES_ARCHIVE}"
    URL_HASH SHA256=${GL4ES_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  # cmake/gl4es_fixes.cmake patches the gl4es it finds extracted, once, and stamps it
  # with its own SHA-256: one stamped by another version of it (a merge, an edit) or
  # not stamped (a configure that stopped) is extracted again from the download
  set(GL4ES_FIXES_STAMP "${FETCHCONTENT_BASE_DIR}/gl4es-src/.gl4es_fixes")
  file(SHA256 "${CMAKE_SOURCE_DIR}/cmake/gl4es_fixes.cmake" GL4ES_FIXES_SHA)
  if(NOT FETCHCONTENT_SOURCE_DIR_GL4ES AND EXISTS "${FETCHCONTENT_BASE_DIR}/gl4es-src")
    set(stamp "")
    if(EXISTS "${GL4ES_FIXES_STAMP}")
      file(READ "${GL4ES_FIXES_STAMP}" stamp)
    endif()
    if(NOT stamp STREQUAL GL4ES_FIXES_SHA)
      message(STATUS "gl4es: patched by another cmake/gl4es_fixes.cmake, extracted again")
      file(REMOVE_RECURSE "${FETCHCONTENT_BASE_DIR}/gl4es-src"
        "${FETCHCONTENT_BASE_DIR}/gl4es-subbuild/gl4es-populate-prefix/src/gl4es-populate-stamp")
    endif()
  endif()
  FetchContent_MakeAvailable(gl4es)
  include(cmake/gl4es_fixes.cmake)
  target_compile_options(GL PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/gl4es_apple.h)
  # gl4es puts its library in the top source folder (lib/); keep it in the build
  set_target_properties(GL PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/gl4es)
  # the runtime's GL calls by name reach gl4es's functions (Mach-O has no aliases)
  set(GL4ES_NAMES ${CMAKE_BINARY_DIR}/gl4es/gl4es_names.h)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gl4es_names.py ${gl4es_SOURCE_DIR} ${GL4ES_NAMES})
  set(GL_INCLUDES ${CMAKE_BINARY_DIR}/gl4es ${gl4es_SOURCE_DIR}/include)
else()
  find_package(OpenGL REQUIRED)
  # the core library uses SDL's OpenGL headers only (the same on every platform); lindbergh-run links SDL
  find_package(SDL2 REQUIRED)
  set(GL_INCLUDES $<TARGET_PROPERTY:SDL2::SDL2,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(APPLE)
  # macOS OpenGL is deprecated but still provides the legacy 2.1 profile the games use
  add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()
if(GAME_CONTROLS STREQUAL "driving")
  add_compile_definitions(LB_DRIVING)   # the steering rests at its centre
endif()
if(GAME_DRIVE_BOARD STREQUAL "status")
  add_compile_definitions(LB_DRIVE_STATUS)
endif()
# the raw values of a gun's left and right edges, then top and bottom, where
# the game's default calibration has them (Let's Go Jungle: a quarter of the
# range, both reversed); without it, the whole range. GAME_GUN_SWAP_XY: the
# first channel of each gun is the vertical one.
if(GAME_GUN_RANGE)
  string(REPLACE ";" "," gun_range "${GAME_GUN_RANGE}")
  add_compile_definitions(LB_GUN_RANGE=${gun_range})
endif()
if(GAME_GUN_SWAP_XY)
  add_compile_definitions(LB_GUN_SWAP_XY)
endif()
# where this game keeps CRI Sofdec's IDCT tables (runtime/sofdec_idct.h): its
# DCT_IsseTrans is then done natively (games/<game>/overrides.txt lists it)
# the text a game formats on each frame of a boot screen that does a step a
# frame: its frames don't wait while it shows (hle_libc.c, LB_BOOT_TEXT)
if(GAME_BOOT_TEXT)
  add_compile_definitions(LB_BOOT_TEXT=${GAME_BOOT_TEXT})
endif()
if(GAME_SOFDEC_IDCT)
  string(REPLACE ";" "," sofdec_idct "${GAME_SOFDEC_IDCT}")
  add_compile_definitions(LB_SOFDEC_IDCT=${sofdec_idct})
endif()
# the settings the front end chooses (afcore options), "name=value|..." lines
if(GAME_OPTIONS)
  set(core_options "CORE_OPTIONS=\"${GAME_OPTIONS}\"")
endif()
# a region to choose: the runtime writes it into the EEPROM's system data
if(GAME_OPTIONS MATCHES "region=")
  add_compile_definitions(LB_REGION)
endif()
# the game ID its keychip reports, for games that check it (games/<game>/keychip.txt)
if(EXISTS ${CMAKE_SOURCE_DIR}/games/${GAME}/keychip.txt)
  file(STRINGS ${CMAKE_SOURCE_DIR}/games/${GAME}/keychip.txt keychip REGEX "^[A-Z0-9]+$" LIMIT_COUNT 1)
  add_compile_definitions(LB_KEYCHIP_ID="${keychip}")
endif()
# everything goes into the core library too, which exports one symbol
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

set(GEN_COUNT ${GAME_GEN_COUNT} CACHE STRING "number of gen_NNN.c files for the main ELF")
set(GEN_TEST_COUNT 8 CACHE STRING "number of gen_NNN.c files for the test ELF")

file(GLOB RECOMP_PY ${CMAKE_SOURCE_DIR}/common/tools/x86recomp/*.py)
set(WARN -O2 -g -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)
# The optimisation level of the translated C (gen_NNN.c). -O1 compiles the
# 50-odd files of a game in a few minutes and has run every game so far; -O2
# takes several times as long and, measured on HotD4 in October, changed
# nothing the GPU-bound frames could show. Set -DLB_GEN_OPT=-O2 to try it on a
# game (Insert-Coin #288 3e).
set(LB_GEN_OPT "-O1" CACHE STRING "optimisation level for the translated C (-O1, -O2)")

# Translate one ELF into its own gen directory: an object library whose
# functions are prefixed with name_ and whose tables are gen_program_<name>.
function(lindbergh_program name elf game_dir gen count)
  math(EXPR last "${count} - 1")
  set(srcs ${gen}/gen_tables.c)
  foreach(i RANGE ${last})
    string(LENGTH "${i}" n)
    if(n EQUAL 1)
      set(i "00${i}")
    elseif(n EQUAL 2)
      set(i "0${i}")
    endif()
    list(APPEND srcs ${gen}/gen_${i}.c)
  endforeach()
  # shared libraries the game ships next to its ELF, translated with it
  set(libs_args)
  set(libs_deps)
  if(EXISTS ${game_dir}/libs.txt)
    set(libs_args --libs ${game_dir}/libs.txt)
    set(libs_deps ${game_dir}/libs.txt)
  endif()
  add_custom_command(
    OUTPUT ${srcs} ${gen}/gen.h
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/common/tools/x86recomp/recomp.py ${elf} ${gen} --name ${name}
            --files ${count} --overrides ${game_dir}/overrides.txt --patches ${game_dir}/patches.txt ${libs_args}
    DEPENDS ${RECOMP_PY} ${elf} ${game_dir}/overrides.txt ${game_dir}/patches.txt ${libs_deps}
    COMMENT "Translating ${elf}")
  add_library(gen_${name} OBJECT ${srcs})
  target_include_directories(gen_${name} PRIVATE runtime ${gen})
  # translated code: LB_GEN_OPT (default -O1, see below), exact IEEE behaviour, no aliasing assumptions
  target_compile_options(gen_${name} PRIVATE ${LB_GEN_OPT} -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
endfunction()

set(ELFS ${GAME_ELF})
lindbergh_program(game ${GAME_ELF} ${CMAKE_SOURCE_DIR}/games/${GAME} ${CMAKE_BINARY_DIR}/gen ${GEN_COUNT})
if(GAME_TEST_ELF)
  list(APPEND ELFS ${GAME_TEST_ELF})
  lindbergh_program(test ${GAME_TEST_ELF} ${CMAKE_SOURCE_DIR}/games/${GAME}-test
                    ${CMAKE_BINARY_DIR}/gen-test ${GEN_TEST_COUNT})
endif()

# one GL bridge for every program: the GL commands any of them names
set(GLGEN ${CMAKE_BINARY_DIR}/glgen)
add_custom_command(
  OUTPUT ${GLGEN}/glnames.txt
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GLGEN}
  COMMAND sh ${CMAKE_SOURCE_DIR}/tools/glnames.sh ${GLGEN}/glnames.txt ${ELFS}
  DEPENDS ${ELFS} ${CMAKE_SOURCE_DIR}/tools/glnames.sh)
add_custom_command(
  OUTPUT ${GLGEN}/hle_gl_gen.c
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/glgen.py ${GL_XML} ${GLGEN}/glnames.txt ${GLGEN}/hle_gl_gen.c
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/glgen.py ${GL_XML} ${GLGEN}/glnames.txt)

# the runtime without a host: lindbergh-run adds host_sdl.c, the core core_api.c
add_library(lb_runtime OBJECT
  runtime/core.c runtime/hle_libc.c runtime/hle_thread.c runtime/hle_gfx.c
  runtime/hle_lindbergh.c runtime/hle_audio.c runtime/glsl_fix.c runtime/nv_vertex_program.c runtime/arb_lower.c runtime/input.c ${GLGEN}/hle_gl_gen.c)
target_include_directories(lb_runtime PRIVATE runtime ${AFCORE_INCLUDE}) # af_debug.h
target_compile_options(lb_runtime PRIVATE ${WARN})
target_include_directories(lb_runtime PRIVATE ${GL_INCLUDES})

if(LB_RUN)
  function(lindbergh_run target name)
    add_executable(${target} runtime/main_linux.c runtime/host_sdl.c
                   $<TARGET_OBJECTS:lb_runtime> $<TARGET_OBJECTS:gen_${name}>)
    target_include_directories(${target} PRIVATE runtime ${AFCORE_INCLUDE})
    target_compile_definitions(${target} PRIVATE LB_PROGRAM=gen_program_${name})
    target_compile_options(${target} PRIVATE ${WARN})
    target_link_libraries(${target} PRIVATE SDL2::SDL2 OpenGL::GL m pthread)
  endfunction()
  lindbergh_run(lindbergh-run game)
  if(GAME_TEST_ELF)
    lindbergh_run(lindbergh-run-test test)
  endif()
endif()

if(LB_CORE)
  set(core afcore_${CORE_ROMSET})
  # a program's path under the game folder (the one holding disk0), its size
  function(core_program_path elf var)
    string(FIND "${elf}" "/disk0/" at REVERSE)
    if(at LESS 0)
      message(FATAL_ERROR "${elf} is not inside a game folder's disk0")
    endif()
    math(EXPR at "${at} + 1")
    string(SUBSTRING "${elf}" ${at} -1 rel)
    file(SIZE ${elf} size)
    set(${var} "${rel}" PARENT_SCOPE)
    set(${var}_SIZE ${size} PARENT_SCOPE)
  endfunction()
  core_program_path(${GAME_ELF} game_elf)
  set(core_programs "CORE_GAME_ELF=\"${game_elf}\"" CORE_GAME_ELF_SIZE=${game_elf_SIZE})
  set(core_gen $<TARGET_OBJECTS:gen_game>)
  # the cabinet's test menu, when it is a program of its own
  if(GAME_TEST_ELF)
    core_program_path(${GAME_TEST_ELF} test_elf)
    list(APPEND core_programs "CORE_TEST_ELF=\"${test_elf}\"" CORE_TEST_ELF_SIZE=${test_elf_SIZE})
    list(APPEND core_gen $<TARGET_OBJECTS:gen_test>)
  endif()
  if(LB_IOS)
    set(core_host runtime/host_core_ios.m runtime/gl4es_apple.c runtime/glsl_pp.c)
  else()
    set(core_host runtime/host_core.c)
  endif()
  add_library(${core} SHARED runtime/core_api.c ${core_host}
              $<TARGET_OBJECTS:lb_runtime> ${core_gen})
  target_include_directories(${core} PRIVATE runtime ${AFCORE_INCLUDE} ${GL_INCLUDES})
  # the core checks the game folder has the very programs it was translated from
  string(REPLACE "." ";" core_version "${GAME_CORE_VERSION}")
  list(GET core_version 0 core_major)
  list(GET core_version 1 core_minor)
  list(GET core_version 2 core_patch)
  target_compile_definitions(${core} PRIVATE AFCORE_BUILD CORE_ROMSET=${CORE_ROMSET} "CORE_TITLE=\"${CORE_TITLE}\""
    CORE_VERSION_MAJOR=${core_major} CORE_VERSION_MINOR=${core_minor} CORE_VERSION_PATCH=${core_patch}
    CORE_WIDTH=${CORE_WIDTH} CORE_HEIGHT=${CORE_HEIGHT} ${core_programs} ${core_options})
  if(GAME_CONTROLS STREQUAL "driving")
    target_compile_definitions(${core} PRIVATE CORE_DRIVING)
  endif()
  # filelists/<set>.h (tools/filelist/make_filelist.py in common/): check_roms
  set(filelist ${CMAKE_SOURCE_DIR}/filelists/${CORE_ROMSET}.h)
  if(EXISTS ${filelist})
    configure_file(${filelist} ${CMAKE_CURRENT_BINARY_DIR}/filelist_${CORE_ROMSET}/afc_filelist.h COPYONLY)
    target_include_directories(${core} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/filelist_${CORE_ROMSET})
    target_compile_definitions(${core} PRIVATE CORE_FILELIST)
  endif()
  target_compile_options(${core} PRIVATE ${WARN})
  if(LB_MTLGLES)
    set_target_properties(${core} PROPERTIES FRAMEWORK TRUE
      MACOSX_FRAMEWORK_IDENTIFIER com.thebrinkoftomorrow.arcadefoundry.insertcoin.${core})
    target_link_libraries(${core} PRIVATE GL afc::mtlgles "-framework Foundation" "-framework CoreVideo" m pthread)
  elseif(LB_IOS)
    set_target_properties(${core} PROPERTIES FRAMEWORK TRUE
      MACOSX_FRAMEWORK_IDENTIFIER com.thebrinkoftomorrow.arcadefoundry.insertcoin.${core})
    target_link_libraries(${core} PRIVATE GL "-framework OpenGLES" "-framework Foundation" "-framework CoreVideo" m pthread)
  elseif(APPLE)
    set_target_properties(${core} PROPERTIES FRAMEWORK TRUE
      MACOSX_FRAMEWORK_IDENTIFIER com.thebrinkoftomorrow.arcadefoundry.insertcoin.${core})
    target_link_libraries(${core} PRIVATE OpenGL::GL m pthread)
  else()
    find_library(EGL_LIBRARY EGL REQUIRED)
    target_link_libraries(${core} PRIVATE ${EGL_LIBRARY} OpenGL::GL m pthread)
  endif()
  # drives the core as the front end does, without a window
  add_executable(core_test tools/core_test.c)
  target_include_directories(core_test PRIVATE ${AFCORE_INCLUDE})
  target_link_libraries(core_test PRIVATE ${CMAKE_DL_LIBS})
endif()
