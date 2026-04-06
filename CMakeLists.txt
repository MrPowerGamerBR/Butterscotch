cmake_minimum_required(VERSION 3.21)
project(butterscotch C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra)

set(PLATFORM "" CACHE STRING "Platform backend")
option(NO_AUDIO "Disable audio support" OFF)

# Build metadata
set(BUTTERSCOTCH_COMMIT_HASH "unknown" CACHE STRING "Git commit hash")
set(BUTTERSCOTCH_COMMIT_DATE "unknown" CACHE STRING "Git commit date")
add_compile_definitions(
    BUTTERSCOTCH_COMMIT_HASH="${BUTTERSCOTCH_COMMIT_HASH}"
    BUTTERSCOTCH_COMMIT_DATE="${BUTTERSCOTCH_COMMIT_DATE}"
)

if(NO_AUDIO)
    add_compile_definitions(NO_AUDIO)
    message(STATUS "Audio: DISABLED")
else()
    message(STATUS "Audio: ENABLED")
endif()

if(PLATFORM STREQUAL "")
    message(FATAL_ERROR "PLATFORM is not set! Use -DPLATFORM=glfw, -DPLATFORM=sdl, -DPLATFORM=miyoo, -DPLATFORM=ps2, or -DPLATFORM=fbdev")
endif()

