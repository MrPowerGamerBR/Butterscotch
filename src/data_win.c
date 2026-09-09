#include "data_win.h"
#include "binary_reader.h"

#include <stdbool.h>
#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h"
#include "math_compat.h"

#include "stb_ds.h"
#include "utils.h"

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
#else
    #include <unistd.h>
    #if defined(_POSIX_MAPPED_FILES) && (_POSIX_MAPPED_FILES > 0)
        #include <sys/mman.h>
    #endif
#endif

static uint8_t *mapFile(FILE *file, size_t size) {
    if (!file || size == 0) return NULL;

#if defined(_WIN32)
    intptr_t osHandle = _get_osfhandle(_fileno(file));
    if (osHandle == -1) return NULL;
    HANDLE hFile = (HANDLE)osHandle;

    HANDLE hMap = CreateFileMappingA(
        hFile,
        NULL,
        PAGE_READONLY,
        0, 0,
        NULL
    );
    if (!hMap) return NULL;

    void *ptr = MapViewOfFile(
        hMap,
        FILE_MAP_READ,
        0, 0,
        size
    );
    CloseHandle(hMap);

    if (!ptr) return NULL;
    return (uint8_t *)ptr;
#elif defined(_POSIX_MAPPED_FILES) && _POSIX_MAPPED_FILES > 0
    int fd = fileno(file);
    if (fd == -1) return NULL;

    void *ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) return NULL;

    return (uint8_t *)ptr;
#else
    return NULL;
#endif
}

static void unmapFile(uint8_t *ptr, size_t size) {
    if (!ptr) return;

#ifdef _WIN32
    (void)size;
    UnmapViewOfFile((LPCVOID)ptr);
#elif defined(_POSIX_MAPPED_FILES) && _POSIX_MAPPED_FILES > 0
    munmap(ptr, size);
#else
    (void)size;
#endif
}

// ===[ HELPERS ]===

// Reads a uint32 absolute file offset, resolves it into the pre-loaded STRG buffer,
// and returns a pointer to the null-terminated string content at that offset.
static const char* readStringPtr(BinaryReader* reader, DataWin* dw) {
    uint32_t offset = BinaryReader_readUint32(reader);
    if (offset == 0) return nullptr;
    return (const char*) (dw->strgBuffer + (offset - dw->strgBufferBase));
}

// Reads a pointer list header: count + absolute-offset pointers.
// Caller must free the returned array.
static uint32_t* readPointerTable(BinaryReader* reader, uint32_t* outCount) {
    *outCount = BinaryReader_readUint32(reader);
    if (*outCount == 0) return nullptr;
    uint32_t* ptrs = (uint32_t *)safeMalloc(*outCount * sizeof(uint32_t));
    repeat(*outCount, i) {
        ptrs[i] = BinaryReader_readUint32(reader);
    }
    return ptrs;
}

// Reads a PointerList of EventAction entries. Used by TMLN and OBJT.
static EventAction* readEventActions(BinaryReader* reader, DataWin* dw, uint32_t* outCount) {
    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    *outCount = count;
    if (count == 0) { free(ptrs); return nullptr; }

    EventAction* actions = (EventAction *)safeMalloc(count * sizeof(EventAction));
    repeat(count, i) {
        BinaryReader_seek(reader, ptrs[i]);
        actions[i].libID = BinaryReader_readUint32(reader);
        actions[i].id = BinaryReader_readUint32(reader);
        actions[i].kind = BinaryReader_readUint32(reader);
        actions[i].useRelative = BinaryReader_readBool32(reader);
        actions[i].isQuestion = BinaryReader_readBool32(reader);
        actions[i].useApplyTo = BinaryReader_readBool32(reader);
        actions[i].exeType = BinaryReader_readUint32(reader);
        actions[i].actionName = readStringPtr(reader, dw);
        actions[i].codeId = BinaryReader_readInt32(reader);
        actions[i].argumentCount = BinaryReader_readUint32(reader);
        actions[i].who = BinaryReader_readInt32(reader);
        actions[i].relative = BinaryReader_readBool32(reader);
        actions[i].isNot = BinaryReader_readBool32(reader);
        actions[i].unknownAlwaysZero = BinaryReader_readUint32(reader);
    }
    free(ptrs);
    return actions;
}

// ===[ PATH INTERNAL COMPUTATION ]===
// Matches HTML5 yyPath.js algorithm exactly.

// Dynamic array of InternalPathPoints for building during computation
static InternalPathPoint* tempIntPoints = nullptr;
static uint32_t tempIntPointCount = 0;

static void addInternalPoint(float x, float y, float speed) {
    InternalPathPoint pt = {0};
    pt.x = x;
    pt.y = y;
    pt.speed = speed;
    arrput(tempIntPoints, pt);
    tempIntPointCount++;
}

// Recursive midpoint subdivision for smooth curves (yyPath.js:225-242)
static void handlePiece(int depth, float x1, float y1, float s1, float x2, float y2, float s2, float x3, float y3, float s3) {
    if (depth == 0) return;

    float mx = (x1 + x2 + x2 + x3) / 4.0f;
    float my = (y1 + y2 + y2 + y3) / 4.0f;
    float ms = (s1 + s2 + s2 + s3) / 4.0f;

    if ((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1) > 16.0f) {
        handlePiece(depth - 1, x1, y1, s1, (x2 + x1) / 2.0f, (y2 + y1) / 2.0f, (s2 + s1) / 2.0f, mx, my, ms);
    }

    addInternalPoint(mx, my, ms);

    if ((x2 - x3) * (x2 - x3) + (y2 - y3) * (y2 - y3) > 16.0f) {
        handlePiece(depth - 1, mx, my, ms, (x3 + x2) / 2.0f, (y3 + y2) / 2.0f, (s3 + s2) / 2.0f, x3, y3, s3);
    }
}

void GamePath_computeInternal(GamePath* path) {
    // Reset temp state
    arrfree(tempIntPoints);
    tempIntPoints = nullptr;
    tempIntPointCount = 0;

    free(path->internalPoints);
    path->internalPoints = nullptr;
    path->internalPointCount = 0;
    path->length = 0.0;

    if (path->pointCount == 0)
        return;

    if (path->isSmooth) {
        // ComputeCurved (yyPath.js:254-292)
        if (!path->isClosed) {
            addInternalPoint(path->points[0].x, path->points[0].y, path->points[0].speed);
        }

        int n;
        if (path->isClosed) {
            n = (int) path->pointCount - 1;
        } else {
            n = (int) path->pointCount - 3;
        }

        repeat(n + 1, i) {
            PathPoint* p1 = &path->points[i % path->pointCount];
            PathPoint* p2 = &path->points[(i + 1) % path->pointCount];
            PathPoint* p3 = &path->points[(i + 2) % path->pointCount];
            handlePiece((int) path->precision,
                        (p1->x + p2->x) / 2.0f, (p1->y + p2->y) / 2.0f, (p1->speed + p2->speed) / 2.0f,
                        p2->x, p2->y, p2->speed,
                        (p2->x + p3->x) / 2.0f, (p2->y + p3->y) / 2.0f, (p2->speed + p3->speed) / 2.0f);
        }

        if (!path->isClosed) {
            PathPoint* last = &path->points[path->pointCount - 1];
            addInternalPoint(last->x, last->y, last->speed);
        } else {
            // Closed smooth: append the first internal point again
            addInternalPoint(tempIntPoints[0].x, tempIntPoints[0].y, tempIntPoints[0].speed);
        }
    } else {
        // ComputeLinear (yyPath.js:192-204)
        repeat(path->pointCount, i) {
            addInternalPoint(path->points[i].x, path->points[i].y, path->points[i].speed);
        }
        if (path->isClosed) {
            addInternalPoint(path->points[0].x, path->points[0].y, path->points[0].speed);
        }
    }

    // ComputeLength (yyPath.js:150-160)
    path->internalPointCount = tempIntPointCount;
    path->internalPoints = (InternalPathPoint *)safeMalloc(tempIntPointCount * sizeof(InternalPathPoint));
    memcpy(path->internalPoints, tempIntPoints, tempIntPointCount * sizeof(InternalPathPoint));
    arrfree(tempIntPoints);
    tempIntPoints = nullptr;
    tempIntPointCount = 0;

    path->length = 0.0;
    if (path->internalPointCount > 0) {
        path->internalPoints[0].l = 0.0;
        repeat(path->internalPointCount - 1, j) {
            uint32_t i = j + 1;
            float dx = path->internalPoints[i].x - path->internalPoints[i - 1].x;
            float dy = path->internalPoints[i].y - path->internalPoints[i - 1].y;
            path->length += sqrtf(dx * dx + dy * dy);
            path->internalPoints[i].l = path->length;
        }
    }
}

// Get interpolated position at t in [0,1] (yyPath.js:362-409)
PathPositionResult GamePath_getPosition(GamePath* path, float t) {
    PathPositionResult result = {0};
    result.speed = 100.0f;

    if (path->internalPointCount == 0) return result;

    if (path->internalPointCount == 1 || path->length == 0.0f || 0.0f >= t) {
        result.x = path->internalPoints[0].x;
        result.y = path->internalPoints[0].y;
        result.speed = path->internalPoints[0].speed;
        return result;
    }

    if (t >= 1.0f) {
        InternalPathPoint* last = &path->internalPoints[path->internalPointCount - 1];
        result.x = last->x;
        result.y = last->y;
        result.speed = last->speed;
        return result;
    }

    // Get the right interval via linear scan
    float l = path->length * t;
    uint32_t pos = 0;
    while (path->internalPointCount - 2 > pos && l >= path->internalPoints[pos + 1].l) {
        pos++;
    }

    InternalPathPoint* node = &path->internalPoints[pos];
    float lRem = l - node->l;
    float w = path->internalPoints[pos + 1].l - node->l;

    if (w != 0.0f) {
        InternalPathPoint* next = &path->internalPoints[pos + 1];
        result.x = node->x + lRem * (next->x - node->x) / w;
        result.y = node->y + lRem * (next->y - node->y) / w;
        result.speed = node->speed + lRem * (next->speed - node->speed) / w;
    } else {
        result.x = node->x;
        result.y = node->y;
        result.speed = node->speed;
    }

    return result;
}

// ===[ CHUNK PARSERS ]===

static void parseGEN8(BinaryReader* reader, DataWin* dw) {
    Gen8* g = &dw->gen8;
    g->isDebuggerDisabled = BinaryReader_readUint8(reader);
    g->wadVersion = BinaryReader_readUint8(reader);
    BinaryReader_skip(reader, 2); // padding

    // WAD8 has TWO known GEN8 layouts that share the same version:
    // Around GMS 1.0.198: 84 bytes
    // Around GMS 1.0.469: Same shape as WAD10's GEN8 except that the displayName is still absent
    bool isCompactWad8 = 8 >= g->wadVersion && 108 > reader->bufferSize;
    if (isCompactWad8) {
        g->fileName = readStringPtr(reader, dw);
        g->config = nullptr;
        g->lastObj = BinaryReader_readUint32(reader);
        g->lastTile = BinaryReader_readUint32(reader);
        g->gameID = BinaryReader_readUint32(reader);
        BinaryReader_readBytes(reader, g->directPlayGuid, 16);
        g->name = nullptr;
        g->major = 1;
        g->minor = 0;
        g->release = 0;
        g->build = 198;
        g->defaultWindowWidth = BinaryReader_readUint32(reader);
        g->defaultWindowHeight = BinaryReader_readUint32(reader);
        g->info = BinaryReader_readUint32(reader);
        g->licenseCRC32 = BinaryReader_readUint32(reader);
        BinaryReader_readBytes(reader, g->licenseMD5, 16);
        g->timestamp = (uint64_t) BinaryReader_readUint32(reader); // WAD8 stores a signed int32 timestamp (FILETIME-derived), sign-extended at use sites
        BinaryReader_skip(reader, 4); // unread 4-byte gap at offset 72
        g->displayName = nullptr;
        g->activeTargets = 0;
        g->functionClassifications = 0;
        g->steamAppID = 0;
        g->debuggerPort = 0;
        g->roomOrderCount = BinaryReader_readUint32(reader);
        if (g->roomOrderCount > 0) {
            g->roomOrder = (int32_t *)safeMalloc(g->roomOrderCount * sizeof(int32_t));
            repeat(g->roomOrderCount, i) {
                g->roomOrder[i] = BinaryReader_readInt32(reader);
            }
        } else {
            g->roomOrder = nullptr;
        }
        DataWin_bumpVersionTo(dw, g->major, g->minor, g->release, g->build);
        return;
    }

    g->fileName = readStringPtr(reader, dw);
    g->config = readStringPtr(reader, dw);
    g->lastObj = BinaryReader_readUint32(reader);
    g->lastTile = BinaryReader_readUint32(reader);
    g->gameID = BinaryReader_readUint32(reader);
    BinaryReader_readBytes(reader, g->directPlayGuid, 16);
    g->name = readStringPtr(reader, dw);
    g->major = BinaryReader_readUint32(reader);
    g->minor = BinaryReader_readUint32(reader);
    g->release = BinaryReader_readUint32(reader);
    g->build = BinaryReader_readUint32(reader);
    g->defaultWindowWidth = BinaryReader_readUint32(reader);
    g->defaultWindowHeight = BinaryReader_readUint32(reader);
    g->info = BinaryReader_readUint32(reader);
    g->licenseCRC32 = BinaryReader_readUint32(reader);
    BinaryReader_readBytes(reader, g->licenseMD5, 16);
    if (12 >= g->wadVersion) {
        int32_t ts = BinaryReader_readInt32(reader); // int32 timestamp (FILETIME-derived)
        g->timestamp = (uint64_t) (int64_t) ts;
        BinaryReader_skip(reader, 4); // unread padding at body+0x60
        // If it is expanded but WAD8 or WAD9, then we won't have the displayName here
        if (9 >= g->wadVersion) {
            g->displayName = nullptr;
        } else {
            g->displayName = readStringPtr(reader, dw);
        }
        g->activeTargets = (g->wadVersion >= 11) ? BinaryReader_readUint64(reader) : 0;
        g->functionClassifications = (g->wadVersion >= 12) ? BinaryReader_readUint64(reader) : 0;
        g->roomOrderCount = BinaryReader_readUint32(reader);
        if (g->roomOrderCount > 0) {
            g->roomOrder = (int32_t *)safeMalloc(g->roomOrderCount * sizeof(int32_t));
            repeat(g->roomOrderCount, i) {
                g->roomOrder[i] = BinaryReader_readInt32(reader);
            }
        } else {
            g->roomOrder = nullptr;
        }
        g->steamAppID = 0;
        g->debuggerPort = 0;
        DataWin_bumpVersionTo(dw, g->major, g->minor, g->release, g->build);
        return;
    }

    g->timestamp = BinaryReader_readUint64(reader);
    g->displayName = readStringPtr(reader, dw);
    g->activeTargets = BinaryReader_readUint64(reader);
    g->functionClassifications = BinaryReader_readUint64(reader);
    g->steamAppID = BinaryReader_readInt32(reader);
    if (g->wadVersion >= 14) {
        g->debuggerPort = BinaryReader_readUint32(reader);
    }

    // Room order SimpleList
    g->roomOrderCount = BinaryReader_readUint32(reader);
    if (g->roomOrderCount > 0) {
        g->roomOrder = (int32_t *)safeMalloc(g->roomOrderCount * sizeof(int32_t));
        repeat(g->roomOrderCount, i) {
            g->roomOrder[i] = BinaryReader_readInt32(reader);
        }
    } else {
        g->roomOrder = nullptr;
    }

    if (g->major >= 2) {
        BinaryReader_skip(reader, 8); // firstRandom (int64)
        BinaryReader_skip(reader, 8*4); // 4 Random Entries (one int64 or two int32)

        g->gms2FPS = BinaryReader_readFloat32(reader);
        BinaryReader_skip(reader, 4); // AllowStatistics (bool32)
        BinaryReader_skip(reader, 16); // GameGUID (16 Bytes, unknown it's use)
    }

    // Seed the detected version from GEN8.
    // Later chunk parsers may bump these upward when they identify newer-format features, because since GM:S 2 the value in the GEN8 chunk is not accurate.
    DataWin_bumpVersionTo(dw, g->major, g->minor, g->release, g->build);
}

