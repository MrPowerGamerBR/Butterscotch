cmake_minimum_required(VERSION 3.21)
project(butterscotch C CXX)

set(CMAKE_C_STANDARD 23)
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
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw, -DPLATFORM=sdl, -DPLATFORM=miyoo, or -DPLATFORM=ps2")
endif()

file(GLOB SOURCES src/*.c)
# Platform specific files builds
if(PLATFORM STREQUAL "miyoo")
    # Miyoo uses SDL platform files
    file(GLOB PLATFORM_SOURCES src/sdl/*.c)
else()
    file(GLOB PLATFORM_SOURCES src/${PLATFORM}/*.c)
endif()

add_executable(butterscotch ${SOURCES} ${PLATFORM_SOURCES})

# For Miyoo platform, compile C files as C++ (project uses C++ features like typed enums)
if(PLATFORM STREQUAL "miyoo")
    set_source_files_properties(${SOURCES} ${PLATFORM_SOURCES} PROPERTIES LANGUAGE CXX)
    # Allow implicit void* conversions for C++ compilation of C code
    target_compile_options(butterscotch PRIVATE -fpermissive)
endif()

target_include_directories(butterscotch PRIVATE ${CMAKE_SOURCE_DIR}/src)

# stb_ds
target_include_directories(butterscotch PUBLIC vendor/stb/ds)

if(PLATFORM STREQUAL "glfw")
    # GLAD
    add_library(glad STATIC vendor/glad/src/glad.c)
    target_include_directories(glad PUBLIC vendor/glad/include)

    # stb_image
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # stb_vorbis
    target_include_directories(butterscotch PUBLIC vendor/stb/vorbis)

    # miniaudio
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    if(MINGW)
        find_package(glfw3 REQUIRED)
        set(GLFW3_LIBRARIES glfw3)

        # Target Windows 7+ (avoids referencing newer APIs like MapViewOfFileNuma2 that MinGW import libs don't provide)
        # https://github.com/mirror/mingw-w64/blob/master/mingw-w64-headers/include/sdkddkver.h
        target_compile_definitions(butterscotch PRIVATE _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000 WIN32_LEAN_AND_MEAN)
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m opengl32 gdi32 winmm)
        target_link_options(butterscotch PRIVATE -static)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(GLFW3 REQUIRED glfw3)
        target_include_directories(butterscotch PRIVATE ${GLFW3_INCLUDE_DIRS})
        target_link_libraries(butterscotch ${GLFW3_LIBRARIES} glad m pthread dl)
    endif()

    # Enable AddressSanitizer by default in Debug builds
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(ENABLE_ASAN_DEFAULT ON)
    else()
        set(ENABLE_ASAN_DEFAULT OFF)
    endif()
    set(ENABLE_ASAN ${ENABLE_ASAN_DEFAULT} CACHE BOOL "Enable AddressSanitizer")
    if(ENABLE_ASAN AND NOT MINGW)
        target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(butterscotch PRIVATE -fsanitize=address)
    endif()
elseif(PLATFORM STREQUAL "sdl")
    # SDL 1.2 with software rendering

    # stb_image for PNG decoding
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    if(MINGW)
        find_package(SDL REQUIRED)
        find_package(SDL_ttf REQUIRED)
        find_package(SDL_image REQUIRED)
        find_package(SDL_mixer REQUIRED)
        target_include_directories(butterscotch PRIVATE ${SDL_INCLUDE_DIR})
        target_link_libraries(butterscotch ${SDL_LIBRARY} ${SDL_TTF_LIBRARY} ${SDL_IMAGE_LIBRARY} ${SDL_MIXER_LIBRARY} m)
        target_link_options(butterscotch PRIVATE -static)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL REQUIRED sdl SDL_ttf SDL_image SDL_mixer)
        target_include_directories(butterscotch PRIVATE ${SDL_INCLUDE_DIRS})
        target_link_libraries(butterscotch ${SDL_LIBRARIES} m pthread)
    endif()
elseif(PLATFORM STREQUAL "miyoo")
    # Miyoo CFW (SDL 1.2 with software rendering, cross-compiled)

    # stb_image for PNG decoding
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # Miyoo-specific definitions (already in toolchain, but ensure for native builds)
    add_compile_definitions(HAVE_STDINT_H VERSION_BITTBOY MIYOO_KEYBINDINGS MIYOO_NO_AUDIO)

    # add_compile_definitions(DISABLE_VM_TRACING)

    # Optimization flags for Miyoo (CMake list format)
    # Use -O2 instead of -Ofast for better stability, disable LTO which can cause hangs
    set(MIYOO_OPT_FLAGS "-Ofast;-ffunction-sections;-fdata-sections;-fno-common;-fno-PIC;-flto" CACHE STRING "Miyoo optimization flags")
    target_compile_options(butterscotch PRIVATE ${MIYOO_OPT_FLAGS})

    # Use pkg-config with sysroot for SDL and SDL_mixer libraries
    set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL REQUIRED sdl SDL_ttf SDL_image SDL_mixer freetype2)
    target_include_directories(butterscotch PRIVATE ${SDL_INCLUDE_DIRS})
    target_link_libraries(butterscotch ${SDL_LIBRARIES} m pthread asound ts mad mpg123 vorbisfile vorbis ogg mikmod png z bz2 freetype)

    # Linker flags for size optimization (strip only for Release, no LTO)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_link_options(butterscotch PRIVATE -Wl,--gc-sections)
    else()
        target_link_options(butterscotch PRIVATE -Wl,--gc-sections -s)
    endif()
    
    # ===[ IPK Package Creation for Miyoo ]===
    # Create IPK package structure for easy installation on Miyoo devices
    set(IPK_ROOT "${CMAKE_BINARY_DIR}/ipk_root")
    set(IPK_DATA_DIR "${IPK_ROOT}/data")
    set(IPK_GAMES_DIR "${IPK_DATA_DIR}/mnt/games/butterscotch")
    set(IPK_GMENU2X_DIR "${IPK_DATA_DIR}/gmenu2x/sections/games")
    
    # Create directory structure
    file(MAKE_DIRECTORY "${IPK_GAMES_DIR}")
    file(MAKE_DIRECTORY "${IPK_GMENU2X_DIR}")
    
    # Copy binary after build
    add_custom_command(TARGET butterscotch POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:butterscotch> ${IPK_GAMES_DIR}/butterscotch
        COMMENT "Copying butterscotch binary to IPK package"
    )
    
    # Create gmenu2x section file
    file(WRITE "${IPK_GMENU2X_DIR}/butterscotch.lnk"
"title=Butterscotch
description=Undertale compatible GameMaker Studio runner
exec=/mnt/games/butterscotch/butterscotch
selectordir=/mnt/
selectorbrowser=true
selectorfilter=.unx,.win
")
    
    # Create control file for IPK
    file(MAKE_DIRECTORY "${IPK_ROOT}/CONTROL")
    file(WRITE "${IPK_ROOT}/CONTROL/control"
"Package: butterscotch
Version: 1.0.0
Source: https://github.com/kptmx/Butterscotch-miyoo
Section: games
Maintainer: MrPowerGamerBR & kptmx
Architecture: arm
Installed-Size: 2048
Description: GameMaker Studio 1.4 runner for Miyoo devices
 Butterscotch is an open-source implementation of GameMaker:Studio's runner.
 It allows you to (kinda) play Undertale on MiyooCFW devices.
")

    # Create debian-binary file
    file(WRITE "${IPK_ROOT}/debian-binary" "2.0\n")
    
    # Create IPK package using shell tar command
    add_custom_target(ipk
        # Create data.tar.gz from data directory
        COMMAND tar -czf ${CMAKE_BINARY_DIR}/data.tar.gz -C ${IPK_DATA_DIR} .
        
        # Create control.tar.gz from CONTROL directory  
        COMMAND tar -czf ${CMAKE_BINARY_DIR}/control.tar.gz -C ${IPK_ROOT}/CONTROL .
        
        # Copy archives to IPK_ROOT for final packaging
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/data.tar.gz ${IPK_ROOT}/data.tar.gz
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/control.tar.gz ${IPK_ROOT}/control.tar.gz
        
        # Create final IPK by combining all parts
        COMMAND tar -cf ${CMAKE_BINARY_DIR}/butterscotch_${CMAKE_PROJECT_VERSION}_all.ipk debian-binary control.tar.gz data.tar.gz
        WORKING_DIRECTORY ${IPK_ROOT}
        DEPENDS butterscotch
        COMMENT "Creating IPK package for Miyoo"
    )
elseif(PLATFORM STREQUAL "ps2")
    # The PS2 EE has a single-precision FPU only, so double is software-emulated
    add_compile_definitions(USE_FLOAT_REALS)
    # By disabling Int64 support, we can fit the entire RValue struct in 8 bytes, improving performance and memory usage
    add_compile_definitions(NO_RVALUE_INT64)
    # Disable Butterscotch VM/interpreter tracing checks
    add_compile_definitions(DISABLE_VM_TRACING)
    # Disable the PS2 audio system
    option(DISABLE_PS2_AUDIO "Disable the PS2 audio system" OFF)
    if(DISABLE_PS2_AUDIO)
        add_compile_definitions(DISABLE_PS2_AUDIO)
        list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/ps2/ps2_audio_system.c)
    endif()
    # Treat unsuffixed float constants as single-precision to avoid accidental
    # float->double promotions on platforms with no hardware double
    add_compile_options(-fsingle-precision-constant)

    if(PS2)
        # Compiling on the Docker container
        target_include_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
        target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
        target_link_libraries(butterscotch gskit dmakit audsrv pad mc patches kernel c m)

        # Embed ps2sdk IRX modules (sio2man, padman) as C arrays via bin2c
        # This avoids loading from rom0: which has compatibility issues between fat/slim PS2
        set(IRX_DIR $ENV{PS2SDK}/iop/irx)
        set(IRX_GEN_DIR ${CMAKE_BINARY_DIR}/generated/irx)
        file(MAKE_DIRECTORY ${IRX_GEN_DIR})

        add_custom_command(
            OUTPUT ${IRX_GEN_DIR}/sio2man_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/sio2man.irx ${IRX_GEN_DIR}/sio2man_irx.c sio2man_irx
            DEPENDS ${IRX_DIR}/sio2man.irx
            COMMENT "Embedding sio2man.irx"
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
            OUTPUT ${IRX_GEN_DIR}/padman_irx.c
            COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/padman.irx ${IRX_GEN_DIR}/padman_irx.c padman_irx
            DEPENDS ${IRX_DIR}/padman.irx
            COMMENT "Embedding padman.irx"
        )
        if(NOT DISABLE_PS2_AUDIO)
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

        target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/sio2man_irx.c ${IRX_GEN_DIR}/mcman_irx.c ${IRX_GEN_DIR}/mcserv_irx.c ${IRX_GEN_DIR}/padman_irx.c)
        if(NOT DISABLE_PS2_AUDIO)
            target_sources(butterscotch PRIVATE ${IRX_GEN_DIR}/freesd_irx.c ${IRX_GEN_DIR}/audsrv_irx.c)
        endif()
        target_include_directories(butterscotch PRIVATE ${IRX_GEN_DIR})

        # ===[ gprof Profiling Support ]===
        option(ENABLE_GPROF_PROFILING "Enable gprof profiling for PS2" OFF)
        if(ENABLE_GPROF_PROFILING)
            add_compile_definitions(GPROF_PROFILING)
            target_compile_options(butterscotch PRIVATE -pg)
            target_link_options(butterscotch PRIVATE -pg)

            # Embed USB mass storage IRX modules for writing gmon.out when not running from host:
            add_custom_command(
                OUTPUT ${IRX_GEN_DIR}/usbd_irx.c
                COMMAND $ENV{PS2SDK}/bin/bin2c ${IRX_DIR}/usbd.irx ${IRX_GEN_DIR}/usbd_irx.c usbd_irx
                DEPENDS ${IRX_DIR}/usbd.irx
                COMMENT "Embedding usbd.irx (profiler)"
            )
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
                ${IRX_GEN_DIR}/usbd_irx.c
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
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