# Core source files (exclude platform-specific subdirectories)
file(GLOB SOURCES 
    src/*.c
    src/vm/*.c
)

# Platform specific files builds
if(PLATFORM STREQUAL "sdl")
    # SDL platform files + SDL_mixer audio (defined later in SDL section)
    set(PLATFORM_SOURCES "")
elseif(PLATFORM STREQUAL "miyoo")
    # Miyoo uses SDL platform files with NOOP audio (no sound)
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

    # But keep libogg and Tremor files as pure C (they don't need C++ features)
    set_source_files_properties(${OGG_SOURCES} PROPERTIES LANGUAGE C)
    set_source_files_properties(${TREMOR_SOURCES} PROPERTIES LANGUAGE C)
    set_source_files_properties(src/sdl/tremor_backend.c PROPERTIES LANGUAGE C)
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
        target_link_directories(butterscotch PRIVATE ${GLFW3_LIBRARY_DIRS})
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
    # SDL 1.2 with software rendering and miniaudio + Tremor for audio

    # SDL platform files + miniaudio Tremor audio + debug menu
    file(GLOB SDL_PLATFORM_SOURCES src/sdl/*.c)
    set(PLATFORM_SOURCES ${SDL_PLATFORM_SOURCES} src/sdl/ma_tremor_audio.cpp src/sdl/tremor_backend.c src/sdl/debug_menu.c)
    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})

    # libogg (static build)
    set(OGG_SOURCES
        vendor/ogg/src/bitwise.c
        vendor/ogg/src/framing.c
    )
    target_sources(butterscotch PRIVATE ${OGG_SOURCES})
    target_include_directories(butterscotch PUBLIC vendor/ogg/include)

    # Tremor (libvorbisidec - integer OGG Vorbis decoder, static build)
    set(TREMOR_SOURCES
        vendor/tremor/block.c
        vendor/tremor/codebook.c
        vendor/tremor/floor0.c
        vendor/tremor/floor1.c
        vendor/tremor/info.c
        vendor/tremor/mapping0.c
        vendor/tremor/mdct.c
        vendor/tremor/registry.c
        vendor/tremor/res012.c
        vendor/tremor/sharedbook.c
        vendor/tremor/synthesis.c
        vendor/tremor/vorbisfile.c
        vendor/tremor/window.c
    )
    target_sources(butterscotch PRIVATE ${TREMOR_SOURCES})
    target_include_directories(butterscotch PUBLIC vendor/tremor)

    # stb_image for PNG decoding
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # miniaudio for audio playback (ALSA only)
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    # Disable unnecessary miniaudio backends for SDL desktop Linux
    add_compile_definitions(MA_NO_PULSEAUDIO MA_NO_JACK MA_NO_SNDIO MA_NO_AUDIO4 MA_NO_OSS MA_NO_COREAUDIO MA_NO_WASAPI MA_NO_DSOUND MA_NO_WINMM MA_NO_AAUDIO MA_NO_OPENSL MA_NO_WEBAUDIO)

    if(MINGW)
        find_package(SDL REQUIRED)
        find_package(SDL_ttf REQUIRED)
        find_package(SDL_image REQUIRED)
        target_include_directories(butterscotch PRIVATE ${SDL_INCLUDE_DIR})
        target_link_libraries(butterscotch ${SDL_LIBRARY} ${SDL_TTF_LIBRARY} ${SDL_IMAGE_LIBRARY} m atomic asound)
        target_link_options(butterscotch PRIVATE -static)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL REQUIRED sdl SDL_ttf SDL_image)
        target_include_directories(butterscotch PRIVATE ${SDL_INCLUDE_DIRS})
        target_link_libraries(butterscotch ${SDL_LIBRARIES} m pthread atomic asound)
    endif()
elseif(PLATFORM STREQUAL "miyoo")
    # Miyoo CFW (SDL 1.2 with software rendering, cross-compiled)
    # Audio via miniaudio + Tremor (ALSA only)

    # SDL platform files + miniaudio Tremor audio
    file(GLOB MIYOO_PLATFORM_SOURCES src/sdl/*.c)
    set(PLATFORM_SOURCES ${MIYOO_PLATFORM_SOURCES} src/sdl/ma_tremor_audio.cpp src/sdl/tremor_backend.c src/sdl/debug_menu.c)
    target_sources(butterscotch PRIVATE ${PLATFORM_SOURCES})

    # libogg (static build)
    set(OGG_SOURCES
        vendor/ogg/src/bitwise.c
        vendor/ogg/src/framing.c
    )
    target_sources(butterscotch PRIVATE ${OGG_SOURCES})
    target_include_directories(butterscotch PUBLIC vendor/ogg/include)

    # Tremor (libvorbisidec - integer OGG Vorbis decoder, static build)
    set(TREMOR_SOURCES
        vendor/tremor/block.c
        vendor/tremor/codebook.c
        vendor/tremor/floor0.c
        vendor/tremor/floor1.c
        vendor/tremor/info.c
        vendor/tremor/mapping0.c
        vendor/tremor/mdct.c
        vendor/tremor/registry.c
        vendor/tremor/res012.c
        vendor/tremor/sharedbook.c
        vendor/tremor/synthesis.c
        vendor/tremor/vorbisfile.c
        vendor/tremor/window.c
    )
    target_sources(butterscotch PRIVATE ${TREMOR_SOURCES})
    target_include_directories(butterscotch PUBLIC vendor/tremor)

    # stb_image for PNG decoding
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # miniaudio for audio playback (ALSA only)
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    # Miyoo-specific definitions
    add_compile_definitions(HAVE_STDINT_H VERSION_BITTBOY MIYOO_KEYBINDINGS)

    # miniaudio: ALSA only backend (no PulseAudio, JACK, etc.)
    # Disable runtime linking — link directly against libasound at compile time
    add_compile_definitions(MA_NO_PULSEAUDIO MA_NO_JACK MA_NO_SNDIO MA_NO_AUDIO4 MA_NO_OSS MA_NO_COREAUDIO MA_NO_WASAPI MA_NO_DSOUND MA_NO_WINMM MA_NO_AAUDIO MA_NO_OPENSL MA_NO_WEBAUDIO MA_NO_NULL MA_NO_RUNTIME_LINKING)

    # Optimization flags for Miyoo (CMake list format)
    # Use -O2 instead of -Ofast for better stability, disable LTO which can cause hangs
    set(MIYOO_OPT_FLAGS "-Ofast;-ffunction-sections;-fdata-sections;-fno-common;-fno-PIC;-flto;-fPIE" CACHE STRING "Miyoo optimization flags")
    target_compile_options(butterscotch PRIVATE ${MIYOO_OPT_FLAGS})

    # Use pkg-config with sysroot for SDL and SDL_mixer libraries
    set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL REQUIRED sdl SDL_ttf SDL_image SDL_mixer freetype2)
    target_include_directories(butterscotch PRIVATE ${SDL_INCLUDE_DIRS})
    target_link_libraries(butterscotch ${SDL_LIBRARIES} m pthread asound ts mad mpg123 mikmod png z bz2 freetype atomic)

    # Linker flags for size optimization (strip only for Release, no LTO)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_link_options(butterscotch PRIVATE -Wl,--gc-sections)
    else()
        target_link_options(butterscotch PRIVATE -Wl,--gc-sections -s)
    endif()
    
    # ===[ IPK Package Creation for Miyoo ]===
    # Install to /mnt/games/butterscotch/ with gmenu2x integration
    set(IPK_ROOT "${CMAKE_BINARY_DIR}/ipk_root")
    set(IPK_DATA_DIR "${IPK_ROOT}/data")
    set(IPK_GAMES_DIR "${IPK_DATA_DIR}/mnt/games/butterscotch")
    set(IPK_GMENU2X_DIR "${IPK_DATA_DIR}//mnt/gmenu2x/sections/games")
    set(IPK_CONTROL_DIR "${IPK_ROOT}/CONTROL")
    
    # Create directory structure
    add_custom_target(ipk
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${IPK_ROOT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${IPK_GAMES_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${IPK_GMENU2X_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${IPK_CONTROL_DIR}
        COMMENT "Creating IPK directory structure"
    )
    
    # Copy binary after build
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:butterscotch> ${IPK_GAMES_DIR}/butterscotch
        COMMENT "Copying butterscotch binary to IPK package"
    )
    
    # Create gmenu2x section file
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "title=Butterscotch" > ${IPK_GMENU2X_DIR}/butterscotch.lnk
        COMMAND ${CMAKE_COMMAND} -E echo "description=GameMaker Studio runner" >> ${IPK_GMENU2X_DIR}/butterscotch.lnk
        COMMAND ${CMAKE_COMMAND} -E echo "exec=/mnt/games/butterscotch/butterscotch" >> ${IPK_GMENU2X_DIR}/butterscotch.lnk
        COMMAND ${CMAKE_COMMAND} -E echo "selectordir=/mnt/" >> ${IPK_GMENU2X_DIR}/butterscotch.lnk
        COMMAND ${CMAKE_COMMAND} -E echo "selectorbrowser=true" >> ${IPK_GMENU2X_DIR}/butterscotch.lnk
        COMMAND ${CMAKE_COMMAND} -E echo "selectorfilter=.unx,.win" >> ${IPK_GMENU2X_DIR}/butterscotch.lnk
        COMMENT "Creating gmenu2x section file"
    )
    
    # Create control file
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Package: butterscotch" > ${IPK_CONTROL_DIR}/control
        COMMAND ${CMAKE_COMMAND} -E echo "Version: 1.1.0" >> ${IPK_CONTROL_DIR}/control
        COMMAND ${CMAKE_COMMAND} -E echo "Section: games" >> ${IPK_CONTROL_DIR}/control
        COMMAND ${CMAKE_COMMAND} -E echo "Architecture: arm" >> ${IPK_CONTROL_DIR}/control
        COMMAND ${CMAKE_COMMAND} -E echo "Description: GameMaker Studio 1.4 runner for Miyoo" >> ${IPK_CONTROL_DIR}/control
        COMMENT "Creating control file"
    )
    
    # Create debian-binary
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "2.0" > ${IPK_ROOT}/debian-binary
        COMMENT "Creating debian-binary"
    )
    
    # Create data.tar.gz
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND tar -czf ${CMAKE_BINARY_DIR}/data.tar.gz -C ${IPK_DATA_DIR} .
        COMMENT "Creating data.tar.gz"
    )
    
    # Create control.tar.gz
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND tar -czf ${CMAKE_BINARY_DIR}/control.tar.gz -C ${IPK_CONTROL_DIR} .
        COMMENT "Creating control.tar.gz"
    )
    
    # Copy archives to IPK_ROOT for final packaging
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/data.tar.gz ${IPK_ROOT}/data.tar.gz
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/control.tar.gz ${IPK_ROOT}/control.tar.gz
        COMMENT "Copying archives to IPK root"
    )

    # Create final IPK
    add_custom_command(TARGET ipk POST_BUILD
        COMMAND tar -cf ${CMAKE_BINARY_DIR}/butterscotch_1.0.0_all.ipk debian-binary control.tar.gz data.tar.gz
        WORKING_DIRECTORY ${IPK_ROOT}
        DEPENDS butterscotch
        COMMENT "Creating butterscotch_1.0.0_all.ipk"
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
elseif(PLATFORM STREQUAL "fbdev")
    # Direct framebuffer renderer for embedded Linux (no SDL/X11/Wayland)
    # Uses /dev/fb0 for rendering and /dev/input/event* for keyboard input

    # Compile as C++ (project uses C++ features like typed enums, nullptr)
    set_source_files_properties(${SOURCES} ${PLATFORM_SOURCES} PROPERTIES LANGUAGE CXX)
    target_compile_options(butterscotch PRIVATE -fpermissive)

    # stb_image for PNG decoding (optional, for texture loading)
    target_include_directories(butterscotch PUBLIC vendor/stb/image)

    # miniaudio for audio (optional)
    target_include_directories(butterscotch PUBLIC vendor/miniaudio)

    # No external libraries needed - uses Linux kernel headers only
    # fbdev and input APIs are part of the standard Linux headers

    # Link with standard libraries
    target_link_libraries(butterscotch m pthread stdc++)

    # Optimization for embedded targets
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_link_options(butterscotch PRIVATE -Wl,--gc-sections -s)
    else()
        target_link_options(butterscotch PRIVATE -Wl,--gc-sections)
    endif()
else()
    message(FATAL_ERROR "Unknown platform! ${PLATFORM}")
endif()