static void parseOPTN(BinaryReader* reader, DataWin* dw) {
    Optn* o = &dw->optn;

    int32_t shaderExtensionFlag = BinaryReader_readInt32(reader);
    bool newFormat = shaderExtensionFlag == (int32_t) 0x80000000;

    if (newFormat) {
        MAYBE_UNUSED int32_t shaderExtVersion = BinaryReader_readInt32(reader);

        o->info = BinaryReader_readUint64(reader);
        o->scale = BinaryReader_readInt32(reader);
        o->windowColor = BinaryReader_readUint32(reader);
        o->colorDepth = BinaryReader_readUint32(reader);
        o->resolution = BinaryReader_readUint32(reader);
        o->frequency = BinaryReader_readUint32(reader);
        o->vertexSync = BinaryReader_readUint32(reader);
        o->priority = BinaryReader_readUint32(reader);
        o->backImage = BinaryReader_readUint32(reader);
        o->frontImage = BinaryReader_readUint32(reader);
        o->loadImage = BinaryReader_readUint32(reader);
        o->loadAlpha = BinaryReader_readUint32(reader);
    } else {
        BinaryReader_seek(reader, BinaryReader_getPosition(reader) - 4);
        // Remap the boolean list into "modern" bitflags
        o->info = 0;
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x1; // FullScreen
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x2; // InterpolatePixels
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x4; // UseNewAudio
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x8; // NoBorder
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x10; // ShowCursor
        o->scale = BinaryReader_readInt32(reader);
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x20; // Sizeable
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x40; // StayOnTop
        o->windowColor = BinaryReader_readUint32(reader);
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x80; // ChangeResolution
        o->colorDepth = BinaryReader_readUint32(reader);
        o->resolution = BinaryReader_readUint32(reader);
        o->frequency = BinaryReader_readUint32(reader);
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x100; // NoButtons
        o->vertexSync = BinaryReader_readUint32(reader);
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x200; // ScreenKey
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x400; // HelpKey
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x800; // QuitKey
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x1000; // SaveKey
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x2000; // ScreenShotKey
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x4000; // CloseSec
        o->priority = BinaryReader_readUint32(reader);
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x8000; // Freeze
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x10000; // ShowProgress
        o->backImage = BinaryReader_readUint32(reader);
        o->frontImage = BinaryReader_readUint32(reader);
        o->loadImage = BinaryReader_readUint32(reader);
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x20000; // LoadTransparent
        o->loadAlpha = BinaryReader_readUint32(reader);
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x40000; // ScaleProgress
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x80000; // DisplayErrors
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x100000; // WriteErrors
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x200000; // AbortErrors
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x400000; // VariableErrors
        if (BinaryReader_readBool32(reader)) o->info |= (uint64_t) 0x800000; // CreationEventOrder
    }

    // Constants SimpleList (absent on WAD8)
    if (8 >= dw->gen8.wadVersion) {
        o->constantCount = 0;
        o->constants = nullptr;
        return;
    }
    o->constantCount = BinaryReader_readUint32(reader);
    if (o->constantCount > 0) {
        o->constants = (OptnConstant *)safeMalloc(o->constantCount * sizeof(OptnConstant));
        repeat(o->constantCount, i) {
            o->constants[i].name = readStringPtr(reader, dw);
            o->constants[i].value = readStringPtr(reader, dw);
        }
    } else {
        o->constants = nullptr;
    }
}

static void parseLANG(BinaryReader* reader, DataWin* dw) {
    Lang* l = &dw->lang;
    l->unknown1 = BinaryReader_readUint32(reader);
    l->languageCount = BinaryReader_readUint32(reader);
    l->entryCount = BinaryReader_readUint32(reader);

    // Entry IDs
    if (l->entryCount > 0) {
        l->entryIds = (const char **)safeMalloc(l->entryCount * sizeof(const char*));
        repeat(l->entryCount, i) {
            l->entryIds[i] = readStringPtr(reader, dw);
        }
    } else {
        l->entryIds = nullptr;
    }

    // Languages
    if (l->languageCount > 0) {
        l->languages = (Language *)safeMalloc(l->languageCount * sizeof(Language));
        repeat(l->languageCount, i) {
            l->languages[i].name = readStringPtr(reader, dw);
            l->languages[i].region = readStringPtr(reader, dw);
            l->languages[i].entryCount = l->entryCount;
            if (l->entryCount > 0) {
                l->languages[i].entries = (const char **)safeMalloc(l->entryCount * sizeof(const char*));
                repeat(l->entryCount, j) {
                    l->languages[i].entries[j] = readStringPtr(reader, dw);
                }
            } else {
                l->languages[i].entries = nullptr;
            }
        }
    } else {
        l->languages = nullptr;
    }
}

// Reads a uint32 at an absolute chunk offset (restoring the read cursor afterwards), or returns 0 if the offset would read past the chunk.
static uint32_t peekUint32At(BinaryReader* reader, size_t absOffset, size_t chunkEnd) {
    if (absOffset + 4 > chunkEnd) return 0;
    size_t saved = BinaryReader_getPosition(reader);
    BinaryReader_seek(reader, absOffset);
    uint32_t value = BinaryReader_readUint32(reader);
    BinaryReader_seek(reader, saved);
    return value;
}

static void parseEXTN(BinaryReader* reader, DataWin* dw) {
    Extn* e = &dw->extn;
    size_t chunkEnd = reader->bufferBase + reader->bufferSize;

    uint32_t extCount;
    uint32_t* extPtrs = readPointerTable(reader, &extCount);
    e->count = extCount;

    if (extCount == 0) { free(extPtrs); e->extensions = nullptr; return; }

    int32_t extStringCount = 0;
    if (dw->gen8.wadVersion >= 17) {
        uint32_t firstExt = extPtrs[0];
        // 2022.6: [folder][name][className][filesPtr][optionsPtr][files list...]; filesPtr == firstExt + 3*4 + 2*4
        if (peekUint32At(reader, firstExt + 12, chunkEnd) == firstExt + 20) {
            extStringCount = 3;

        // 2023.4+: an extra Version string sits between name and className, shifting everything by 4 bytes
        // We also verify that firstExt + 12 is >= 0x1000 to avoid a false positive with old extensions that have exactly 2 files (where firstExt + 12 is fileCount = 2).
        } else if (peekUint32At(reader, firstExt + 16, chunkEnd) == firstExt + 24 && peekUint32At(reader, firstExt + 12, chunkEnd) >= 0x1000) {
            extStringCount = 4;
        }
    }

    e->extensions = (Extension *)safeMalloc(extCount * sizeof(Extension));
    repeat(extCount, i) {
        BinaryReader_seek(reader, extPtrs[i]);
        Extension* ext = &e->extensions[i];
        ext->folderName = readStringPtr(reader, dw);
        ext->name = readStringPtr(reader, dw);
        // GM 2023.4+ inserts a Version string here.
        if (extStringCount >= 4) BinaryReader_readUint32(reader);
        ext->className = readStringPtr(reader, dw);

        // In the new format (GM 2022.6+) the header now holds a Files pointer and an Options pointer.
        // Seek to the Files pointer to reach the actual list.
        // In the old format the Files PointerList is inline right here.
        if (extStringCount > 0) {
            uint32_t filesPtr = BinaryReader_readUint32(reader);
            BinaryReader_readUint32(reader); // optionsPtr (Extension options are not used by the runner)
            BinaryReader_seek(reader, filesPtr);
        }

        // Files PointerList
        uint32_t fileCount;
        uint32_t* filePtrs = readPointerTable(reader, &fileCount);
        ext->fileCount = fileCount;

        if (fileCount > 0) {
            ext->files = (ExtensionFile *)safeMalloc(fileCount * sizeof(ExtensionFile));
            repeat(fileCount, j) {
                BinaryReader_seek(reader, filePtrs[j]);
                ExtensionFile* file = &ext->files[j];
                file->filename = readStringPtr(reader, dw);
                file->cleanupScript = readStringPtr(reader, dw);
                file->initScript = readStringPtr(reader, dw);
                file->kind = BinaryReader_readUint32(reader);

                // Functions PointerList
                uint32_t funcCount;
                uint32_t* funcPtrs = readPointerTable(reader, &funcCount);
                file->functionCount = funcCount;

                if (funcCount > 0) {
                    file->functions = (ExtensionFunction *)safeMalloc(funcCount * sizeof(ExtensionFunction));
                    repeat(funcCount, k) {
                        BinaryReader_seek(reader, funcPtrs[k]);
                        ExtensionFunction* func = &file->functions[k];
                        func->name = readStringPtr(reader, dw);
                        func->id = BinaryReader_readUint32(reader);
                        func->kind = BinaryReader_readUint32(reader);
                        func->retType = BinaryReader_readUint32(reader);
                        func->extName = readStringPtr(reader, dw);

                        // Arguments SimpleList
                        func->argumentCount = BinaryReader_readUint32(reader);
                        if (func->argumentCount > 0) {
                            func->arguments = (uint32_t *)safeMalloc(func->argumentCount * sizeof(uint32_t));
                            repeat(func->argumentCount, a) {
                                func->arguments[a] = BinaryReader_readUint32(reader);
                            }
                        } else {
                            func->arguments = nullptr;
                        }
                    }
                } else {
                    file->functions = nullptr;
                }
                free(funcPtrs);
            }
        } else {
            ext->files = nullptr;
        }
        free(filePtrs);
    }
    free(extPtrs);

    // TODO: Product ID data (16 bytes per extension, wadVersion >= 14)
}

static void parseSOND(BinaryReader* reader, DataWin* dw) {
    Sond* s = &dw->sond;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->sounds = nullptr; return; }

    if (DataWin_isVersionAtLeast(dw, 2023, 2, 0, 0) && !DataWin_isVersionAtLeast(dw, 2024, 6, 0, 0)) {
        uint32_t soundPtrs[2];
        uint32_t soundCount = 0;
        repeat(count, i) {
            if (ptrs[i] == 0)
                continue;
            soundPtrs[soundCount++] = ptrs[i];
            if (soundCount >= 2)
                break;
        }

        if (soundCount >= 2) {
            if (soundPtrs[0] + (4 * 9) == soundPtrs[1] - 4) {
                DataWin_bumpVersionTo(dw, 2024, 6, 0, 0);
            }
        } else if (soundCount == 1) {
            size_t savedPos = BinaryReader_getPosition(reader);
            size_t probe = (size_t) (soundPtrs[0] + (4 * 9));
            requireMessageFormatted(__FILE__, __LINE__, (probe % 16) != 4, "parseSOND: unexpected SOND alignment at 0x%zx");
            BinaryReader_seek(reader, probe);
            if (BinaryReader_readUint32(reader) != 0) {
                DataWin_bumpVersionTo(dw, 2024, 6, 0, 0);
            }
            BinaryReader_seek(reader, savedPos);
        }
    }

    s->sounds = (Sound *)safeCalloc(count, sizeof(Sound));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        Sound* snd = &s->sounds[i];
        snd->present = true;
        snd->name = readStringPtr(reader, dw);
        snd->flags = BinaryReader_readUint32(reader);
        snd->type = readStringPtr(reader, dw);
        snd->file = readStringPtr(reader, dw);
        snd->effects = BinaryReader_readUint32(reader);
        snd->volume = BinaryReader_readFloat32(reader);
        if (12 >= dw->gen8.wadVersion) {
            // Pre-WAD13 games store pan instead of pitch, and stores the embedded flag as a separate boolean.
            snd->pan = BinaryReader_readFloat32(reader);

            bool embedded = BinaryReader_readBool32(reader);
            if (embedded)
                snd->flags |= AUDIO_ENTRY_FLAG_IS_EMBEDDED;

            snd->pitch = 1.0f;
            snd->audioGroup = 0;
            snd->audioFile = BinaryReader_readInt32(reader);
            continue;
        }
        snd->pan = 0.0f;
        snd->pitch = BinaryReader_readFloat32(reader);

        // AudioGroup or preload field at offset +28
        // For GMS 1.4.x (wadVersion >= 14) with Regular flag: resource_id
        if ((snd->flags & AUDIO_ENTRY_FLAG_REGULAR) == AUDIO_ENTRY_FLAG_REGULAR && dw->gen8.wadVersion >= 14) {
            snd->audioGroup = BinaryReader_readInt32(reader);
        } else {
            int32_t preload = BinaryReader_readInt32(reader);
            (void)preload;
            snd->audioGroup = 0; // default audio group
        }

        snd->audioFile = BinaryReader_readInt32(reader);
    }
    free(ptrs);
}

static void parseAGRP(BinaryReader* reader, DataWin* dw) {
    Agrp* a = &dw->agrp;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    a->count = count;

    if (count == 0) { free(ptrs); a->audioGroups = nullptr; return; }

    // GM 2024.14+ added a "path" parameter for each AudioGroup
    // To detect it, we'll check if the difference between two pointers is 8 (two int32)
    // We CAN'T figure out if there aren't at least two AudioGroups, but for any meaningful purposes any game that has external AudioGroups WILL have
    // at least two entries, one for the default AudioGroup and another for the external AudioGroup
    if (DataWin_isVersionAtLeast(dw, 2024, 13, 0, 0)) {
        if (count >= 2) {
            uint32_t diff = ptrs[1] - ptrs[0];

            if (diff >= 8) {
                DataWin_bumpVersionTo(dw, 2024, 14, 0, 0);
            }
        } else if (count == 1) {
            // If there's only one entry, we CAN'T figure out easily based on the pointer diffs
            // But here's the trick: We can read it twice, if the path is null for the FIRST audiogroup, then it is NOT 2024.14
            if (ptrs[0] == 0) {
                // Somehow in a empty GameMaker 2026.0.0.23 game the pointer can be 0 even though it has one audio group...?
                // If that's the case, we'll just bail out
                free(ptrs);
                a->audioGroups = nullptr;
                a->count = 0;
                return;
            }

            BinaryReader_seek(reader, ptrs[0]);
            const char* name = readStringPtr(reader, dw);
            const char* path = readStringPtr(reader, dw);

            if (strcmp(name, "audiogroup_default") == 0 && path != nullptr) {
                DataWin_bumpVersionTo(dw, 2024, 14, 0, 0);
            }
        }
    }

    a->audioGroups = (AudioGroup *)safeCalloc(count, sizeof(AudioGroup));

    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        a->audioGroups[i].present = true;
        a->audioGroups[i].name = readStringPtr(reader, dw);
        if (DataWin_isVersionAtLeast(dw, 2024, 14, 0, 0)) {
            a->audioGroups[i].path = readStringPtr(reader, dw);
        }
    }
    free(ptrs);
}

static void parseSPRT(BinaryReader* reader, DataWin* dw, bool skipLoadingPreciseMasksForNonPreciseSprites) {
    Sprt* s = &dw->sprt;
    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;
    s->parsedCount = count;

    if (count == 0) { free(ptrs); s->sprites = nullptr; return; }

    s->sprites = (Sprite *)safeCalloc(count, sizeof(Sprite));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        Sprite* spr = &s->sprites[i];
        spr->present = true;
        spr->name = readStringPtr(reader, dw);
        spr->width = BinaryReader_readUint32(reader);
        spr->height = BinaryReader_readUint32(reader);
        spr->marginLeft = BinaryReader_readInt32(reader);
        spr->marginRight = BinaryReader_readInt32(reader);
        spr->marginBottom = BinaryReader_readInt32(reader);
        spr->marginTop = BinaryReader_readInt32(reader);
        spr->transparent = BinaryReader_readBool32(reader);
        spr->smooth = BinaryReader_readBool32(reader);
        spr->preload = BinaryReader_readBool32(reader);
        spr->bboxMode = BinaryReader_readUint32(reader);
        spr->sepMasks = BinaryReader_readUint32(reader);
        spr->originX = BinaryReader_readInt32(reader);
        spr->originY = BinaryReader_readInt32(reader);

        // Detect special type vs normal: peek next int32
        int32_t check = BinaryReader_readInt32(reader);
        uint32_t nineSliceOffset = 0;
        if (check == -1) {
            spr->specialType = true;
            spr->sVersion = BinaryReader_readUint32(reader);
            spr->sSpriteType = BinaryReader_readUint32(reader);
            if (spr->sSpriteType == 0) {
                // Normal "special" sprite, technically only used for GameMaker: Studio 2+, but some modding tools (like UndertaleModTool) may inject special sprite types,
                // even though the data.win is NOT GM:S 2+
                if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0)) {
                    spr->gms2PlaybackSpeed = BinaryReader_readFloat32(reader);
                    spr->gms2PlaybackSpeedType = BinaryReader_readUint32(reader);
                    if (spr->sVersion >= 2) {
                        BinaryReader_skip(reader, 4); //sequenceOffset;
                        if (spr->sVersion >= 3) {
                            nineSliceOffset = BinaryReader_readUint32(reader);
                        }
                    } check = BinaryReader_readUint32(reader);
                } else {
                    // Technically should NEVER happen on legit data.wins
                    check = 0;
                }
            } else {
                logWarn("DataWin: Detected special sprite type %u (%s), but we don't support it yet!\n", spr->sSpriteType, spr->sSpriteType == 2 ? "Spine" : spr->sSpriteType == 1 ? "SWF" : "Unknown");
                spr->textureCount = 0;
                spr->tpagIndices = nullptr;
                spr->maskCount = 0;
                spr->masks = nullptr;
                continue;
            }
        }

        // 'check' is the texture count (start of SimpleList)
        spr->textureCount = (uint32_t)check;
        if (spr->textureCount > 0) {
            // Temporarily store the absolute file offsets here; parseTPAG resolves them in-place to TPAG indices once the TPAG table is known.
            spr->tpagIndices = (int32_t *)safeMalloc(spr->textureCount * sizeof(int32_t));
            repeat(spr->textureCount, j) {
                spr->tpagIndices[j] = (int32_t) BinaryReader_readUint32(reader);
            }
        } else {
            spr->tpagIndices = nullptr;
        }

        // Collision mask data
        // sepMasks: 0 = axis-aligned rect (no mask data stored in some cases)
        //           1 = precise per-frame masks
        //           2 = rotated rect (no mask data)
        // Mask format: each bit = 1 pixel, MSB first, row-major
        // Width in bytes = (spriteWidth + 7) / 8, total = widthInBytes * spriteHeight
        // After all masks, data is padded to 4-byte alignment
        // Zero-dimension sprites (placeholder/empty assets in test files) omit the mask block entirely
        // GMS 2024.6+ stores collision masks at bounding-box dimensions (marginRight-marginLeft+1 by marginBottom-marginTop+1) instead of the full sprite size.
        // Pre-2024.6 they cover the full sprite.
        if (DataWin_isVersionAtLeast(dw, 2024, 6, 0, 0)) {
            spr->maskWidth = (uint32_t) (spr->marginRight - spr->marginLeft + 1);
            spr->maskHeight = (uint32_t) (spr->marginBottom - spr->marginTop + 1);
            spr->maskOffsetX = spr->marginLeft;
            spr->maskOffsetY = spr->marginTop;
        } else {
            spr->maskWidth = spr->width;
            spr->maskHeight = spr->height;
            spr->maskOffsetX = 0;
            spr->maskOffsetY = 0;
        }

        if (spr->width == 0 || spr->height == 0) {
            spr->maskCount = 0;
            spr->masks = nullptr;
            continue;
        }
        uint32_t maskDataCount = BinaryReader_readUint32(reader);
        spr->maskCount = maskDataCount;
        if (maskDataCount > 0 && spr->maskWidth > 0 && spr->maskHeight > 0) {
            uint32_t bytesPerRow = (spr->maskWidth + 7) / 8;
            uint32_t bytesPerMask = bytesPerRow * spr->maskHeight;

            if (spr->sepMasks == 1 || !skipLoadingPreciseMasksForNonPreciseSprites) {
                spr->masks = (uint8_t **)safeMalloc(maskDataCount * sizeof(uint8_t*));
                if (dw->mappedFile) {
                    repeat(maskDataCount, j) {
                        spr->masks[j] = dw->mappedFile + BinaryReader_getPosition(reader);
                    }
                } else {
                    repeat(maskDataCount, j) {
                        spr->masks[j] = (uint8_t *)safeMalloc(bytesPerMask);
                        BinaryReader_readBytes(reader, spr->masks[j], bytesPerMask);
                    }
                }
            } else {
                BinaryReader_skip(reader, bytesPerMask * maskDataCount);
                spr->masks = nullptr;
            }
            // Pad the TOTAL mask data to 4-byte alignment (not per-mask)
            uint32_t totalMaskBytes = bytesPerMask * maskDataCount;
            uint32_t remainder = totalMaskBytes % 4;
            if (remainder != 0) {
                BinaryReader_skip(reader, 4 - remainder);
            }
        } else {
            spr->masks = nullptr;
        }

        // Nine-slice block (40 bytes). Located at nineSliceOffset (absolute file offset) elsewhere in the chunk.
        if (nineSliceOffset != 0) {
            size_t savedPos = BinaryReader_getPosition(reader);
            BinaryReader_seek(reader, (size_t) nineSliceOffset);
            spr->nsLeft = BinaryReader_readInt32(reader);
            spr->nsTop = BinaryReader_readInt32(reader);
            spr->nsRight = BinaryReader_readInt32(reader);
            spr->nsBottom = BinaryReader_readInt32(reader);
            spr->nineSliceEnabled = BinaryReader_readBool32(reader);
            repeat(5, j) {
                int32_t mode = BinaryReader_readInt32(reader);
                spr->nsTileModes[j] = (uint8_t) mode;
            }
            BinaryReader_seek(reader, savedPos);
        }
    }

    free(ptrs);
}

