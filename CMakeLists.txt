cmake_minimum_required(VERSION 3.21)
project(butterscotch C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw (or another supported platform).")
endif()

# To avoid unused code in production builds that are only used for a single game (example: PlayStation 2 target), we can enable/disable specific bytecode versions
#
# If only a single bytecode version is enabled, the checks will be collapsed and removed by the compiler during build time, reducing code size and improving
# performance on low end hardware!
option(ENABLE_BC16 "Enable support for Bytecode Version 16" ON)
option(ENABLE_BC17 "Enable support for Bytecode Version 17" ON)
if(NOT ENABLE_BC16 AND NOT ENABLE_BC17)
    message(FATAL_ERROR "You need to build Butterscotch with at least one bytecode version enabled!")
endif()
if(ENABLE_BC16)
    add_compile_definitions(ENABLE_BC16)
endif()
if(ENABLE_BC17)
    add_compile_definitions(ENABLE_BC17)
endif()

file(GLOB SOURCES src/*.c)
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES})
target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

# stb_ds
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform/${PLATFORM}.cmake")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform/${PLATFORM}.cmake)
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
