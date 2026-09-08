#ifndef _BS_COLLISION_H_
#define _BS_COLLISION_H_

#include "common.h"
#include "data_win.h"
#include "instance.h"
#include "runner.h"
#include "vm.h"

#include "math_compat.h"

// Checks if an instance matches a collision target.
// target >= INSTANCE_ID_BASE: instance ID (match specific instance)
// target == INSTANCE_ALL (-3): match any instance
// target >= 0 && < INSTANCE_ID_BASE: object index (match via parent chain)
static inline bool Collision_matchesTarget(DataWin* dataWin, Instance* inst, int32_t target) {
    if (target >= INSTANCE_ID_BASE) return inst->instanceId == (uint32_t) target;
    if (target == INSTANCE_ALL) return true;
    return VM_isObjectOrDescendant(dataWin, inst->objectIndex, target);
}

typedef struct {
    GMLReal left, right, top, bottom;
    bool valid;
} InstanceBBox;

// Returns the collision sprite for an instance (mask sprite if set, else display sprite)
static inline Sprite* Collision_getSprite(DataWin* dataWin, Instance* inst) {
    int32_t sprIdx = (inst->maskIndex >= 0) ? inst->maskIndex : inst->spriteIndex;
    if (0 > sprIdx || (uint32_t) sprIdx >= dataWin->sprt.count) return nullptr;
    return &dataWin->sprt.sprites[sprIdx];
}

// Computes the axis-aligned bounding box for an instance using its collision sprite
static inline InstanceBBox Collision_computeBBox(Runner* runner, Instance* inst) {
    InstanceBBox ret;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (spr == nullptr) {
        ZERO_STRUCT(ret);
        return ret;
    }

    GMLReal marginL = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginLeft;
    GMLReal marginR = (spr->bboxMode == 1) ? (GMLReal) spr->width : (GMLReal) (spr->marginRight + 1);
    GMLReal marginT = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginTop;
    GMLReal marginB = (spr->bboxMode == 1) ? (GMLReal) spr->height : (GMLReal) (spr->marginBottom + 1);
    GMLReal originX = (GMLReal) spr->originX;
    GMLReal originY = (GMLReal) spr->originY;

    GMLReal left, right, top, bottom;
    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        // Compute rotated AABB: transform the 4 corners of the unrotated bbox
        GMLReal rad = inst->imageAngle * M_PI / 180.0;
        GMLReal cs = GMLReal_cos(rad);
        GMLReal sn = GMLReal_sin(rad);

        // Local-space corners relative to origin, scaled
        GMLReal lx0 = inst->imageXscale * (marginL - originX);
        GMLReal ly0 = inst->imageYscale * (marginT - originY);
        GMLReal lx1 = inst->imageXscale * (marginR - originX);
        GMLReal ly1 = inst->imageYscale * (marginB - originY);

        // Rotate all 4 corners (CW rotation matching renderer's negated angle for Y-down screen coords)
        GMLReal cx[4], cy[4];
        cx[0] = cs * lx0 + sn * ly0;  cy[0] = -sn * lx0 + cs * ly0;
        cx[1] = cs * lx1 + sn * ly0;  cy[1] = -sn * lx1 + cs * ly0;
        cx[2] = cs * lx0 + sn * ly1;  cy[2] = -sn * lx0 + cs * ly1;
        cx[3] = cs * lx1 + sn * ly1;  cy[3] = -sn * lx1 + cs * ly1;

        GMLReal minX = cx[0], maxX = cx[0], minY = cy[0], maxY = cy[0];
        for (int c = 1; 4 > c; c++) {
            if (minX > cx[c]) minX = cx[c];
            if (cx[c] > maxX) maxX = cx[c];
            if (minY > cy[c]) minY = cy[c];
            if (cy[c] > maxY) maxY = cy[c];
        }

        left   = inst->x + minX;
        right  = inst->x + maxX;
        top    = inst->y + minY;
        bottom = inst->y + maxY;
    } else {
        // No rotation fast path
        left   = inst->x + inst->imageXscale * (marginL - originX);
        right  = inst->x + inst->imageXscale * (marginR - originX);
        top    = inst->y + inst->imageYscale * (marginT - originY);
        bottom = inst->y + inst->imageYscale * (marginB - originY);

        // Normalize if negative scale
        if (left > right) { GMLReal tmp = left; left = right; right = tmp; }
        if (top > bottom) { GMLReal tmp = top; top = bottom; bottom = tmp; }
    }

    if (runner->collisionCompatibilityMode) {
        left   = GMLReal_bankersRound(left);
        top    = GMLReal_bankersRound(top);
        right  = GMLReal_bankersRound(right);
        bottom = GMLReal_bankersRound(bottom);
    }

    ret.left = left;
    ret.right = right;
    ret.top = top;
    ret.bottom = bottom;
    ret.valid = true;
    return ret;
}