static void parseBGND(BinaryReader* reader, DataWin* dw, uint32_t chunkEnd) {
    Bgnd* b = &dw->bgnd;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    b->count = count;

    if (count == 0) { free(ptrs); b->backgrounds = nullptr; return; }

    // GM 2024.14.1 added tile separation parameters for each background
    // To detect it, we'll check if the background's end position is at the chunks end position (if there's only one background) or the start of the next background
    // If it isn't at either of those, then that means it is 2024.14.1+
    if (DataWin_isVersionAtLeast(dw, 2024, 13, 0, 0) && !DataWin_isVersionAtLeast(dw, 2024, 14, 1, 0)) {
        repeat(count, i) {
            if (ptrs[i] == 0) continue;

            // Skip to where the item per tile count + tile count should be in pre-2024.14.1 versions
            BinaryReader_seek(reader, ptrs[i] + (11 * 4));
            uint32_t itemsPerTileCount = BinaryReader_readUint32(reader);
            uint32_t tileCount = BinaryReader_readUint32(reader);

            // Get what might be the end position to compare it with the actual end position
            size_t tpos = ptrs[i] + (16 * 4) + (itemsPerTileCount * tileCount * 4);
            if (count >= 2 && i < count - 1) {
                // Next thing at end position is a background

                // Align to 8 bytes
                if ((tpos % 8) != 0) tpos += 8 - (tpos % 8);

                if (tpos != ptrs[i + 1]) {
                    DataWin_bumpVersionTo(dw, 2024, 14, 1, 0);
                    break;
                }
            }
            else {
                // Next thing at end position is the end of the chunk

                // Align to 16 bytes
                if ((tpos % 16) != 0) tpos += 16 - (tpos % 16);

                if (tpos != chunkEnd) {
                    DataWin_bumpVersionTo(dw, 2024, 14, 1, 0);
                    break;
                }
            }
        }
    }

    b->backgrounds = (Background *)safeCalloc(count, sizeof(Background));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        Background* bg = &b->backgrounds[i];
        bg->present = true;
        bg->name = readStringPtr(reader, dw);
        bg->transparent = BinaryReader_readBool32(reader);
        bg->smooth = BinaryReader_readBool32(reader);
        bg->preload = BinaryReader_readBool32(reader);
        // Temporarily store the absolute file offset; parseTPAG resolves it in-place to a TPAG index once the TPAG table is known.
        bg->tpagIndex = (int32_t) BinaryReader_readUint32(reader);
        if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0)) {
            bg->gms2UnknownAlways2 = BinaryReader_readUint32(reader);
            bg->gms2TileWidth = BinaryReader_readUint32(reader);
            bg->gms2TileHeight = BinaryReader_readUint32(reader);
            if (DataWin_isVersionAtLeast(dw, 2024, 14, 1, 0)) {
                bg->gms2TileSeparationX = BinaryReader_readUint32(reader);
                bg->gms2TileSeparationY = BinaryReader_readUint32(reader);
            }
            bg->gms2OutputBorderX = BinaryReader_readUint32(reader);
            bg->gms2OutputBorderY = BinaryReader_readUint32(reader);
            bg->gms2TileColumns = BinaryReader_readUint32(reader);
            bg->gms2ItemsPerTileCount = BinaryReader_readUint32(reader);
            bg->gms2TileCount = BinaryReader_readUint32(reader);
            bg->gms2ExportedSpriteIndex = BinaryReader_readInt32(reader);
            bg->gms2FrameLength = BinaryReader_readInt64(reader);
            int tileIdCount = bg->gms2TileCount * bg->gms2ItemsPerTileCount;
            bg->gms2TileIds = (uint32_t *)safeMalloc(tileIdCount*sizeof(uint32_t));
            repeat(tileIdCount, j) {
                bg->gms2TileIds[j] = BinaryReader_readUint32(reader);
            }
        }
    }
    free(ptrs);
}

static void parsePATH(BinaryReader* reader, DataWin* dw) {
    PathChunk* p = &dw->path;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    p->count = count;

    if (count == 0) { free(ptrs); p->paths = nullptr; return; }

    p->paths = (GamePath *)safeCalloc(count, sizeof(GamePath));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        GamePath* path = &p->paths[i];
        path->present = true;
        path->internalPoints = nullptr;
        path->internalPointCount = 0;
        path->length = 0.0;
        path->name = readStringPtr(reader, dw);
        path->isSmooth = BinaryReader_readBool32(reader);
        path->isClosed = BinaryReader_readBool32(reader);
        path->precision = BinaryReader_readUint32(reader);
        path->exists = true;

        // Points SimpleList
        path->pointCount = BinaryReader_readUint32(reader);
        if (path->pointCount > 0) {
            path->points = (PathPoint *)safeMalloc(path->pointCount * sizeof(PathPoint));
            repeat(path->pointCount, j) {
                path->points[j].x = BinaryReader_readFloat32(reader);
                path->points[j].y = BinaryReader_readFloat32(reader);
                path->points[j].speed = BinaryReader_readFloat32(reader);
            }
        } else {
            path->points = nullptr;
        }

        // Precompute internal representation for path following
        GamePath_computeInternal(path);
    }
    free(ptrs);
}

static void parseSCPT(BinaryReader* reader, DataWin* dw) {
    Scpt* s = &dw->scpt;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->scripts = nullptr; return; }

    s->scripts = (Script *)safeCalloc(count, sizeof(Script));
    repeat(count, i) {
        if (ptrs[i] == 0) { s->scripts[i].codeId = -1; continue; }
        BinaryReader_seek(reader, ptrs[i]);
        s->scripts[i].present = true;
        s->scripts[i].name = readStringPtr(reader, dw);
        s->scripts[i].codeId = BinaryReader_readInt32(reader);
    }
    free(ptrs);
}

static void parseACRV(BinaryReader* reader, DataWin* dw) {
    Acrv* a = &dw->acrv;

    // Align to 4-byte boundary
    while (BinaryReader_getPosition(reader) % 4 != 0) BinaryReader_readUint8(reader);

    uint32_t version = BinaryReader_readUint32(reader);
    if (version != 1) {
        logWarn("ACRV: unexpected version %u (expected 1)\n", version);
        return;
    }

    if (!DataWin_isVersionAtLeast(dw, 2, 3, 1, 0)) {
        size_t saved = BinaryReader_getPosition(reader);

        uint32_t count = BinaryReader_readUint32(reader);
        if (count == 0) {
            BinaryReader_seek(reader, saved);
            return;
        }

        uint32_t firstPtr = BinaryReader_readUint32(reader);
        BinaryReader_seek(reader, firstPtr);
        BinaryReader_skip(reader, 8);

        if (BinaryReader_readUint32(reader) != 0) {
            DataWin_bumpVersionTo(dw, 2, 3, 1, 0);
        } else if (BinaryReader_readUint32(reader) == 0) {
            DataWin_bumpVersionTo(dw, 2, 3, 1, 0);
        }

        BinaryReader_seek(reader, saved);
    }

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    a->count = count;
    if (count == 0) { free(ptrs); a->curves = nullptr; return; }

    // Whether the per-point format includes the 4 trailing float bezier handles (24 bytes) instead of the 2.3.0 layout (12 bytes, just X/Value + 4 pad).
    bool isV231Plus = DataWin_isVersionAtLeast(dw, 2, 3, 1, 0);

    a->curves = (AnimCurve *)safeCalloc(count, sizeof(AnimCurve));

    uint32_t globalChannelCount = 0;
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        AnimCurve* cur = &a->curves[i];
        cur->present = true;
        cur->name = readStringPtr(reader, dw);
        cur->graphType = BinaryReader_readUint32(reader);
        cur->channelCount = BinaryReader_readUint32(reader);
        cur->channels = (cur->channelCount > 0) ? (AnimCurveChannel *)safeCalloc(cur->channelCount, sizeof(AnimCurveChannel)) : nullptr;
        repeat(cur->channelCount, c) {
            AnimCurveChannel* ch = &cur->channels[c];
            ch->name = readStringPtr(reader, dw);
            ch->curveType = (AnimCurveType) BinaryReader_readUint32(reader);
            ch->iterations = BinaryReader_readUint32(reader);
            ch->pointCount = BinaryReader_readUint32(reader);
            ch->points = (ch->pointCount > 0) ? (AnimCurvePoint *)safeMalloc(ch->pointCount * sizeof(AnimCurvePoint)) : nullptr;
            repeat(ch->pointCount, p) {
                AnimCurvePoint* pt = &ch->points[p];
                pt->x = BinaryReader_readFloat32(reader);
                pt->value = BinaryReader_readFloat32(reader);
                if (isV231Plus) {
                    pt->bezierX0 = BinaryReader_readFloat32(reader);
                    pt->bezierY0 = BinaryReader_readFloat32(reader);
                    pt->bezierX1 = BinaryReader_readFloat32(reader);
                    pt->bezierY1 = BinaryReader_readFloat32(reader);
                } else {
                    BinaryReader_readUint32(reader); // padding
                    pt->bezierX0 = pt->bezierY0 = pt->bezierX1 = pt->bezierY1 = 0.0f;
                }
            }
            ch->globalId = (int32_t) globalChannelCount;
            globalChannelCount++;
        }
    }
    free(ptrs);

    // Build the flat global channel table for handle resolution
    a->allChannelsCount = globalChannelCount;
    if (globalChannelCount > 0) {
        a->allChannels = (AnimCurveChannel **)safeMalloc(globalChannelCount * sizeof(AnimCurveChannel*));
        uint32_t idx = 0;
        repeat(count, i) {
            AnimCurve* cur = &a->curves[i];
            if (!cur->present) continue;
            repeat(cur->channelCount, c) {
                a->allChannels[idx++] = &cur->channels[c];
            }
        }
    } else {
        a->allChannels = nullptr;
    }
}

static void parseGLOB(BinaryReader* reader, DataWin* dw) {
    Glob* g = &dw->glob;

    g->count = BinaryReader_readUint32(reader);
    if (g->count > 0) {
        g->codeIds = (int32_t *)safeMalloc(g->count * sizeof(int32_t));
        repeat(g->count, i) {
            g->codeIds[i] = BinaryReader_readInt32(reader);
        }
    } else {
        g->codeIds = nullptr;
    }
}

static void parseSHDR(BinaryReader* reader, DataWin* dw) {
    Shdr* s = &dw->shdr;

    uint32_t* ptrs = readPointerTable(reader, &s->count);
    s->shaders = (Shader *)safeCalloc(s->count, sizeof(Shader));

    repeat(s->count, i) {
        // Some GameMaker games have a nullptr for the shader, so we'll just mark them as not-present...
        if (ptrs[i] == 0) {
            Shader* sh = &s->shaders[i];
            sh->present = false;
            continue;
        }

        BinaryReader_seek(reader, ptrs[i]);
        Shader* sh = &s->shaders[i];
        sh->present = true;
        sh->name = readStringPtr(reader, dw);
        sh->type = BinaryReader_readUint32(reader) & 0x7FFFFFFF;
        sh->glslES_Vertex = readStringPtr(reader, dw);
        sh->glslES_Fragment = readStringPtr(reader, dw);
        sh->glsl_Vertex = readStringPtr(reader, dw);
        sh->glsl_Fragment = readStringPtr(reader, dw);
        sh->hlsl9_Vertex = readStringPtr(reader, dw);
        sh->hlsl9_Fragment = readStringPtr(reader, dw);
        sh->hlsl11_VertexOffset = BinaryReader_readUint32(reader);
        sh->hlsl11_PixelOffset = BinaryReader_readUint32(reader);

        // Vertex attributes SimpleList
        sh->vertexAttributeCount = BinaryReader_readUint32(reader);
        if (sh->vertexAttributeCount > 0) {
            sh->vertexAttributes = (const char **)safeMalloc(sh->vertexAttributeCount * sizeof(const char*));
            repeat(sh->vertexAttributeCount, j) {
                sh->vertexAttributes[j] = readStringPtr(reader, dw);
            }
        } else {
            sh->vertexAttributes = nullptr;
        }

        // Version field and console shader variants only exist on wadVersion > 13.
        if (dw->gen8.wadVersion > 13) {
            sh->version = BinaryReader_readInt32(reader);

            sh->pssl_VertexOffset = BinaryReader_readUint32(reader);
            sh->pssl_VertexLen = BinaryReader_readUint32(reader);
            sh->pssl_PixelOffset = BinaryReader_readUint32(reader);
            sh->pssl_PixelLen = BinaryReader_readUint32(reader);
            sh->cgVita_VertexOffset = BinaryReader_readUint32(reader);
            sh->cgVita_VertexLen = BinaryReader_readUint32(reader);
            sh->cgVita_PixelOffset = BinaryReader_readUint32(reader);
            sh->cgVita_PixelLen = BinaryReader_readUint32(reader);

            if (sh->version >= 2) {
                sh->cgPS3_VertexOffset = BinaryReader_readUint32(reader);
                sh->cgPS3_VertexLen = BinaryReader_readUint32(reader);
                sh->cgPS3_PixelOffset = BinaryReader_readUint32(reader);
                sh->cgPS3_PixelLen = BinaryReader_readUint32(reader);
            } else {
                sh->cgPS3_VertexOffset = 0;
                sh->cgPS3_VertexLen = 0;
                sh->cgPS3_PixelOffset = 0;
                sh->cgPS3_PixelLen = 0;
            }
        } else {
            sh->version = 0;
            sh->pssl_VertexOffset = 0;
            sh->pssl_VertexLen = 0;
            sh->pssl_PixelOffset = 0;
            sh->pssl_PixelLen = 0;
            sh->cgVita_VertexOffset = 0;
            sh->cgVita_VertexLen = 0;
            sh->cgVita_PixelOffset = 0;
            sh->cgVita_PixelLen = 0;
            sh->cgPS3_VertexOffset = 0;
            sh->cgPS3_VertexLen = 0;
            sh->cgPS3_PixelOffset = 0;
            sh->cgPS3_PixelLen = 0;
        }

        // Blob data follows but we skip it (pointer list seeking handles position)
    }
    free(ptrs);
}

