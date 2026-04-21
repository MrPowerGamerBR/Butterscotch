#include "sdl2_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL2/SDL.h>

// ===[ Helpers ]===

// The caller must make sure to free the returned string!
static char* buildFullPath(SDL2FileSystem* fs, const char* relativePath) {
    if (strstr(relativePath, fs->basePath) != nullptr) return safeStrdup(relativePath);
    size_t baseLen = strlen(fs->basePath);
    size_t relLen = strlen(relativePath);
    char* fullPath = safeMalloc(baseLen + relLen + 1);
    memcpy(fullPath, fs->basePath, baseLen);
    memcpy(fullPath + baseLen, relativePath, relLen);
    fullPath[baseLen + relLen] = '\0';
    return fullPath;
}

// ===[ Vtable Implementations ]===

// The caller must make sure to free the returned string!
static char* sdl2ResolvePath(FileSystem* fs, const char* relativePath) {
    return buildFullPath((SDL2FileSystem*) fs, relativePath);
}

static bool sdl2FileExists(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((SDL2FileSystem*) fs, relativePath);
    struct stat st;
    bool exists = (stat(fullPath, &st) == 0);
    free(fullPath);
    return exists;
}

static char* sdl2ReadFileText(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((SDL2FileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr)
        return nullptr;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(content, 1, (size_t) size, f);
    content[bytesRead] = '\0';
    fclose(f);
    return content;
}

static bool sdl2WriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    char* fullPath = buildFullPath((SDL2FileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr)
        return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool sdl2DeleteFile(FileSystem* fs, const char* relativePath) {
    char* fullPath = buildFullPath((SDL2FileSystem*) fs, relativePath);
    int result = remove(fullPath);
    free(fullPath);
    return result == 0;
}

static bool sdl2ReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    char* fullPath = buildFullPath((SDL2FileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "rb");
    free(fullPath);
    if (f == nullptr)
        return false;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* data = safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, f);
    fclose(f);

    *outData = data;
    *outSize = (int32_t) bytesRead;
    return true;
}

static bool sdl2WriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    char* fullPath = buildFullPath((SDL2FileSystem*) fs, relativePath);
    FILE* f = fopen(fullPath, "wb");
    free(fullPath);
    if (f == nullptr)
        return false;

    size_t written = fwrite(data, 1, (size_t) size, f);
    fclose(f);
    return written == (size_t) size;
}

// ===[ Vtable ]===

static FileSystemVtable sdl2FileSystemVtable = {
    .resolvePath = sdl2ResolvePath,
    .fileExists = sdl2FileExists,
    .readFileText = sdl2ReadFileText,
    .writeFileText = sdl2WriteFileText,
    .deleteFile = sdl2DeleteFile,
    .readFileBinary = sdl2ReadFileBinary,
    .writeFileBinary = sdl2WriteFileBinary,
};

// ===[ Lifecycle ]===

SDL2FileSystem* SDL2FileSystem_create(const char* dataWinPath) {
    SDL2FileSystem* fs = safeCalloc(1, sizeof(SDL2FileSystem));
    fs->base.vtable = &sdl2FileSystemVtable;

    // Derive basePath by stripping the filename from dataWinPath
    const char* lastSlash = strrchr(dataWinPath, '/');
    const char* lastBackslash = strrchr(dataWinPath, '\\');
    if (lastBackslash != nullptr && (lastSlash == nullptr || lastBackslash > lastSlash))
        lastSlash = lastBackslash;
    if (lastSlash != nullptr) {
        size_t dirLen = (size_t) (lastSlash - dataWinPath + 1); // include the trailing /
        fs->basePath = safeMalloc(dirLen + 1);
        memcpy(fs->basePath, dataWinPath, dirLen);
        fs->basePath[dirLen] = '\0';
    } else {
        // data.win is in current directory
        fs->basePath = safeStrdup("./");
    }

    return fs;
}

void SDL2FileSystem_destroy(SDL2FileSystem* fs) {
    if (fs == nullptr) return;
    free(fs->basePath);
    free(fs);
}
