package com.mrpowergamerbr.kgmsruntime.runtime

import com.mrpowergamerbr.kgmsruntime.data.GameData
import com.mrpowergamerbr.kgmsruntime.data.RoomData
import com.mrpowergamerbr.kgmsruntime.graphics.Renderer
import com.mrpowergamerbr.kgmsruntime.vm.GMLValue
import com.mrpowergamerbr.kgmsruntime.vm.VM
import kotlin.math.*

class GameRunner(
    val gameData: GameData,
    val vm: VM,
    val renderer: Renderer,
) {
    val instances = mutableListOf<Instance>()
    val globalVariables = mutableMapOf<String, GMLValue>()

    var currentRoomIndex = -1
    var currentRoom: RoomData? = null
    var pendingRoomGoto = -1
    var nextInstanceId = 100000

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
    }

    fun initialize() {
        vm.runner = this

        // Load first room
        val firstRoom = gameData.gen8.roomOrder.firstOrNull() ?: 0
        gotoRoom(firstRoom)
    }

    fun step() {
        frameCount++

        // Handle pending room transition
        if (pendingRoomGoto >= 0) {
            performRoomTransition(pendingRoomGoto)
            pendingRoomGoto = -1
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
        for (key in keysPressed) {
            dispatchKeyEvent(EVENT_KEYPRESS, key)
        }

        // 4. Step
        dispatchEvent(EVENT_STEP, 0)

        // 5. End Step
        dispatchEvent(EVENT_STEP, 2)

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
                inst.xprevious = inst.x; inst.yprevious = inst.y
                inst.x += inst.hspeed; inst.y += inst.vspeed
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

        // Clear per-frame input
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
                renderer.setView(view.viewX, view.viewY, view.viewW, view.viewH,
                    view.portX, view.portY, view.portW, view.portH)

                // Clear with room bg color
                if (viewIdx == 0 && room.drawBgColor) {
                    renderer.clear(room.bgColor)
                }

                // Draw room backgrounds (non-foreground)
                drawRoomBackgrounds(room, false)

                // Draw all instances sorted by depth (descending)
                val sorted = instances.filter { !it.destroyed }.sortedByDescending { it.depth }
                for (inst in sorted) {
                    drawInstance(inst)
                }

                // Draw foreground backgrounds
                drawRoomBackgrounds(room, true)
            }
        } else {
            renderer.currentView = 0
            renderer.setView(0, 0, room.width, room.height, 0, 0,
                gameData.gen8.windowWidth, gameData.gen8.windowHeight)

            if (room.drawBgColor) {
                renderer.clear(room.bgColor)
            }

            drawRoomBackgrounds(room, false)

            val sorted = instances.filter { !it.destroyed }.sortedByDescending { it.depth }
            for (inst in sorted) {
                drawInstance(inst)
            }

            drawRoomBackgrounds(room, true)
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

    fun fireEvent(inst: Instance, eventType: Int, subtype: Int) {
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
                        vm.executeCode(action.codeId, inst)
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

        // Remove non-persistent instances
        val persistent = instances.filter { it.persistent }
        instances.clear()
        instances.addAll(persistent)

        // Load new room
        currentRoomIndex = roomIndex
        currentRoom = gameData.rooms[roomIndex]
        val room = currentRoom!!

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

        // Fire Room Start only for persistent instances that survived the room transition
        // (Newly created room instances should not get Room Start on their initial room load)
        println("  Room setup complete. Total instances: ${instances.size}")
        for (inst in ArrayList(instances)) {
            if (inst.destroyed) continue
            if (inst in persistent) {
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

    data class BBox(val left: Double, val right: Double, val top: Double, val bottom: Double)

    fun computeBBox(inst: Instance): BBox? {
        val spriteIdx = if (inst.maskIndex >= 0) inst.maskIndex else inst.spriteIndex
        if (spriteIdx !in gameData.sprites.indices) return null
        val s = gameData.sprites[spriteIdx]
        val x1 = inst.x + (s.marginLeft - s.originX) * inst.imageXscale
        val x2 = inst.x + (s.marginRight - s.originX) * inst.imageXscale
        val y1 = inst.y + (s.marginTop - s.originY) * inst.imageYscale
        val y2 = inst.y + (s.marginBottom - s.originY) * inst.imageYscale
        return BBox(minOf(x1, x2), maxOf(x1, x2), minOf(y1, y2), maxOf(y1, y2))
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