static void parseFONT(BinaryReader* reader, DataWin* dw) {
    FontChunk* f = &dw->font;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    f->count = count;

    if (count == 0) { free(ptrs); f->fonts = nullptr; return; }

    // We need to figure out how many uint32 fields are between here and the PointerList
    uint32_t fontOptionalCount = (dw->gen8.wadVersion >= 17) ? 1u : 0u;
    {
        size_t baseAfterScaleY = (size_t) ptrs[0] + 40;
        for (uint32_t trial = fontOptionalCount; 4 >= trial; trial++) {
            size_t listStart = baseAfterScaleY + 4u * trial;
            BinaryReader_seek(reader, listStart);
            uint32_t probedGlyphCount = BinaryReader_readUint32(reader);
            if (probedGlyphCount == 0 || probedGlyphCount > 0x10000) continue;
            uint32_t probedFirstPtr = BinaryReader_readUint32(reader);
            size_t expectedFirstPtr = listStart + 4u + 4u * probedGlyphCount;
            if ((size_t) probedFirstPtr == expectedFirstPtr) {
                fontOptionalCount = trial;
                break;
            }
        }
    }

    f->fonts = (Font *)safeCalloc(count, sizeof(Font));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        Font* font = &f->fonts[i];
        font->present = true;
        font->name = readStringPtr(reader, dw);
        font->displayName = readStringPtr(reader, dw);
        uint32_t rawEmSize = BinaryReader_readUint32(reader);
        if (rawEmSize & (1u << 31)) {
            float negated;
            memcpy(&negated, &rawEmSize, sizeof(negated));
            font->emSize = -negated;
        } else {
            font->emSize = (float) rawEmSize;
        }
        font->bold = BinaryReader_readBool32(reader);
        font->italic = BinaryReader_readBool32(reader);
        font->rangeStart = BinaryReader_readUint16(reader);
        font->charset = BinaryReader_readUint8(reader);
        font->antiAliasing = BinaryReader_readUint8(reader);
        font->rangeEnd = BinaryReader_readUint32(reader);
        // Temporarily store the absolute file offset; parseTPAG resolves it in-place to a TPAG index once the TPAG table is known.
        font->tpagIndex = (int32_t) BinaryReader_readUint32(reader);
        font->scaleX = BinaryReader_readFloat32(reader);
        font->scaleY = BinaryReader_readFloat32(reader);
        // Optional fields appear in this order when present: AscenderOffset (WAD17+),
        // Ascender, SDFSpread, LineHeight. `fontOptionalCount` says how many are actually on disk.
        font->ascenderOffset = 0;
        font->ascender = 0;
        font->sdfSpread = 0;
        font->lineHeight = 0;
        font->hasAscender = false;
        font->hasSDFSpread = false;
        font->hasLineHeight = false;
        uint32_t readSoFar = 0;
        if (dw->gen8.wadVersion >= 17 && fontOptionalCount > readSoFar) {
            font->ascenderOffset = BinaryReader_readInt32(reader);
            readSoFar++;
        }
        if (fontOptionalCount > readSoFar) {
            font->ascender = BinaryReader_readUint32(reader);
            font->hasAscender = true;
            readSoFar++;
        }
        if (fontOptionalCount > readSoFar) {
            font->sdfSpread = BinaryReader_readUint32(reader);
            font->hasSDFSpread = true;
            readSoFar++;
        }
        if (fontOptionalCount > readSoFar) {
            font->lineHeight = BinaryReader_readUint32(reader);
            font->hasLineHeight = true;
            readSoFar++;
        }
        font->isSpriteFont = false;
        font->spriteIndex = -1;
        font->spriteOriginYAdjust = 0;

        // Glyphs PointerList
        uint32_t glyphCount;
        uint32_t* glyphPtrs = readPointerTable(reader, &glyphCount);
        font->glyphCount = glyphCount;

        uint32_t maxGlyphHeight = 0;
        if (glyphCount > 0) {
            font->glyphs = (FontGlyph *)safeMalloc(glyphCount * sizeof(FontGlyph));
            repeat(glyphCount, j) {
                BinaryReader_seek(reader, glyphPtrs[j]);
                FontGlyph* glyph = &font->glyphs[j];
                glyph->character = BinaryReader_readUint16(reader);
                glyph->sourceX = BinaryReader_readUint16(reader);
                glyph->sourceY = BinaryReader_readUint16(reader);
                glyph->sourceWidth = BinaryReader_readUint16(reader);
                glyph->sourceHeight = BinaryReader_readUint16(reader);
                glyph->shift = BinaryReader_readInt16(reader);
                glyph->offset = BinaryReader_readInt16(reader);

                if (glyph->sourceHeight > maxGlyphHeight) maxGlyphHeight = glyph->sourceHeight;

                // Kerning SimpleListShort (uint16 count)
                glyph->kerningCount = BinaryReader_readUint16(reader);
                if (glyph->kerningCount > 0) {
                    glyph->kerning = (KerningPair *)safeMalloc(glyph->kerningCount * sizeof(KerningPair));
                    for (uint16_t k = 0; glyph->kerningCount > k; k++) {
                        glyph->kerning[k].character = BinaryReader_readInt16(reader);
                        glyph->kerning[k].shiftModifier = BinaryReader_readInt16(reader);
                    }
                } else {
                    glyph->kerning = nullptr;
                }
            }
        } else {
            font->glyphs = nullptr;
        }
        font->maxGlyphHeight = maxGlyphHeight;
        Font_buildGlyphLUT(font);
        free(glyphPtrs);
    }
    free(ptrs);

    // 512 bytes of trailing padding -- skipped by chunkEnd seek
}

static void parseTMLN(BinaryReader* reader, DataWin* dw) {
    Tmln* t = &dw->tmln;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    t->count = count;

    if (count == 0) { free(ptrs); t->timelines = nullptr; return; }

    t->timelines = (Timeline *)safeCalloc(count, sizeof(Timeline));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        Timeline* tl = &t->timelines[i];
        tl->present = true;
        tl->name = readStringPtr(reader, dw);
        tl->momentCount = BinaryReader_readUint32(reader);

        if (tl->momentCount > 0) {
            tl->moments = (TimelineMoment *)safeMalloc(tl->momentCount * sizeof(TimelineMoment));

            // Pass 1: Read step + event pointer pairs
            uint32_t* eventPtrs = (uint32_t *)safeMalloc(tl->momentCount * sizeof(uint32_t));
            repeat(tl->momentCount, j) {
                tl->moments[j].step = BinaryReader_readUint32(reader);
                eventPtrs[j] = BinaryReader_readUint32(reader);
            }

            // Pass 2: Parse event action lists
            {
            repeat(tl->momentCount, j) {
                BinaryReader_seek(reader, eventPtrs[j]);
                tl->moments[j].actions = readEventActions(reader, dw, &tl->moments[j].actionCount);
            }
            }
            free(eventPtrs);
        } else {
            tl->moments = nullptr;
        }
    }
    free(ptrs);
}

static void parseOBJT(BinaryReader* reader, DataWin* dw) {
    Objt* o = &dw->objt;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    o->count = count;

    if (count == 0) { free(ptrs); o->objects = nullptr; return; }

    // Detect GMS 2022.5+ by probing the first game object's event list structure.
    if (DataWin_isVersionAtLeast(dw, 2, 3, 0, 0) && !DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0)) {
        uint32_t probePtr = 0;
        repeat(count, i) { if (ptrs[i] != 0) { probePtr = ptrs[i]; break; } }
        if (probePtr != 0) {
        // Skip the 16 fixed uint32 header fields (name..angularDamping) to reach physicsVertexCount.
        BinaryReader_seek(reader, probePtr + 16 * 4);
        int32_t vertexCount = BinaryReader_readInt32(reader);
        if (vertexCount >= 0) {
            // Skip friction + awake + kinematic (12 bytes) and physics vertices (8 bytes each).
            uint32_t skipCount = 12 + vertexCount * 8;
            uint32_t newLocation = reader->bufferPos + skipCount;
            bool isOldFormat = false;
            if (newLocation < reader->bufferSize) {
                BinaryReader_skip(reader, skipCount);
                uint32_t eventTypeCount = BinaryReader_readUint32(reader);
                if (eventTypeCount == OBJT_EVENT_TYPE_COUNT) {
                    uint32_t firstSubEventPtr = BinaryReader_readUint32(reader);
                    uint32_t currentAbsPos = (uint32_t) BinaryReader_getPosition(reader);
                    // The remaining 14 outer-list pointers sit between here and the first sub-event list.
                    if (firstSubEventPtr == currentAbsPos + 14 * 4) {
                        isOldFormat = true;
                    }
                }
            }
            if (!isOldFormat) {
                DataWin_bumpVersionTo(dw, 2022, 5, 0, 0);
            }
        }
        }
    }

    o->objects = (GameObject *)safeCalloc(count, sizeof(GameObject));
    repeat(count, i) {
        if (ptrs[i] == 0) { o->objects[i].parentId = -1; o->objects[i].spriteId = -1; o->objects[i].textureMaskId = -1; continue; }
        BinaryReader_seek(reader, ptrs[i]);
        GameObject* obj = &o->objects[i];
        obj->present = true;
        obj->name = readStringPtr(reader, dw);
        obj->spriteId = BinaryReader_readInt32(reader);
        obj->visible = BinaryReader_readBool32(reader);
        if (DataWin_isVersionAtLeast(dw, 2022, 5, 0, 0)) {
            obj->managed = BinaryReader_readBool32(reader);
        } else {
            obj->managed = false;
        }
        obj->solid = BinaryReader_readBool32(reader);
        obj->depth = BinaryReader_readInt32(reader);
        obj->persistent = BinaryReader_readBool32(reader);
        obj->parentId = BinaryReader_readInt32(reader);
        obj->textureMaskId = BinaryReader_readInt32(reader);
        obj->usesPhysics = BinaryReader_readBool32(reader);
        obj->isSensor = BinaryReader_readBool32(reader);
        obj->collisionShape = BinaryReader_readUint32(reader);
        obj->density = BinaryReader_readFloat32(reader);
        obj->restitution = BinaryReader_readFloat32(reader);
        obj->group = BinaryReader_readUint32(reader);
        obj->linearDamping = BinaryReader_readFloat32(reader);
        obj->angularDamping = BinaryReader_readFloat32(reader);
        obj->physicsVertexCount = BinaryReader_readInt32(reader);
        // WAD8 object records end at physicsVertexCount (no friction/awake/kinematic before the events list)
        if (8 >= dw->gen8.wadVersion) {
            obj->friction = 0;
            obj->awake = false;
            obj->kinematic = false;
        } else {
            obj->friction = BinaryReader_readFloat32(reader);
            obj->awake = BinaryReader_readBool32(reader);
            obj->kinematic = BinaryReader_readBool32(reader);
        }

        // Physics vertices
        if (obj->physicsVertexCount > 0) {
            obj->physicsVertices = (PhysicsVertex *)safeMalloc(obj->physicsVertexCount * sizeof(PhysicsVertex));
            for (int32_t j = 0; obj->physicsVertexCount > j; j++) {
                obj->physicsVertices[j].x = BinaryReader_readFloat32(reader);
                obj->physicsVertices[j].y = BinaryReader_readFloat32(reader);
            }
        } else {
            obj->physicsVertices = nullptr;
        }

        // Events: UndertalePointerList<UndertalePointerList<Event>>
        // Outer pointer list: one entry per event type
        // Inner pointer list: events for that type
        uint32_t eventTypeCount;
        uint32_t* eventTypePtrs = readPointerTable(reader, &eventTypeCount);

        for (uint32_t eventType = 0; eventTypeCount > eventType && OBJT_EVENT_TYPE_COUNT > eventType; eventType++) {
            BinaryReader_seek(reader, eventTypePtrs[eventType]);

            // Inner pointer list: events for this type
            uint32_t eventCount;
            uint32_t* eventPtrs = readPointerTable(reader, &eventCount);

            obj->eventLists[eventType].eventCount = eventCount;

            if (eventCount > 0) {
                obj->eventLists[eventType].events = (ObjectEvent *)safeMalloc(eventCount * sizeof(ObjectEvent));
                repeat(eventCount, j) {
                    BinaryReader_seek(reader, eventPtrs[j]);
                    obj->eventLists[eventType].events[j].eventSubtype = BinaryReader_readUint32(reader);
                    obj->eventLists[eventType].events[j].actions = readEventActions(reader, dw, &obj->eventLists[eventType].events[j].actionCount);
                }
            } else {
                obj->eventLists[eventType].events = nullptr;
            }

            free(eventPtrs);
        }

        // Zero-fill any unused event type slots
        {
        for (uint32_t eventType = eventTypeCount; OBJT_EVENT_TYPE_COUNT > eventType; eventType++) {
            obj->eventLists[eventType].eventCount = 0;
            obj->eventLists[eventType].events = nullptr;
        }
        }

        free(eventTypePtrs);
    }
    free(ptrs);
}

// ===[ Room payload parsing helpers ]===
// Each of these assumes the caller has seeked to the start of the relevant PointerList (where the uint32 "count" of the list is).
// They allocate and populate the corresponding fields on Room.
// They are used by both the eager parse path and the lazy load path (DataWin_loadRoomPayload).

static void readRoomBackgrounds(BinaryReader* reader, Room* room) {
    uint32_t bgCount;
    uint32_t* bgPtrs = readPointerTable(reader, &bgCount);
    room->backgrounds = (RoomBackground *)safeMalloc(8 * sizeof(RoomBackground));
    uint32_t fillEnd = bgCount < 8 ? bgCount : 8;
    for (uint32_t j = 0; fillEnd > j; j++) {
        BinaryReader_seek(reader, bgPtrs[j]);
        RoomBackground* bg = &room->backgrounds[j];
        bg->enabled = BinaryReader_readBool32(reader);
        bg->foreground = BinaryReader_readBool32(reader);
        bg->backgroundDefinition = BinaryReader_readInt32(reader);
        bg->x = BinaryReader_readInt32(reader);
        bg->y = BinaryReader_readInt32(reader);
        bg->tileX = BinaryReader_readInt32(reader);
        bg->tileY = BinaryReader_readInt32(reader);
        bg->speedX = BinaryReader_readInt32(reader);
        bg->speedY = BinaryReader_readInt32(reader);
        bg->stretch = BinaryReader_readBool32(reader);
    }
    {
    for (uint32_t j = fillEnd; 8 > j; j++) {
        memset(&room->backgrounds[j], 0, sizeof(RoomBackground));
    }
    }
    free(bgPtrs);
}

static void readRoomViews(BinaryReader* reader, Room* room) {
    uint32_t viewCount;
    uint32_t* viewPtrsArr = readPointerTable(reader, &viewCount);
    room->views = (RoomView *)safeMalloc(8 * sizeof(RoomView));
    for (uint32_t j = 0; viewCount > j && 8 > j; j++) {
        BinaryReader_seek(reader, viewPtrsArr[j]);
        RoomView* view = &room->views[j];
        view->enabled = BinaryReader_readBool32(reader);
        view->viewX = BinaryReader_readInt32(reader);
        view->viewY = BinaryReader_readInt32(reader);
        view->viewWidth = BinaryReader_readInt32(reader);
        view->viewHeight = BinaryReader_readInt32(reader);
        view->portX = BinaryReader_readInt32(reader);
        view->portY = BinaryReader_readInt32(reader);
        view->portWidth = BinaryReader_readInt32(reader);
        view->portHeight = BinaryReader_readInt32(reader);
        view->borderX = BinaryReader_readUint32(reader);
        view->borderY = BinaryReader_readUint32(reader);
        view->speedX = BinaryReader_readInt32(reader);
        view->speedY = BinaryReader_readInt32(reader);
        view->objectId = BinaryReader_readInt32(reader);
    }
    {
    for (uint32_t j = viewCount; 8 > j; j++) {
        memset(&room->views[j], 0, sizeof(RoomView));
    }
    }
    free(viewPtrsArr);
}

static void readRoomGameObjects(BinaryReader* reader, DataWin* dw, Room* room) {
    uint32_t objCount;
    uint32_t* objPtrs = readPointerTable(reader, &objCount);
    room->gameObjectCount = objCount;
    if (objCount > 0) {
        room->gameObjects = (RoomGameObject *)safeMalloc(objCount * sizeof(RoomGameObject));
        repeat(objCount, j) {
            BinaryReader_seek(reader, objPtrs[j]);
            RoomGameObject* go = &room->gameObjects[j];
            go->x = BinaryReader_readInt32(reader);
            go->y = BinaryReader_readInt32(reader);
            go->objectDefinition = BinaryReader_readInt32(reader);
            go->instanceID = BinaryReader_readUint32(reader);
            go->creationCode = BinaryReader_readInt32(reader);
            go->scaleX = BinaryReader_readFloat32(reader);
            go->scaleY = BinaryReader_readFloat32(reader);
            if (DataWin_isVersionAtLeast(dw, 2, 2, 2, 302)) {
                go->imageSpeed = BinaryReader_readFloat32(reader);
                go->imageIndex = BinaryReader_readInt32(reader);
            } else {
                go->imageSpeed = 1.0f;
                go->imageIndex = 0;
            }
            go->color = BinaryReader_readUint32(reader);
            go->rotation = BinaryReader_readFloat32(reader);
            if (dw->gen8.wadVersion >= 16) {
                go->preCreateCode = BinaryReader_readInt32(reader);
            } else {
                go->preCreateCode = -1;
            }
        }
    } else {
        room->gameObjects = nullptr;
    }
    free(objPtrs);
}

static float tileAlphaFromColor(uint32_t color) {
    // Extract alpha from high byte, default to 1.0 if alpha byte is 0
    uint8_t alphaByte = (uint8_t) ((color >> 24) & 0xFF);
    return alphaByte == 0 ? 1.0f : (float) alphaByte / 255.0f;
}

static void readRoomTiles(BinaryReader* reader, DataWin* dw, Room* room) {
    uint32_t tileCount;
    uint32_t* tilePtrs = readPointerTable(reader, &tileCount);
    room->tileCount = tileCount;
    if (tileCount > 0) {
        room->tiles = (RoomTile *)safeMalloc(tileCount * sizeof(RoomTile));
        repeat(tileCount, j) {
            BinaryReader_seek(reader, tilePtrs[j]);
            RoomTile* tile = &room->tiles[j];
            tile->x = BinaryReader_readInt32(reader);
            tile->y = BinaryReader_readInt32(reader);
            tile->useSpriteDefinition = DataWin_isVersionAtLeast(dw, 2, 0, 0, 0);
            tile->backgroundDefinition = BinaryReader_readInt32(reader);
            tile->sourceX = BinaryReader_readInt32(reader);
            tile->sourceY = BinaryReader_readInt32(reader);
            tile->width = BinaryReader_readUint32(reader);
            tile->height = BinaryReader_readUint32(reader);
            tile->tileDepth = BinaryReader_readInt32(reader);
            tile->instanceID = BinaryReader_readUint32(reader);
            tile->scaleX = BinaryReader_readFloat32(reader);
            tile->scaleY = BinaryReader_readFloat32(reader);
            tile->color = BinaryReader_readUint32(reader);
            tile->alpha = tileAlphaFromColor(tile->color);
        }
    } else {
        room->tiles = nullptr;
    }
    free(tilePtrs);
}