static inline bool Collision_hasFrameMasks(Sprite* sprite) {
    return sprite != nullptr && sprite->sepMasks == 1 && sprite->masks != nullptr && sprite->maskCount > 0;
}

// Oriented bounding box for a sprite-bearing instance.
// Local rect is the sprite's collision-margin rectangle, scaled by image_xscale/yscale; (cs, sn) is the rotation that takes local-space points to world-space relative to the instance origin (matches Collision_computeBBox: world = inst.pos + (cs*lx + sn*ly, -sn*lx + cs*ly)).
typedef struct {
    GMLReal x, y; // World position (instance origin).
    GMLReal lx0, lx1; // Local rect X extents (lx0 <= lx1).
    GMLReal ly0, ly1; // Local rect Y extents (ly0 <= ly1).
    GMLReal cs, sn; // cos/sin of imageAngle (in radians).
    bool rotated; // true if abs(imageAngle) > epsilon.
} InstanceOBB;

static inline InstanceOBB Collision_instanceOBB(Sprite* spr, Instance* inst) {
    InstanceOBB obb;
    obb.x = inst->x;
    obb.y = inst->y;
    GMLReal marginL = spr->bboxMode == 1 ? 0.0 : (GMLReal) spr->marginLeft;
    GMLReal marginR = spr->bboxMode == 1 ? (GMLReal) spr->width : (GMLReal) (spr->marginRight + 1);
    GMLReal marginT = spr->bboxMode == 1 ? 0.0 : (GMLReal) spr->marginTop;
    GMLReal marginB = spr->bboxMode == 1 ? (GMLReal) spr->height : (GMLReal) (spr->marginBottom + 1);
    GMLReal originX = (GMLReal) spr->originX;
    GMLReal originY = (GMLReal) spr->originY;
    obb.lx0 = inst->imageXscale * (marginL - originX);
    obb.lx1 = inst->imageXscale * (marginR - originX);
    obb.ly0 = inst->imageYscale * (marginT - originY);
    obb.ly1 = inst->imageYscale * (marginB - originY);
    if (obb.lx0 > obb.lx1) { GMLReal t = obb.lx0; obb.lx0 = obb.lx1; obb.lx1 = t; }
    if (obb.ly0 > obb.ly1) { GMLReal t = obb.ly0; obb.ly0 = obb.ly1; obb.ly1 = t; }
    obb.rotated = GMLReal_fabs(inst->imageAngle) > 0.0001;
    if (obb.rotated) {
        GMLReal rad = inst->imageAngle * M_PI / 180.0;
        obb.cs = GMLReal_cos(rad);
        obb.sn = GMLReal_sin(rad);
    } else {
        obb.cs = 1.0;
        obb.sn = 0.0;
    }
    return obb;
}

// Inverse-transforms a world point into OBB local coordinates.
static inline void Collision_obbWorldToLocal(const InstanceOBB* obb, GMLReal wx, GMLReal wy, GMLReal* outLx, GMLReal* outLy) {
    GMLReal dx = wx - obb->x;
    GMLReal dy = wy - obb->y;
    *outLx = dx * obb->cs - dy * obb->sn;
    *outLy = dx * obb->sn + dy * obb->cs;
}

