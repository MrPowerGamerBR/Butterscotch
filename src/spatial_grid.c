#include "spatial_grid.h"

#include "collision.h"
#include "instance.h"
#include "runner.h"
#include "utils.h"

// Forward declarations
typedef struct Runner Runner;

SpatialGrid* SpatialGrid_create(uint32_t roomWidth, uint32_t roomHeight) {
    SpatialGrid* grid = safeCalloc(1, sizeof(SpatialGrid));

    // +1 to avoid truncation
    uint32_t gridWidth = (roomWidth / SPATIAL_GRID_CELL_SIZE) + 1;
    uint32_t gridHeight = (roomHeight / SPATIAL_GRID_CELL_SIZE) + 1;

#ifdef ENABLE_SPATIAL_GRID_LOGS
    fprintf(stderr, "SpatialGrid: Grid size: %dx%d\n", gridWidth, gridHeight);
#endif
    grid->gridWidth = gridWidth;
    grid->gridHeight = gridHeight;

    grid->grid = safeCalloc(gridWidth * gridHeight, sizeof(Instance**));

    return grid;
}

void SpatialGrid_free(SpatialGrid* grid) {
    int32_t totalCells = grid->gridWidth * grid->gridHeight;
    repeat(totalCells, i) {
        arrfree(grid->grid[i]);
    }
    free(grid->grid);
    free(grid);
}

static void removeInstanceFromGridCells(SpatialGrid* grid, Instance* instance) {
    repeat(arrlen(instance->collisionCells), i) {
        uint32_t gridCoordinates = instance->collisionCells[i];
        int32_t gridX = SpatialGrid_unpackGridX(gridCoordinates);
        int32_t gridY = SpatialGrid_unpackGridY(gridCoordinates);
        int32_t cellIndex = SpatialGrid_cellIndex(grid, gridX, gridY);
        repeat(arrlen(grid->grid[cellIndex]), j) {
            if (grid->grid[cellIndex][j] == instance) {
                arrdel(grid->grid[cellIndex], j);
                break;
            }
        }
    }
}

void SpatialGrid_syncGrid(Runner* runner, SpatialGrid* grid) {
    bool requiresResync = arrlen(grid->dirtyInstances);
    if (!requiresResync) return;

#ifdef ENABLE_SPATIAL_GRID_LOGS
    fprintf(stderr, "SpatialGrid: Syncing grid with %d dirty instances\n", arrlen(grid->dirtyInstances));
#endif

    repeat(arrlen(grid->dirtyInstances), i) {
        int32_t instanceId = grid->dirtyInstances[i];
        Instance* instance = hmget(runner->instancesToId, instanceId);

        // We do not care about removed/inactive/destroyed instances, because they would've been already been removed from the grid on the "SpatialGrid_markInstanceAsDirty" call
        // We also do not care if the spatial grid is not dirty
        if (instance == nullptr || !instance->active || instance->destroyed || !instance->spatialGridDirty)
            continue;

        instance->spatialGridDirty = false;

        // Remove from old cells
        removeInstanceFromGridCells(grid, instance);

        InstanceBBox bbox = Collision_computeBBox(runner->dataWin, instance);

        arrsetlen(instance->collisionCells, 0);

        if (!bbox.valid)
            continue;

        SpatialGridRange range = SpatialGrid_computeCellRange(grid, bbox.left, bbox.top, bbox.right, bbox.bottom);

        for (int32_t gx = range.minGridX; range.maxGridX >= gx; gx++) {
            for (int32_t gy = range.minGridY; range.maxGridY >= gy; gy++) {
                arrput(grid->grid[SpatialGrid_cellIndex(grid, gx, gy)], instance);
                arrput(instance->collisionCells, SpatialGrid_packGridCoordinates(gx, gy));
            }
        }

        // And that's it for now!
    }

    arrsetlen(grid->dirtyInstances, 0);
}

void SpatialGrid_markInstanceAsDirty(SpatialGrid* grid, Instance* dirtyInstance) {
    if (!dirtyInstance->active || dirtyInstance->destroyed) {
        // Destroyed instances are updated instantly because, if we didn't, we would need to track the ID + all grids that the instance is in
        removeInstanceFromGridCells(grid, dirtyInstance);
        return;
    }

    if (dirtyInstance->spatialGridDirty)
        return;

    dirtyInstance->spatialGridDirty = true;

    // You may be thinking "why don't we store the Instance pointer?"
    // Well, it is because the Instance* may not be valid when SpatialGrid_syncGrid is ran
    arrput(grid->dirtyInstances, dirtyInstance->instanceId);
}