static void readRoomLayers(BinaryReader* reader, DataWin* dw, Room* room) {
    uint32_t layerCount;
    uint32_t* layerPtrs = readPointerTable(reader, &layerCount);
    room->layerCount = layerCount;

    if (layerCount == 0) {
        room->layers = nullptr;
        free(layerPtrs);
        return;
    }

    room->layers = (RoomLayer *)safeMalloc(layerCount * sizeof(RoomLayer));
    repeat(layerCount, j) {
        BinaryReader_seek(reader, layerPtrs[j]);
        RoomLayer* layer = &room->layers[j];
        layer->name = readStringPtr(reader, dw);
        layer->id = BinaryReader_readUint32(reader);
        layer->type = BinaryReader_readUint32(reader);
        layer->depth = BinaryReader_readInt32(reader);
        layer->xOffset = BinaryReader_readFloat32(reader);
        layer->yOffset = BinaryReader_readFloat32(reader);
        layer->hSpeed = BinaryReader_readFloat32(reader);
        layer->vSpeed = BinaryReader_readFloat32(reader);
        layer->visible = BinaryReader_readBool32(reader);
        layer->assetsData = nullptr;
        layer->backgroundData = nullptr;
        layer->instancesData = nullptr;
        layer->tilesData = nullptr;
        if (DataWin_isVersionAtLeast(dw, 2022, 1, 0, 0)) {
            // EffectEnabled (bool32), EffectType (string ptr), EffectProperties (SimpleList<EffectProperty>)
            BinaryReader_skip(reader, 4); // EffectEnabled
            BinaryReader_skip(reader, 4); // EffectType (string ptr)
            uint32_t effectPropCount = BinaryReader_readUint32(reader);
            // Each EffectProperty is 12 bytes: Kind(int32) + Name(ptr) + Value(ptr)
            BinaryReader_skip(reader, effectPropCount * 12);
        }
        switch (layer->type) {
            case RoomLayerType_Path:
            case RoomLayerType_Path2:
                break; // Nothing to do
            case RoomLayerType_Effect:
                // In GMS 2022.1+, Effect layer data is empty (fields moved to layer header).
                if (!DataWin_isVersionAtLeast(dw, 2022, 1, 0, 0)) {
                    BinaryReader_skip(reader, 4); // EffectType (string ptr)
                    uint32_t propCount = BinaryReader_readUint32(reader);
                    BinaryReader_skip(reader, propCount * 12);
                }
                break;

            case RoomLayerType_Assets: {
                RoomLayerAssetsData* assets = (RoomLayerAssetsData *)safeMalloc(sizeof(RoomLayerAssetsData));
                uint32_t legacyTilesPtr = BinaryReader_readUint32(reader);
                uint32_t spritesPtr = BinaryReader_readUint32(reader);

                BinaryReader_seek(reader, legacyTilesPtr);
                uint32_t *innerTilePtrs = readPointerTable(reader, &assets->legacyTileCount);
                if (assets->legacyTileCount > 0) {
                    assets->legacyTiles = (RoomTile *)safeMalloc(assets->legacyTileCount * sizeof(RoomTile));
                    repeat(assets->legacyTileCount, k) {
                        BinaryReader_seek(reader, innerTilePtrs[k]);
                        RoomTile* tile = &assets->legacyTiles[k];
                        tile->x = BinaryReader_readInt32(reader);
                        tile->y = BinaryReader_readInt32(reader);
                        tile->useSpriteDefinition = DataWin_isVersionAtLeast(dw, 2, 0, 0, 0);
                        tile->backgroundDefinition = BinaryReader_readInt32(reader);
                        tile->sourceX = BinaryReader_readInt32(reader);
                        tile->sourceY = BinaryReader_readInt32(reader);
                        tile->width = BinaryReader_readUint32(reader);
                        tile->height = BinaryReader_readUint32(reader);
                        tile->tileDepth = BinaryReader_readInt32(reader);
                        tile->instanceID = BinaryReader_readUint32(reader);
                        tile->scaleX = BinaryReader_readFloat32(reader);
                        tile->scaleY = BinaryReader_readFloat32(reader);
                        tile->color = BinaryReader_readUint32(reader);
                        tile->alpha = tileAlphaFromColor(tile->color);
                    }
                } else {
                    assets->legacyTiles = nullptr;
                }
                free(innerTilePtrs);

                BinaryReader_seek(reader, spritesPtr);
                uint32_t *spritePtrs = readPointerTable(reader, &assets->spriteCount);
                if (assets->spriteCount > 0) {
                    assets->sprites = (SpriteInstance *)safeMalloc(assets->spriteCount * sizeof(SpriteInstance));
                    repeat(assets->spriteCount, k) {
                        BinaryReader_seek(reader, spritePtrs[k]);
                        SpriteInstance* sprite = &assets->sprites[k];
                        sprite->name = readStringPtr(reader, dw);
                        sprite->spriteIndex = BinaryReader_readInt32(reader);
                        sprite->x = BinaryReader_readInt32(reader);
                        sprite->y = BinaryReader_readInt32(reader);
                        sprite->scaleX = BinaryReader_readFloat32(reader);
                        sprite->scaleY = BinaryReader_readFloat32(reader);
                        sprite->color = BinaryReader_readUint32(reader);
                        sprite->animationSpeed = BinaryReader_readFloat32(reader);
                        sprite->animationSpeedType = BinaryReader_readUint32(reader);
                        sprite->frameIndex = BinaryReader_readFloat32(reader);
                        sprite->rotation = BinaryReader_readFloat32(reader);
                    }
                } else {
                    assets->sprites = nullptr;
                }
                free(spritePtrs);

                layer->assetsData = assets;
                break;
            }

            case RoomLayerType_Background: {
                RoomLayerBackgroundData* bg = (RoomLayerBackgroundData *)safeMalloc(sizeof(RoomLayerBackgroundData));
                bg->visible = BinaryReader_readBool32(reader);
                bg->foreground = BinaryReader_readBool32(reader);
                bg->spriteIndex = BinaryReader_readInt32(reader);
                bg->hTiled = BinaryReader_readBool32(reader);
                bg->vTiled = BinaryReader_readBool32(reader);
                bg->stretch = BinaryReader_readBool32(reader);
                bg->color = BinaryReader_readUint32(reader);
                bg->firstFrame = BinaryReader_readFloat32(reader);
                bg->animSpeed = BinaryReader_readFloat32(reader);
                bg->animSpeedType = BinaryReader_readUint32(reader);
                layer->backgroundData = bg;
                break;
            }
            case RoomLayerType_Instances: {
                RoomLayerInstancesData* inst = (RoomLayerInstancesData *)safeMalloc(sizeof(RoomLayerInstancesData));
                inst->instanceCount = BinaryReader_readUint32(reader);
                if (inst->instanceCount > 0) {
                    inst->instanceIds = (uint32_t *)safeMalloc(inst->instanceCount * sizeof(uint32_t));
                    repeat(inst->instanceCount, k) {
                        inst->instanceIds[k] = BinaryReader_readUint32(reader);
                    }
                } else {
                    inst->instanceIds = nullptr;
                }
                layer->instancesData = inst;
                break;
            }
            case RoomLayerType_Tiles: {
                RoomLayerTilesData* tiles = (RoomLayerTilesData *)safeMalloc(sizeof(RoomLayerTilesData));
                tiles->backgroundIndex = BinaryReader_readInt32(reader);
                tiles->tilesX = BinaryReader_readUint32(reader);
                tiles->tilesY = BinaryReader_readUint32(reader);
                uint32_t totalTiles = tiles->tilesX * tiles->tilesY;
                if (totalTiles > 0) {
                    tiles->tileData = (uint32_t *)safeMalloc(totalTiles * sizeof(uint32_t));
                    if (DataWin_isVersionAtLeast(dw, 2024, 2, 0, 0)) {
                        // GM 2024.2+ games have RLE-compressed tile data
                        // GM 2024.4+ aligns to 4 bytes after the stream
                        uint32_t produced = 0;
                        while (totalTiles > produced) {
                            uint8_t length = BinaryReader_readUint8(reader);
                            if (length >= 128) {
                                uint32_t runLength = (uint32_t) ((length & 0x7F) + 1);
                                uint32_t tile = BinaryReader_readUint32(reader);
                                if (runLength > totalTiles - produced) runLength = totalTiles - produced;
                                for (uint32_t k = 0; runLength > k; k++) tiles->tileData[produced + k] = tile;
                                produced += runLength;
                            } else {
                                uint32_t runLength = (uint32_t) length;
                                if (runLength > totalTiles - produced) runLength = totalTiles - produced;
                                for (uint32_t k = 0; runLength > k; k++) tiles->tileData[produced + k] = BinaryReader_readUint32(reader);
                                produced += runLength;
                            }
                        }
                        // Trailing padding: if the last two real tiles differ, the writer inserts a (0x81, -1) pad pair.
                        bool hasPadding = false;
                        if (totalTiles == 1) {
                            hasPadding = true;
                        } else if (totalTiles >= 2) {
                            hasPadding = tiles->tileData[totalTiles - 1] != tiles->tileData[totalTiles - 2];
                        }
                        if (hasPadding) {
                            uint8_t length = BinaryReader_readUint8(reader);
                            uint32_t tile = BinaryReader_readUint32(reader);
                            (void) length; (void) tile;
                        }
                        if (DataWin_isVersionAtLeast(dw, 2024, 4, 0, 0)) {
                            size_t pos = BinaryReader_getPosition(reader);
                            size_t aligned = (pos + 3u) & ~(size_t) 3u;
                            if (aligned > pos) BinaryReader_skip(reader, aligned - pos);
                        }
                    } else {
                        repeat(totalTiles, k) {
                            tiles->tileData[k] = BinaryReader_readUint32(reader);
                        }
                    }
                } else {
                    tiles->tileData = nullptr;
                }
                layer->tilesData = tiles;
                break;
            }
            default: {
                logError("Unsupported Room Layer Type %u\n", layer->type);
                exit(0);
            }
        }
    }
    free(layerPtrs);
}

// Reads all 5 payload sections for a single room via the given reader.
// Assumes the caller has populated room->*FileOffset from the header pass.
static void readRoomPayload(BinaryReader* reader, DataWin* dw, Room* room) {
    require(!room->payloadLoaded);

    BinaryReader_seek(reader, room->backgroundsFileOffset);
    readRoomBackgrounds(reader, room);

    BinaryReader_seek(reader, room->viewsFileOffset);
    readRoomViews(reader, room);

    BinaryReader_seek(reader, room->gameObjectsFileOffset);
    readRoomGameObjects(reader, dw, room);

    BinaryReader_seek(reader, room->tilesFileOffset);
    readRoomTiles(reader, dw, room);

    room->layerCount = 0;
    room->layers = nullptr;
    if (room->layersFileOffset != 0) {
        BinaryReader_seek(reader, room->layersFileOffset);
        readRoomLayers(reader, dw, room);
    }

    room->payloadLoaded = true;
}

// Returns true when "name" is in the eager-load set.
static bool isRoomNameInEagerList(const char* name, StringBooleanEntry* eagerSet) {
    if (name == nullptr || eagerSet == nullptr) return false;
    return shgeti(eagerSet, name) >= 0;
}

static bool probeRleTileLayer(BinaryReader* reader, uint32_t gridStart, uint32_t regionLen, uint32_t totalCells) {
    if (regionLen == 0) return false;
    BinaryReader_seek(reader, gridStart);
    uint32_t produced = 0;
    uint32_t consumed = 0;
    while (produced < totalCells) {
        if (consumed + 1 > regionLen) return false;
        uint8_t length = BinaryReader_readUint8(reader);
        consumed += 1;
        if (length >= 128) {
            uint32_t runLength = (length & 0x7F) + 1;
            if (consumed + 4 > regionLen) return false;
            BinaryReader_skip(reader, 4);
            consumed += 4;
            if (runLength > totalCells - produced) runLength = totalCells - produced;
            produced += runLength;
        } else {
            uint32_t runLength = length;
            if (runLength > totalCells - produced) runLength = totalCells - produced;
            if (consumed + (size_t) runLength * 4 > regionLen) return false;
            BinaryReader_skip(reader, (size_t) runLength * 4);
            consumed += (size_t) runLength * 4;
            produced += runLength;
        }
    }
    // for you two watchin the stream, after every cell, only a 4-byte pad plus alignment may remain, and basically more means it isn't rle..
    uint32_t remaining = regionLen - consumed;
    return remaining <= 7;
}

