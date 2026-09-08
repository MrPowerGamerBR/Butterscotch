#include "spatial_grid.h"

#include "collision.h"
#include "instance.h"
#include "runner.h"
#include "utils.h"

SpatialGrid* SpatialGrid_create(uint32_t roomWidth, uint32_t roomHeight) {
    SpatialGrid* grid = (SpatialGrid *)safeCalloc(1, sizeof(SpatialGrid));

    // +1 to avoid truncation
    uint32_t gridWidth = (roomWidth / SPATIAL_GRID_CELL_SIZE) + 1;
    uint32_t gridHeight = (roomHeight / SPATIAL_GRID_CELL_SIZE) + 1;

#ifdef ENABLE_SPATIAL_GRID_LOGS
    logInfo("SpatialGrid: Grid size: %dx%d\n", gridWidth, gridHeight);
#endif
    grid->gridWidth = gridWidth;
    grid->gridHeight = gridHeight;

    grid->grid = (Instance ***)safeCalloc(gridWidth * gridHeight, sizeof(Instance**));

    return grid;
}

void SpatialGrid_free(SpatialGrid* grid) {
    int32_t totalCells = grid->gridWidth * grid->gridHeight;
    repeat(totalCells, i) {
        arrfree(grid->grid[i]);
    }
    free(grid->grid);
    arrfree(grid->dirtyInstances);
    free(grid);
}

void SpatialGrid_removeInstance(SpatialGrid* grid, Instance* instance) {
    int32_t totalCells = (int32_t)grid->gridWidth * (int32_t)grid->gridHeight;
    bool removedAny = false;

    // First remove from any cells named in this instance's cached tracking data.
    repeat(arrlen(instance->collisionCells), i) {
        uint32_t gridCoordinates = instance->collisionCells[i];
        int32_t gridX = SpatialGrid_unpackGridX(gridCoordinates);
        int32_t gridY = SpatialGrid_unpackGridY(gridCoordinates);
        int32_t cellIndex = SpatialGrid_cellIndex(grid, gridX, gridY);
        if (cellIndex < 0 || cellIndex >= totalCells) continue;

        Instance** cell = grid->grid[cellIndex];
        int32_t cellLen = (int32_t) arrlen(cell);
        for (int32_t j = 0; j < cellLen;) {
            if (cell[j] == instance) {
                if (j < cellLen - 1) memmove(&cell[j], &cell[j + 1], (size_t) (cellLen - 1 - j) * sizeof(Instance*));
                arrsetlen(cell, cellLen - 1);
                removedAny = true;
                cellLen--;
            } else {
                j++;
            }
        }
    }

    // Some room transitions and destructions can leave stale grid entries even when
    // collisionCells was cleared or never populated for the current grid. Fall back to
    // a full-grid scan so the instance pointer is not left dangling in the spatial hash.
    repeat(totalCells, cellIndex) {
        Instance** cell = grid->grid[cellIndex];
        int32_t cellLen = (int32_t) arrlen(cell);
        for (int32_t j = 0; j < cellLen;) {
            if (cell[j] == instance) {
                if (j < cellLen - 1) memmove(&cell[j], &cell[j + 1], (size_t) (cellLen - 1 - j) * sizeof(Instance*));
                arrsetlen(cell, cellLen - 1);
                removedAny = true;
                cellLen--;
            } else {
                j++;
            }
        }
    }

    if (removedAny) {
        arrsetlen(instance->collisionCells, 0);
        instance->spatialGridDirty = false;
    }
}

void SpatialGrid_syncGrid(Runner* runner, SpatialGrid* grid) {
    bool requiresResync = arrlen(grid->dirtyInstances);
    if (!requiresResync) return;

#ifdef ENABLE_SPATIAL_GRID_LOGS
    logInfo("SpatialGrid: Syncing grid with %d dirty instances\n", arrlen(grid->dirtyInstances));
#endif

    repeat(arrlen(grid->dirtyInstances), i) {
        int32_t instanceId = grid->dirtyInstances[i];
        Instance* instance = hmget(runner->instancesById, instanceId);

        // We do not care about removed/inactive/destroyed instances, because they would've been already been removed from the grid on the "SpatialGrid_markInstanceAsDirty" call
        // We also do not care if the spatial grid is not dirty
        if (instance == nullptr || !instance->active || instance->destroyed || !instance->spatialGridDirty)
            continue;

        instance->spatialGridDirty = false;

        // Remove from old cells
        SpatialGrid_removeInstance(grid, instance);

        InstanceBBox bbox = Collision_computeBBox(runner, instance);

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
    // Structs should NOT be included in the spatial grid!
    if (dirtyInstance->objectIndex == STRUCT_OBJECT_INDEX)
        return;

    if (!dirtyInstance->active || dirtyInstance->destroyed) {
        // Destroyed instances are updated instantly because, if we didn't, we would need to track the ID + all grids that the instance is in
        // Guard against stale entries left behind by room transitions or earlier clears of collisionCells.
        SpatialGrid_removeInstance(grid, dirtyInstance);
        return;
    }

    if (dirtyInstance->spatialGridDirty)
        return;

    dirtyInstance->spatialGridDirty = true;

    // You may be thinking "why don't we store the Instance pointer?"
    // Well, it is because the Instance* may not be valid when SpatialGrid_syncGrid is ran
    arrput(grid->dirtyInstances, dirtyInstance->instanceId);
}

SpatialGridQuery SpatialGrid_prepareQuery(Runner* runner, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, int32_t target) {
    // We do let INSTANCE_ALL through
    requireMessageFormatted(__FILE__, __LINE__, target >= 0 || target == INSTANCE_ALL, "SpatialGrid: [%s] Query target cannot be instance type %d!", runner->vmContext->currentCodeName, target);

    SpatialGridRange callerRange = SpatialGrid_computeCellRange(runner->spatialGrid, x1, y1, x2, y2);
    bool filterByObject = target >= 0 && INSTANCE_ID_BASE > target;
    bool filterByInstanceId = target >= INSTANCE_ID_BASE;
    uint32_t queryId = ++runner->collisionQueryCounter;
    SpatialGridQuery ret = {0};
    ret.range = callerRange;
    ret.filterByObject = filterByObject;
    ret.filterByInstanceId = filterByInstanceId;
    ret.matchAll = target == INSTANCE_ALL;
    ret.queryId = queryId;
    return ret;
}