// Returns true iff the OBB needs SAT-style testing rather than AABB. Only sepMasks == 2 sprites that are actually rotated qualify; everything else (axis-aligned, or precise sprites which fall through to per-pixel scans) is handled correctly by AABB.
static inline bool Collision_obbNeedsSAT(Sprite* spr, Instance* inst) {
    return spr != nullptr && spr->sepMasks == 2 && GMLReal_fabs(inst->imageAngle) > 0.0001;
}

static inline bool Collision_rectOverlapsInstance(Runner* runner, Instance* inst, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;

    if (rx1 >= bbox.right || bbox.left > rx2 || ry1 >= bbox.bottom || bbox.top > ry2) return false;

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) return true;

    // OBB-vs-AABB SAT for sepMasks==2 with rotation. Native uses SeparatingAxisCollisionBox here.
    InstanceOBB obb = Collision_instanceOBB(spr, inst);

    // Project the 4 world-rect corners onto the OBB's local axes; if they don't overlap the local rect on either axis, no collision.
    GMLReal corners[4][2] = { {rx1, ry1}, {rx2, ry1}, {rx1, ry2}, {rx2, ry2} };
    GMLReal uMin = 0, uMax = 0, vMin = 0, vMax = 0;
    repeat(4, c) {
        GMLReal pu, pv;
        Collision_obbWorldToLocal(&obb, corners[c][0], corners[c][1], &pu, &pv);
        if (c == 0) { uMin = uMax = pu; vMin = vMax = pv; }
        else {
            if (pu < uMin) uMin = pu; else if (pu > uMax) uMax = pu;
            if (pv < vMin) vMin = pv; else if (pv > vMax) vMax = pv;
        }
    }
    if (uMin >= obb.lx1 || obb.lx0 >= uMax) return false;
    if (vMin >= obb.ly1 || obb.ly0 >= vMax) return false;
    return true;
}

// Tests whether a world point lies inside the instance's collision rect (margins, rotated/scaled). Cheaper and more correct than Collision_pointInInstance for sepMasks != 1, since point_in_instance bounds-checks against the full sprite texture rather than the bbox margins.
static inline bool Collision_pointInsideInstanceBox(Runner* runner, Instance* inst, GMLReal px, GMLReal py) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;
    if (bbox.left > px || px >= bbox.right || bbox.top > py || py >= bbox.bottom) return false;

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) return true;

    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lx, ly;
    Collision_obbWorldToLocal(&obb, px, py, &lx, &ly);
    return lx >= obb.lx0 && obb.lx1 > lx && ly >= obb.ly0 && obb.ly1 > ly;
}

// Circle (cx, cy, radius) vs instance collision rect. Falls back to circle-vs-AABB when the instance isn't a rotated sepMasks==2 sprite.
static inline bool Collision_circleOverlapsInstance(Runner* runner, Instance* inst, GMLReal cx, GMLReal cy, GMLReal radius) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;
    GMLReal rSq = radius * radius;

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) {
        // Closest point on AABB to circle center.
        GMLReal closestX = cx;
        if (bbox.left > closestX) closestX = bbox.left;
        if (closestX > bbox.right) closestX = bbox.right;
        GMLReal closestY = cy;
        if (bbox.top > closestY) closestY = bbox.top;
        if (closestY > bbox.bottom) closestY = bbox.bottom;
        GMLReal dx = closestX - cx, dy = closestY - cy;
        return dx * dx + dy * dy <= rSq;
    }

    // Loose AABB pre-pass.
    GMLReal qx1 = cx - radius, qy1 = cy - radius, qx2 = cx + radius, qy2 = cy + radius;
    if (qx1 >= bbox.right || bbox.left >= qx2 || qy1 >= bbox.bottom || bbox.top >= qy2) return false;

    // Transform circle center into OBB local frame; clamp to local rect; squared distance.
    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lx, ly;
    Collision_obbWorldToLocal(&obb, cx, cy, &lx, &ly);
    GMLReal closestX = lx;
    if (obb.lx0 > closestX) closestX = obb.lx0;
    if (closestX > obb.lx1) closestX = obb.lx1;
    GMLReal closestY = ly;
    if (obb.ly0 > closestY) closestY = obb.ly0;
    if (closestY > obb.ly1) closestY = obb.ly1;
    GMLReal dx = closestX - lx, dy = closestY - ly;
    return dx * dx + dy * dy <= rSq;
}

