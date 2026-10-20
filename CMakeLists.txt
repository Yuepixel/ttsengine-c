# =============================================================================
#  TtsEngine-C
#  A C++17 re-implementation of the local Qwen3-TTS server.
#
#  Layout
#    third_party/qwentts.cpp   inference engine (MIT, upstream project)
#    src/                      our service layer: HTTP, voices, WSOLA, perf
#    tests/                    unit tests (no model required)
#
#  The engine is deliberately *not* re-implemented here: the upstream project
#  already ships a tuned GGML/CUDA implementation of the full Qwen3-TTS
#  pipeline (talker, code predictor, 12 Hz codec) behind a stable C ABI.
#  Everything in src/ is the layer that upstream does not provide:
#  a drop-in replacement for the legacy Python server's HTTP contract.
# =============================================================================
cmake_minimum_required(VERSION 3.20)

project(TtsEngineC VERSION 0.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Single output directory: the server, the CLI tools and the backend DLLs must
# live together or ggml_backend_load_all() cannot find its modules.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(TTSC_BUILD_TESTS "Build unit tests" ON)

# -----------------------------------------------------------------------------
#  ggml source tree
# -----------------------------------------------------------------------------
# Upstream's CMakeLists derives this from ${CMAKE_SOURCE_DIR}, which is *this*
# project when the vendored tree is added as a subdirectory. Pin it here so the
# nested configure resolves ggml correctly; the nested set(... CACHE ...) is a
# no-op once the entry exists.
if(NOT DEFINED GGML_SOURCE_DIR)
    set(GGML_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qwentts.cpp/ggml"
        CACHE PATH "ggml source tree" FORCE)
endif()

# -----------------------------------------------------------------------------
#  Engine (vendored)
# -----------------------------------------------------------------------------
# Upstream defines the targets qwen-core, httplib, yyjson and its own CLI
# tools. Building it as a subdirectory keeps one source of truth for the
# inference code; the tools are a useful fallback for A/B measurements.
# EXCLUDE_FROM_ALL keeps them out of the default build; benches target them
# explicitly (build.ps1 does the same for qwen-tts.exe).
add_subdirectory(third_party/qwentts.cpp EXCLUDE_FROM_ALL)

# The vendored tree generates version.h into its own binary directory but adds
# ${CMAKE_BINARY_DIR} to the include path, which is *this* project when nested.
# Point the upstream targets at the real location so qwen.cpp and the CLI tools
# can find the generated header.
set(TTSC_QWEN_VERSION_DIR "${CMAKE_BINARY_DIR}/third_party/qwentts.cpp")
foreach(_tgt qwen-core quantize qwen-codec qwen-tts tts-server test-abi-c)
    if(TARGET ${_tgt})
        target_include_directories(${_tgt} PRIVATE "${TTSC_QWEN_VERSION_DIR}")
    endif()
endforeach()

# -----------------------------------------------------------------------------
#  Source lists
# -----------------------------------------------------------------------------
set(TTSC_SOURCES
    src/main.cpp
    src/config.cpp
    src/wav.cpp
    src/wsola.cpp
    src/perf.cpp
    src/voice_store.cpp
    src/engine.cpp
    src/http_api.cpp
)
set(TTSC_TEST_SOURCES
    tests/test_main.cpp
    tests/test_config.cpp
    tests/test_wav.cpp
    tests/test_wsola.cpp
    tests/test_voice_store.cpp
    src/config.cpp
    src/wav.cpp
    src/wsola.cpp
    src/perf.cpp
    src/voice_store.cpp
)

# -----------------------------------------------------------------------------
#  Header dependency tracking
# -----------------------------------------------------------------------------
# MSVC prints its /showIncludes lines in the installed UI language here, and
# Ninja only recognises the English prefix, so CMake's detection fails silently:
# editing a header rebuilds nothing (we lost real time to this) and the include
# list floods the log. Until the toolchain is English or CMake learns the
# localized prefix, declare the headers as explicit object dependencies of our
# own sources. Coarse, but correct: our tree is small.
file(GLOB_RECURSE TTSC_HEADERS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qwentts.cpp/src/*.h")
foreach(_src ${TTSC_SOURCES} ${TTSC_TEST_SOURCES})
    set_source_files_properties(${_src} PROPERTIES OBJECT_DEPENDS "${TTSC_HEADERS}")
endforeach()

# -----------------------------------------------------------------------------
#  Service layer
# -----------------------------------------------------------------------------
add_executable(ttsc-server ${TTSC_SOURCES})

target_include_directories(ttsc-server PRIVATE src)
target_link_libraries(ttsc-server PRIVATE qwen-core httplib yyjson)

if(MSVC)
    # /utf-8: sources contain Chinese comments and the docs strings are UTF-8.
    target_compile_options(ttsc-server PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(ttsc-server PRIVATE -Wall -Wextra -Wpedantic)
endif()

# -----------------------------------------------------------------------------
#  Desktop GUI (ttsc-gui.exe)
# -----------------------------------------------------------------------------
# The customer-facing build: same service layer, Win32 window, no console.
# WIN32 subsystem means no console window; the engine's stdout is redirected
# into a pipe by gui_main.cpp and shown in the log pane instead.
set(TTSC_GUI_SOURCES ${TTSC_SOURCES})
list(REMOVE_ITEM TTSC_GUI_SOURCES src/main.cpp)
list(APPEND TTSC_GUI_SOURCES src/gui_main.cpp)

add_executable(ttsc-gui WIN32 ${TTSC_GUI_SOURCES})
target_include_directories(ttsc-gui PRIVATE src)
target_link_libraries(ttsc-gui PRIVATE qwen-core httplib yyjson
                      comctl32 winmm shell32 ole32)
if(MSVC)
    target_compile_options(ttsc-gui PRIVATE /W4 /utf-8 /permissive-)
endif()

# Windowless stub that puts the CUDA runtime on PATH and starts the GUI. Needed
# because ggml.dll -> ggml-cuda.dll -> cublas64_13.dll is a load-time import
# chain that Windows resolves before the GUI's own code can run.
add_executable(ttsc-launch WIN32 src/launch_main.cpp)
if(MSVC)
    target_compile_options(ttsc-launch PRIVATE /W4 /utf-8 /permissive-)
endif()

# -----------------------------------------------------------------------------
#  Tests (no GPU, no model files required)
# -----------------------------------------------------------------------------
if(TTSC_BUILD_TESTS)
    enable_testing()
    add_executable(ttsc-tests ${TTSC_TEST_SOURCES})
    target_include_directories(ttsc-tests PRIVATE src)
    # voice_store.cpp parses voices.json with yyjson; the target needs both the
    # header path and the archive that the vendored indexer derives.
    target_link_libraries(ttsc-tests PRIVATE yyjson)
    if(MSVC)
        target_compile_options(ttsc-tests PRIVATE /utf-8)
    endif()
    add_test(NAME ttsc-tests COMMAND ttsc-tests)
endif()

# -----------------------------------------------------------------------------
#  Win32 resources (icon + version info) for every executable
# -----------------------------------------------------------------------------
# A single resource script (src/resources/app.rc) embeds the multi-size icon and
# the version block; per-target preprocessor definitions give each exe its own
# FileDescription/OriginalFilename. Driven from here rather than from each
# add_executable so the set of binaries stays in one place.
set(TTSC_RC "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/app.rc")
set(TTSC_ICON "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/app.ico")

function(ttsc_add_win_resources _target _desc _origname)
    if(NOT WIN32 OR NOT MSVC)
        return()
    endif()
    if(NOT TARGET ${_target})
        return()
    endif()
    target_sources(${_target} PRIVATE "${TTSC_RC}")
    # Forward slashes: rc.exe accepts them and they need no escaping in a -D.
    target_compile_definitions(${_target} PRIVATE
        "TTSC_ICON_PATH=\"${TTSC_ICON}\""
        "TTSC_FILE_DESC=\"${_desc}\""
        "TTSC_ORIG_NAME=\"${_origname}\"")
    set_source_files_properties("${TTSC_RC}" PROPERTIES OBJECT_DEPENDS "${TTSC_ICON}")
endfunction()

ttsc_add_win_resources(ttsc-server "TtsEngine-C service (HTTP TTS)"      "ttsc-server.exe")
ttsc_add_win_resources(ttsc-gui    "TtsEngine-C desktop panel"           "ttsc-gui.exe")
ttsc_add_win_resources(ttsc-launch "TtsEngine-C launcher (CUDA PATH)"    "ttsc-launch.exe")
ttsc_add_win_resources(ttsc-tests  "TtsEngine-C unit tests"              "ttsc-tests.exe")
ttsc_add_win_resources(qwen-tts    "Qwen3-TTS CLI (qwentts.cpp)"         "qwen-tts.exe")
