# The PS2 EE has a single-precision FPU only, so double is software-emulated
add_compile_definitions(USE_FLOAT_REALS)
# By disabling Int64 support, we can fit the entire RValue struct in 8 bytes, improving performance and memory usage
add_compile_definitions(NO_RVALUE_INT64)

# Butterscotch VM/interpreter profiler
option(ENABLE_VM_PROFILER "Enable Butterscotch VM/interpreter profiler" OFF)
if(ENABLE_VM_PROFILER)
    add_compile_definitions(ENABLE_VM_PROFILER)
endif()

# Butterscotch VM/interpreter tracing
option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" OFF)
if(ENABLE_VM_TRACING)
    add_compile_definitions(ENABLE_VM_TRACING)
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

# PS2 audio system
option(ENABLE_PS2_AUDIO "Enable the PS2 audio system" ON)
if(ENABLE_PS2_AUDIO)
    add_compile_definitions(ENABLE_PS2_AUDIO)
else()
    list(REMOVE_ITEM PLATFORM_SOURCES ${CMAKE_SOURCE_DIR}/src/ps2/ps2_audio_system.c)
endif()

# -fsingle-precision-constant: Treat unsuffixed float constants as single-precision to avoid accidental  float->double promotions on platforms with no hardware double
# -Werror=double-promotion: Warn when something is being promoted to a double
# -fomit-frame-pointer: frees a register on a target with tight register pressure
# -ffast-math: enables aggressive floating point optimizations
target_compile_options(butterscotch PRIVATE -fsingle-precision-constant -Werror=double-promotion -fomit-frame-pointer -ffast-math)

if(PS2)
    # Compiling on the Docker container
    target_include_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/include $ENV{PS2SDK}/common/include $ENV{PS2DEV}/ee/include $ENV{GSKIT}/include)
    target_link_directories(butterscotch PRIVATE $ENV{PS2SDK}/ee/lib $ENV{GSKIT}/lib)
    target_link_libraries(butterscotch gskit dmakit audsrv pad mc kbd patches kernel c m)

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
    if(ENABLE_PS2_AUDIO)
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
    if(ENABLE_PS2_AUDIO)
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