// Tests whether world point (px, py) lies inside the ellipse defined by opposite corners (x1,y1)-(x2,y2).
static inline bool Collision_pointInEllipse(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, GMLReal px, GMLReal py) {
    GMLReal cx = (x1 + x2) * 0.5;
    GMLReal cy = (y1 + y2) * 0.5;
    GMLReal rx = (x2 - x1) * 0.5;
    GMLReal ry = (y2 - y1) * 0.5;
    GMLReal a = (px - cx) / rx;
    GMLReal b = (py - cy) / ry;
    return a * a + b * b <= 1.0;
}

// Ellipse (defined by opposite corners) vs instance collision shape.
// Mirrors GameMaker-HTML5's yyInstance.Collision_Ellipse flow: AABB rejection, degenerate
// (zero width/height) fallback to the rectangle test, nearest-corner PtInEllipse rejection,
// and ellipse-vs-OBB SAT for rotated sepMasks==2 sprites.
static inline bool Collision_ellipseOverlapsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;

    // Native rounds the ellipse corners in compatibility mode, before computing the centre/radii.
    if (runner->collisionCompatibilityMode) {
        x1 = GMLReal_bankersRound(x1);
        y1 = GMLReal_bankersRound(y1);
        x2 = GMLReal_bankersRound(x2);
        y2 = GMLReal_bankersRound(y2);
    }

    GMLReal minX = GMLReal_fmin(x1, x2);
    GMLReal maxX = GMLReal_fmax(x1, x2);
    GMLReal minY = GMLReal_fmin(y1, y2);
    GMLReal maxY = GMLReal_fmax(y1, y2);

    // Reject when the ellipse box and the instance bbox are separated.
    if (minX >= bbox.right || maxX < bbox.left || minY >= bbox.bottom || maxY < bbox.top) return false;

    // Degenerate ellipse (a line)
    if (minX == maxX || minY == maxY) return Collision_rectOverlapsInstance(runner, inst, minX, minY, maxX, maxY);

    GMLReal cx = (x1 + x2) * 0.5;
    GMLReal cy = (y1 + y2) * 0.5;
    GMLReal rx = (x2 - x1) * 0.5;
    GMLReal ry = (y2 - y1) * 0.5;

    if (!(bbox.left <= cx && bbox.right >= cx) &&
        !(bbox.top <= cy && bbox.bottom >= cy)) {
        GMLReal px = (bbox.right <= cx) ? bbox.right : bbox.left;
        GMLReal py = (bbox.bottom <= cy) ? bbox.bottom : bbox.top;
        if (!Collision_pointInEllipse(x1, y1, x2, y2, px, py)) return false;
    }

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) return true;

    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lcx, lcy;
    Collision_obbWorldToLocal(&obb, cx, cy, &lcx, &lcy);

    GMLReal axes[4][2] = {
        { 1.0, 0.0 },
        { 0.0, 1.0 },
        { obb.cs, obb.sn },
        { -obb.sn, obb.cs }
    };
    GMLReal corners[4][2] = {
        { obb.lx0, obb.ly0 }, { obb.lx1, obb.ly0 },
        { obb.lx0, obb.ly1 }, { obb.lx1, obb.ly1 }
    };

    repeat(4, axIdx) {
        GMLReal nx = axes[axIdx][0], ny = axes[axIdx][1];
        GMLReal rMin = corners[0][0] * nx + corners[0][1] * ny;
        GMLReal rMax = rMin;
        {
        for (int c = 1; 4 > c; c++) {
            GMLReal p = corners[c][0] * nx + corners[c][1] * ny;
            if (p < rMin) rMin = p;
            else if (p > rMax) rMax = p;
        }
        }
        GMLReal du = nx * obb.cs + ny * obb.sn;
        GMLReal dv = -nx * obb.sn + ny * obb.cs;
        GMLReal halfLen = GMLReal_sqrt(rx * rx * du * du + ry * ry * dv * dv);
        GMLReal c = lcx * nx + lcy * ny;
        if (c + halfLen <= rMin || c - halfLen >= rMax) return false;
    }
    return true;
}

