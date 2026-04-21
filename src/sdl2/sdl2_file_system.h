#pragma once

#include "common.h"
#include "file_system.h"

typedef struct {
    FileSystem base;
    char* basePath; // directory containing data.win, with trailing separator
} SDL2FileSystem;

// Creates a SDL2FileSystem from the path to the data.win file
// The basePath is derived by stripping the filename from dataWinPath.
SDL2FileSystem* SDL2FileSystem_create(const char* dataWinPath);
void SDL2FileSystem_destroy(SDL2FileSystem* fs);
