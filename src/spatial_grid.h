#ifndef _BS_SPATIAL_GRID_H_
#define _BS_SPATIAL_GRID_H_
#include <stdint.h>

// Forward declarations
#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif

#include "instance.h"
#include "real_type.h"

#define SPATIAL_GRID_CELL_SIZE 64

// Used for collisions
typedef struct {
    // The grid used for collisions, each instance stays in the grid
    // The "end" of the grid is a stb_ds array
    int16_t gridWidth;
    int16_t gridHeight;
    int32_t* dirtyInstances;
    // Flat 2D grid of Instance* stb_ds arrays
    Instance*** grid;
} SpatialGrid;

static inline int32_t SpatialGrid_cellIndex(SpatialGrid* grid, int32_t x, int32_t y) {
    return (y * grid->gridWidth) + x;
}

static inline uint32_t SpatialGrid_packGridCoordinates(uint16_t gridX, uint16_t gridY) {
    return ((uint32_t) gridX << 16) | gridY;
}

static inline uint16_t SpatialGrid_unpackGridX(uint32_t gridPosition) {
    return (uint16_t) (gridPosition >> 16);
}

static inline uint16_t SpatialGrid_unpackGridY(uint32_t gridPosition) {
    return (uint16_t) (gridPosition & 0xFFFF);
}

typedef struct {
    uint16_t minGridX;
    uint16_t minGridY;
    uint16_t maxGridX;
    uint16_t maxGridY;
} SpatialGridRange;

static inline SpatialGridRange SpatialGrid_computeCellRange(SpatialGrid* grid, GMLReal left, GMLReal top, GMLReal right, GMLReal bottom) {
    int16_t minGridX = (int16_t) (left / SPATIAL_GRID_CELL_SIZE);
    int16_t minGridY = (int16_t) (top / SPATIAL_GRID_CELL_SIZE);
    int16_t maxGridX = (int16_t) (right / SPATIAL_GRID_CELL_SIZE);
    int16_t maxGridY = (int16_t) (bottom / SPATIAL_GRID_CELL_SIZE);
    if (0 > minGridX) minGridX = 0;
    if (0 > minGridY) minGridY = 0;
    if (0 > maxGridX) maxGridX = 0;
    if (0 > maxGridY) maxGridY = 0;
    if (minGridX > grid->gridWidth - 1) minGridX = grid->gridWidth - 1;
    if (minGridY > grid->gridHeight - 1) minGridY = grid->gridHeight - 1;
    if (maxGridX > grid->gridWidth - 1) maxGridX = grid->gridWidth - 1;
    if (maxGridY > grid->gridHeight - 1) maxGridY = grid->gridHeight - 1;
    SpatialGridRange ret;
    ret.minGridX = minGridX;
    ret.minGridY = minGridY;
    ret.maxGridX = maxGridX;
    ret.maxGridY = maxGridY;
    return ret;
}

static inline bool SpatialGrid_instanceOverlapsRange(Instance* instance, SpatialGridRange range) {
    repeat(arrlen(instance->collisionCells), i) {
        uint16_t cellX = SpatialGrid_unpackGridX(instance->collisionCells[i]);
        uint16_t cellY = SpatialGrid_unpackGridY(instance->collisionCells[i]);
        if (cellX >= range.minGridX && range.maxGridX >= cellX && cellY >= range.minGridY && range.maxGridY >= cellY)
            return true;
    }
    return false;
}

SpatialGrid* SpatialGrid_create(uint32_t gridWidth, uint32_t gridHeight);
void SpatialGrid_free(SpatialGrid* grid);
void SpatialGrid_removeInstance(SpatialGrid* grid, Instance* instance);

void SpatialGrid_syncGrid(Runner* runner, SpatialGrid* grid);
void SpatialGrid_markInstanceAsDirty(SpatialGrid* grid, Instance* instance);

typedef struct {
    SpatialGridRange range;
    bool filterByObject;
    bool filterByInstanceId;
    bool matchAll;
    uint32_t queryId;
} SpatialGridQuery;

SpatialGridQuery SpatialGrid_prepareQuery(Runner* runner, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, int32_t target);

#endif /* _BS_SPATIAL_GRID_H_ */