// Liang-Barsky clip of a parametric line p(t) = p1 + t*(p2-p1), t in [0,1], against an axis-aligned rect [rx1,rx2] x [ry1,ry2].
// Returns true if the segment intersects the rect, and writes the clipped parametric range to *outTEnter/*outTExit.
static inline bool Collision_segmentVsAARectClip(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2, GMLReal* outTEnter, GMLReal* outTExit) {
    GMLReal tEnter = 0.0, tExit = 1.0;
    GMLReal dx = x2 - x1, dy = y2 - y1;
    GMLReal p[4] = { -dx, dx, -dy, dy };
    GMLReal q[4] = { x1 - rx1, rx2 - x1, y1 - ry1, ry2 - y1 };
    for (int i = 0; 4 > i; i++) {
        if (GMLReal_fabs(p[i]) < 1e-9) {
            if (q[i] < 0) return false;
            continue;
        }
        GMLReal t = q[i] / p[i];
        if (p[i] < 0) {
            if (t > tEnter) tEnter = t;
        } else {
            if (t < tExit) tExit = t;
        }
        if (tEnter > tExit) return false;
    }
    *outTEnter = tEnter;
    *outTExit = tExit;
    return true;
}

static inline bool Collision_segmentVsAARect(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2) {
    GMLReal tEnter, tExit;
    return Collision_segmentVsAARectClip(x1, y1, x2, y2, rx1, ry1, rx2, ry2, &tEnter, &tExit);
}

// Line segment (x1,y1)-(x2,y2) vs instance collision rect.
static inline bool Collision_lineOverlapsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;

    // Epsilon from GameMaker-HTML5, we apply it because the BBox is "exclusive" (outside of the sprite) but we need to put the right/bottom INSIDE of the sprite
    // So we nudge it to be inside on it
    GMLReal eps = 1e-5;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) {
        return Collision_segmentVsAARect(x1, y1, x2, y2, bbox.left, bbox.top, bbox.right - eps, bbox.bottom - eps);
    }

    // For rotated OBB: transform line endpoints into local frame and clip against local rect.
    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lx1, ly1, lx2, ly2;
    Collision_obbWorldToLocal(&obb, x1, y1, &lx1, &ly1);
    Collision_obbWorldToLocal(&obb, x2, y2, &lx2, &ly2);
    return Collision_segmentVsAARect(lx1, ly1, lx2, ly2, obb.lx0, obb.ly0, obb.lx1 - eps, obb.ly1 - eps);
}

