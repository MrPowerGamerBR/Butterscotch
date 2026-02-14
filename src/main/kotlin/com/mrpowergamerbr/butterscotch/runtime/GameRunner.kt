package com.mrpowergamerbr.butterscotch.runtime

import com.mrpowergamerbr.butterscotch.Butterscotch
import com.mrpowergamerbr.butterscotch.data.GameData
import com.mrpowergamerbr.butterscotch.data.RoomData
import com.mrpowergamerbr.butterscotch.graphics.Renderer
import com.mrpowergamerbr.butterscotch.vm.GMLValue
import com.mrpowergamerbr.butterscotch.vm.VM
import kotlin.math.*
import kotlin.random.Random

class GameRunner(
    val gameData: GameData,
    val vm: VM,
    val renderer: Renderer,
    val initialRngSeed: Long,
    val alwaysUseInitialRngSeed: Boolean
) {
    val instances = mutableListOf<Instance>()
    val globalVariables = mutableMapOf<String, GMLValue>()

    var currentRoomIndex = -1
    var currentRoom: RoomData? = null
    var pendingRoomGoto = -1
    var nextInstanceId = 100000

    // Room persistence: when room_persistent = true, leaving a room saves its state
    val roomPersistentFlags = mutableMapOf<Int, Boolean>()  // roomIndex -> persistent flag
    val savedRoomStates = mutableMapOf<Int, List<Instance>>()  // roomIndex -> saved instances

    // Input state
    var keyboardKey = 0
    var keyboardLastKey = 0
    val keysHeld = mutableSetOf<Int>()
    val keysPressed = mutableSetOf<Int>()
    val keysReleased = mutableSetOf<Int>()
    var mouseX = 0.0
    var mouseY = 0.0

    var fps = 30
    var shouldQuit = false
    var gameStarted = false
    var frameCount = 0

    // Input recording/playback
    var inputRecording: MutableMap<Int, List<Int>>? = null
    var inputPlayback: Map<Int, List<Int>>? = null
    private var previousPlaybackKeys = emptySet<Int>()

    var random = Random(initialRngSeed)

    // Current event context for event_inherited() support
    data class EventContext(
        val eventType: Int,
        val subtype: Int,
        val objectIndex: Int  // The object that owns the currently executing event
    )
    var currentEventContext: EventContext? = null

    // Event type indices (matching GM:S event system)
    companion object {
        const val EVENT_CREATE = 0
        const val EVENT_DESTROY = 1
        const val EVENT_ALARM = 2
        const val EVENT_STEP = 3
        const val EVENT_COLLISION = 4
        const val EVENT_KEYBOARD = 5
        const val EVENT_MOUSE = 6
        const val EVENT_OTHER = 7
        const val EVENT_DRAW = 8
        const val EVENT_KEYPRESS = 9
        const val EVENT_KEYRELEASE = 10
        const val EVENT_TRIGGER = 11

        // Other event subtypes
        const val OTHER_ROOM_START = 4
        const val OTHER_ROOM_END = 5
        const val OTHER_GAME_START = 2
        const val OTHER_GAME_END = 3
        const val OTHER_ANIMATION_END = 7
        const val OTHER_OUTSIDE_ROOM = 0
    }

    fun initialize(startRoomOverride: Int? = null) {
        vm.runner = this

        // Load first room (or the overridden start room)
        val firstRoom = startRoomOverride ?: (gameData.gen8.roomOrder.firstOrNull() ?: 0)
        gotoRoom(firstRoom)
    }

    fun step() {
        frameCount++

        // Playback: override all input state from recording
        val inputPlayback = this.inputPlayback
        if (inputPlayback != null) {
            val currentKeys = (inputPlayback[frameCount] ?: emptyList()).toSet()
            keysPressed.clear()
            keysReleased.clear()
            keysHeld.clear()
            for (k in currentKeys) {
                if (k !in previousPlaybackKeys) keysPressed.add(k)
            }
            for (k in previousPlaybackKeys) {
                if (k !in currentKeys) keysReleased.add(k)
            }
            keysHeld.addAll(currentKeys)
            if (keysPressed.isNotEmpty()) {
                keyboardKey = keysPressed.last()
                keyboardLastKey = keysPressed.last()
            }
            previousPlaybackKeys = currentKeys
        }

        // Record: save current input state
        if (keysHeld.isNotEmpty()) {
            inputRecording?.put(frameCount, keysHeld.toList().sorted())
        }

        // Handle pending room transition
        if (pendingRoomGoto >= 0) {
            performRoomTransition(pendingRoomGoto)
            pendingRoomGoto = -1
        }

        // Update xprevious/yprevious at step start (before any movement)
        for (inst in instances) {
            if (inst.destroyed) continue
            inst.xprevious = inst.x
            inst.yprevious = inst.y
        }

        // 1. Begin Step
        dispatchEvent(EVENT_STEP, 1)

        // 2. Alarms
        for (inst in ArrayList(instances)) {
            if (inst.destroyed) continue
            for (alarmIdx in 0 until 12) {
                if (inst.alarm[alarmIdx] >= 0) {
                    inst.alarm[alarmIdx]--
                    if (inst.alarm[alarmIdx] == 0) {
                        inst.alarm[alarmIdx] = -1
                        val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                        println("  [DEBUG] Alarm[$alarmIdx] fires for $objName (id=${inst.id}, frame=$frameCount)")
                        fireEvent(inst, EVENT_ALARM, alarmIdx)
                    }
                }
            }
        }

        // 3. Keyboard events
        for (key in keysHeld) {
            dispatchKeyEvent(EVENT_KEYBOARD, key)
        }
        for (key in keysPressed) {
            dispatchKeyEvent(EVENT_KEYPRESS, key)
        }
        for (key in keysReleased) {
            dispatchKeyEvent(EVENT_KEYRELEASE, key)
        }

        // 4. Step
        dispatchEvent(EVENT_STEP, 0)

        // 5. Collision events (with solid object handling)
        dispatchCollisionEvents()

        // 5b. Resolve solid overlaps: any instance that moved into a solid object
        // gets pushed back to xprevious/yprevious. In GameMaker, the solid flag
        // prevents other instances from overlapping solid objects.
        resolveSolidOverlaps()

        // 6. End Step
        dispatchEvent(EVENT_STEP, 2)

        // Apply path following
        updatePathFollowing()

        // Apply movement physics
        for (inst in instances) {
            if (inst.destroyed) continue
            // Apply gravity
            if (inst.gravity != 0.0) {
                inst.hspeed += inst.gravity * cos(Math.toRadians(inst.gravityDirection))
                inst.vspeed -= inst.gravity * sin(Math.toRadians(inst.gravityDirection))
                inst.speed = sqrt(inst.hspeed * inst.hspeed + inst.vspeed * inst.vspeed)
                inst.direction = (Math.toDegrees(atan2(-inst.vspeed, inst.hspeed)) + 360) % 360
            }
            // Apply friction
            if (inst.friction != 0.0 && inst.speed != 0.0) {
                val newSpeed = inst.speed - inst.friction
                if (newSpeed <= 0.0) {
                    inst.speed = 0.0; inst.hspeed = 0.0; inst.vspeed = 0.0
                } else {
                    inst.speed = newSpeed
                    inst.hspeed = newSpeed * cos(Math.toRadians(inst.direction))
                    inst.vspeed = -newSpeed * sin(Math.toRadians(inst.direction))
                }
            }
            // Apply velocity
            if (inst.hspeed != 0.0 || inst.vspeed != 0.0) {
                inst.x += inst.hspeed; inst.y += inst.vspeed
            }
        }

        // Check "Outside Room" event (Other 0)
        // GM fires this when the ENTIRE bounding box is outside the room
        val roomW = (currentRoom?.width ?: 0).toDouble()
        val roomH = (currentRoom?.height ?: 0).toDouble()
        for (inst in ArrayList(instances)) {
            if (inst.destroyed) continue
            val bbox = computeBBox(inst)
            val isOutside = if (bbox != null) {
                bbox.right < 0 || bbox.left > roomW || bbox.bottom < 0 || bbox.top > roomH
            } else {
                inst.x < 0 || inst.x > roomW || inst.y < 0 || inst.y > roomH
            }
            if (isOutside) {
                if (inst.hasBeenMarkedAsOutsideRoom) continue

                inst.hasBeenMarkedAsOutsideRoom = true
                fireEvent(inst, EVENT_OTHER, OTHER_OUTSIDE_ROOM)
            } else {
                inst.hasBeenMarkedAsOutsideRoom = false
            }
        }

        // Animate sprites
        for (inst in instances) {
            if (inst.destroyed) continue
            if (inst.spriteIndex >= 0 && inst.spriteIndex < gameData.sprites.size) {
                val sprite = gameData.sprites[inst.spriteIndex]
                if (sprite.tpagIndices.size > 1) {
                    inst.imageIndex += inst.imageSpeed
                    if (inst.imageIndex >= sprite.tpagIndices.size) {
                        inst.imageIndex -= sprite.tpagIndices.size
                        // Fire animation end event
                        fireEvent(inst, EVENT_OTHER, OTHER_ANIMATION_END)
                    }
                }
            }
        }

        // Remove destroyed instances
        instances.removeAll { it.destroyed }
    }

    fun clearPerFrameInput() {
        keysPressed.clear()
        keysReleased.clear()
    }

    fun draw() {
        val room = currentRoom ?: return

        // Set up view
        val hasEnabledView = room.views.any { it.enabled }

        if (hasEnabledView) {
            for ((viewIdx, view) in room.views.withIndex()) {
                if (!view.enabled) continue
                renderer.currentView = viewIdx
                // Clamp view to room bounds
                view.viewX = view.viewX.coerceIn(0, maxOf(0, room.width - view.viewW))
                view.viewY = view.viewY.coerceIn(0, maxOf(0, room.height - view.viewH))
                renderer.setView(view.viewX, view.viewY, view.viewW, view.viewH,
                    view.portX, view.portY, view.portW, view.portH)

                // Clear with room bg color
                if (viewIdx == 0 && room.drawBgColor) {
                    renderer.clear(room.bgColor)
                }

                // Draw room backgrounds (non-foreground)
                drawRoomBackgrounds(room, false)

                // Draw instances and tiles interleaved by depth
                drawInstancesAndTiles(room)

                // Draw foreground backgrounds
                drawRoomBackgrounds(room, true)

                // Draw debug overlays on top
                if (Butterscotch.drawPaths) drawPathOverlays(room)
                if (Butterscotch.drawMasks) drawMaskOverlays()
            }
        } else {
            renderer.currentView = 0
            renderer.setView(0, 0, room.width, room.height, 0, 0,
                gameData.gen8.windowWidth, gameData.gen8.windowHeight)

            if (room.drawBgColor) {
                renderer.clear(room.bgColor)
            }

            drawRoomBackgrounds(room, false)

            drawInstancesAndTiles(room)

            drawRoomBackgrounds(room, true)

            if (Butterscotch.drawPaths) drawPathOverlays(room)
            if (Butterscotch.drawMasks) drawMaskOverlays()
        }
    }

    private fun drawMaskOverlays() {
        val savedColor = renderer.drawColor
        val savedAlpha = renderer.drawAlpha

        for (inst in instances) {
            if (inst.destroyed) continue
            val spriteIdx = if (inst.maskIndex >= 0) inst.maskIndex else inst.spriteIndex
            val sprite = gameData.sprites.getOrNull(spriteIdx) ?: continue
            val isPrecise = sprite.collisionMaskType == 1 && sprite.masks.isNotEmpty()

            if (isPrecise) {
                // Draw per-pixel mask in blue (BGR: 0xFF0000)
                val frameIndex = inst.imageIndex.toInt()
                val maskIdx = if (sprite.masks.size > 1) frameIndex % sprite.masks.size else 0
                val mask = sprite.masks[maskIdx]
                renderer.drawMaskPixels(
                    mask, sprite.width, sprite.height,
                    sprite.originX, sprite.originY,
                    inst.x, inst.y,
                    inst.imageXscale, inst.imageYscale,
                    0f, 0f, 1f, 0.5f
                )
            } else {
                // Draw bbox rectangle in green (BGR: 0x00FF00)
                val bbox = computeBBox(inst) ?: continue
                renderer.drawColor = 0x00FF00
                renderer.drawAlpha = 0.5
                renderer.drawRectangle(bbox.left, bbox.top, bbox.right, bbox.bottom, false)
            }
        }

        renderer.drawColor = savedColor
        renderer.drawAlpha = savedAlpha
    }

    private fun drawPathOverlays(room: RoomData) {
        val drawnPaths = mutableSetOf<Int>()

        for (inst in instances) {
            if (inst.destroyed || inst.pathIndex < 0) continue
            val pathIndex = inst.pathIndex
            val path = gameData.paths.getOrNull(pathIndex) ?: continue

            // Draw the full path line (once per unique path index)
            if (pathIndex !in drawnPaths) {
                drawnPaths.add(pathIndex)
                val pts = path.points
                if (pts.size >= 2) {
                    val linePoints = mutableListOf<Pair<Double, Double>>()
                    for (pt in pts) {
                        linePoints.add(Pair(pt.x.toDouble(), pt.y.toDouble()))
                    }
                    if (path.isClosed) {
                        linePoints.add(Pair(pts[0].x.toDouble(), pts[0].y.toDouble()))
                    }
                    // Draw path line in green
                    renderer.drawLineStrip(linePoints, 0f, 1f, 0f, 0.8f)
                }

                // Draw point markers as small rectangles
                val savedColor = renderer.drawColor
                val savedAlpha = renderer.drawAlpha
                renderer.drawColor = 0x00FF00  // Green in BGR
                renderer.drawAlpha = 0.9
                for (pt in pts) {
                    renderer.drawRectangle(
                        pt.x.toDouble() - 2, pt.y.toDouble() - 2,
                        pt.x.toDouble() + 2, pt.y.toDouble() + 2,
                        false
                    )
                }
                renderer.drawColor = savedColor
                renderer.drawAlpha = savedAlpha
            }

            // Draw current position marker in red
            val pos = interpolatePathPosition(pathIndex, inst.pathPosition)
            if (pos != null) {
                val cx = pos.first + inst.pathXOffset
                val cy = pos.second + inst.pathYOffset
                val savedColor = renderer.drawColor
                val savedAlpha = renderer.drawAlpha
                renderer.drawColor = 0x0000FF  // Red in BGR
                renderer.drawAlpha = 1.0
                renderer.drawRectangle(cx - 3, cy - 3, cx + 3, cy + 3, false)
                renderer.drawColor = savedColor
                renderer.drawAlpha = savedAlpha
            }
        }
    }

    private fun drawRoomBackgrounds(room: RoomData, foreground: Boolean) {
        for (bg in room.backgrounds) {
            if (!bg.enabled || bg.foreground != foreground || bg.bgDefIndex < 0) continue
            if (bg.bgDefIndex >= gameData.backgrounds.size) continue
            val bgDef = gameData.backgrounds[bg.bgDefIndex]
            if (bgDef.tpagIndex < 0) continue
            renderer.drawBackground(bgDef.tpagIndex, bg.x, bg.y, bg.tileX, bg.tileY)
        }
    }

    private fun drawInstancesAndTiles(room: RoomData) {
        // Group tiles by depth
        val tilesByDepth = room.tiles.groupBy { it.depth }

        // Collect all unique depths from instances and tiles
        val allDepths = mutableSetOf<Int>()
        for (inst in instances) {
            if (!inst.destroyed) allDepths.add(inst.depth)
        }
        allDepths.addAll(tilesByDepth.keys)

        // Sort descending (higher depth = drawn first = behind)
        val sortedDepths = allDepths.sortedDescending()

        for (depth in sortedDepths) {
            // Draw tiles at this depth
            val tiles = tilesByDepth[depth]
            if (tiles != null) {
                for (tile in tiles) {
                    drawTile(tile)
                }
            }
            // Draw instances at this depth
            val instancesDuringDraw = instances.toList()
            for (inst in instancesDuringDraw) {
                if (!inst.destroyed && inst.depth == depth) {
                    drawInstance(inst)
                }
            }
        }
    }

    private fun drawTile(tile: com.mrpowergamerbr.butterscotch.data.RoomTileData) {
        if (tile.bgDefIndex < 0 || tile.bgDefIndex >= gameData.backgrounds.size) return
        val bgDef = gameData.backgrounds[tile.bgDefIndex]
        if (bgDef.tpagIndex < 0) return
        renderer.drawTile(
            bgDef.tpagIndex, tile.x, tile.y,
            tile.sourceX, tile.sourceY,
            tile.width, tile.height,
            tile.scaleX, tile.scaleY,
            tile.color
        )
    }

    private fun drawInstance(inst: Instance) {
        if (!inst.visible) return

        // Debug logging removed

        // Walk parent chain to find Draw event
        var objIdx = inst.objectIndex
        while (objIdx >= 0 && objIdx < gameData.objects.size) {
            val objDef = gameData.objects[objIdx]
            val drawEvents = objDef.events.getOrNull(EVENT_DRAW)
            val drawEvent = drawEvents?.find { it.subtype == 0 }

            if (drawEvent != null && drawEvent.actions.isNotEmpty()) {
                val prevContext = currentEventContext
                currentEventContext = EventContext(EVENT_DRAW, 0, objIdx)
                val codeId = drawEvent.actions[0].codeId
                if (codeId >= 0) {
                    vm.executeCode(codeId, inst)
                }
                currentEventContext = prevContext
                return
            }

            objIdx = objDef.parentId
        }

        // Default draw (no Draw event found in chain)
        if (inst.spriteIndex >= 0) {
            renderer.drawSprite(inst.spriteIndex, inst.imageIndex.toInt(), inst.x, inst.y,
                inst.imageXscale, inst.imageYscale, inst.imageAngle,
                inst.imageBlend, inst.imageAlpha)
        }
    }

    fun dispatchEvent(eventType: Int, subtype: Int) {
        for (inst in ArrayList(instances)) {
            if (inst.destroyed) continue
            fireEvent(inst, eventType, subtype)
        }
    }

    private fun dispatchKeyEvent(eventType: Int, key: Int) {
        for (inst in ArrayList(instances)) {
            if (inst.destroyed) continue
            fireEvent(inst, eventType, key)
        }
    }

    private fun dispatchCollisionEvents() {
        val snapshot = ArrayList(instances)
        for (inst in snapshot) {
            if (inst.destroyed) continue
            var instBBox = computeBBox(inst) ?: continue
            val targets = collectCollisionTargets(inst.objectIndex)
            for (targetObjId in targets) {
                if (inst.destroyed) break
                for (other in snapshot) {
                    if (other.destroyed || other === inst) continue
                    if (other.objectIndex != targetObjId && !isChildOf(other.objectIndex, targetObjId)) continue
                    val otherBBox = computeBBox(other) ?: continue
                    // AABB overlap test (bbox values are exclusive-end on right/bottom)
                    if (instBBox.left < otherBBox.right && instBBox.right > otherBBox.left &&
                        instBBox.top < otherBBox.bottom && instBBox.bottom > otherBBox.top) {
                        // Precise mask check if either sprite uses precise collision
                        if (!checkPreciseOverlap(inst, other)) continue

                        // GM solid handling: before firing the collision event,
                        // move the non-solid instance back to its previous position.
                        // If other is solid, move inst back; if inst is solid, move other back.
                        if (other.solid) {
                            inst.x = inst.xprevious
                            inst.y = inst.yprevious
                            instBBox = computeBBox(inst) ?: break
                        }
                        if (inst.solid) {
                            other.x = other.xprevious
                            other.y = other.yprevious
                        }

                        fireEvent(inst, EVENT_COLLISION, targetObjId, other)
                        break // Only fire once per target object type per instance
                    }
                }
                // Recompute instBBox in case a collision event modified inst's position
                instBBox = computeBBox(inst) ?: break
            }
        }
    }

    /**
     * Resolve solid overlaps: if an instance overlaps a solid object,
     * move it back to xprevious/yprevious.
     *
     * In GameMaker, the solid flag prevents other instances from overlapping
     * solid objects. This catches cases where no collision event was defined
     * for the overlapping pair.
     */
    private fun resolveSolidOverlaps() {
        for (inst in instances) {
            if (inst.destroyed) continue
            // Only check instances that actually moved this frame
            if (inst.x == inst.xprevious && inst.y == inst.yprevious) continue

            val instBBox = computeBBox(inst) ?: continue

            for (other in instances) {
                if (other.destroyed || other === inst) continue
                if (!other.solid) continue

                val otherBBox = computeBBox(other) ?: continue
                if (instBBox.left < otherBBox.right && instBBox.right > otherBBox.left &&
                    instBBox.top < otherBBox.bottom && instBBox.bottom > otherBBox.top) {
                    if (!checkPreciseOverlap(inst, other)) continue
                    // Overlap with solid object - move back to previous position
                    inst.x = inst.xprevious
                    inst.y = inst.yprevious
                    break
                }
            }
        }
    }

    private fun collectCollisionTargets(objectIndex: Int): List<Int> {
        val targets = mutableListOf<Int>()
        var objIdx = objectIndex
        while (objIdx >= 0 && objIdx < gameData.objects.size) {
            val objDef = gameData.objects[objIdx]
            val collisionEvents = objDef.events.getOrNull(EVENT_COLLISION)
            if (collisionEvents != null) {
                for (ev in collisionEvents) {
                    if (ev.subtype !in targets) {
                        targets.add(ev.subtype)
                    }
                }
            }
            objIdx = objDef.parentId
        }
        return targets
    }

    fun fireEvent(inst: Instance, eventType: Int, subtype: Int, other: Instance? = null) {
        if (Butterscotch.traceFireEvents.isNotEmpty()) {
            val objectData = inst.getObjectData(vm)
            if (Butterscotch.traceFireEvents.contains("*") || objectData.name in Butterscotch.traceFireEvents) {
                println("[DEBUG] Firing event for ${objectData.name} (${inst.id}) with type=$eventType subtype=$subtype other=${other?.getObjectData(vm)?.name ?: "null"} at frame=$frameCount")
            }
        }

        // Walk the parent chain to find the first object that has this event
        var objIdx = inst.objectIndex
        while (objIdx >= 0 && objIdx < gameData.objects.size) {
            val objDef = gameData.objects[objIdx]
            val eventList = objDef.events.getOrNull(eventType)
            val event = eventList?.find { it.subtype == subtype }

            if (event != null) {
                val prevContext = currentEventContext
                currentEventContext = EventContext(eventType, subtype, objIdx)
                for (action in event.actions) {
                    if (action.codeId >= 0) {
                        if (frameCount <= 3) {
                            val codeName = if (action.codeId in gameData.codeEntries.indices) gameData.codeEntries[action.codeId].name else "INVALID(${action.codeId})"
                            println("    [frame=$frameCount] ${objDef.name} event($eventType,$subtype) -> code $codeName (idx=${action.codeId})")
                        }
                        vm.executeCode(action.codeId, inst, other)
                    }
                }
                currentEventContext = prevContext
                return  // Found and executed event, done
            }

            // Event not found on this object, try parent
            objIdx = objDef.parentId
        }
    }

    /**
     * Called by event_inherited() to execute the parent's version of the currently running event.
     */
    fun fireEventInherited(inst: Instance) {
        val ctx = currentEventContext ?: return
        val objDef = if (ctx.objectIndex in gameData.objects.indices) gameData.objects[ctx.objectIndex] else return
        val parentId = objDef.parentId
        if (parentId < 0 || parentId >= gameData.objects.size) return

        // Walk the parent chain from the parent to find the event
        var objIdx = parentId
        while (objIdx >= 0 && objIdx < gameData.objects.size) {
            val parentDef = gameData.objects[objIdx]
            val eventList = parentDef.events.getOrNull(ctx.eventType)
            val event = eventList?.find { it.subtype == ctx.subtype }

            if (event != null) {
                val prevContext = currentEventContext
                currentEventContext = EventContext(ctx.eventType, ctx.subtype, objIdx)
                for (action in event.actions) {
                    if (action.codeId >= 0) {
                        vm.executeCode(action.codeId, inst)
                    }
                }
                currentEventContext = prevContext
                return
            }

            objIdx = parentDef.parentId
        }
    }

    fun gotoRoom(roomIndex: Int) {
        val roomName = if (roomIndex in gameData.rooms.indices) gameData.rooms[roomIndex].name else "INVALID($roomIndex)"
        println("  >>> gotoRoom($roomIndex = $roomName) called at frame=$frameCount")
        Thread.currentThread().stackTrace.take(8).drop(1).forEach { println("      at $it") }
        pendingRoomGoto = roomIndex
    }

    private fun performRoomTransition(roomIndex: Int) {
        if (roomIndex < 0 || roomIndex >= gameData.rooms.size) return

        // Fire Room End for all instances
        dispatchEvent(EVENT_OTHER, OTHER_ROOM_END)

        // Save room state if current room is persistent
        val leavingRoomIndex = currentRoomIndex
        if (leavingRoomIndex >= 0 && roomPersistentFlags[leavingRoomIndex] == true) {
            // Save all non-object-persistent instances (object-persistent ones travel with the player)
            val roomInstances = instances.filter { !it.persistent && !it.destroyed }
            savedRoomStates[leavingRoomIndex] = ArrayList(roomInstances)
            println("  Saved ${roomInstances.size} instances for persistent room $leavingRoomIndex")
        }

        // Remove non-persistent instances
        val persistent = instances.filter { it.persistent }
        instances.clear()
        instances.addAll(persistent)

        // Load new room
        currentRoomIndex = roomIndex
        currentRoom = gameData.rooms[roomIndex]
        val room = currentRoom!!

        // Check if we have a saved persistent state for this room
        val savedState = savedRoomStates.remove(roomIndex)
        if (savedState != null) {
            // Restore saved instances - no Create events, no creation code
            println("Restoring persistent room ${room.name} (${room.width}x${room.height}), ${savedState.size} saved instances")
            for (inst in savedState) {
                inst.destroyed = false  // Ensure not marked destroyed
                instances.add(inst)
            }

            // Fire Game Start (only once, on first room)
            if (!gameStarted) {
                gameStarted = true
                println("  Firing Game Start event")
                dispatchEvent(EVENT_OTHER, OTHER_GAME_START)
            }

            // Fire Room Start for ALL instances
            println("  Room restored. Total instances: ${instances.size}")
            for (inst in ArrayList(instances)) {
                if (inst.destroyed) continue
                fireEvent(inst, EVENT_OTHER, OTHER_ROOM_START)
            }
        } else {
            // Normal room loading - create instances from room definition
            println("Entering room ${room.name} (${room.width}x${room.height}), ${room.instances.size} instances, creationCode=${room.creationCodeId}")

            // Create room instances
            for (roomInst in room.instances) {
                val inst = createInstance(roomInst.objectDefId, roomInst.x.toDouble(), roomInst.y.toDouble(), roomInst.instanceId)
                inst.imageXscale = roomInst.scaleX.toDouble()
                inst.imageYscale = roomInst.scaleY.toDouble()
                inst.imageAngle = roomInst.rotation.toDouble()

                // Run instance creation code
                if (roomInst.creationCodeId >= 0) {
                    vm.executeCode(roomInst.creationCodeId, inst)
                }
            }

            // Run room creation code
            if (room.creationCodeId >= 0) {
                // Create a dummy instance for room creation code
                val dummyInst = instances.firstOrNull() ?: createInstance(-1, 0.0, 0.0)
                vm.executeCode(room.creationCodeId, dummyInst)
            }

            // Fire Create events (only for newly created instances, not persistent survivors)
            println("  Firing Create events for new instances (${instances.size} total, ${persistent.size} persistent)")
            for (inst in ArrayList(instances)) {
                if (inst.destroyed) continue
                if (inst in persistent) continue  // Persistent instances already had their Create event
                val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "unknown"
                println("    Create: $objName (id=${inst.id})")
                fireEvent(inst, EVENT_CREATE, 0)
            }

            // Fire Game Start (only once, on first room)
            if (!gameStarted) {
                gameStarted = true
                println("  Firing Game Start event")
                dispatchEvent(EVENT_OTHER, OTHER_GAME_START)
            }

            // Fire Room Start for ALL instances after a room transition
            println("  Room setup complete. Total instances: ${instances.size}")
            for (inst in ArrayList(instances)) {
                if (inst.destroyed) continue
                fireEvent(inst, EVENT_OTHER, OTHER_ROOM_START)
            }
        }
    }

    fun createInstance(objectDefId: Int, x: Double, y: Double, forceId: Int = -1): Instance {
        val id = if (forceId >= 0) forceId else nextInstanceId++
        if (forceId >= nextInstanceId) nextInstanceId = forceId + 1

        val inst = Instance(id = id, objectIndex = objectDefId, x = x, y = y)
        inst.gameRunner = this
        inst.xstart = x
        inst.ystart = y
        inst.xprevious = x
        inst.yprevious = y

        if (objectDefId in gameData.objects.indices) {
            val objDef = gameData.objects[objectDefId]
            inst.spriteIndex = objDef.spriteIndex
            inst.visible = objDef.visible
            inst.solid = objDef.solid
            inst.depth = objDef.depth
            inst.persistent = objDef.persistent
            inst.maskIndex = objDef.maskId
        }

        instances.add(inst)
        return inst
    }

    fun destroyInstance(inst: Instance) {
        fireEvent(inst, EVENT_DESTROY, 0)
        inst.destroyed = true
    }

    // ========== Path helpers ==========
    fun computePathLength(pathIndex: Int): Double {
        val path = gameData.paths.getOrNull(pathIndex) ?: return 0.0
        val pts = path.points
        if (pts.size < 2) return 0.0
        var length = 0.0
        val segCount = if (path.isClosed) pts.size else pts.size - 1
        for (i in 0 until segCount) {
            val p1 = pts[i]
            val p2 = pts[(i + 1) % pts.size]
            val dx = (p2.x - p1.x).toDouble()
            val dy = (p2.y - p1.y).toDouble()
            length += sqrt(dx * dx + dy * dy)
        }
        return length
    }

    fun interpolatePathPosition(pathIndex: Int, position: Double): Pair<Double, Double>? {
        val path = gameData.paths.getOrNull(pathIndex) ?: return null
        val pts = path.points
        if (pts.isEmpty()) return null
        if (pts.size == 1) return Pair(pts[0].x.toDouble(), pts[0].y.toDouble())

        val segCount = if (path.isClosed) pts.size else pts.size - 1
        // Build cumulative distances
        val segLengths = DoubleArray(segCount)
        var totalLength = 0.0
        for (i in 0 until segCount) {
            val p1 = pts[i]
            val p2 = pts[(i + 1) % pts.size]
            val dx = (p2.x - p1.x).toDouble()
            val dy = (p2.y - p1.y).toDouble()
            segLengths[i] = sqrt(dx * dx + dy * dy)
            totalLength += segLengths[i]
        }
        if (totalLength == 0.0) return Pair(pts[0].x.toDouble(), pts[0].y.toDouble())

        val clampedPos = position.coerceIn(0.0, 1.0)
        val targetDist = clampedPos * totalLength
        var accumulated = 0.0
        for (i in 0 until segCount) {
            if (accumulated + segLengths[i] >= targetDist || i == segCount - 1) {
                val segT = if (segLengths[i] > 0.0) (targetDist - accumulated) / segLengths[i] else 0.0
                val p1 = pts[i]
                val p2 = pts[(i + 1) % pts.size]
                val x = p1.x + (p2.x - p1.x) * segT
                val y = p1.y + (p2.y - p1.y) * segT
                return Pair(x.toDouble(), y.toDouble())
            }
            accumulated += segLengths[i]
        }
        val last = pts.last()
        return Pair(last.x.toDouble(), last.y.toDouble())
    }

    fun pathEnd(inst: Instance) {
        if (Butterscotch.tracePaths.isNotEmpty()) {
            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
            if (Butterscotch.tracePaths.contains("*") || objName in Butterscotch.tracePaths) {
                println("[TRACE PATH] $objName (id=${inst.id}): path ended at frame=$frameCount")
            }
        }
        inst.pathIndex = -1
        inst.pathSpeed = 0.0
    }

    private fun updatePathFollowing() {
        val tracing = Butterscotch.tracePaths.isNotEmpty()

        for (inst in instances) {
            if (inst.destroyed || inst.pathIndex < 0) continue
            // Skip path following when speed is 0 - no movement to apply.
            // This prevents path_action_stop from forcing x/y to the path endpoint every frame.
            if (inst.pathSpeed == 0.0) continue
            val pathLength = computePathLength(inst.pathIndex)
            if (pathLength <= 0.0) continue

            val shouldTrace = tracing && run {
                val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                Butterscotch.tracePaths.contains("*") || objName in Butterscotch.tracePaths
            }

            val oldX = inst.x
            val oldY = inst.y

            // Advance position
            inst.pathPosition += inst.pathSpeed / pathLength

            // Handle end of path
            if (inst.pathPosition >= 1.0) {
                when (inst.pathEndAction) {
                    0 -> { // path_action_stop
                        inst.pathPosition = 1.0
                        inst.pathSpeed = 0.0
                        val pos = interpolatePathPosition(inst.pathIndex, 1.0)
                        if (pos != null) {
                            val newX = pos.first + inst.pathXOffset
                            val newY = pos.second + inst.pathYOffset
                            updateDirectionFromPath(inst, oldX, oldY, newX, newY)
                            inst.x = newX
                            inst.y = newY
                        }
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_stop at pathPos=${inst.pathPosition} frame=$frameCount")
                        }
                        continue
                    }
                    1 -> { // path_action_restart
                        inst.pathPosition -= 1.0
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_restart at pathPos=${inst.pathPosition} frame=$frameCount")
                        }
                    }
                    2 -> { // path_action_continue
                        inst.pathPosition = 1.0
                        val pos = interpolatePathPosition(inst.pathIndex, 1.0)
                        if (pos != null) {
                            val newX = pos.first + inst.pathXOffset
                            val newY = pos.second + inst.pathYOffset
                            updateDirectionFromPath(inst, oldX, oldY, newX, newY)
                            inst.x = newX
                            inst.y = newY
                        }
                        // GM: path_action_continue ends the path but keeps movement
                        // Set speed/hspeed/vspeed so the instance continues in the same direction
                        val spd = Math.abs(inst.pathSpeed)
                        inst.speed = spd
                        val dirRad = Math.toRadians(inst.direction)
                        inst.hspeed = spd * Math.cos(dirRad)
                        inst.vspeed = -spd * Math.sin(dirRad)
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_continue at pathPos=${inst.pathPosition} speed=$spd dir=${inst.direction} frame=$frameCount")
                        }
                        pathEnd(inst)
                        continue
                    }
                    3 -> { // path_action_reverse
                        inst.pathPosition = 1.0 - (inst.pathPosition - 1.0)
                        inst.pathSpeed = -inst.pathSpeed
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_reverse at pathPos=${inst.pathPosition} pathSpeed=${inst.pathSpeed} frame=$frameCount")
                        }
                    }
                }
            } else if (inst.pathPosition <= 0.0) {
                when (inst.pathEndAction) {
                    0 -> { // path_action_stop
                        inst.pathPosition = 0.0
                        inst.pathSpeed = 0.0
                        val pos = interpolatePathPosition(inst.pathIndex, 0.0)
                        if (pos != null) {
                            val newX = pos.first + inst.pathXOffset
                            val newY = pos.second + inst.pathYOffset
                            updateDirectionFromPath(inst, oldX, oldY, newX, newY)
                            inst.x = newX
                            inst.y = newY
                        }
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_stop at pathPos=${inst.pathPosition} frame=$frameCount")
                        }
                        continue
                    }
                    1 -> { // path_action_restart
                        inst.pathPosition += 1.0
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_restart at pathPos=${inst.pathPosition} frame=$frameCount")
                        }
                    }
                    2 -> { // path_action_continue
                        inst.pathPosition = 0.0
                        val pos = interpolatePathPosition(inst.pathIndex, 0.0)
                        if (pos != null) {
                            val newX = pos.first + inst.pathXOffset
                            val newY = pos.second + inst.pathYOffset
                            updateDirectionFromPath(inst, oldX, oldY, newX, newY)
                            inst.x = newX
                            inst.y = newY
                        }
                        // GM: path_action_continue ends the path but keeps movement
                        val spdR = Math.abs(inst.pathSpeed)
                        inst.speed = spdR
                        val dirRadR = Math.toRadians(inst.direction)
                        inst.hspeed = spdR * Math.cos(dirRadR)
                        inst.vspeed = -spdR * Math.sin(dirRadR)
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_continue at pathPos=${inst.pathPosition} speed=$spdR dir=${inst.direction} frame=$frameCount")
                        }
                        pathEnd(inst)
                        continue
                    }
                    3 -> { // path_action_reverse
                        inst.pathPosition = -inst.pathPosition
                        inst.pathSpeed = -inst.pathSpeed
                        if (shouldTrace) {
                            val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                            println("[TRACE PATH] $objName (id=${inst.id}): path_action_reverse at pathPos=${inst.pathPosition} pathSpeed=${inst.pathSpeed} frame=$frameCount")
                        }
                    }
                }
            }

            // Interpolate new position
            val pos = interpolatePathPosition(inst.pathIndex, inst.pathPosition)
            if (pos != null) {
                val newX = pos.first + inst.pathXOffset
                val newY = pos.second + inst.pathYOffset
                updateDirectionFromPath(inst, oldX, oldY, newX, newY)
                inst.x = newX
                inst.y = newY
            }

            if (shouldTrace) {
                val objName = if (inst.objectIndex in gameData.objects.indices) gameData.objects[inst.objectIndex].name else "??"
                println("[TRACE PATH] $objName (id=${inst.id}): pathIdx=${inst.pathIndex} pathPos=${"%.4f".format(inst.pathPosition)} pathSpeed=${inst.pathSpeed} pos=(${inst.x}, ${inst.y}) dir=${inst.direction} frame=$frameCount")
            }
        }
    }

    /**
     * Update instance direction based on path movement delta.
     * In GameMaker, path following automatically updates direction from the path tangent.
     * We set the direction field directly to avoid side effects on hspeed/vspeed.
     */
    private fun updateDirectionFromPath(inst: Instance, oldX: Double, oldY: Double, newX: Double, newY: Double) {
        val dx = newX - oldX
        val dy = newY - oldY
        if (dx != 0.0 || dy != 0.0) {
            inst.direction = (Math.toDegrees(kotlin.math.atan2(-dy, dx)) + 360) % 360
        }
    }

    data class BBox(val left: Double, val right: Double, val top: Double, val bottom: Double)

    /**
     * Computes the bounding box of an instance in world coordinates.
     * The returned BBox uses exclusive-end semantics for right/bottom:
     * - left/top: inclusive start of the first collision pixel
     * - right/bottom: exclusive end (one past the last collision pixel)
     *
     * Sprite margin values (marginLeft/Right/Top/Bottom) are 0-indexed inclusive
     * pixel positions within the sprite. At scale > 1, each pixel occupies multiple
     * world units, so we use (margin + 1) for right/bottom to get the full extent.
     */
    fun computeBBox(inst: Instance): BBox? {
        val spriteIdx = if (inst.maskIndex >= 0) inst.maskIndex else inst.spriteIndex
        if (spriteIdx !in gameData.sprites.indices) return null
        val s = gameData.sprites[spriteIdx]
        val x1 = inst.x + (s.marginLeft - s.originX) * inst.imageXscale
        val x2 = inst.x + (s.marginRight + 1 - s.originX) * inst.imageXscale
        val y1 = inst.y + (s.marginTop - s.originY) * inst.imageYscale
        val y2 = inst.y + (s.marginBottom + 1 - s.originY) * inst.imageYscale
        return BBox(minOf(x1, x2), maxOf(x1, x2), minOf(y1, y2), maxOf(y1, y2))
    }

    /**
     * Returns the effective sprite used for collision masking for an instance.
     * Uses maskIndex if set, otherwise spriteIndex.
     */
    private fun getCollisionSprite(inst: Instance): com.mrpowergamerbr.butterscotch.data.SpriteData? {
        val spriteIdx = if (inst.maskIndex >= 0) inst.maskIndex else inst.spriteIndex
        if (spriteIdx !in gameData.sprites.indices) return null
        return gameData.sprites[spriteIdx]
    }

    /**
     * Check if a world-space point hits a sprite's collision mask pixel.
     * Returns true if the mask bit is set at that world position.
     */
    fun checkMaskPixel(
        sprite: com.mrpowergamerbr.butterscotch.data.SpriteData,
        frameIndex: Int,
        worldX: Double, worldY: Double,
        instX: Double, instY: Double,
        xscale: Double, yscale: Double
    ): Boolean {
        if (sprite.masks.isEmpty()) return true // No mask data, treat as solid
        if (sprite.collisionMaskType != 1) return true // Not precise, treat as solid (bbox already passed)

        // Transform world coords to sprite-local coords
        val localX = (worldX - instX) / xscale + sprite.originX
        val localY = (worldY - instY) / yscale + sprite.originY

        val col = localX.toInt()
        val row = localY.toInt()
        if (col < 0 || col >= sprite.width || row < 0 || row >= sprite.height) return false

        // Select mask frame
        val maskIdx = if (sprite.masks.size > 1) frameIndex % sprite.masks.size else 0
        val mask = sprite.masks[maskIdx]
        val stride = (sprite.width + 7) / 8
        val byteIdx = row * stride + col / 8
        if (byteIdx < 0 || byteIdx >= mask.size) return false
        val bit = (mask[byteIdx].toInt() shr (7 - col % 8)) and 1
        return bit != 0
    }

    /**
     * Check if two instances overlap using precise pixel mask collision.
     * Both instances must have AABB overlap (fast-reject should be done before calling).
     */
    fun checkPreciseOverlap(instA: Instance, instB: Instance): Boolean {
        val spriteA = getCollisionSprite(instA) ?: return false
        val spriteB = getCollisionSprite(instB) ?: return false

        // If neither has precise masks, bbox overlap is sufficient
        val aPrecise = spriteA.collisionMaskType == 1 && spriteA.masks.isNotEmpty()
        val bPrecise = spriteB.collisionMaskType == 1 && spriteB.masks.isNotEmpty()
        if (!aPrecise && !bPrecise) return true // Both non-precise, bbox was enough

        val bboxA = computeBBox(instA) ?: return false
        val bboxB = computeBBox(instB) ?: return false

        // Find overlap region in world space
        val overlapLeft = maxOf(bboxA.left, bboxB.left)
        val overlapRight = minOf(bboxA.right, bboxB.right)
        val overlapTop = maxOf(bboxA.top, bboxB.top)
        val overlapBottom = minOf(bboxA.bottom, bboxB.bottom)

        if (overlapLeft >= overlapRight || overlapTop >= overlapBottom) return false

        // Determine step size: use the smallest scale for accuracy
        val stepX = minOf(abs(instA.imageXscale), abs(instB.imageXscale)).coerceAtLeast(0.5)
        val stepY = minOf(abs(instA.imageYscale), abs(instB.imageYscale)).coerceAtLeast(0.5)

        val frameA = instA.imageIndex.toInt()
        val frameB = instB.imageIndex.toInt()

        var wy = overlapTop + stepY / 2
        while (wy < overlapBottom) {
            var wx = overlapLeft + stepX / 2
            while (wx < overlapRight) {
                val aHit = if (aPrecise) checkMaskPixel(spriteA, frameA, wx, wy, instA.x, instA.y, instA.imageXscale, instA.imageYscale) else true
                val bHit = if (bPrecise) checkMaskPixel(spriteB, frameB, wx, wy, instB.x, instB.y, instB.imageXscale, instB.imageYscale) else true
                if (aHit && bHit) return true
                wx += stepX
            }
            wy += stepY
        }
        return false
    }

    fun findInstancesByObjectOrId(targetId: Int): List<Instance> {
        return if (targetId >= 100000) {
            // Instance ID
            instances.filter { it.id == targetId && !it.destroyed }
        } else {
            // Object index
            instances.filter { (it.objectIndex == targetId || isChildOf(it.objectIndex, targetId)) && !it.destroyed }
        }
    }

    fun findFirstInstanceByObject(objectId: Int): Instance? {
        return instances.firstOrNull { (it.objectIndex == objectId || isChildOf(it.objectIndex, objectId)) && !it.destroyed }
    }

    private fun isChildOf(childObjId: Int, parentObjId: Int): Boolean {
        if (childObjId < 0 || childObjId >= gameData.objects.size) return false
        var current = childObjId
        var depth = 0
        while (depth < 32) {
            val obj = gameData.objects[current]
            if (obj.parentId == parentObjId) return true
            if (obj.parentId < 0 || obj.parentId >= gameData.objects.size) return false
            current = obj.parentId
            depth++
        }
        return false
    }

    fun onKeyDown(key: Int) {
        if (key !in keysHeld) {
            keysPressed.add(key)
        }
        keysHeld.add(key)
        keyboardKey = key
        keyboardLastKey = key
    }

    fun onKeyUp(key: Int) {
        keysHeld.remove(key)
        keysReleased.add(key)
    }
}