static void parseROOM(BinaryReader* reader, DataWin* dw, bool lazyLoadRooms, StringBooleanEntry* eagerlyLoadedRooms) {
    RoomChunk* rc = &dw->room;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    rc->count = count;

    if (count == 0) { free(ptrs); rc->rooms = nullptr; return; }

    // Detect whether RoomGameObject includes ImageSpeed/ImageIndex fields (added in GMS 2.2.2.302).
    // UndertaleModTool detects this via the distance between the first two game object pointers: 40 bytes = legacy format, 48 bytes = new format with ImageSpeed+ImageIndex.
    // We skip if we already know that we are at or above 2.2.2.302.
    if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0) && !DataWin_isVersionAtLeast(dw, 2, 2, 2, 302)) {
        repeat(count, i) {
            if (ptrs[i] == 0) continue;
            BinaryReader_seek(reader, ptrs[i]);
            // Room header layout (before gameObjectsPtr): name, caption, width, height, speed, persistent,
            // bgColor, drawBgColor, creationCodeId, flags, backgroundsPtr, viewsPtr = 12 uint32s.
            BinaryReader_skip(reader, 12 * 4);
            uint32_t gameObjectsPtr = BinaryReader_readUint32(reader);
            BinaryReader_seek(reader, gameObjectsPtr);
            uint32_t objCount = BinaryReader_readUint32(reader);
            if (objCount >= 2) {
                uint32_t firstPtr = BinaryReader_readUint32(reader);
                uint32_t secondPtr = BinaryReader_readUint32(reader);
                if (secondPtr - firstPtr == 48) {
                    DataWin_bumpVersionTo(dw, 2, 2, 2, 302);
                }
                break;
            }
        }
    }

    // Detect whether Layer headers include EffectEnabled/EffectType/EffectProperties fields (added in GMS 2022.1).
    if (DataWin_isVersionAtLeast(dw, 2, 3, 0, 0) && !DataWin_isVersionAtLeast(dw, 2022, 1, 0, 0)) {
        repeat(count, i) {
            if (ptrs[i] == 0) continue;
            BinaryReader_seek(reader, ptrs[i]);
            // Room header before layersPtr: 22 uint32s (name..metersPerPixel).
            BinaryReader_skip(reader, 22 * 4);
            uint32_t layersPtr = BinaryReader_readUint32(reader);
            uint32_t seqnPtr = BinaryReader_readUint32(reader);
            BinaryReader_seek(reader, layersPtr);
            uint32_t layerCount = BinaryReader_readUint32(reader);
            if (layerCount == 0) continue;
            uint32_t jumpOffset = BinaryReader_readUint32(reader);
            uint32_t nextOffset = (layerCount == 1) ? seqnPtr : BinaryReader_readUint32(reader);
            // Layer header: name(4) id(4) type(4) depth(4) xOff(4) yOff(4) hSpd(4) vSpd(4) visible(4) = 9 uint32s = 36 bytes.
            // jumpOffset points to start of the layer; we seek to jumpOffset+8 to skip name+id then read type.
            BinaryReader_seek(reader, jumpOffset + 8);
            uint32_t layerType = BinaryReader_readUint32(reader);
            if (layerType == RoomLayerType_Path || layerType == RoomLayerType_Path2) continue;
            bool detected = false;
            switch (layerType) {
                case RoomLayerType_Background: {
                    // After type, there's depth+xOff+yOff+hSpd+vSpd+visible = 6*4 = 24, then 10 background fields = 40 bytes.
                    // Total legacy body after type read: 24 + 40 = 64 bytes. 2022.1 adds effect data > 64 bytes of additional data past the next layer boundary.
                    size_t absPos = BinaryReader_getPosition(reader);
                    if (nextOffset - absPos > 16 * 4) detected = true;
                    break;
                }
                case RoomLayerType_Instances: {
                    BinaryReader_skip(reader, 6 * 4);
                    uint32_t instanceCount = BinaryReader_readUint32(reader);
                    size_t absPos = BinaryReader_getPosition(reader);
                    if (nextOffset - absPos != instanceCount * 4) detected = true;
                    break;
                }
                case RoomLayerType_Assets: {
                    BinaryReader_skip(reader, 6 * 4);
                    uint32_t tileOffset = BinaryReader_readUint32(reader);
                    size_t absPos = BinaryReader_getPosition(reader);
                    if (tileOffset != absPos + 8 && tileOffset != absPos + 12) detected = true;
                    break;
                }
                case RoomLayerType_Tiles: {
                    BinaryReader_skip(reader, 7 * 4);
                    uint32_t tileMapWidth = BinaryReader_readUint32(reader);
                    uint32_t tileMapHeight = BinaryReader_readUint32(reader);
                    size_t absPos = BinaryReader_getPosition(reader);
                    if (nextOffset - absPos != tileMapWidth * tileMapHeight * 4) detected = true;
                    break;
                }
                case RoomLayerType_Effect: {
                    BinaryReader_skip(reader, 7 * 4);
                    uint32_t propertyCount = BinaryReader_readUint32(reader);
                    size_t absPos = BinaryReader_getPosition(reader);
                    if (nextOffset - absPos != propertyCount * 3 * 4) detected = true;
                    break;
                }
            }
            if (detected) DataWin_bumpVersionTo(dw, 2022, 1, 0, 0);
            break;
        }
    }

    // Detect RLE-compressed tile data (Added in GMS 2024.2)
    if (DataWin_isVersionAtLeast(dw, 2023, 2, 0, 0) && !DataWin_isVersionAtLeast(dw, 2024, 2, 0, 0)) {
        // Iterate over rooms to find tile layers
        repeat(count, i) {
            if (ptrs[i] == 0) continue;

            BinaryReader_seek(reader, ptrs[i]);
            BinaryReader_skip(reader, 22 * 4); // Skip to layersPtr

            uint32_t layersPtr = BinaryReader_readUint32(reader);
            uint32_t seqnPtr = BinaryReader_readUint32(reader);

            BinaryReader_seek(reader, layersPtr);
            uint32_t layerCount = BinaryReader_readUint32(reader);
            if (layerCount <= 0) continue;

            bool found2024_2 = false;

            for (uint32_t layerNum = 0; layerNum < layerCount; layerNum++) {
                size_t layerPtr = (size_t)(layersPtr + 4 + (4 * layerNum));
                BinaryReader_seek(reader, layerPtr + 4);

                uint32_t jumpOffset = BinaryReader_readUint32(reader) + 8;
                uint32_t nextOffset = (layerNum == layerCount - 1) ? seqnPtr : BinaryReader_readUint32(reader);

                BinaryReader_seek(reader, jumpOffset);
                uint32_t layerType = BinaryReader_readUint32(reader);

                if (layerType != RoomLayerType_Tiles) continue;

                // Skip to tile map dimensions
                BinaryReader_skip(reader, 32);
                uint32_t effectCount = BinaryReader_readUint32(reader);
                BinaryReader_skip(reader, effectCount * 12 + 4);

                uint32_t tileMapWidth = BinaryReader_readUint32(reader);
                uint32_t tileMapHeight = BinaryReader_readUint32(reader);
                uint32_t totalCells = tileMapWidth * tileMapHeight;
                size_t gridStart = BinaryReader_getPosition(reader);
                if (nextOffset <= (uint32_t) gridStart) continue;
                uint32_t regionLen = nextOffset - (uint32_t) gridStart;

                if (totalCells > 0 && probeRleTileLayer(reader, (uint32_t) gridStart, regionLen, totalCells)) {
                    DataWin_bumpVersionTo(dw, 2024, 2, 0, 0);
                    found2024_2 = true;
                    break;
                }
            }

            if (found2024_2) break;
        }
    }

    // Detect alignment after the stream of said RLE data (Added in GMS 2024.4)
    if (DataWin_isVersionAtLeast(dw, 2024, 2, 0, 0) && !DataWin_isVersionAtLeast(dw, 2024, 4, 0, 0)) {
        bool hasNonAlignedLayer = false;

        repeat(count, i) {
            if (ptrs[i] == 0) continue;

            BinaryReader_seek(reader, ptrs[i]);
            BinaryReader_skip(reader, 22 * 4);

            uint32_t layersPtr = BinaryReader_readUint32(reader);

            BinaryReader_seek(reader, layersPtr);
            uint32_t layerCount = BinaryReader_readUint32(reader);
            if (layerCount <= 0) continue;

            for (uint32_t layerNum = 0; layerNum < layerCount; layerNum++) {
                size_t layerPtr = (size_t)(layersPtr + 4 + (4 * layerNum));
                if (layerPtr % 4 != 0) {
                    hasNonAlignedLayer = true;
                    break;
                }
            }

            if (hasNonAlignedLayer) break;
        }

        // If no non-aligned layers found, it's 2024.4+
        if (!hasNonAlignedLayer) {
            DataWin_bumpVersionTo(dw, 2024, 4, 0, 0);
        }
    }

    rc->rooms = (Room *)safeCalloc(count, sizeof(Room));
    repeat(count, i) {
        if (ptrs[i] == 0) { rc->rooms[i].creationCodeId = -1; continue; }
        BinaryReader_seek(reader, ptrs[i]);
        Room* room = &rc->rooms[i];
        room->present = true;

        // ===[ Header pass ]===
        room->name = readStringPtr(reader, dw);
        room->caption = readStringPtr(reader, dw);
        room->width = BinaryReader_readUint32(reader);
        room->height = BinaryReader_readUint32(reader);
        room->speed = BinaryReader_readUint32(reader);
        room->persistent = BinaryReader_readBool32(reader);
        room->backgroundColor = BinaryReader_readUint32(reader);
        room->drawBackgroundColor = BinaryReader_readBool32(reader);
        room->creationCodeId = BinaryReader_readInt32(reader);
        room->flags = BinaryReader_readUint32(reader);
        room->backgroundsFileOffset = BinaryReader_readUint32(reader);
        room->viewsFileOffset = BinaryReader_readUint32(reader);
        room->gameObjectsFileOffset = BinaryReader_readUint32(reader);
        room->tilesFileOffset = BinaryReader_readUint32(reader);
        room->world = BinaryReader_readBool32(reader);
        room->top = BinaryReader_readUint32(reader);
        room->left = BinaryReader_readUint32(reader);
        room->right = BinaryReader_readUint32(reader);
        room->bottom = BinaryReader_readUint32(reader);
        room->gravityX = BinaryReader_readFloat32(reader);
        room->gravityY = BinaryReader_readFloat32(reader);
        room->metersPerPixel = BinaryReader_readFloat32(reader);
        if (DataWin_isVersionAtLeast(dw, 2024, 13, 0, 0)) {
            // skip instanceCreationOrderIDs
            BinaryReader_skip(reader, 4);
        }
        room->layersFileOffset = 0;
        if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0)) {
            room->layersFileOffset = BinaryReader_readUint32(reader);
            if (DataWin_isVersionAtLeast(dw, 2, 3, 0, 0)) {
                BinaryReader_skip(reader, 4); // sequencesPtr
            }
        }

        room->payloadLoaded = false;
        room->eagerlyLoaded = false;
        room->backgrounds = nullptr;
        room->views = nullptr;
        room->gameObjects = nullptr;
        room->gameObjectCount = 0;
        room->tiles = nullptr;
        room->tileCount = 0;
        room->layers = nullptr;
        room->layerCount = 0;

        // Load the room payload if needed
        bool eager = !lazyLoadRooms || isRoomNameInEagerList(room->name, eagerlyLoadedRooms);
        if (eager) {
            readRoomPayload(reader, dw, room);
            if (lazyLoadRooms) {
                room->eagerlyLoaded = true;
            }
        }
    }
    free(ptrs);
}

// Parses a TexturePageItem at the current reader position
// If i = -1, a new item entry will be allocated AND will be marked as a WinPack WAD
// Returns the index of the TPAG
static int32_t parseTexturePageItem(BinaryReader* reader, DataWin* dw, int32_t i) {
    int32_t position = i;
    if (i == -1) {
        logWarn("DataWin: Allocated new TPAG! Was the WAD built with WinPack? (TranslaTale)\n");
        uint32_t newCount = dw->tpag.count + 1;
        TexturePageItem* newItems = (TexturePageItem *)safeCalloc(newCount, sizeof(TexturePageItem));
        memcpy(newItems, dw->tpag.items, dw->tpag.count * sizeof(TexturePageItem));
        free(dw->tpag.items);

        dw->tpag.count = newCount;

        dw->tpag.items = newItems;
        position = (int32_t) newCount - 1;
    }

    TexturePageItem* item = &dw->tpag.items[position];
    item->present = true;
    item->sourceX = BinaryReader_readUint16(reader);
    item->sourceY = BinaryReader_readUint16(reader);
    item->sourceWidth = BinaryReader_readUint16(reader);
    item->sourceHeight = BinaryReader_readUint16(reader);
    item->targetX = BinaryReader_readUint16(reader);
    item->targetY = BinaryReader_readUint16(reader);
    item->targetWidth = BinaryReader_readUint16(reader);
    item->targetHeight = BinaryReader_readUint16(reader);
    item->boundingWidth = BinaryReader_readUint16(reader);
    item->boundingHeight = BinaryReader_readUint16(reader);
    item->texturePageId = BinaryReader_readInt16(reader);

    if (i == -1) {
        // WinPack texture pages are off by one, because uuhh... it seems that it considers the runner allocated 1x1 white texture for some reason?!
        item->texturePageId -= 1;
    }

    return position;
}

// Sprite/Background/Font initially store an absolute file offset to their TexturePageItem (since SPRT/BGND/FONT are parsed before TPAG).
// resolveAllTPAGReferences translates those offsets to TPAG indices once the table is known. ptrs[] is the TPAG pointer table in monotonically increasing file order, so we can binary search it.
// Offsets that don't resolve (or are 0) become -1.
static int32_t findTPAGIndexByOffset(BinaryReader* reader, DataWin* dw, uint32_t* ptrs, uint32_t count, uint32_t offset) {
    if (offset == 0)
        return -1;

    uint32_t lo = 0, hi = count;
    while (hi > lo) {
        uint32_t mid = (lo + hi) >> 1;
        uint32_t v = ptrs[mid];

        if (v == offset)
            return (int32_t) mid;

        if (offset > v)
            lo = mid + 1;
        else
            hi = mid;
    }

    // This is stupidly annoying
    // WinPack (used by TranslaTale) stores TPAGs OUTSIDE of the IFF chunk and those entries are NOT present in the TPAG list
    // So we need to manually read it
    // The offset is an absolute position
    if (reader->fileSize > offset) {
        BinaryReader_seek(reader, offset);

        return parseTexturePageItem(reader, dw, -1);
    }

    return -1;
}

static void resolveAllTPAGReferences(BinaryReader* reader, DataWin* dw, uint32_t* ptrs, uint32_t count) {
    repeat(dw->sprt.count, i) {
        Sprite* spr = &dw->sprt.sprites[i];
        repeat(spr->textureCount, j) {
            spr->tpagIndices[j] = findTPAGIndexByOffset(reader, dw, ptrs, count, (uint32_t) spr->tpagIndices[j]);
        }
    }
    {
    repeat(dw->bgnd.count, i) {
        Background* bg = &dw->bgnd.backgrounds[i];
        bg->tpagIndex = findTPAGIndexByOffset(reader, dw, ptrs, count, (uint32_t) bg->tpagIndex);
    }
    }
    {
    repeat(dw->font.count, i) {
        Font* fnt = &dw->font.fonts[i];
        fnt->tpagIndex = findTPAGIndexByOffset(reader, dw, ptrs, count, (uint32_t) fnt->tpagIndex);
    }
    }
}

static void parseTPAG(BinaryReader* reader, DataWin* dw) {
    Tpag* t = &dw->tpag;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    t->count = count;

    if (count == 0) { free(ptrs); t->items = nullptr; return; }

    t->items = (TexturePageItem *)safeCalloc(count, sizeof(TexturePageItem));
    repeat(count, i) {
        if (ptrs[i] == 0) { t->items[i].texturePageId = -1; continue; }
        BinaryReader_seek(reader, ptrs[i]);
        parseTexturePageItem(reader, dw, i);
    }

    resolveAllTPAGReferences(reader, dw, ptrs, count);

    free(ptrs);
}

static void parseCODE(BinaryReader* reader, DataWin* dw, uint32_t chunkLength, size_t chunkDataStart) {
    Code* c = &dw->code;

    if (chunkLength == 0) {
        // YYC-compiled game, no bytecode
        c->count = 0;
        c->entries = nullptr;
        return;
    }

    // Standard pointer list at chunk start. Each entry has a relative offset
    // (bytecodeRelAddr) that points to the actual bytecode blob elsewhere in the chunk.

    uint32_t codeCount;
    uint32_t* codePtrs = readPointerTable(reader, &codeCount);
    c->count = codeCount;

    if (codeCount == 0) { free(codePtrs); c->entries = nullptr; return; }

    bool oldFormat = 14 >= dw->gen8.wadVersion;

    c->entries = (CodeEntry *)safeCalloc(codeCount, sizeof(CodeEntry));
    repeat(codeCount, i) {
        if (codePtrs[i] == 0) continue;
        BinaryReader_seek(reader, codePtrs[i]);
        CodeEntry* entry = &c->entries[i];
        entry->present = true;
        entry->name = readStringPtr(reader, dw);
        entry->length = BinaryReader_readUint32(reader);

        if (oldFormat) {
            // BC<=14: instructions are inline right after the length field.
            // No localsCount/argumentsCount/bytecodeRelAddr/offset fields.
            entry->localsCount = 0;
            entry->argumentsCount = 0;
            entry->offset = 0;
            entry->bytecodeAbsoluteOffset = (uint32_t) BinaryReader_getPosition(reader);
            BinaryReader_skip(reader, entry->length);
        } else {
            entry->localsCount = BinaryReader_readUint16(reader);
            entry->argumentsCount = BinaryReader_readUint16(reader);

            // bytecodeRelAddr is relative to the position of this field
            size_t relAddrFieldPos = BinaryReader_getPosition(reader);
            int32_t bytecodeRelAddr = BinaryReader_readInt32(reader);
            entry->bytecodeAbsoluteOffset = (uint32_t)((int64_t)relAddrFieldPos + bytecodeRelAddr);

            entry->offset = BinaryReader_readUint32(reader);
        }
    }
    free(codePtrs);

    size_t chunkEnd = chunkDataStart + chunkLength;

    if (oldFormat) {
        // BC<=14: bytecode is intermixed with entry headers. Capture the whole chunk as the bytecode buffer so that the per-entry bytecodeAbsoluteOffset values resolve correctly into it.
        dw->bytecodeBufferBase = chunkDataStart;
        dw->bytecodeBuffer = BinaryReader_readBytesAt(reader, chunkDataStart, chunkLength);
        return;
    }

    // Compute bytecode blob range and load into owned buffer.
    // The bytecode blob starts at the minimum bytecodeAbsoluteOffset and
    // extends to the end of the CODE chunk.
    uint32_t blobStart = UINT32_MAX;
    {
    repeat(codeCount, i) {
        if (!c->entries[i].present) continue;
        if (blobStart > c->entries[i].bytecodeAbsoluteOffset) {
            blobStart = c->entries[i].bytecodeAbsoluteOffset;
        }
    }
    }
    if (blobStart == UINT32_MAX) blobStart = (uint32_t) chunkDataStart;
    size_t blobSize = chunkEnd - blobStart;

    dw->bytecodeBufferBase = blobStart;
    dw->bytecodeBuffer = BinaryReader_readBytesAt(reader, blobStart, blobSize);
}

static void parseVARI(BinaryReader* reader, DataWin* dw, uint32_t chunkLength) {
    Vari* v = &dw->vari;

    // BC<=14 has no header (varCount1/varCount2/maxLocalVarCount) and 12-byte entries (no instanceType/varID).
    // BC>=15 has a 12-byte header and 20-byte entries.
    bool oldFormat = dw->gen8.wadVersion <= 14;

    if (oldFormat) {
        v->varCount1 = 0;
        v->varCount2 = 0;
        v->maxLocalVarCount = 0;
        v->variableCount = chunkLength / 12;
    } else {
        v->varCount1 = BinaryReader_readUint32(reader);
        v->varCount2 = BinaryReader_readUint32(reader);
        v->maxLocalVarCount = BinaryReader_readUint32(reader);
        v->variableCount = (chunkLength - 12) / 20;
    }

    if (v->variableCount > 0) {
        v->variables = (Variable *)safeMalloc(v->variableCount * sizeof(Variable));
        repeat(v->variableCount, i) {
            Variable* var = &v->variables[i];
            var->name = readStringPtr(reader, dw);
            if (oldFormat) {
                var->instanceType = 0;
                var->varID = 0;
            } else {
                var->instanceType = BinaryReader_readInt32(reader);
                var->varID = BinaryReader_readInt32(reader);
            }
            var->occurrences = BinaryReader_readUint32(reader);
            var->firstAddress = BinaryReader_readUint32(reader);
        }
    } else {
        v->variables = nullptr;
    }
}