// Tests if world point (px, py) is inside the given instance's collision shape.
// The point is inverse-transformed into sprite-local coords (translation, rotation, inverse scale, origin) and bounds-checked against the full sprite texture [0, spr.width) x [0, spr.height).
// Precise sprites (sepMasks == 1) additionally require the mask bit at the resulting local pixel to be set.
static inline bool Collision_pointInInstance(Sprite* spr, Instance* inst, GMLReal px, GMLReal py) {
    if (spr == nullptr) return false;

    // Reject degenerate scales to avoid divide-by-zero.
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;

    // Transform world coords to sprite-local coords
    GMLReal dx = px - inst->x;
    GMLReal dy = py - inst->y;

    // Inverse of CW rotation is standard CCW rotation (positive angle)
    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        GMLReal rad = inst->imageAngle * M_PI / 180.0;
        GMLReal cs = GMLReal_cos(rad);
        GMLReal sn = GMLReal_sin(rad);
        GMLReal rx = cs * dx - sn * dy;
        GMLReal ry = sn * dx + cs * dy;
        dx = rx;
        dy = ry;
    }

    // Inverse scale + add origin
    GMLReal localX = dx / inst->imageXscale + (GMLReal) spr->originX;
    GMLReal localY = dy / inst->imageYscale + (GMLReal) spr->originY;

    int32_t ix = (int32_t) localX;
    int32_t iy = (int32_t) localY;

    // Bounds check
    if (0 > ix || 0 > iy || ix >= (int32_t) spr->width || iy >= (int32_t) spr->height) return false;

    if (Collision_hasFrameMasks(spr)) {
        // Pick mask for current frame
        uint32_t frameIdx = ((uint32_t) inst->imageIndex) % spr->maskCount;
        uint8_t* mask = spr->masks[frameIdx];
        // Masks are stored at maskWidth x maskHeight starting at (maskOffsetX, maskOffsetY) in sprite-local space.
        // Pre-2024.6 this is the full sprite with zero offset; 2024.6+ it is the bounding box.
        int32_t mx = ix - spr->maskOffsetX;
        int32_t my = iy - spr->maskOffsetY;
        if (0 > mx || 0 > my || mx >= (int32_t) spr->maskWidth || my >= (int32_t) spr->maskHeight) return false;
        uint32_t bytesPerRow = (spr->maskWidth + 7) / 8;
        return (mask[my * bytesPerRow + (mx >> 3)] & (1 << (7 - (mx & 7)))) != 0;
    }

    return true;
}

