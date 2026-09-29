# CMakeLists.txt — the whole build is one command: cmake --build build
#
# Two targets on purpose:
#   gbcore — the emulator core. It never links SDL, so it builds and runs on a
#            machine with no display, which is what makes the CI harness possible.
#   gbemu  — the SDL host. It links gbcore and SDL3 (or nothing at all when
#            SDL3 is missing, in which case only the headless host is built).
cmake_minimum_required(VERSION 3.24)
project(gbemu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(GBEMU_ENABLE_SDL "Build the SDL3 host" ON)
option(GBEMU_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

set(GBEMU_WARNINGS -Wall -Wextra -Wshadow -Wstrict-prototypes -Wpointer-arith)
if(GBEMU_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# --- Core: no SDL, no windowing, no host code ---------------------------
add_library(gbcore STATIC
    src/gb.c
    src/cpu.c
    src/mmu.c
    src/timer.c
    src/joypad.c
    src/ppu.c
    src/apu.c
    src/cart.c
    src/debug.c
)
target_include_directories(gbcore PUBLIC include)
target_compile_options(gbcore PRIVATE ${GBEMU_WARNINGS})

# --- Core unit tests: registers, flags, bus, timer, joypad ----------------
add_executable(gb_tests tests/test_flags.c)
target_link_libraries(gb_tests PRIVATE gbcore)
target_compile_options(gb_tests PRIVATE ${GBEMU_WARNINGS})

# --- Host ---------------------------------------------------------------
add_executable(gbemu src/main.c)
target_link_libraries(gbemu PRIVATE gbcore)
target_compile_options(gbemu PRIVATE ${GBEMU_WARNINGS})

if(GBEMU_ENABLE_SDL)
    find_package(SDL3 QUIET CONFIG)
    if(SDL3_FOUND)
        message(STATUS "host: SDL3 ${SDL3_VERSION} (CMake package)")
        target_link_libraries(gbemu PRIVATE SDL3::SDL3)
        target_compile_definitions(gbemu PRIVATE GBEMU_HAVE_SDL=1)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
        endif()
        if(SDL3_FOUND)
            message(STATUS "host: SDL3 ${SDL3_VERSION} (pkg-config)")
            target_link_libraries(gbemu PRIVATE PkgConfig::SDL3)
            target_compile_definitions(gbemu PRIVATE GBEMU_HAVE_SDL=1)
        else()
            message(WARNING
                "SDL3 not found: building the headless host only. "
                "Install it with 'brew install sdl3' or '-DGBEMU_ENABLE_SDL=OFF' "
                "to silence this warning.")
        endif()
    endif()
endif()

# --- Tests ---------------------------------------------------------------
enable_testing()
add_test(NAME core COMMAND gb_tests)
# The ROM harness generates its own test ROMs, so `ctest` needs no arguments
# beyond the emulator binary it is grading.
add_test(NAME emulator COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh $<TARGET_FILE:gbemu>)

install(TARGETS gbemu RUNTIME DESTINATION bin)