static void parseFUNC(BinaryReader* reader, DataWin* dw, uint32_t chunkLength) {
    Func* f = &dw->func;

    // BC<=14 packs functions as a flat 12-byte-per-entry array (no SimpleList count prefix) and has no CodeLocals section.
    if (dw->gen8.wadVersion <= 14) {
        f->functionCount = chunkLength / 12;
        if (f->functionCount > 0) {
            f->functions = (Function *)safeMalloc(f->functionCount * sizeof(Function));
            repeat(f->functionCount, i) {
                f->functions[i].name = readStringPtr(reader, dw);
                f->functions[i].occurrences = BinaryReader_readUint32(reader);
                f->functions[i].firstAddress = BinaryReader_readUint32(reader);
            }
        } else {
            f->functions = nullptr;
        }
        f->codeLocalsCount = 0;
        f->codeLocals = nullptr;
        return;
    }

    size_t funcChunkStart = BinaryReader_getPosition(reader);
    size_t funcChunkEnd = funcChunkStart + chunkLength;
    if (!DataWin_isVersionAtLeast(dw, 2024, 8, 0, 0) && chunkLength != 0) {
        uint32_t probeCount = BinaryReader_readUint32(reader);
        size_t afterFunctions = BinaryReader_getPosition(reader) + (size_t) probeCount * 12;
        bool is2024_8 = false;
        if (afterFunctions == funcChunkEnd) {
            // Reached the chunk end immediately after the function list: code locals are definitely gone.
            is2024_8 = true;
        } else if (funcChunkEnd > afterFunctions) {
            // Otherwise the remainder must be nothing but 16-byte alignment padding to qualify.
            BinaryReader_seek(reader, afterFunctions);
            int paddingBytesRead = 0;
            bool onlyPadding = true;
            while ((BinaryReader_getPosition(reader) & 15) != 0) {
                if (BinaryReader_getPosition(reader) >= funcChunkEnd || BinaryReader_readUint8(reader) != 0) {
                    onlyPadding = false;
                    break;
                }
                paddingBytesRead++;
            }
            // <4 padding bytes can't be a real (empty) list header; with >=4 we need a code entry to be sure.
            if (onlyPadding && BinaryReader_getPosition(reader) == funcChunkEnd && (4 > paddingBytesRead || dw->code.count > 0)) {
                is2024_8 = true;
            }
        }
        if (is2024_8) {
            DataWin_bumpVersionTo(dw, 2024, 8, 0, 0);
        }
        BinaryReader_seek(reader, funcChunkStart);
    }

    // Part 1: Functions SimpleList
    f->functionCount = BinaryReader_readUint32(reader);
    if (f->functionCount > 0) {
        f->functions = (Function *)safeMalloc(f->functionCount * sizeof(Function));
        repeat(f->functionCount, i) {
            f->functions[i].name = readStringPtr(reader, dw);
            f->functions[i].occurrences = BinaryReader_readUint32(reader);
            uint32_t rawAddr = BinaryReader_readUint32(reader);
            // In GMS 2.3+, firstAddress points to the operand word (instruction + 4), not the instruction itself
            if (DataWin_isVersionAtLeast(dw, 2, 3, 0, 0) && rawAddr != (uint32_t) -1) {
                rawAddr -= 4;
            }
            f->functions[i].firstAddress = rawAddr;
        }
    } else {
        f->functions = nullptr;
    }

    // Part 2: Code Locals SimpleList
    if (DataWin_isVersionAtLeast(dw, 2024, 8, 0, 0)) {
        f->codeLocalsCount = 0;
        f->codeLocals = nullptr;
        return;
    }
    f->codeLocalsCount = BinaryReader_readUint32(reader);
    if (f->codeLocalsCount > 0) {
        f->codeLocals = (CodeLocals *)safeMalloc(f->codeLocalsCount * sizeof(CodeLocals));
        repeat(f->codeLocalsCount, i) {
            CodeLocals* cl = &f->codeLocals[i];
            cl->localVarCount = BinaryReader_readUint32(reader);
            cl->name = readStringPtr(reader, dw);

            if (cl->localVarCount > 0) {
                cl->locals = (LocalVar *)safeMalloc(cl->localVarCount * sizeof(LocalVar));
                repeat(cl->localVarCount, j) {
                    cl->locals[j].varID = BinaryReader_readUint32(reader);
                    cl->locals[j].name = readStringPtr(reader, dw);
                }
            } else {
                cl->locals = nullptr;
            }
        }
    } else {
        f->codeLocals = nullptr;
    }
}

static void parseSTRG(BinaryReader* reader, DataWin* dw) {
    Strg* s = &dw->strg;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    s->count = count;

    if (count == 0) { free(ptrs); s->strings = nullptr; return; }

    s->strings = (const char **)safeCalloc(count, sizeof(const char*));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        // Pointer table points to the string's length prefix.
        // The actual string content starts 4 bytes after.
        s->strings[i] = (const char*)(dw->strgBuffer + (ptrs[i] + 4 - dw->strgBufferBase));
    }
    free(ptrs);
}

static void parseTXTR(BinaryReader* reader, DataWin* dw, size_t chunkEnd, bool loadTextureDataLazily) {
    Txtr* t = &dw->txtr;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    t->count = count;

    if (count == 0) { free(ptrs); t->textures = nullptr; return; }

    // Read metadata entries
    bool hasGeneratedMips = DataWin_isVersionAtLeast(dw, 2, 0, 0, 0);

    // Detect GMS 2022.3+ (TextureBlockSize field) and 2022.9+ (Width/Height/IndexInGroup fields) by probing the distance between the first two entry pointers.
    // Only works when there are at least 2 textures (which is almost always the case for real games).
    // Layouts:
    //   pre-2022.3: scaled+generatedMips+blobOffset = 12 bytes
    //   2022.3+: ... + textureBlockSize = 16 bytes
    //   2022.9+: ... + width + height + indexInGroup = 28 bytes
    bool has2022_3 = DataWin_isVersionAtLeast(dw, 2022, 3, 0, 0);
    bool has2022_9 = DataWin_isVersionAtLeast(dw, 2022, 9, 0, 0);
    if (count >= 2 && hasGeneratedMips && !has2022_9 && ptrs[0] != 0 && ptrs[1] != 0) {
        uint32_t diff = ptrs[1] - ptrs[0];
        if (diff == 28) {
            DataWin_bumpVersionTo(dw, 2022, 9, 0, 0);
            has2022_3 = true;
            has2022_9 = true;
        } else if (diff == 16 && !has2022_3) {
            DataWin_bumpVersionTo(dw, 2022, 3, 0, 0);
            has2022_3 = true;
        }
    }

    t->textures = (Texture *)safeCalloc(count, sizeof(Texture));
    repeat(count, i) {
        if (ptrs[i] == 0) { t->textures[i].blobOffset = 0; t->textures[i].blobData = nullptr; continue; }
        BinaryReader_seek(reader, ptrs[i]);
        t->textures[i].present = true;
        t->textures[i].scaled = BinaryReader_readUint32(reader);
        if (hasGeneratedMips) {
            t->textures[i].generatedMips = BinaryReader_readUint32(reader);
        } else {
            t->textures[i].generatedMips = 0;
        }
        if (has2022_3) {
            t->textures[i].textureBlockSize = BinaryReader_readUint32(reader);
        } else {
            t->textures[i].textureBlockSize = 0;
        }
        if (has2022_9) {
            t->textures[i].textureWidth = BinaryReader_readInt32(reader);
            t->textures[i].textureHeight = BinaryReader_readInt32(reader);
            t->textures[i].indexInGroup = BinaryReader_readInt32(reader);
        } else {
            t->textures[i].textureWidth = 0;
            t->textures[i].textureHeight = 0;
            t->textures[i].indexInGroup = 0;
        }
        t->textures[i].blobOffset = BinaryReader_readUint32(reader);
        t->textures[i].blobData = nullptr;
    }
    free(ptrs);

    // Compute blob sizes from successive offsets
    {
    repeat(count, i) {
        if (t->textures[i].blobOffset == 0) {
            t->textures[i].blobSize = 0; // external texture
            continue;
        }
        if (count > i + 1 && t->textures[i + 1].blobOffset != 0) {
            t->textures[i].blobSize = t->textures[i + 1].blobOffset - t->textures[i].blobOffset;
        } else {
            t->textures[i].blobSize = (uint32_t)(chunkEnd - t->textures[i].blobOffset);
        }
    }
    }

    // Load blob data into owned buffers
    if (!loadTextureDataLazily) {
        repeat(count, i) {
            if (t->textures[i].blobOffset == 0 || t->textures[i].blobSize == 0) continue;
            if (dw->mappedFile) {
                t->textures[i].blobData = dw->mappedFile + t->textures[i].blobOffset;
                t->textures[i].mapped = true;
            } else
                t->textures[i].blobData = BinaryReader_readBytesAt(reader, t->textures[i].blobOffset, t->textures[i].blobSize);
        }
    }
}

void DataWin_loadTxtrIfNeeded(DataWin* dw, uint32_t textureId) {
    Txtr* t = &dw->txtr;
    Texture* tex = &t->textures[textureId];

    if (tex->blobOffset == 0 || tex->blobSize == 0) return;
    if (tex->blobData != nullptr) return;

    if (!dw->lazyLoadFile) {
        logWarn("loadTxtrIfNeeded: called without a lazy load file.\n");
        return;
    }

    tex->blobData = (uint8_t *)safeMalloc(tex->blobSize);

    memset(tex->blobData, 0, tex->blobSize);
    long old_seek = ftell(dw->lazyLoadFile);
    fseek(dw->lazyLoadFile, tex->blobOffset, SEEK_SET);
    size_t read = fread(tex->blobData, 1, tex->blobSize, dw->lazyLoadFile);
    fseek(dw->lazyLoadFile, old_seek, SEEK_SET);

    if (read != tex->blobSize) {
        logWarn("loadTxtrIfNeeded: couldn't read %u bytes to load a texture.\n", tex->blobSize);
    }
}

static void parseAUDO(BinaryReader* reader, DataWin* dw, bool loadAudioDataLazily) {
    Audo* a = &dw->audo;

    uint32_t count;
    uint32_t* ptrs = readPointerTable(reader, &count);
    a->count = count;

    if (count == 0) { free(ptrs); a->entries = nullptr; return; }

    a->entries = (AudioEntry *)safeCalloc(count, sizeof(AudioEntry));
    repeat(count, i) {
        if (ptrs[i] == 0) continue;
        BinaryReader_seek(reader, ptrs[i]);
        a->entries[i].present = true;
        a->entries[i].dataSize = BinaryReader_readUint32(reader);
        a->entries[i].dataOffset = (uint32_t)BinaryReader_getPosition(reader);
        // Load audio data into owned buffer
        if (loadAudioDataLazily) {
            a->entries[i].data = nullptr;
        } else if (dw->mappedFile) {
            a->entries[i].data = dw->mappedFile + a->entries[i].dataOffset;
        } else if (a->entries[i].dataSize > 0) {
            a->entries[i].data = (uint8_t *)safeMalloc(a->entries[i].dataSize);
            BinaryReader_readBytes(reader, a->entries[i].data, a->entries[i].dataSize);
        } else {
            a->entries[i].data = nullptr;
        }
    }
    free(ptrs);
}

void DataWin_loadAudoIfNeeded(DataWin* dw, uint32_t audioEntryId) {
    Audo* a = &dw->audo;
    AudioEntry* entry = &a->entries[audioEntryId];

    if (!entry->present || entry->dataSize == 0) return;
    if (entry->data != nullptr) return;

    if (!dw->lazyLoadFile) {
        logError("loadAudoIfNeeded: called without a lazy load file.\n");
        return;
    }

    entry->data = (uint8_t *)safeMalloc(entry->dataSize);

    memset(entry->data, 0, entry->dataSize);
    long old_seek = ftell(dw->lazyLoadFile);
    fseek(dw->lazyLoadFile, entry->dataOffset, SEEK_SET);
    size_t read = fread(entry->data, 1, entry->dataSize, dw->lazyLoadFile);
    fseek(dw->lazyLoadFile, old_seek, SEEK_SET);

    if (read != entry->dataSize) {
        logError("loadAudoIfNeeded: couldn't read %u bytes to load audio entry %u.\n", entry->dataSize, audioEntryId);
    }
}

// ===[ MAIN PARSE FUNCTION ]===

