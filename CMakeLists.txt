cmake_minimum_required(VERSION 3.21)

if(PLATFORM STREQUAL "ps3")
set(CMAKE_TOOLCHAIN_FILE ${CMAKE_SOURCE_DIR}/cmake/ppu.cmake)
endif()

if(PLATFORM STREQUAL "vita")

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

set(BACKEND "sdl2")

elseif(PLATFORM STREQUAL "switch")
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{DEVKITPRO})
        message(FATAL_ERROR "Please define DEVKITPRO to point to your SDK path!")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/cmake/Switch.cmake" CACHE PATH "toolchain file")
endif()

set(BACKEND "sdl2")

endif()

project(butterscotch C)

if (PLATFORM STREQUAL "vita")
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Butterscotch")
set(VITA_TITLEID  "BUTTRSTCH")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(MSVC)
    if(WERROR)
        add_compile_options(/WX)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wvla)
    if(WERROR)
        add_compile_options(-Werror)
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(MSVC)
        # equivalent to -fomit-frame-pointer
        add_compile_options(/Oy)
    else()
        add_compile_options(-ffast-math -fno-finite-math-only)

        # some targets don't support -fomit-frame-pointer IIRC
        include(CheckCCompilerFlag)
        check_c_compiler_flag("-fomit-frame-pointer" HAS_FOMIT_FRAME_POINTER)
        if(HAS_FOMIT_FRAME_POINTER)
            add_compile_options(-fomit-frame-pointer)
        endif()
    endif()
endif()

set(PLATFORM "cli" CACHE STRING "Platform backend")
set(BACKEND "" CACHE STRING "Desktop platform backend")
set(AUDIO_BACKEND "" CACHE STRING "Audio backend")

if(PLATFORM STREQUAL "cli" AND BACKEND STREQUAL "")
    message(FATAL_ERROR "You need to set a BACKEND!")
endif()

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

# Audio backend defaults
if(AUDIO_BACKEND STREQUAL "")
    if(PLATFORM STREQUAL "web")
        set(AUDIO_BACKEND "web")
    elseif(PLATFORM STREQUAL "web-meta")
        set(AUDIO_BACKEND "none")
    elseif(PLATFORM STREQUAL "ps3" OR PLATFORM STREQUAL "vita" OR PLATFORM STREQUAL "switch")
        set(AUDIO_BACKEND "openal")
    elseif(PLATFORM STREQUAL "ps2")
        set(AUDIO_BACKEND "ps2")
    else()
        set(AUDIO_BACKEND "miniaudio")
    endif()
endif()