// Returns true if the two instances' collision shapes overlap.
//
// Matches the native GMS 1.4 runner's flow in FUN_0043fde0:
//   1. AABB overlap test on the two precomputed bboxes.
//   2. If neither sprite is precise (sepMasks == 1), the AABB overlap is enough.
//   3. Otherwise walk the pixel intersection and test BOTH instances on every
//      pixel via Collision_pointInInstance. Both sides get inverse-transformed
//      regardless of whether they're individually precise, so a rotated
//      non-precise sprite collides as an OBB as long as its partner is precise.
static inline bool Collision_instancesOverlapPrecise(Runner* runner, Instance* a, Instance* b, InstanceBBox bboxA, InstanceBBox bboxB) {
    bool compatMode = runner->collisionCompatibilityMode;
    DataWin* dataWin = runner->dataWin;
    // Compute world-space intersection of the two AABBs
    GMLReal iLeft   = GMLReal_fmax(bboxA.left, bboxB.left);
    GMLReal iRight  = GMLReal_fmin(bboxA.right, bboxB.right);
    GMLReal iTop    = GMLReal_fmax(bboxA.top, bboxB.top);
    GMLReal iBottom = GMLReal_fmin(bboxA.bottom, bboxB.bottom);

    // AABB overlap test. Native uses identical semantics in both modern and compat for axis-aligned integer-coordinate cases (compat shifts bbox.right/bottom by -1 *and* the test by +1, which cancel).
    if (iLeft >= iRight || iTop >= iBottom) return false;

    Sprite* sprA = Collision_getSprite(dataWin, a);
    Sprite* sprB = Collision_getSprite(dataWin, b);
    if (sprA == nullptr || sprB == nullptr) return false;

    bool preciseA = Collision_hasFrameMasks(sprA);
    bool preciseB = Collision_hasFrameMasks(sprB);

    // Neither sprite precise? Then we need to check if either side is a rotated sepMasks==2 sprite, in which case the loose AABB engulfs space the rotated rect doesn't, and we need OBB-vs-OBB SAT to match native SeparatingAxisCollisionBox.
    if (!preciseA && !preciseB) {
        bool needSatA = Collision_obbNeedsSAT(sprA, a);
        bool needSatB = Collision_obbNeedsSAT(sprB, b);
        if (!needSatA && !needSatB) return true;

        InstanceOBB obbA = Collision_instanceOBB(sprA, a);
        InstanceOBB obbB = Collision_instanceOBB(sprB, b);

        // Compute the 4 world corners of each OBB. Indices: 0=(lx0,ly0), 1=(lx1,ly0), 2=(lx0,ly1), 3=(lx1,ly1).
        GMLReal ax[4], ay[4], bx[4], by[4];
        GMLReal lxA[2] = {obbA.lx0, obbA.lx1}, lyA[2] = {obbA.ly0, obbA.ly1};
        GMLReal lxB[2] = {obbB.lx0, obbB.lx1}, lyB[2] = {obbB.ly0, obbB.ly1};
        repeat(2, i) {
            repeat(2, j) {
                int k = j * 2 + i;
                ax[k] = obbA.x + obbA.cs * lxA[i] + obbA.sn * lyA[j];
                ay[k] = obbA.y - obbA.sn * lxA[i] + obbA.cs * lyA[j];
                bx[k] = obbB.x + obbB.cs * lxB[i] + obbB.sn * lyB[j];
                by[k] = obbB.y - obbB.sn * lxB[i] + obbB.cs * lyB[j];
            }
        }

        // SAT: 4 axes (each OBB has 2 unique edge normals). axes[0]/[1] are A's local-x/local-y in world space, axes[2]/[3] are B's.
        GMLReal axes[4][2] = {
            { obbA.cs, -obbA.sn },
            { obbA.sn,  obbA.cs },
            { obbB.cs, -obbB.sn },
            { obbB.sn,  obbB.cs }
        };
        repeat(4, axIdx) {
            GMLReal nx = axes[axIdx][0], ny = axes[axIdx][1];
            GMLReal aMin = ax[0]*nx + ay[0]*ny, aMax = aMin;
            GMLReal bMin = bx[0]*nx + by[0]*ny, bMax = bMin;
            for (int j = 1; 4 > j; j++) {
                GMLReal pa = ax[j]*nx + ay[j]*ny;
                GMLReal pb = bx[j]*nx + by[j]*ny;
                if (pa < aMin) aMin = pa; else if (pa > aMax) aMax = pa;
                if (pb < bMin) bMin = pb; else if (pb > bMax) bMax = pb;
            }
            if (aMax <= bMin || bMax <= aMin) return false;
        }
        return true;
    }

    // Pixel scan over the AABB intersection.
    // Modern: floor..ceil with exclusive upper bound, sample pixel centers (+0.5).
    // Compatibility: truncated int range with inclusive upper bound, sample pixel corners (no +0.5).
    int32_t startX, endX, startY, endY;
    GMLReal sampleOffset;
    if (compatMode) {
        startX = (int32_t) iLeft;
        endX   = (int32_t) iRight;
        startY = (int32_t) iTop;
        endY   = (int32_t) iBottom;
        sampleOffset = 0.0;
    } else {
        startX = (int32_t) GMLReal_floor(iLeft);
        endX   = (int32_t) GMLReal_ceil(iRight);
        startY = (int32_t) GMLReal_floor(iTop);
        endY   = (int32_t) GMLReal_ceil(iBottom);
        sampleOffset = 0.5;
    }

    for (int32_t py = startY; (compatMode ? py <= endY : py < endY); py++) {
        for (int32_t px = startX; (compatMode ? px <= endX : px < endX); px++) {
            GMLReal wpx = (GMLReal) px + sampleOffset;
            GMLReal wpy = (GMLReal) py + sampleOffset;

            if (!Collision_pointInInstance(sprA, a, wpx, wpy)) continue;
            if (!Collision_pointInInstance(sprB, b, wpx, wpy)) continue;
            return true;
        }
    }

    return false;
}

#endif /* _BS_COLLISION_H_ */