DataWin* DataWin_parse(const char* filePath, DataWinParserOptions options) {
    FILE* file = fopen(filePath, "rb");
    if (!file) {
        logError("Failed to open file: %s\n", filePath);
        exit(1);
    }

    // Use a large read buffer to reduce the number of physical reads
    // This is critical for slow I/O devices like the PS2 CDVD drive, where each fread
    // call would otherwise trigger a separate disc read of just a few sectors
    setvbuf(file, nullptr, _IOFBF, 128 * 1024);

    fseek(file, 0, SEEK_END);
    long fileSizeRaw = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (0 >= fileSizeRaw) {
        logError("Invalid file size: %ld\n", fileSizeRaw);
        fclose(file);
        exit(1);
    }
    size_t fileSize = (size_t) fileSizeRaw;

    // Allocate and zero-initialize DataWin
    DataWin* dw = (DataWin *)safeCalloc(1, sizeof(DataWin));

    BinaryReader reader = BinaryReader_create(file, (size_t) fileSize);

    // Some WAD files, such as ones made with https://github.com/AlexWaveDiver/TranslaTale (I think?) have pointers inside a chunk pointing to data in OTHER chunks
    // The original runner doesn't care because it loads the entire file in memory up front, so we do the same if asked
    // (we don't do that by default because some low end platforms would NOT be able to handle it)
    uint8_t* wholeFileData = nullptr;
    if (options.loadType == DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME) {
        wholeFileData = (uint8_t *)safeMalloc((size_t) fileSize);
        safeFread(wholeFileData, fileSize, file, filePath);
        BinaryReader_setBuffer(&reader, wholeFileData, 0, (size_t) fileSize);
    } else if (options.loadType == DATAWINLOADTYPE_MAP_FILE) {
        wholeFileData = mapFile(file, fileSize);
        if (!wholeFileData) {
            logError("Failed to map file\n");
            fclose(file);
            exit(1);
        }
        BinaryReader_setBuffer(&reader, wholeFileData, 0, (size_t) fileSize);
        dw->mappedFile = wholeFileData;
    }

    // Validate FORM header
    char formMagic[4];
    BinaryReader_readBytes(&reader, formMagic, 4);
    // Some games may purposely corrupt the magic value so that UndertaleModTool doesn't open it
    // The native runner does not care about verifying the magic value, so we'll validate it and warn, but we won't exit
    if (memcmp(formMagic, "FORM", 4) != 0) {
        logWarn("The file does not have the expected FORM magic, got '%.4s'. The file may not be a WAD or it may have been tampered with!\n", formMagic);
    }

    uint32_t formLength = BinaryReader_readUint32(&reader);
    (void) formLength;

    // Pass 1: Count total chunks and find STRG chunk offset.
    // All other chunks reference strings from STRG, so it must be loaded first.
    // We also check if the CODE chunk exists.
    int totalChunks = 0;
    bool codeExists = false;
    BinaryReader_seek(&reader, 8); // reset to after FORM header

    while ((size_t) fileSize > BinaryReader_getPosition(&reader)) {
        if (BinaryReader_getPosition(&reader) + 8 > (size_t) fileSize) break;

        char chunkName[5] = {0};
        BinaryReader_readBytes(&reader, chunkName, 4);
        uint32_t chunkLength = BinaryReader_readUint32(&reader);
        size_t chunkDataStart = BinaryReader_getPosition(&reader);

        if (options.parseStrg && memcmp(chunkName, "STRG", 4) == 0) {
            dw->strgBufferBase = chunkDataStart;
            if (dw->mappedFile)
                dw->strgBuffer = dw->mappedFile + chunkDataStart;
            else
                dw->strgBuffer = BinaryReader_readBytesAt(&reader, chunkDataStart, chunkLength);
        }

        if ((memcmp(chunkName, "CODE", 4) == 0) && chunkLength > 0) {
            codeExists = true;
        }

        // Bump detected version based on chunk presence, so later chunks can use the right version during parsing (parseOBJT needs to know we're >= 2.3 to probe for the GMS 2022.5+ Managed field).
        if (memcmp(chunkName, "ACRV", 4) == 0 || memcmp(chunkName, "SEQN", 4) == 0 || memcmp(chunkName, "TAGS", 4) == 0) {
            DataWin_bumpVersionTo(dw, 2, 3, 0, 0);
        } else if (memcmp(chunkName, "FEDS", 4) == 0) {
            DataWin_bumpVersionTo(dw, 2, 3, 6, 0);
        } else if (memcmp(chunkName, "FEAT", 4) == 0) {
            DataWin_bumpVersionTo(dw, 2022, 8, 0, 0);
        } else if (memcmp(chunkName, "UILR", 4) == 0) {
            DataWin_bumpVersionTo(dw, 2024, 13, 0, 0);
        } else if (memcmp(chunkName, "PSEM", 4) == 0 || memcmp(chunkName, "PSYS", 4) == 0) {
            DataWin_bumpVersionTo(dw, 2023, 2, 0, 0);
        }

        if (chunkDataStart + chunkLength > fileSize) {
            logWarn("Chunk data extends beyond file size: chunkDataStart=%zu, chunkLength=%u, fileSize=%zu! Are you running a GameMaker Raspberry Pi game? Skipping bytes out of bounds...\n", chunkDataStart, chunkLength, fileSize);
            break;
        }

        BinaryReader_seek(&reader, chunkDataStart + chunkLength);
        totalChunks++;
    }

    if (!codeExists && options.parseCode) {
        logError("CODE chunk does not exist or is empty! This usually means you're loading a YYC game.\n");
        fclose(file);
        exit(1);
    }

    // Pass 2: Parse all chunks
    // For each chunk that will be parsed, we bulk-read the entire chunk into memory first
    // and then parse from the memory buffer. This dramatically reduces the number of physical
    // reads on slow I/O devices like the PS2 CDVD drive.
    BinaryReader_seek(&reader, 8); // skip past FORM header
    int chunkIndex = 0;
    while ((size_t) fileSize > BinaryReader_getPosition(&reader)) {
        if (BinaryReader_getPosition(&reader) + 8 > (size_t) fileSize) break;

        char chunkName[5] = {0};
        BinaryReader_readBytes(&reader, chunkName, 4);
        uint32_t chunkLength = BinaryReader_readUint32(&reader);
        size_t chunkDataStart = BinaryReader_getPosition(&reader);
        size_t chunkEnd = chunkDataStart + chunkLength;

        if (options.progressCallback) {
            options.progressCallback(chunkName, chunkIndex, totalChunks, dw, options.progressCallbackUserData);
        }

        // Determine if this chunk will be parsed (and thus needs bulk loading)
        bool shouldParse =
            (options.parseGen8 && memcmp(chunkName, "GEN8", 4) == 0) ||
            (options.parseOptn && memcmp(chunkName, "OPTN", 4) == 0) ||
            (options.parseLang && memcmp(chunkName, "LANG", 4) == 0) ||
            (options.parseExtn && memcmp(chunkName, "EXTN", 4) == 0) ||
            (options.parseSond && memcmp(chunkName, "SOND", 4) == 0) ||
            (options.parseAgrp && memcmp(chunkName, "AGRP", 4) == 0) ||
            (options.parseSprt && memcmp(chunkName, "SPRT", 4) == 0) ||
            (options.parseBgnd && memcmp(chunkName, "BGND", 4) == 0) ||
            (options.parsePath && memcmp(chunkName, "PATH", 4) == 0) ||
            (options.parseScpt && memcmp(chunkName, "SCPT", 4) == 0) ||
            (options.parseGlob && memcmp(chunkName, "GLOB", 4) == 0) ||
            (options.parseShdr && memcmp(chunkName, "SHDR", 4) == 0) ||
            (options.parseFont && memcmp(chunkName, "FONT", 4) == 0) ||
            (options.parseTmln && memcmp(chunkName, "TMLN", 4) == 0) ||
            (options.parseObjt && memcmp(chunkName, "OBJT", 4) == 0) ||
            (options.parseRoom && memcmp(chunkName, "ROOM", 4) == 0) ||
            (options.parseTpag && memcmp(chunkName, "TPAG", 4) == 0) ||
            (options.parseCode && memcmp(chunkName, "CODE", 4) == 0) ||
            (options.parseVari && memcmp(chunkName, "VARI", 4) == 0) ||
            (options.parseFunc && memcmp(chunkName, "FUNC", 4) == 0) ||
            (options.parseStrg && memcmp(chunkName, "STRG", 4) == 0) ||
            (options.parseTxtr && memcmp(chunkName, "TXTR", 4) == 0) ||
            (options.parseAudo && memcmp(chunkName, "AUDO", 4) == 0) ||
            (memcmp(chunkName, "ACRV", 4) == 0);

        // Bulk-read the chunk data into memory for fast parsing
        uint8_t* chunkBuffer = nullptr;
        if (shouldParse && chunkLength > 0 && options.loadType == DATAWINLOADTYPE_LOAD_PER_CHUNK) {
            chunkBuffer = (uint8_t *)malloc(chunkLength);
            if (chunkBuffer) {
                size_t read = fread(chunkBuffer, 1, chunkLength, reader.file);
                if (read != chunkLength) {
                    logError("DataWin: short read on chunk %.4s (expected %u, got %zu)\n", chunkName, chunkLength, read);
                    exit(1);
                }
                BinaryReader_setBuffer(&reader, chunkBuffer, chunkDataStart, chunkLength);
            }
        }

        if (options.parseGen8 && memcmp(chunkName, "GEN8", 4) == 0) {
            parseGEN8(&reader, dw);
        } else if (options.parseOptn && memcmp(chunkName, "OPTN", 4) == 0) {
            parseOPTN(&reader, dw);
        } else if (options.parseLang && memcmp(chunkName, "LANG", 4) == 0) {
            parseLANG(&reader, dw);
        } else if (options.parseExtn && memcmp(chunkName, "EXTN", 4) == 0) {
            parseEXTN(&reader, dw);
        } else if (options.parseSond && memcmp(chunkName, "SOND", 4) == 0) {
            parseSOND(&reader, dw);
        } else if (options.parseAgrp && memcmp(chunkName, "AGRP", 4) == 0) {
            parseAGRP(&reader, dw);
        } else if (options.parseSprt && memcmp(chunkName, "SPRT", 4) == 0) {
            parseSPRT(&reader, dw, options.skipLoadingPreciseMasksForNonPreciseSprites);
        } else if (options.parseBgnd && memcmp(chunkName, "BGND", 4) == 0) {
            parseBGND(&reader, dw, chunkEnd);
        } else if (options.parsePath && memcmp(chunkName, "PATH", 4) == 0) {
            parsePATH(&reader, dw);
        } else if (options.parseScpt && memcmp(chunkName, "SCPT", 4) == 0) {
            parseSCPT(&reader, dw);
        } else if (options.parseGlob && memcmp(chunkName, "GLOB", 4) == 0) {
            parseGLOB(&reader, dw);
        } else if (options.parseShdr && memcmp(chunkName, "SHDR", 4) == 0) {
            parseSHDR(&reader, dw);
        } else if (options.parseFont && memcmp(chunkName, "FONT", 4) == 0) {
            parseFONT(&reader, dw);
        } else if (options.parseTmln && memcmp(chunkName, "TMLN", 4) == 0) {
            parseTMLN(&reader, dw);
        } else if (options.parseObjt && memcmp(chunkName, "OBJT", 4) == 0) {
            parseOBJT(&reader, dw);
        } else if (options.parseRoom && memcmp(chunkName, "ROOM", 4) == 0) {
            parseROOM(&reader, dw, options.lazyLoadRooms, options.eagerlyLoadedRooms);
        } else if (memcmp(chunkName, "DAFL", 4) == 0) {
            // Empty chunk, nothing to parse
        } else if (memcmp(chunkName, "EMBI", 4) == 0) {
            // Embedded Images chunk
        } else if (memcmp(chunkName, "TGIN", 4) == 0) {
            // Texture Group Info chunk (wadVersion >= 17)
        } else if (memcmp(chunkName, "ACRV", 4) == 0) {
            // Animation Curves chunk (GMS 2.3+)
            DataWin_bumpVersionTo(dw, 2, 3, 0, 0);
            parseACRV(&reader, dw);
        } else if (memcmp(chunkName, "SEQN", 4) == 0) {
            // Sequences chunk (GMS 2.3+)
            DataWin_bumpVersionTo(dw, 2, 3, 0, 0);
        } else if (memcmp(chunkName, "TAGS", 4) == 0) {
            // Tags chunk (GMS 2.3+)
            DataWin_bumpVersionTo(dw, 2, 3, 0, 0);
        } else if (memcmp(chunkName, "FEDS", 4) == 0) {
            // Filter Effects Data chunk (GMS 2.3.6+)
            DataWin_bumpVersionTo(dw, 2, 3, 6, 0);
        } else if (options.parseTpag && memcmp(chunkName, "TPAG", 4) == 0) {
            parseTPAG(&reader, dw);
        } else if (options.parseCode && memcmp(chunkName, "CODE", 4) == 0) {
            parseCODE(&reader, dw, chunkLength, chunkDataStart);
        } else if (options.parseVari && memcmp(chunkName, "VARI", 4) == 0) {
            parseVARI(&reader, dw, chunkLength);
        } else if (options.parseFunc && memcmp(chunkName, "FUNC", 4) == 0) {
            parseFUNC(&reader, dw, chunkLength);
        } else if (options.parseStrg && memcmp(chunkName, "STRG", 4) == 0) {
            parseSTRG(&reader, dw);
        } else if (options.parseTxtr && memcmp(chunkName, "TXTR", 4) == 0) {
            parseTXTR(&reader, dw, chunkEnd, options.lazyLoadTextures);
        } else if (options.parseAudo && memcmp(chunkName, "AUDO", 4) == 0) {
            parseAUDO(&reader, dw, options.lazyLoadAudio);
        } else {
            logInfo("Unknown chunk: %.4s (length %u at offset 0x%zX)\n", chunkName, chunkLength, chunkDataStart - 8);
        }

        // Free the chunk buffer and revert to FILE*-based reads for the next header
        if (chunkBuffer != nullptr) {
            BinaryReader_clearBuffer(&reader);
            free(chunkBuffer);
        }

        // Seek to chunk end (skip any unread data or trailing padding)
        if (options.loadType != DATAWINLOADTYPE_LOAD_PER_CHUNK) {
            BinaryReader_seek(&reader, chunkEnd);
        } else {
            fseek(reader.file, (long) chunkEnd, SEEK_SET);
        }
        chunkIndex++;
    }

    // GMS2: apply default FPS to rooms with speed=0
    if (dw->gen8.gms2FPS > 0) {
        repeat(dw->room.count, i) {
            if (dw->room.rooms[i].speed == 0) {
                dw->room.rooms[i].speed = (uint32_t) dw->gen8.gms2FPS;
            }
        }
    }

    // If lazy-loading rooms, keep the file handle open for DataWin_loadRoomPayload, otherwise close it now
    dw->lazyLoadRooms = options.lazyLoadRooms;
    dw->lazyLoadTextures = options.lazyLoadTextures;
    dw->lazyLoadAudio = options.lazyLoadAudio;
    if (options.lazyLoadRooms || options.lazyLoadTextures || options.lazyLoadAudio) {
        dw->lazyLoadFile = file;
        dw->fileSize = (size_t) fileSize;
    } else {
        dw->lazyLoadFile = nullptr;
        dw->fileSize = 0;
        fclose(file);
    }

    if (options.loadType == DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME)
        free(wholeFileData);

    return dw;
}

// ===[ FREE ]===

void DataWin_free(DataWin* dw) {
    if (!dw) return;

    // GEN8
    free(dw->gen8.roomOrder);

    // OPTN
    free(dw->optn.constants);

    // LANG
    free(dw->lang.entryIds);
    if (dw->lang.languages) {
        repeat(dw->lang.languageCount, i) {
            free(dw->lang.languages[i].entries);
        }
        free(dw->lang.languages);
    }

    // EXTN
    if (dw->extn.extensions) {
        repeat(dw->extn.count, i) {
            Extension* ext = &dw->extn.extensions[i];
            if (ext->files) {
                repeat(ext->fileCount, j) {
                    ExtensionFile* file = &ext->files[j];
                    if (file->functions) {
                        repeat(file->functionCount, k) {
                            free(file->functions[k].arguments);
                        }
                        free(file->functions);
                    }
                }
                free(ext->files);
            }
        }
        free(dw->extn.extensions);
    }

    // SOND
    free(dw->sond.sounds);

    // AGRP
    free(dw->agrp.audioGroups);

    // SPRT
    if (dw->sprt.sprites) {
        repeat(dw->sprt.count, i) {
            free(dw->sprt.sprites[i].tpagIndices);
            if (dw->sprt.sprites[i].masks != nullptr) {
                if (!dw->mappedFile) {
                    repeat(dw->sprt.sprites[i].maskCount, j) {
                        free(dw->sprt.sprites[i].masks[j]);
                    }
                }
                free(dw->sprt.sprites[i].masks);
            }
            // Runtime-allocated sprites (indices >= parsedCount) own their synthesized name
            if (i >= dw->sprt.parsedCount) free((char*) dw->sprt.sprites[i].name);
        }
        free(dw->sprt.sprites);
    }


    // BGND
    if (dw->bgnd.backgrounds) {
        repeat(dw->bgnd.count, i) {
            free(dw->bgnd.backgrounds[i].gms2TileIds);
        }
    }
    free(dw->bgnd.backgrounds);

    // PATH
    if (dw->path.paths) {
        repeat(dw->path.count, i) {
            free(dw->path.paths[i].points);
            free(dw->path.paths[i].internalPoints);
        }
        free(dw->path.paths);
    }

    // SCPT
    free(dw->scpt.scripts);

    // GLOB
    free(dw->glob.codeIds);

    // SHDR
    if (dw->shdr.shaders) {
        repeat(dw->shdr.count, i) {
            free(dw->shdr.shaders[i].vertexAttributes);
        }
        free(dw->shdr.shaders);
    }

    // FONT
    if (dw->font.fonts) {
        repeat(dw->font.count, i) {
            Font* font = &dw->font.fonts[i];
            if (font->glyphs) {
                repeat(font->glyphCount, j) {
                    free(font->glyphs[j].kerning);
                }
                free(font->glyphs);
            }
        }
        free(dw->font.fonts);
    }

    // TMLN
    if (dw->tmln.timelines) {
        repeat(dw->tmln.count, i) {
            Timeline* tl = &dw->tmln.timelines[i];
            if (tl->moments) {
                repeat(tl->momentCount, j) {
                    free(tl->moments[j].actions);
                }
                free(tl->moments);
            }
        }
        free(dw->tmln.timelines);
    }

    // OBJT
    if (dw->objt.objects) {
        repeat(dw->objt.count, i) {
            GameObject* obj = &dw->objt.objects[i];
            free(obj->physicsVertices);
            repeat(OBJT_EVENT_TYPE_COUNT, e) {
                ObjectEventList* list = &obj->eventLists[e];
                if (list->events) {
                    repeat(list->eventCount, j) {
                        free(list->events[j].actions);
                    }
                    free(list->events);
                }
            }
        }
        free(dw->objt.objects);
    }

    // ACRV
    if (dw->acrv.curves) {
        repeat(dw->acrv.count, i) {
            AnimCurve* cur = &dw->acrv.curves[i];
            if (cur->channels) {
                repeat(cur->channelCount, c) {
                    free(cur->channels[c].points);
                }
                free(cur->channels);
            }
        }
        free(dw->acrv.curves);
    }
    free(dw->acrv.allChannels);

    // ROOM
    if (dw->room.rooms) {
        repeat(dw->room.count, i) {
            DataWin_freeRoomPayload(&dw->room.rooms[i]);
        }
        free(dw->room.rooms);
    }

    // TPAG
    free(dw->tpag.items);

    // CODE
    free(dw->code.entries);

    // VARI
    free(dw->vari.variables);

    // FUNC
    free(dw->func.functions);
    if (dw->func.codeLocals) {
        repeat(dw->func.codeLocalsCount, i) {
            free(dw->func.codeLocals[i].locals);
        }
        free(dw->func.codeLocals);
    }

    // STRG
    free(dw->strg.strings);

    // TXTR
    if (dw->txtr.textures) {
        repeat(dw->txtr.count, i) {
            if (!dw->txtr.textures[i].mapped)
                free(dw->txtr.textures[i].blobData);
        }
        free(dw->txtr.textures);
    }

    // AUDO
    if (dw->audo.entries) {
        if (!dw->mappedFile) {
            repeat(dw->audo.count, i) {
                free(dw->audo.entries[i].data);
            }
        }
        free(dw->audo.entries);
    }

    // Owned buffers
    if (!dw->mappedFile)
        free(dw->strgBuffer);
    free(dw->bytecodeBuffer);

    // Close the lazy-load file handle (only open when lazyLoadRooms/lazyLoadTextures was enabled)
    if (dw->lazyLoadFile != nullptr) {
        fclose(dw->lazyLoadFile);
        dw->lazyLoadFile = nullptr;
    }

    unmapFile(dw->mappedFile, dw->fileSize);

    free(dw);
}

// ===[ Lazy Room Payload ]===

void DataWin_freeRoomPayload(Room* room) {
    requireNotNull(room);
    free(room->backgrounds);
    room->backgrounds = nullptr;
    free(room->views);
    room->views = nullptr;
    free(room->gameObjects);
    room->gameObjects = nullptr;
    room->gameObjectCount = 0;
    free(room->tiles);
    room->tiles = nullptr;
    room->tileCount = 0;
    if (room->layerCount != 0 && room->layers != nullptr) {
        repeat(room->layerCount, j) {
            RoomLayer* layer = &room->layers[j];
            if (layer->assetsData) {
                free(layer->assetsData->legacyTiles);
                free(layer->assetsData->sprites);
                free(layer->assetsData);
            }
            if (layer->backgroundData) free(layer->backgroundData);
            if (layer->instancesData) {
                free(layer->instancesData->instanceIds);
                free(layer->instancesData);
            }
            if (layer->tilesData) {
                free(layer->tilesData->tileData);
                free(layer->tilesData);
            }
        }
    }
    free(room->layers);
    room->layers = nullptr;
    room->layerCount = 0;
    room->payloadLoaded = false;
}

void DataWin_loadRoomPayload(DataWin* dw, int32_t roomIndex) {
    require(roomIndex >= 0 && dw->room.count > (uint32_t) roomIndex);
    Room* room = &dw->room.rooms[roomIndex];
    if (room->payloadLoaded) return;
    requireMessage(dw->lazyLoadFile != nullptr, "DataWin_loadRoomPayload called without an open lazy-load FILE*");

    FILE* f = dw->lazyLoadFile;
    BinaryReader lazyReader = BinaryReader_create(f, dw->fileSize);
    readRoomPayload(&lazyReader, dw, room);
}

// ===[ Dynamic Sprite Slot Allocation ]===

uint32_t DataWin_allocSpriteSlot(DataWin* dw, uint32_t startIndex) {
    uint32_t newIndex;
    for (uint32_t i = startIndex; dw->sprt.count > i; i++) {
        if (dw->sprt.sprites[i].textureCount == 0) {
            newIndex = i;
            goto assignName;
        }
    }
    newIndex = dw->sprt.count;
    dw->sprt.count++;
    dw->sprt.sprites = (Sprite *)safeRealloc(dw->sprt.sprites, dw->sprt.count * sizeof(Sprite));
    memset(&dw->sprt.sprites[newIndex], 0, sizeof(Sprite));
assignName:
    // Match the native runner: set a "__newsprite<N>" name so asset_get_index can find it.
    // A reused slot preserves its name across glDeleteSprite's memset, so we only strdup when the slot is freshly appended (name is still NULL).
    if (!dw->sprt.sprites[newIndex].name) {
        char buf[32];
        snprintf(buf, sizeof(buf), "__newsprite%u", newIndex);
        dw->sprt.sprites[newIndex].name = safeStrdup(buf);
    }
    return newIndex;
}

// ===[ Version Detection ]===

bool DataWin_isVersionAtLeast(const DataWin* dw, uint32_t major, uint32_t minor, uint32_t release, uint32_t build) {
    const DetectedFormat* f = &dw->detectedFormat;
    if (f->major != major) return f->major > major;
    if (f->minor != minor) return f->minor > minor;
    if (f->release != release) return f->release > release;
    return f->build >= build;
}

void DataWin_bumpVersionTo(DataWin* dw, uint32_t major, uint32_t minor, uint32_t release, uint32_t build) {
    if (DataWin_isVersionAtLeast(dw, major, minor, release, build)) return;
    dw->detectedFormat.major = major;
    dw->detectedFormat.minor = minor;
    dw->detectedFormat.release = release;
    dw->detectedFormat.build = build;
}