if(AUDIO_BACKEND STREQUAL "openal")
    file(GLOB AUDIO_SOURCES src/audio/openal/*.c)
elseif(AUDIO_BACKEND STREQUAL "miniaudio")
    file(GLOB AUDIO_SOURCES src/audio/miniaudio/*.c)
    if(PLATFORM STREQUAL "vita")
        add_compile_definitions(MA_NO_RUNTIME_LINKING)
    endif()
elseif(AUDIO_BACKEND STREQUAL "web")
    file(GLOB AUDIO_SOURCES src/audio/web/*.c src/audio/miniaudio/*.c)
elseif(AUDIO_BACKEND STREQUAL "ps2")
    file(GLOB AUDIO_SOURCES src/audio/ps2/*.c)
elseif(NOT AUDIO_BACKEND STREQUAL "none")
    message(FATAL_ERROR "Invalid AUDIO_BACKEND!")
endif()

# To avoid unused code in production builds that are only used for a single game (example: PlayStation 2 target), we can enable/disable specific features
#
# If only a single WAD version is enabled, the checks will be collapsed and removed by the compiler during build time, reducing code size and improving
# performance on low end hardware!
option(ENABLE_WAD14 "Enable support for WAD Version 14 and below" ON)
option(ENABLE_WAD16 "Enable support for WAD Version 15/16" ON)
option(ENABLE_WAD17 "Enable support for WAD Version 17" ON)
option(ENABLE_LEGACY_GL "Enable the legacy OpenGL renderer" ON)
if(PLATFORM STREQUAL "switch")
    set(ENABLE_LEGACY_GL OFF CACHE BOOL "Enable the legacy OpenGL renderer" FORCE)
endif()
option(ENABLE_MODERN_GL "Enable the modern OpenGL renderer" ON)
option(ENABLE_SW_RENDERER "Enable the software renderer" ON)
option(ENABLE_NOOP_RENDERER "Enable the no-op renderer (headless stub, no drawing)" OFF)
# Noop renderer is exclusive to the noop desktop backend (no window, no GL).
# Auto-enable for BACKEND=noop and reject otherwise.
if(BACKEND STREQUAL "noop")
    set(ENABLE_NOOP_RENDERER ON CACHE BOOL "Enable the no-op renderer (headless stub, no drawing)" FORCE)
    set(ENABLE_LEGACY_GL OFF CACHE BOOL "Enable the legacy OpenGL renderer" FORCE)
    set(ENABLE_MODERN_GL OFF CACHE BOOL "Enable the modern OpenGL renderer" FORCE)
endif()
if(ENABLE_NOOP_RENDERER AND NOT BACKEND STREQUAL "noop")
    message(FATAL_ERROR "ENABLE_NOOP_RENDERER is exclusive to BACKEND=noop")
endif()
if(NOT ENABLE_WAD14 AND NOT ENABLE_WAD16 AND NOT ENABLE_WAD17)
    message(FATAL_ERROR "You need to build Butterscotch with at least one WAD version enabled!")
endif()
if(ENABLE_WAD14)
    add_compile_definitions(ENABLE_WAD14)
endif()
if(ENABLE_WAD16)
    add_compile_definitions(ENABLE_WAD16)
endif()
if(ENABLE_WAD17)
    add_compile_definitions(ENABLE_WAD17)
endif()
if(ENABLE_LEGACY_GL)
    add_compile_definitions(ENABLE_LEGACY_GL)
endif()
if(ENABLE_MODERN_GL)
    add_compile_definitions(ENABLE_MODERN_GL)
endif()
if(ENABLE_SW_RENDERER)
    add_compile_definitions(ENABLE_SW_RENDERER)
endif()
if(ENABLE_NOOP_RENDERER)
    add_compile_definitions(ENABLE_NOOP_RENDERER)
endif()

if(PLATFORM STREQUAL "web-meta")
    # Metadata-only build: just the data.win parser and its direct deps. No VM, no runner, no I/O subsystems.
    set(SOURCES
        src/data_win.c
        src/binary_reader.c
		src/log.c
		src/stb_ds.c
    )
else()
    file(GLOB SOURCES src/*.c)
    # These platforms haven't been converted to the refactored system yet.
    # This list should NEVER grow, all new platforms should use the new system.
    if(PLATFORM STREQUAL "ps2" OR PLATFORM STREQUAL "ps3" OR PLATFORM STREQUAL "web" OR PLATFORM STREQUAL "android")
        list(REMOVE_ITEM SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/loop.c")
    endif()
endif()
# Platform specific files builds
file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)

if(PLATFORM STREQUAL "android")
    add_library(butterscotch SHARED ${SOURCES} ${PLATFORM_SOURCES} ${AUDIO_SOURCES})
else()
    add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES} ${AUDIO_SOURCES})
endif()

target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(butterscotch PRIVATE src/${PLATFORM})

if(AUDIO_BACKEND STREQUAL "openal")
    target_include_directories(butterscotch PUBLIC src/audio/openal)
    add_compile_definitions(USE_OPENAL)
    if(APPLE)
        set(AUDIO_LIBRARIES "-framework OpenAL")
    elseif(PLATFORM STREQUAL "vita" OR PLATFORM STREQUAL "switch")
        set(AUDIO_LIBRARIES openal pthread)
    elseif(NOT PLATFORM STREQUAL "ps3")
        find_package(OpenAL REQUIRED)
        target_include_directories(butterscotch PUBLIC "${OPENAL_INCLUDE_DIR}")
        set(AUDIO_LIBRARIES "${OPENAL_LIBRARY}")
    endif()
elseif(AUDIO_BACKEND STREQUAL "miniaudio")
    target_include_directories(butterscotch SYSTEM PUBLIC vendor/miniaudio)
    target_include_directories(butterscotch PUBLIC src/audio/miniaudio)
    add_compile_definitions(USE_MINIAUDIO)
    if (NOT WIN32)
        set(AUDIO_LIBRARIES pthread)
    endif()
elseif(AUDIO_BACKEND STREQUAL "web")
    target_include_directories(butterscotch SYSTEM PUBLIC vendor/miniaudio)
    target_include_directories(butterscotch PUBLIC src/audio/web)
    target_include_directories(butterscotch PUBLIC src/audio/miniaudio)
    add_compile_definitions(USE_WEB_AUDIO)
elseif(AUDIO_BACKEND STREQUAL "ps2")
    target_include_directories(butterscotch PUBLIC src/audio/ps2)
    add_compile_definitions(USE_PS2_AUDIO)
endif()

# stb_ds
add_library(stb_ds INTERFACE)
target_include_directories(stb_ds SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/vendor/stb/ds)

# stb_vorbis
add_library(stb_vorbis INTERFACE)
target_include_directories(stb_vorbis SYSTEM INTERFACE ${CMAKE_SOURCE_DIR}/vendor/stb/vorbis)

# RFC 1321 reference MD5 implementation
target_sources(butterscotch PRIVATE vendor/md5/md5.c)
target_include_directories(butterscotch PUBLIC vendor/md5)

# Steve Reid's SHA1 implementation
add_library(sha1 STATIC vendor/sha1/sha1.c)
target_include_directories(sha1 SYSTEM PUBLIC vendor/sha1)
target_compile_options(sha1 PRIVATE -w)

# Base64 implementation
target_sources(butterscotch PRIVATE vendor/base64/base64.c)
target_include_directories(butterscotch PUBLIC vendor/base64)

# bzip2
add_library(bzip2 STATIC
    vendor/bzip2/bz_internal_error.c
    vendor/bzip2/bzlib.c
    vendor/bzip2/crctable.c
    vendor/bzip2/decompress.c
    vendor/bzip2/huffman.c
    vendor/bzip2/randtable.c
)
target_include_directories(bzip2 PUBLIC vendor/bzip2)
target_compile_options(bzip2 PRIVATE -w)

# Spatial grid logs
option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
if(ENABLE_SPATIAL_GRID_LOGS)
    add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
endif()

# VM Exceptions logs
option(ENABLE_VM_EXCEPTIONS_LOGS "Enable VM Exceptions logs" OFF)
if(ENABLE_VM_EXCEPTIONS_LOGS)
    add_compile_definitions(ENABLE_VM_EXCEPTIONS_LOGS)
endif()

if(PLATFORM STREQUAL "cli" OR PLATFORM STREQUAL "vita" OR PLATFORM STREQUAL "switch")
    if(PLATFORM STREQUAL "vita")
        # Normally, __VITA__ should already be defined, but for some reason,
        # in my case, this didn't really work... so just in case, when we're
        # building for the Vita, just force-feed these...
        add_compile_definitions(__VITA__)
        add_compile_definitions(PLATFORM_VITA)

        set(VM_GML_PROFILER_DEFAULT OFF)
        set(VM_TRACING_DEFAULT OFF)
        set(VM_OPCODE_PROFILER_DEFAULT OFF)
        set(VM_STUB_LOGS_DEFAULT OFF)
    elseif(PLATFORM STREQUAL "switch")
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_options(-ffast-math)
        endif()

        add_compile_definitions(PLATFORM_SWITCH)

        set(VM_GML_PROFILER_DEFAULT OFF)
        set(VM_TRACING_DEFAULT OFF)
        set(VM_OPCODE_PROFILER_DEFAULT OFF)
        set(VM_STUB_LOGS_DEFAULT OFF)
    else()
        set(VM_GML_PROFILER_DEFAULT ON)
        set(VM_TRACING_DEFAULT ON)
        set(VM_OPCODE_PROFILER_DEFAULT ON)
        set(VM_STUB_LOGS_DEFAULT ON)
    endif()
    # Matches the GitHub Actions container flags (FORTIFY requires building with optimizations enabled!)
    target_compile_options(butterscotch PRIVATE "$<$<CONFIG:Release,RelWithDebInfo>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")

    if(NOT ENABLE_LEGACY_GL AND NOT ENABLE_MODERN_GL AND NOT ENABLE_NOOP_RENDERER AND NOT ENABLE_SW_RENDERER)
        message(FATAL_ERROR "You must enable at least one renderer!")
    endif()
    file(GLOB GL_SOURCES src/image/*.c)
    if(ENABLE_LEGACY_GL OR ENABLE_MODERN_GL)
        file(GLOB GL_SOURCES ${GL_SOURCES} src/gl_common/*.c)
        target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl)
        target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_common)
    endif()
    if(ENABLE_LEGACY_GL)
        file(GLOB GL_SOURCES ${GL_SOURCES} src/gl_legacy/*.c)
        target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_legacy)
    endif()
    if(ENABLE_MODERN_GL)
        file(GLOB GL_SOURCES ${GL_SOURCES} src/gl/*.c)
    endif()
    if(ENABLE_SW_RENDERER)
        file(GLOB GL_SOURCES ${GL_SOURCES} src/sw/*.c)
        target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/sw)
    endif()
    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/image)
    if (MSVC)
        target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/compat/getopt)
        add_compile_definitions(NO_STRTOK_R)
        add_compile_definitions(NO_STRCASECMP)
    endif()

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" ${VM_GML_PROFILER_DEFAULT})
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" ${VM_TRACING_DEFAULT})
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" ${VM_OPCODE_PROFILER_DEFAULT})
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ${VM_STUB_LOGS_DEFAULT})
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    set(PLATFORM_LIBRARIES bzip2 stb_ds sha1 stb_vorbis)
    if(NOT PLATFORM STREQUAL "vita" AND NOT PLATFORM STREQUAL "switch")
        if(ENABLE_LEGACY_GL OR ENABLE_MODERN_GL OR BACKEND STREQUAL "glfw3" OR BACKEND STREQUAL "glfw2")
            # GLAD
            add_library(glad STATIC vendor/glad/src/glad.c)
            target_include_directories(glad PUBLIC vendor/glad/include)
            target_compile_options(glad PRIVATE -Wno-unused-but-set-variable)
            set(PLATFORM_LIBRARIES ${PLATFORM_LIBRARIES} glad)
        endif()
    endif()

    if(NOT MSVC)
        set(PLATFORM_LIBRARIES ${PLATFORM_LIBRARIES} m)
    endif()
    if(WIN32)
        set(PLATFORM_LIBRARIES ${PLATFORM_LIBRARIES} opengl32 gdi32 winmm)
    elseif(NOT PLATFORM STREQUAL "vita" AND NOT PLATFORM STREQUAL "switch")
        set(PLATFORM_LIBRARIES ${PLATFORM_LIBRARIES} ${CMAKE_DL_LIBS})
    endif()

    if(BACKEND STREQUAL "glfw3")
        add_compile_definitions(USE_GLFW3)

        if(MINGW)
            find_package(glfw3 CONFIG REQUIRED)

            # Target Windows 7+ (avoids referencing newer APIs like MapViewOfFileNuma2 that MinGW import libs don't provide)
            # https://github.com/mirror/mingw-w64/blob/master/mingw-w64-headers/include/sdkddkver.h
            target_compile_definitions(butterscotch PRIVATE _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000 WIN32_LEAN_AND_MEAN)
            target_link_options(butterscotch PRIVATE -static)
            set(BACKEND_LIBRARIES glfw opengl32 gdi32 winmm)
        elseif(MSVC)
            target_compile_definitions(butterscotch PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
            find_package(glfw3 CONFIG REQUIRED)
            set(BACKEND_LIBRARIES glfw opengl32 gdi32 winmm)
        else()
            find_package(PkgConfig REQUIRED)
            pkg_check_modules(GLFW3 REQUIRED glfw3)
            target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
            target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
            set(BACKEND_LIBRARIES ${GLFW3_LIBRARIES})
        endif()
    elseif(BACKEND STREQUAL "glfw2")
        add_compile_definitions(USE_GLFW2)

        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW2 REQUIRED libglfw)
        target_include_directories(butterscotch PRIVATE ${GLFW2_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${GLFW2_LIBRARY_DIRS})
        set(BACKEND_LIBRARIES ${GLFW2_LIBRARIES})
    elseif(BACKEND STREQUAL "sdl1")
        add_compile_definitions(USE_SDL1)

        find_package(SDL REQUIRED)
        target_include_directories(butterscotch PUBLIC ${SDL_INCLUDE_DIR})
        set(BACKEND_LIBRARIES ${SDL_LIBRARY})
    elseif(BACKEND STREQUAL "sdl2")
        add_compile_definitions(USE_SDL2)

        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED sdl2)
        target_include_directories(butterscotch PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${SDL2_LIBRARY_DIRS})
        set(BACKEND_LIBRARIES ${SDL2_LIBRARIES})
    elseif(BACKEND STREQUAL "sdl3")
        add_compile_definitions(USE_SDL3)

        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED sdl3)
        target_include_directories(butterscotch PRIVATE ${SDL3_INCLUDE_DIRS})
        target_link_directories(butterscotch PRIVATE ${SDL3_LIBRARY_DIRS})
        set(BACKEND_LIBRARIES ${SDL3_LIBRARIES})
    elseif(BACKEND STREQUAL "appkit")
        add_compile_definitions(USE_APPKIT)
        target_compile_definitions(butterscotch PRIVATE GL_SILENCE_DEPRECATION)
        set(BACKEND_LIBRARIES "-framework Cocoa" "-framework GameController")
    elseif(BACKEND STREQUAL "noop")
        if(NOT ENABLE_NOOP_RENDERER)
            message(FATAL_ERROR "BACKEND=noop requires ENABLE_NOOP_RENDERER=ON")
        endif()
        add_compile_definitions(USE_NOOP)
        set(BACKEND_LIBRARIES "")
    endif()
    if (PLATFORM STREQUAL "vita")
        target_link_libraries(butterscotch PRIVATE
            ${BACKEND_LIBRARIES}
            ${AUDIO_LIBRARIES}
            ${PLATFORM_LIBRARIES}
            vitaGL
            stdc++
            SceShaccCgExt
            taihen_stub
            SceShaccCg_stub
        )

        vita_create_self(eboot.bin butterscotch UNSAFE)
        vita_create_vpk(Butterscotch.vpk ${VITA_TITLEID} eboot.bin
            VERSION ${VITA_VERSION}
            NAME ${VITA_APP_NAME}
        )
    else()
        target_link_libraries(butterscotch PRIVATE ${BACKEND_LIBRARIES} ${AUDIO_LIBRARIES} ${PLATFORM_LIBRARIES})

        if (PLATFORM STREQUAL "switch")
            target_link_libraries(butterscotch PRIVATE GLESv2 stdc++)
            nx_generate_nacp(OUTPUT Butterscotch.nacp NAME "Butterscotch" AUTHOR "Butterscotch" VERSION "1.0.0")
            nx_create_nro(butterscotch NACP Butterscotch.nacp)
        endif()
    endif()
    if(BACKEND STREQUAL "appkit")
        target_sources(butterscotch PRIVATE src/backends/${BACKEND}.m)
    else()
        target_sources(butterscotch PRIVATE src/backends/${BACKEND}.c)
    endif()

    # Enable AddressSanitizer by default in Debug builds
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(ENABLE_ASAN_DEFAULT ON)
    else()
        set(ENABLE_ASAN_DEFAULT OFF)
    endif()
    set(ENABLE_ASAN ${ENABLE_ASAN_DEFAULT} CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN AND NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Haiku")
        target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(butterscotch PRIVATE -fsanitize=address)
    endif()

    #gamecontrollerdb
    add_custom_command(TARGET butterscotch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/vendor/gamecontrollerdb.txt
            $<TARGET_FILE_DIR:butterscotch>/gamecontrollerdb.txt
    )
elseif(PLATFORM STREQUAL "web")
    if (NOT ENABLE_MODERN_GL)
        message(FATAL_ERROR "Web requires modern gl!")
    endif()
    file(GLOB GL_SOURCES src/gl/*.c src/gl_common/*.c src/image/*.c)
    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_common)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/image)

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # bzip2
    target_link_libraries(butterscotch PRIVATE bzip2 stb_ds sha1 stb_vorbis)

    set_target_properties(butterscotch PROPERTIES SUFFIX ".mjs")

    target_link_options(
            butterscotch
            PRIVATE
            "-sMODULARIZE=1"
            "-sEXPORT_ES6=1"
            "-sENVIRONMENT=worker"
            "-sWASMFS=1"
            "-sEXPORTED_RUNTIME_METHODS=['HEAP8', 'HEAPU8', 'HEAPF32', 'ccall', 'cwrap', 'specialHTMLTargets', 'UTF8ToString']"
            "-sEXPORTED_FUNCTIONS=['_main', '_mountOpfs', '_startRunner', '_stopRunner', '_malloc', '_free', '_getKeyDownPtr', '_getKeyUpPtr', '_getKeyCount', '_setAudioSampleRate', '_pullAudioFrames']"
            "-sOFFSCREENCANVAS_SUPPORT=1"
            "-sOFFSCREEN_FRAMEBUFFER=1"
            "-sMIN_WEBGL_VERSION=2"
            "-sMAX_WEBGL_VERSION=2"
            "-sFULL_ES3=1"
            "-sALLOW_MEMORY_GROWTH"
            "-sSHARED_MEMORY=1"
            "-sPTHREAD_POOL_SIZE=4"
            "-pthread"
    )

    target_compile_options(butterscotch PRIVATE "-pthread")
elseif(PLATFORM STREQUAL "web-meta")
    set_target_properties(butterscotch PROPERTIES
        SUFFIX ".mjs"
        OUTPUT_NAME "butterscotch-meta"
    )

    target_link_libraries(butterscotch PRIVATE stb_ds)

    target_link_options(
            butterscotch
            PRIVATE
            "-sMODULARIZE=1"
            "-sEXPORT_ES6=1"
            "-sENVIRONMENT=web"
            "-sEXPORTED_RUNTIME_METHODS=['HEAPU8', 'ccall', 'cwrap', 'UTF8ToString', 'FS']"
            "-sEXPORTED_FUNCTIONS=['_main', '_parseDataWin', '_freeDataWin', '_getGameName', '_getGameDisplayName', '_getMajorVersion', '_getMinorVersion', '_getRelease', '_getBuild', '_getDefaultWindowWidth', '_getDefaultWindowHeight', '_malloc', '_free']"
            "-sALLOW_MEMORY_GROWTH=1"
            "-sFORCE_FILESYSTEM=1"
    )
elseif(PLATFORM STREQUAL "android")
    if (NOT ENABLE_MODERN_GL)
        message(FATAL_ERROR "Android requires modern gl!")
    endif()
    file(GLOB GL_SOURCES src/gl/*.c src/gl_common/*.c src/image/*.c)
    target_sources(butterscotch PRIVATE ${GL_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_common)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/image)
    add_compile_definitions(ENABLE_GLES PLATFORM_ANDROID)

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" ON)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" ON)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # Spatial grid logs
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ON)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)
    # stb_vorbis (for miniaudio's OGG decoding)
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)
    # bzip2
    target_link_libraries(butterscotch PRIVATE bzip2)

    # Android NDK system libraries:
    # - log: __android_log_*
    # - GLESv3: OpenGL ES 3.0 functions
    # - OpenSLES: miniaudio's OpenSL ES backend (it dlopen's libaaudio.so at runtime when available, so no link-time aaudio dep)
    target_link_libraries(butterscotch PRIVATE log GLESv3 OpenSLES bzip2 stb_ds sha1 stb_vorbis)
elseif(PLATFORM STREQUAL "ps2")
    file(GLOB DEBUG_FONT_SOURCES src/debug_font/*.c)
    target_sources(butterscotch PRIVATE ${DEBUG_FONT_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/debug_font)

    # The PS2 EE has a single-precision FPU only, so double is software-emulated
    add_compile_definitions(USE_FLOAT_REALS)
    # By disabling Int64 support, we can fit the entire RValue struct in 8 bytes, improving performance and memory usage
    add_compile_definitions(NO_RVALUE_INT64)

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" OFF)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # Spatial grid logs
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" OFF)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # Platform identifier for PS2-specific code paths (e.g., scratchpad allocation)
    add_compile_definitions(PLATFORM_PS2)

    # PS2 renderer hot path logs
    option(ENABLE_PS2_RENDERER_LOGS "Enable hot path logs in the PS2 renderer" OFF)
    if(ENABLE_PS2_RENDERER_LOGS)
        add_compile_definitions(ENABLE_PS2_RENDERER_LOGS)
    endif()

    # -fsingle-precision-constant: Treat unsuffixed float constants as single-precision to avoid accidental  float->double promotions on platforms with no hardware double
    # -Werror=double-promotion: Warn when something is being promoted to a double
    # -Wno-format: because there are too many formatting issues that, at the end of the day, do not matter
    target_compile_options(butterscotch PRIVATE -fsingle-precision-constant -Werror=double-promotion -Wno-format)

    if(PS2)
        # Compiling on the Docker container
        target_include_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(${AUDIO_LIBRARIES} butterscotch gskit dmakit audsrv pad mc kbd patches kernel c m stb_ds sha1)

        # Embed ps2sdk IRX modules (freesio2, freepad) as C arrays via bin2c
        # This avoids loading from rom0: which has compatibility issues between fat/slim PS2
        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/freesio2_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freesio2.irx ${IRX_GEN_DIR}/freesio2_irx.c freesio2_irx
            DEPENDS ${IRX_DIR}/freesio2.irx
            COMMENT "Embedding freesio2.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcman.irx ${IRX_GEN_DIR}/mcman_irx.c mcman_irx
            DEPENDS ${IRX_DIR}/mcman.irx
            COMMENT "Embedding mcman.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/mcserv_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/mcserv.irx ${IRX_GEN_DIR}/mcserv_irx.c mcserv_irx
            DEPENDS ${IRX_DIR}/mcserv.irx
            COMMENT "Embedding mcserv.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/freepad_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freepad.irx ${IRX_GEN_DIR}/freepad_irx.c freepad_irx
            DEPENDS ${IRX_DIR}/freepad.irx
            COMMENT "Embedding freepad.irx"
        )
        # We need the USB mass storage IRX modules for writing gmon.out when not running from host: and for keyboard support
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/usbd_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/usbd.irx ${IRX_GEN_DIR}/usbd_irx.c usbd_irx
            DEPENDS ${IRX_DIR}/usbd.irx
            COMMENT "Embedding usbd.irx"
        )
        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/ps2kbd_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/ps2kbd.irx ${IRX_GEN_DIR}/ps2kbd_irx.c ps2kbd_irx
            DEPENDS ${IRX_DIR}/ps2kbd.irx
            COMMENT "Embedding ps2kbd.irx"
        )
        if(AUDIO_BACKEND STREQUAL "ps2")
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/freesd_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/freesd.irx ${IRX_GEN_DIR}/freesd_irx.c freesd_irx
                DEPENDS ${IRX_DIR}/freesd.irx
                COMMENT "Embedding freesd.irx"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/audsrv_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/audsrv.irx ${IRX_GEN_DIR}/audsrv_irx.c audsrv_irx
                DEPENDS ${IRX_DIR}/audsrv.irx
                COMMENT "Embedding audsrv.irx"
            )
        endif()

        target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/freesio2_irx.c ${IRX_GEN_DIR}/mcman_irx.c ${IRX_GEN_DIR}/mcserv_irx.c ${IRX_GEN_DIR}/freepad_irx.c ${IRX_GEN_DIR}/usbd_irx.c ${IRX_GEN_DIR}/ps2kbd_irx.c)
        if(AUDIO_BACKEND STREQUAL "ps2")
            target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/freesd_irx.c ${IRX_GEN_DIR}/audsrv_irx.c)
        endif()
        target_include_directories(butterscotch PRIVATE ${IRX_GEN_DIR})

        # ===[ gprof Profiling Support ]===
        option(ENABLE_GPROF_PROFILING "Enable gprof profiling for PS2" OFF)
        if(ENABLE_GPROF_PROFILING)
            add_compile_definitions(GPROF_PROFILING)
            target_compile_options(butterscotch PRIVATE -pg)
            target_link_options(butterscotch PRIVATE -pg)

            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/bdm_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/bdm.irx ${IRX_GEN_DIR}/bdm_irx.c bdm_irx
                DEPENDS ${IRX_DIR}/bdm.irx
                COMMENT "Embedding bdm.irx (profiler)"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/bdmfs_fatfs.irx ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c bdmfs_fatfs_irx
                DEPENDS ${IRX_DIR}/bdmfs_fatfs.irx
                COMMENT "Embedding bdmfs_fatfs.irx (profiler)"
            )
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/usbmass_bd_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/usbmass_bd.irx ${IRX_GEN_DIR}/usbmass_bd_irx.c usbmass_bd_irx
                DEPENDS ${IRX_DIR}/usbmass_bd.irx
                COMMENT "Embedding usbmass_bd.irx (profiler)"
            )
            target_sources(butterscotch PRIVATE
                ${IRX_GEN_DIR}/bdm_irx.c
                ${IRX_GEN_DIR}/bdmfs_fatfs_irx.c
                ${IRX_GEN_DIR}/usbmass_bd_irx.c
            )
        endif()
    else()
        # This block is for when we are developing Butterscotch with CLion!
        # ps2sdk and gsKit scatter headers across per-module subdirectories;
        # the flat include/ layout only exists after building the SDK.
        # We use file(GLOB) to discover all include dirs automatically.
        file(GLOB PS2SDK_EE_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/*/include)
        file(GLOB PS2SDK_EE_RPC_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/rpc/*/include)
        file(GLOB PS2SDK_EE_NETWORK_INCLUDES ${CMAKE_SOURCE_DIR}/ps2sdk/ee/network/*/include)
        file(GLOB GSKIT_INCLUDES ${CMAKE_SOURCE_DIR}/gsKit/ee/*/include)

        # _EE is normally set by the PS2 toolchain; without it, tamtypes.h
        # refuses to define u64/u32/etc. We set it so CLion can resolve them.
        target_compile_definitions(butterscotch PRIVATE _EE)

        target_include_directories(butterscotch PRIVATE
                ${PS2SDK_EE_INCLUDES}
                ${PS2SDK_EE_RPC_INCLUDES}
                ${PS2SDK_EE_NETWORK_INCLUDES}
                ${CMAKE_SOURCE_DIR}/ps2sdk/common/include
                ${GSKIT_INCLUDES}
                # Newlib and GCC built-in headers are build artifacts (not in any repo),
                # so these are still extracted from Docker by setup.sh.
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/newlib-include
                ${CMAKE_SOURCE_DIR}/ps2dev-headers/gcc-include
        )
    endif()
elseif(PLATFORM STREQUAL "ps3")
    if (NOT ENABLE_LEGACY_GL)
        message(FATAL_ERROR "PS3 requires legacy gl!")
    endif()
    file(GLOB GL_SOURCES src/gl_common/*.c src/gl_legacy/*.c src/image/*.c)
    file(GLOB DEBUG_FONT_SOURCES src/debug_font/*.c)
    target_sources(butterscotch PRIVATE ${GL_SOURCES} ${DEBUG_FONT_SOURCES})
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_legacy)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/image)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/debug_font)
    target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src/gl_common)

    # ps3gl
    file(GLOB PS3GL_SOURCES vendor/ps3gl/*.c)
    add_library(ps3gl STATIC ${PS3GL_SOURCES})
    target_include_directories(ps3gl SYSTEM PUBLIC vendor/ps3gl)
    target_compile_options(ps3gl PRIVATE -w)

    # mojoal
    add_library(mojoal STATIC vendor/mojoal/mojoal.c)
    target_include_directories(mojoal SYSTEM PUBLIC vendor/mojoal)
    target_compile_definitions(mojoal PRIVATE AL_LIBTYPE_STATIC)
    target_compile_options(mojoal PRIVATE -w)

    # Butterscotch VM/interpreter profiler
    option(ENABLE_VM_GML_PROFILER "Enable Butterscotch VM/interpreter profiler" OFF)
    if(ENABLE_VM_GML_PROFILER)
        add_compile_definitions(ENABLE_VM_GML_PROFILER)
    endif()

    # Butterscotch VM/interpreter tracing
    option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
    if(ENABLE_VM_TRACING)
        add_compile_definitions(ENABLE_VM_TRACING)
    endif()

    # Butterscotch VM opcode count profiler
    option(ENABLE_VM_OPCODE_PROFILER "Enable Butterscotch VM opcode count profiler" OFF)
    if(ENABLE_VM_OPCODE_PROFILER)
        add_compile_definitions(ENABLE_VM_OPCODE_PROFILER)
    endif()

    # Spatial grid logs
    option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" OFF)
    if(ENABLE_SPATIAL_GRID_LOGS)
        add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
    endif()

    # VM unknown/stubbed function logs
    option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" OFF)
    if(ENABLE_VM_STUB_LOGS)
        add_compile_definitions(ENABLE_VM_STUB_LOGS)
    endif()

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # Platform identifier for PS3-specific code paths
    add_compile_definitions(PLATFORM_PS3)
    add_compile_definitions(AL_LIBTYPE_STATIC)

    set(CMAKE_EXE_LINKER_FLAGS "-mhard-float -fmodulo-sched -Wl,--gc-sections -L$ENV{PS3DEV}/ppu/lib -L$ENV{PS3DEV}/portlibs/ppu/lib")

    find_package(SDL2 REQUIRED)
    target_include_directories(butterscotch PRIVATE ${SDL2_INCLUDE_DIRS})
    # mojoal calls SDL atomics/threads, so SDL2 must link after it (static link order)
    target_link_libraries(mojoal PUBLIC ${SDL2_LIBRARIES})
    target_link_libraries(butterscotch PRIVATE bzip2 sha1 ps3gl mojoal ${SDL2_LIBRARIES} gcm_sys rsx sysutil io audio rt lv2 m stb_ds stb_vorbis)
elseif(PLATFORM STREQUAL "vita")
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
