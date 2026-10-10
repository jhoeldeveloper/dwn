cmake_minimum_required(VERSION 3.20)
project(dwn_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# TinyProcessLibrary v2.0.4 calls cmake_minimum_required(VERSION 2.8), which
# CMake 4.x refuses outright. Tell CMake to accept the old-style minimum for
# fetched dependencies; CMake < 4 simply ignores this.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING
    "Minimum policy version accepted from fetched dependencies")

# Arch ships `make` but not the `gmake` symlink that CMake probes for first,
# which produces a confusing "no such file or directory" compiler test failure.
if(NOT CMAKE_MAKE_PROGRAM)
    find_program(MAKE_EXECUTABLE NAMES gmake make)
    if(MAKE_EXECUTABLE)
        set(CMAKE_MAKE_PROGRAM "${MAKE_EXECUTABLE}" CACHE FILEPATH "" FORCE)
    endif()
endif()

include(CTest)

# Must be declared BEFORE FetchContent_MakeAvailable. TinyProcessLibrary calls
# option(BUILD_TESTING "" OFF) when built as a subproject, and option() is a
# no-op once the cache entry exists. Declaring it here first therefore pins it
# to ON, which is what we want — without this our tests are silently skipped.
option(BUILD_TESTING "Build the unit tests" ON)

include(FetchContent)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
)
FetchContent_Declare(tiny_process_library
  GIT_REPOSITORY https://gitlab.com/eidheim/tiny-process-library.git
  GIT_TAG        v2.0.4
)
FetchContent_MakeAvailable(nlohmann_json tiny_process_library)

# ── Core library ─────────────────────────────────────────────────────────
# Split out so the tests can link it without pulling in main().
add_library(dwncore STATIC
  src/protocol.cpp
  src/process.cpp
  src/resolve.cpp
  src/fetcher.cpp
  src/downloader.cpp
  src/host.cpp
)

target_include_directories(dwncore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dwncore PUBLIC
  nlohmann_json::nlohmann_json
  tiny-process-library
)

find_package(Threads REQUIRED)
target_link_libraries(dwncore PUBLIC Threads::Threads)

# ── Host executable ──────────────────────────────────────────────────────
add_executable(dwn-host src/main.cpp)
target_link_libraries(dwn-host PRIVATE dwncore)

# ── Tests ────────────────────────────────────────────────────────────────
if(BUILD_TESTING)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../tests ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()

# ── Install ──────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS dwn-host RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# dwn.host.json.example is deliberately NOT installed: it is a reference for
# humans, with placeholder paths. The manifest a browser actually reads is
# generated per machine by scripts/register-host.sh, into the browser's own
# NativeMessagingHosts directory. Installing a template there would create a
# file that looks authoritative and cannot work.