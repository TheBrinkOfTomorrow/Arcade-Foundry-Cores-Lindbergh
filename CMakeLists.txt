cmake_minimum_required(VERSION 3.16)
project(arcade_foundry_cores_lindbergh C)

# The game's ELF is translated at build time; nothing derived from it is committed.
set(GAME "hotd4a" CACHE STRING "game directory under games/")
# per-game defaults (games/<game>/game.cmake): HotD4's when a game has none
set(GAME_GEN_COUNT 54)
set(GAME_TITLE "The House of the Dead 4")
set(GAME_WIDTH 1280)
set(GAME_HEIGHT 768)
set(GAME_CONTROLS gun)            # gun or driving: what the core maps the front end's controls to
set(GAME_DRIVE_BOARD position)    # the steering drive board on /dev/tts/0: position (Race TV) or status (OutRun 2 SP)
set(GAME_CORE_VERSION 1.0.0)      # the game's core version (afcore_api.core_version; HotD4's here, the others' in their game.cmake): bump it with a change to how it plays or saves
if(EXISTS ${CMAKE_SOURCE_DIR}/games/${GAME}/game.cmake)
  include(${CMAKE_SOURCE_DIR}/games/${GAME}/game.cmake)
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

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# iOS (iPadOS): the core only, on OpenGL ES through gl4es (desktop GL 2.x, fixed
# function and ARB programs translated to GLES); see runtime/host_core_ios.m
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(LB_IOS ON)
  set(LB_RUN OFF)
  set(LB_CORE ON)
  enable_language(OBJC)
  add_compile_definitions(LB_IOS GLES_SILENCE_DEPRECATION GL_SILENCE_DEPRECATION)
  include(FetchContent)
  # gl4es (MIT, ptitSeb/gl4es), fetched at a fixed commit at build time
  set(STATICLIB ON CACHE BOOL "" FORCE)
  set(NOEGL ON CACHE BOOL "" FORCE)
  set(NOX11 ON CACHE BOOL "" FORCE)
  set(NO_LOADER ON CACHE BOOL "" FORCE)
  set(NO_INIT_CONSTRUCTOR ON CACHE BOOL "" FORCE)
  FetchContent_Declare(gl4es
    URL https://codeload.github.com/ptitSeb/gl4es/tar.gz/2037865590d410bcd7b8d1f22b841472ed6d39ac
    URL_HASH SHA256=eaee3043a95eab40570539592f3f85f23d855286418dfacd961fa91d5211560d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
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

file(GLOB RECOMP_PY ${CMAKE_SOURCE_DIR}/tools/x86recomp/*.py)
set(WARN -O2 -g -Wall -Wno-unused-function -fno-strict-aliasing -fwrapv)

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
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/x86recomp/recomp.py ${elf} ${gen} --name ${name}
            --files ${count} --overrides ${game_dir}/overrides.txt --patches ${game_dir}/patches.txt ${libs_args}
    DEPENDS ${RECOMP_PY} ${elf} ${game_dir}/overrides.txt ${game_dir}/patches.txt ${libs_deps}
    COMMENT "Translating ${elf}")
  add_library(gen_${name} OBJECT ${srcs})
  target_include_directories(gen_${name} PRIVATE runtime ${gen})
  # translated code: modest optimisation, exact IEEE behaviour, no aliasing assumptions
  target_compile_options(gen_${name} PRIVATE -O1 -w -fno-strict-aliasing -fwrapv -ffp-contract=off)
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
target_include_directories(lb_runtime PRIVATE runtime)
target_compile_options(lb_runtime PRIVATE ${WARN})
target_include_directories(lb_runtime PRIVATE ${GL_INCLUDES})

if(LB_RUN)
  function(lindbergh_run target name)
    add_executable(${target} runtime/main_linux.c runtime/host_sdl.c
                   $<TARGET_OBJECTS:lb_runtime> $<TARGET_OBJECTS:gen_${name}>)
    target_include_directories(${target} PRIVATE runtime)
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
  target_include_directories(${core} PRIVATE runtime include ${GL_INCLUDES})
  # the core checks the game folder has the very programs it was translated from
  string(REPLACE "." ";" core_version "${GAME_CORE_VERSION}")
  list(GET core_version 0 core_major)
  list(GET core_version 1 core_minor)
  list(GET core_version 2 core_patch)
  target_compile_definitions(${core} PRIVATE AFCORE_BUILD CORE_ROMSET=${CORE_ROMSET} "CORE_TITLE=\"${CORE_TITLE}\""
    CORE_VERSION_MAJOR=${core_major} CORE_VERSION_MINOR=${core_minor} CORE_VERSION_PATCH=${core_patch}
    CORE_WIDTH=${CORE_WIDTH} CORE_HEIGHT=${CORE_HEIGHT} ${core_programs})
  if(GAME_CONTROLS STREQUAL "driving")
    target_compile_definitions(${core} PRIVATE CORE_DRIVING)
  endif()
  target_compile_options(${core} PRIVATE ${WARN})
  if(LB_IOS)
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
  target_include_directories(core_test PRIVATE include)
  target_link_libraries(core_test PRIVATE ${CMAKE_DL_LIBS})
endif()
