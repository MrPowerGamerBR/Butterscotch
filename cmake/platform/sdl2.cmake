# Butterscotch VM/interpreter profiler
option(ENABLE_VM_PROFILER "Enable Butterscotch VM/interpreter profiler" ON)
if(ENABLE_VM_PROFILER)
    add_compile_definitions(ENABLE_VM_PROFILER)
endif()

# Butterscotch VM/interpreter tracing
option(ENABLE_VM_TRACING "Enable Butterscotch VM/interpreter tracing checks" ON)
if(ENABLE_VM_TRACING)
    add_compile_definitions(ENABLE_VM_TRACING)
endif()

# Spatial grid logs
option(ENABLE_SPATIAL_GRID_LOGS "Enable Spatial Grid logs" ON)
if(ENABLE_SPATIAL_GRID_LOGS)
    add_compile_definitions(ENABLE_SPATIAL_GRID_LOGS)
endif()

# VM unknown/stubbed function logs
option(ENABLE_VM_STUB_LOGS "Enable VM unknown/stubbed function logs" ON)
if(ENABLE_VM_STUB_LOGS)
    add_compile_definitions(ENABLE_VM_STUB_LOGS)
endif()

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
    find_package(SDL2 REQUIRED)
    set(SDL2_LIBRARIES sdl2)

    # Target Windows 7+ (avoids referencing newer APIs like MapViewOfFileNuma2 that MinGW import libs don't provide)
    # https://github.com/mirror/mingw-w64/blob/master/mingw-w64-headers/include/sdkddkver.h
    target_compile_definitions(butterscotch PRIVATE _WIN32_WINNT=0x0601 NTDDI_VERSION=0x06010000 WIN32_LEAN_AND_MEAN)
    target_link_libraries(butterscotch ${SDL2_LIBRARIES} glad m opengl32 gdi32 winmm bz2)
    target_link_options(butterscotch PRIVATE -static)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Haiku")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    target_include_directories(butterscotch PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_directories(butterscotch PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(butterscotch ${SDL2_LIBRARIES} glad m pthread bz2)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    target_include_directories(butterscotch PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_directories(butterscotch PRIVATE ${SDL2_LIBRARY_DIRS})
    target_link_libraries(butterscotch ${SDL2_LIBRARIES} glad m pthread dl bz2)
endif()

# Enable AddressSanitizer by default in Debug builds
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(ENABLE_ASAN_DEFAULT ON)
else()
    set(ENABLE_ASAN_DEFAULT OFF)
endif()
set(ENABLE_ASAN ${ENABLE_ASAN_DEFAULT} CACHE BOOL "Enable AddressSanitizer")
if(ENABLE_ASAN AND NOT MINGW AND NOT CMAKE_SYSTEM_NAME STREQUAL "Haiku")
    target_compile_options(butterscotch PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(butterscotch PRIVATE -fsanitize=address)
endif()

#gamecontrollerdb
add_custom_command(TARGET butterscotch POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/vendor/gamecontrollerdb.txt
        $<TARGET_FILE_DIR:butterscotch>/gamecontrollerdb.txt
)