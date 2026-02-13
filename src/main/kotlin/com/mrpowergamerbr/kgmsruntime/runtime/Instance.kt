package com.mrpowergamerbr.kgmsruntime.runtime

import com.mrpowergamerbr.kgmsruntime.vm.GMLValue
import kotlin.math.abs
import kotlin.math.floor

class Instance(
    val id: Int,
    val objectIndex: Int,
    var x: Double = 0.0,
    var y: Double = 0.0,
) {
    var spriteIndex: Int = -1
    var imageIndex: Double = 0.0
    var imageSpeed: Double = 1.0
    var imageXscale: Double = 1.0
    var imageYscale: Double = 1.0
    var imageAngle: Double = 0.0
    var imageBlend: Int = 0xFFFFFF
    var imageAlpha: Double = 1.0

    var depth: Int = 0
    var visible: Boolean = true
    var solid: Boolean = false
    var persistent: Boolean = false

    var xprevious: Double = 0.0
    var yprevious: Double = 0.0
    var xstart: Double = 0.0
    var ystart: Double = 0.0

    var hspeed: Double = 0.0
    var vspeed: Double = 0.0
    var speed: Double = 0.0
    var direction: Double = 0.0
    var friction: Double = 0.0
    var gravity: Double = 0.0
    var gravityDirection: Double = 270.0

    var maskIndex: Int = -1
    var gameRunner: GameRunner? = null

    // Path following
    var pathIndex: Int = -1
    var pathPosition: Double = 0.0
    var pathSpeed: Double = 0.0
    var pathEndAction: Int = 0  // 0=stop, 1=restart, 2=continue, 3=reverse
    var pathOrientation: Double = 0.0
    var pathScale: Double = 1.0
    // Offset for relative path following (absolute=false)
    var pathXOffset: Double = 0.0
    var pathYOffset: Double = 0.0

    val alarm = IntArray(12) { -1 }

    // User variables
    val variables = mutableMapOf<String, GMLValue>()

    // Return value for script calls
    var returnValue: GMLValue? = null

    var destroyed = false

    fun getArrayElement(name: String, index: Int): GMLValue = when (name) {
        "alarm" -> if (index in alarm.indices) GMLValue.of(alarm[index].toDouble()) else GMLValue.of(-1.0)
        else -> {
            val arr = variables[name]
            if (arr is GMLValue.ArrayVal) arr.data[0]?.get(index) ?: GMLValue.ZERO
            else GMLValue.ZERO
        }
    }

    fun setArrayElement(name: String, index: Int, value: GMLValue) {
        when (name) {
            "alarm" -> if (index in alarm.indices) alarm[index] = value.toInt()
            else -> {
                val arr = variables.getOrPut(name) { GMLValue.ArrayVal() } as? GMLValue.ArrayVal
                if (arr != null) {
                    arr.data.getOrPut(0) { mutableMapOf() }[index] = value
                } else {
                    // Overwrite non-array with array
                    val newArr = GMLValue.ArrayVal()
                    newArr.data.getOrPut(0) { mutableMapOf() }[index] = value
                    variables[name] = newArr
                }
            }
        }
    }

    fun getBuiltinOrVar(name: String): GMLValue = when (name) {
        "x" -> GMLValue.of(x)
        "y" -> GMLValue.of(y)
        "xprevious" -> GMLValue.of(xprevious)
        "yprevious" -> GMLValue.of(yprevious)
        "xstart" -> GMLValue.of(xstart)
        "ystart" -> GMLValue.of(ystart)
        "hspeed" -> GMLValue.of(hspeed)
        "vspeed" -> GMLValue.of(vspeed)
        "speed" -> GMLValue.of(speed)
        "direction" -> GMLValue.of(direction)
        "friction" -> GMLValue.of(friction)
        "gravity" -> GMLValue.of(gravity)
        "gravity_direction" -> GMLValue.of(gravityDirection)
        "sprite_index" -> GMLValue.of(spriteIndex.toDouble())
        "image_index" -> GMLValue.of(imageIndex)
        "image_speed" -> GMLValue.of(imageSpeed)
        "image_xscale" -> GMLValue.of(imageXscale)
        "image_yscale" -> GMLValue.of(imageYscale)
        "image_angle" -> GMLValue.of(imageAngle)
        "image_blend" -> GMLValue.of(imageBlend.toDouble())
        "image_alpha" -> GMLValue.of(imageAlpha)
        "image_number" -> GMLValue.of(0.0) // will be resolved by runner
        "depth" -> GMLValue.of(depth.toDouble())
        "visible" -> GMLValue.of(visible)
        "solid" -> GMLValue.of(solid)
        "persistent" -> GMLValue.of(persistent)
        "object_index" -> GMLValue.of(objectIndex.toDouble())
        "id" -> GMLValue.of(id.toDouble())
        "mask_index" -> GMLValue.of(maskIndex.toDouble())
        "path_index" -> GMLValue.of(pathIndex.toDouble())
        "path_position" -> GMLValue.of(pathPosition)
        "path_speed" -> GMLValue.of(pathSpeed)
        "path_endaction" -> GMLValue.of(pathEndAction.toDouble())
        "path_orientation" -> GMLValue.of(pathOrientation)
        "path_scale" -> GMLValue.of(pathScale)
        "alarm" -> GMLValue.ZERO // array access handled separately
        "bbox_left" -> { val bb = gameRunner?.computeBBox(this); GMLValue.of(if (bb != null) floor(bb.left) else 0.0) }
        "bbox_right" -> { val bb = gameRunner?.computeBBox(this); GMLValue.of(if (bb != null) floor(bb.right) else 0.0) }
        "bbox_top" -> { val bb = gameRunner?.computeBBox(this); GMLValue.of(if (bb != null) floor(bb.top) else 0.0) }
        "bbox_bottom" -> { val bb = gameRunner?.computeBBox(this); GMLValue.of(if (bb != null) floor(bb.bottom) else 0.0) }
        "sprite_width" -> {
            val s = gameRunner?.gameData?.sprites?.getOrNull(spriteIndex)
            GMLValue.of(if (s != null) (s.width * abs(imageXscale)) else 0.0)
        }
        "sprite_height" -> {
            val s = gameRunner?.gameData?.sprites?.getOrNull(spriteIndex)
            GMLValue.of(if (s != null) (s.height * abs(imageYscale)) else 0.0)
        }
        else -> variables[name] ?: gameRunner?.vm?.getGlobalBuiltin(name) ?: GMLValue.ZERO
    }

    fun setBuiltinOrVar(name: String, value: GMLValue) {
        when (name) {
            "x" -> x = value.toReal()
            "y" -> y = value.toReal()
            "xprevious" -> xprevious = value.toReal()
            "yprevious" -> yprevious = value.toReal()
            "xstart" -> xstart = value.toReal()
            "ystart" -> ystart = value.toReal()
            "hspeed" -> hspeed = value.toReal()
            "vspeed" -> vspeed = value.toReal()
            "speed" -> speed = value.toReal()
            "direction" -> direction = value.toReal()
            "friction" -> friction = value.toReal()
            "gravity" -> gravity = value.toReal()
            "gravity_direction" -> gravityDirection = value.toReal()
            "sprite_index" -> spriteIndex = value.toInt()
            "image_index" -> imageIndex = value.toReal()
            "image_speed" -> imageSpeed = value.toReal()
            "image_xscale" -> imageXscale = value.toReal()
            "image_yscale" -> imageYscale = value.toReal()
            "image_angle" -> imageAngle = value.toReal()
            "image_blend" -> imageBlend = value.toInt()
            "image_alpha" -> imageAlpha = value.toReal()
            "depth" -> depth = value.toInt()
            "visible" -> visible = value.toBool()
            "solid" -> solid = value.toBool()
            "persistent" -> persistent = value.toBool()
            "mask_index" -> maskIndex = value.toInt()
            "path_index" -> pathIndex = value.toInt()
            "path_position" -> pathPosition = value.toReal()
            "path_speed" -> pathSpeed = value.toReal()
            "path_endaction" -> pathEndAction = value.toInt()
            "path_orientation" -> pathOrientation = value.toReal()
            "path_scale" -> pathScale = value.toReal()
            else -> variables[name] = value
        }
    }
}
