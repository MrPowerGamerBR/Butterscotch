package com.mrpowergamerbr.butterscotch.builtin

import com.mrpowergamerbr.butterscotch.runtime.GameRunner
import com.mrpowergamerbr.butterscotch.vm.GMLValue
import com.mrpowergamerbr.butterscotch.vm.VM
import kotlin.math.*
import kotlin.random.Random

fun registerBuiltins(vm: VM) {
    val f = vm.builtinFunctions

    // ========== Math ==========
    f["random"] = { _, args -> GMLValue.of(Random.nextDouble() * args[0].toReal()) }
    f["irandom"] = { _, args -> GMLValue.of(Random.nextInt(args[0].toInt() + 1).toDouble()) }
    f["random_range"] = { _, args -> GMLValue.of(args[0].toReal() + Random.nextDouble() * (args[1].toReal() - args[0].toReal())) }
    f["irandom_range"] = { _, args ->
        val lo = args[0].toInt(); val hi = args[1].toInt()
        GMLValue.of(if (hi >= lo) (lo + Random.nextInt(hi - lo + 1)).toDouble() else lo.toDouble())
    }
    f["round"] = { _, args -> GMLValue.of(Math.round(args[0].toReal()).toDouble()) }
    f["floor"] = { _, args -> GMLValue.of(floor(args[0].toReal())) }
    f["ceil"] = { _, args -> GMLValue.of(ceil(args[0].toReal())) }
    f["abs"] = { _, args -> GMLValue.of(abs(args[0].toReal())) }
    f["sign"] = { _, args -> GMLValue.of(args[0].toReal().let { if (it > 0) 1.0 else if (it < 0) -1.0 else 0.0 }) }
    f["min"] = { _, args -> GMLValue.of(args.minOf { it.toReal() }) }
    f["max"] = { _, args -> GMLValue.of(args.maxOf { it.toReal() }) }
    f["clamp"] = { _, args -> GMLValue.of(args[0].toReal().coerceIn(args[1].toReal(), args[2].toReal())) }
    f["sqrt"] = { _, args -> GMLValue.of(sqrt(args[0].toReal())) }
    f["power"] = { _, args -> GMLValue.of(args[0].toReal().pow(args[1].toReal())) }
    f["sin"] = { _, args -> GMLValue.of(sin(Math.toRadians(args[0].toReal()))) }
    f["cos"] = { _, args -> GMLValue.of(cos(Math.toRadians(args[0].toReal()))) }
    f["degtorad"] = { _, args -> GMLValue.of(Math.toRadians(args[0].toReal())) }
    f["radtodeg"] = { _, args -> GMLValue.of(Math.toDegrees(args[0].toReal())) }
    f["point_direction"] = { _, args ->
        val dx = args[2].toReal() - args[0].toReal()
        val dy = args[3].toReal() - args[1].toReal()
        GMLValue.of((Math.toDegrees(atan2(-dy, dx)) + 360) % 360)
    }
    f["point_distance"] = { _, args ->
        val dx = args[2].toReal() - args[0].toReal()
        val dy = args[3].toReal() - args[1].toReal()
        GMLValue.of(sqrt(dx * dx + dy * dy))
    }
    f["distance_to_point"] = { v, args ->
        val px = args[0].toReal()
        val py = args[1].toReal()
        val self = v.currentSelf
        val runner = v.runner
        val bb = if (self != null) runner.computeBBox(self) else null
        if (bb != null) {
            val nearestX = px.coerceIn(bb.left, bb.right)
            val nearestY = py.coerceIn(bb.top, bb.bottom)
            val dx = px - nearestX
            val dy = py - nearestY
            GMLValue.of(sqrt(dx * dx + dy * dy))
        } else {
            GMLValue.of(0.0)
        }
    }
    f["lengthdir_x"] = { _, args -> GMLValue.of(args[0].toReal() * cos(Math.toRadians(args[1].toReal()))) }
    f["lengthdir_y"] = { _, args -> GMLValue.of(-args[0].toReal() * sin(Math.toRadians(args[1].toReal()))) }
    f["choose"] = { _, args -> if (args.isNotEmpty()) args[Random.nextInt(args.size)] else GMLValue.ZERO }
    f["lerp"] = { _, args -> GMLValue.of(args[0].toReal() + (args[1].toReal() - args[0].toReal()) * args[2].toReal()) }

    // ========== String ==========
    f["string"] = { _, args -> GMLValue.of(args[0].toStr()) }
    f["real"] = { _, args -> GMLValue.of(args[0].toReal()) }
    f["string_length"] = { _, args -> GMLValue.of(args[0].toStr().length.toDouble()) }
    f["string_char_at"] = { _, args ->
        val s = args[0].toStr(); val i = args[1].toInt() - 1
        GMLValue.of(if (i in s.indices) s[i].toString() else "")
    }
    f["string_copy"] = { _, args ->
        val s = args[0].toStr(); val idx = (args[1].toInt() - 1).coerceAtLeast(0)
        val len = args[2].toInt()
        GMLValue.of(s.substring(idx, (idx + len).coerceAtMost(s.length)))
    }
    f["string_pos"] = { _, args ->
        val sub = args[0].toStr(); val s = args[1].toStr()
        GMLValue.of((s.indexOf(sub) + 1).toDouble())
    }
    f["string_delete"] = { _, args ->
        val s = args[0].toStr(); val idx = (args[1].toInt() - 1).coerceAtLeast(0)
        val len = args[2].toInt()
        GMLValue.of(s.removeRange(idx, (idx + len).coerceAtMost(s.length)))
    }
    f["string_lower"] = { _, args -> GMLValue.of(args[0].toStr().lowercase()) }
    f["string_upper"] = { _, args -> GMLValue.of(args[0].toStr().uppercase()) }
    f["string_replace"] = { _, args -> GMLValue.of(args[0].toStr().replaceFirst(args[1].toStr(), args[2].toStr())) }
    f["string_replace_all"] = { _, args -> GMLValue.of(args[0].toStr().replace(args[1].toStr(), args[2].toStr())) }
    f["string_count"] = { _, args ->
        val sub = args[0].toStr(); val s = args[1].toStr()
        var count = 0; var idx = 0
        while (true) { idx = s.indexOf(sub, idx); if (idx < 0) break; count++; idx += sub.length }
        GMLValue.of(count.toDouble())
    }
    f["string_width"] = { _, args -> GMLValue.of(vm.runner.renderer.measureStringWidth(args[0].toStr())) }
    f["string_height"] = { _, args -> GMLValue.of(vm.runner.renderer.measureStringHeight(args[0].toStr())) }
    f["string_hash_to_newline"] = { _, args -> GMLValue.of(args[0].toStr().replace("#", "\n")) }
    f["chr"] = { _, args -> GMLValue.of(args[0].toInt().toChar().toString()) }
    f["ord"] = { _, args -> val s = args[0].toStr(); GMLValue.of(if (s.isNotEmpty()) s[0].code.toDouble() else 0.0) }
    f["ansi_char"] = { _, args -> GMLValue.of(args[0].toInt().toChar().toString()) }

    // ========== Drawing ==========
    f["draw_sprite"] = { _, args ->
        vm.runner.renderer.drawSprite(args[0].toInt(), args[1].toInt(), args[2].toReal(), args[3].toReal())
        GMLValue.ZERO
    }
    f["draw_sprite_ext"] = { _, args ->
        vm.runner.renderer.drawSprite(args[0].toInt(), args[1].toInt(), args[2].toReal(), args[3].toReal(),
            args[4].toReal(), args[5].toReal(), args[6].toReal(), args[7].toInt(), args[8].toReal())
        GMLValue.ZERO
    }
    f["draw_sprite_part"] = { _, args ->
        vm.runner.renderer.drawSpritePart(
            args[0].toInt(), args[1].toInt(),
            args[2].toInt(), args[3].toInt(), args[4].toInt(), args[5].toInt(),
            args[6].toReal(), args[7].toReal()
        )
        GMLValue.ZERO
    }
    f["draw_set_color"] = { _, args -> vm.runner.renderer.drawColor = args[0].toInt(); GMLValue.ZERO }
    f["draw_set_alpha"] = { _, args -> vm.runner.renderer.drawAlpha = args[0].toReal(); GMLValue.ZERO }
    f["draw_get_color"] = { _, _ -> GMLValue.of(vm.runner.renderer.drawColor.toDouble()) }
    f["draw_get_alpha"] = { _, _ -> GMLValue.of(vm.runner.renderer.drawAlpha) }
    f["draw_set_font"] = { _, args -> vm.runner.renderer.drawFont = args[0].toInt(); GMLValue.ZERO }
    f["draw_set_halign"] = { _, args -> vm.runner.renderer.drawHalign = args[0].toInt(); GMLValue.ZERO }
    f["draw_set_valign"] = { _, args -> vm.runner.renderer.drawValign = args[0].toInt(); GMLValue.ZERO }
    f["draw_text"] = { _, args ->
        vm.runner.renderer.drawText(args[0].toReal(), args[1].toReal(), args[2].toStr())
        GMLValue.ZERO
    }
    f["draw_text_ext"] = { _, args ->
        // draw_text_ext(x, y, string, sep, w)
        vm.runner.renderer.drawText(args[0].toReal(), args[1].toReal(), args[2].toStr())
        GMLValue.ZERO
    }
    f["draw_text_transformed"] = { _, args ->
        // draw_text_transformed(x, y, string, xscale, yscale, angle)
        vm.runner.renderer.drawTextTransformed(
            args[0].toReal(), args[1].toReal(), args[2].toStr(),
            args[3].toReal(), args[4].toReal(), args[5].toReal()
        )
        GMLValue.ZERO
    }
    f["draw_rectangle"] = { _, args ->
        vm.runner.renderer.drawRectangle(args[0].toReal(), args[1].toReal(), args[2].toReal(), args[3].toReal(), args[4].toBool())
        GMLValue.ZERO
    }
    f["draw_set_blend_mode"] = { _, _ -> GMLValue.ZERO }
    f["draw_set_blend_mode_ext"] = { _, _ -> GMLValue.ZERO }
    f["make_color_rgb"] = { _, args ->
        val r = args[0].toInt() and 0xFF; val g = args[1].toInt() and 0xFF; val b = args[2].toInt() and 0xFF
        GMLValue.of((b shl 16 or (g shl 8) or r).toDouble())
    }
    f["make_colour_rgb"] = f["make_color_rgb"]!!
    f["color_get_red"] = { _, args -> GMLValue.of((args[0].toInt() and 0xFF).toDouble()) }
    f["color_get_green"] = { _, args -> GMLValue.of(((args[0].toInt() shr 8) and 0xFF).toDouble()) }
    f["color_get_blue"] = { _, args -> GMLValue.of(((args[0].toInt() shr 16) and 0xFF).toDouble()) }
    f["merge_color"] = { _, args ->
        val c1 = args[0].toInt(); val c2 = args[1].toInt(); val t = args[2].toReal()
        val r = ((c1 and 0xFF) + ((c2 and 0xFF) - (c1 and 0xFF)) * t).toInt()
        val g = (((c1 shr 8) and 0xFF) + (((c2 shr 8) and 0xFF) - ((c1 shr 8) and 0xFF)) * t).toInt()
        val b = (((c1 shr 16) and 0xFF) + (((c2 shr 16) and 0xFF) - ((c1 shr 16) and 0xFF)) * t).toInt()
        GMLValue.of((b shl 16 or (g shl 8) or r).toDouble())
    }
    f["merge_colour"] = f["merge_color"]!!
    f["draw_background"] = { _, args ->
        val bgIdx = args[0].toInt()
        if (bgIdx in vm.gameData.backgrounds.indices) {
            val bg = vm.gameData.backgrounds[bgIdx]
            vm.runner.renderer.drawBackground(bg.tpagIndex, args[1].toInt(), args[2].toInt(), false, false)
        }
        GMLValue.ZERO
    }
    f["draw_background_ext"] = f["draw_background"]!!

    // ========== Instance ==========
    f["instance_create"] = { v, args ->
        val objIdx = args[2].toInt()
        val inst = vm.runner.createInstance(objIdx, args[0].toReal(), args[1].toReal())
        inst.variables["creator"] = GMLValue.of(v.currentSelf?.id?.toDouble() ?: -4.0)
        vm.runner.fireEvent(inst, GameRunner.EVENT_CREATE, 0)
        GMLValue.of(inst.id.toDouble())
    }
    // Obsolete D&D function: action_create_object(obj, x, y) - same as instance_create but arg order differs
    f["action_create_object"] = { v, args ->
        val objIdx = args[0].toInt()
        val inst = vm.runner.createInstance(objIdx, args[1].toReal(), args[2].toReal())
        inst.variables["creator"] = GMLValue.of(v.currentSelf?.id?.toDouble() ?: -4.0)
        vm.runner.fireEvent(inst, GameRunner.EVENT_CREATE, 0)
        GMLValue.of(inst.id.toDouble())
    }
    f["instance_destroy"] = { v, _ ->
        // Undertale does not use instance_destroy(other) nor does it use instance_destroy(object)
        val self = v.currentSelf
        if (self != null) {
            v.runner.destroyInstance(self)
        }
        GMLValue.ZERO
    }
    f["instance_change"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            val newObjIdx = args[0].toInt()
            val perf = args[1].toReal() > 0.5
            if (perf) {
                vm.runner.fireEvent(self, GameRunner.EVENT_DESTROY, 0)
            }
            self.objectIndex = newObjIdx
            if (newObjIdx in vm.runner.gameData.objects.indices) {
                val objDef = vm.runner.gameData.objects[newObjIdx]
                self.spriteIndex = objDef.spriteIndex
                self.visible = objDef.visible
                self.solid = objDef.solid
                self.depth = objDef.depth
                self.persistent = objDef.persistent
                self.maskIndex = objDef.maskId
            }
            if (perf) {
                vm.runner.fireEvent(self, GameRunner.EVENT_CREATE, 0)
            }
        }
        GMLValue.ZERO
    }
    f["instance_exists"] = { _, args ->
        val targetId = args[0].toInt()
        val found = vm.runner.findInstancesByObjectOrId(targetId)
        GMLValue.of(found.isNotEmpty())
    }
    f["instance_number"] = { _, args ->
        val objIdx = args[0].toInt()
        GMLValue.of(vm.runner.instances.count { it.objectIndex == objIdx && !it.destroyed }.toDouble())
    }
    f["instance_find"] = { _, args ->
        val objIdx = args[0].toInt(); val n = args[1].toInt()
        val matches = vm.runner.instances.filter { it.objectIndex == objIdx && !it.destroyed }
        GMLValue.of(if (n in matches.indices) matches[n].id.toDouble() else -4.0)
    }

    // ========== DnD Actions ==========
    f["action_kill_object"] = { v, _ ->
        val self = v.currentSelf
        if (self != null) {
            v.runner.destroyInstance(self)
        }
        GMLValue.ZERO
    }
    f["action_move_to"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            self.x = args[0].toReal()
            self.y = args[1].toReal()
        }
        GMLValue.ZERO
    }
    f["move_towards_point"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            val targetX = args[0].toReal()
            val targetY = args[1].toReal()
            val sp = args[2].toReal()
            val dx = targetX - self.x
            val dy = targetY - self.y
            self.direction = (Math.toDegrees(atan2(-dy, dx)) + 360) % 360
            self.speed = sp
            self.hspeed = sp * cos(Math.toRadians(self.direction))
            self.vspeed = -sp * sin(Math.toRadians(self.direction))
        }
        GMLValue.ZERO
    }
    f["action_set_friction"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            self.friction = args[0].toReal()
        }
        GMLValue.ZERO
    }
    f["action_set_alarm"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            self.alarm[args[1].toInt()] = args[0].toInt()
        }
        GMLValue.ZERO
    }
    f["action_move"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            val dirString = args[0].toStr()
            val dirAngles = intArrayOf(225, 270, 315, 180, -1, 0, 135, 90, 45)
            val enabled = mutableListOf<Int>()
            for (i in dirString.indices) {
                if (dirString[i] == '1') enabled.add(i)
            }
            if (enabled.contains(4)) {
                self.speed = 0.0
                self.hspeed = 0.0
                self.vspeed = 0.0
            } else if (enabled.isNotEmpty()) {
                val chosen = enabled.random()
                val dir = dirAngles[chosen].toDouble()
                val sp = args[1].toReal()
                self.direction = dir
                self.speed = sp
                self.hspeed = sp * cos(Math.toRadians(dir))
                self.vspeed = -sp * sin(Math.toRadians(dir))
            }
        }
        GMLValue.ZERO
    }

    // ========== Paths ==========
    f["path_start"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            val pathIdx = args[0].toInt()
            val speed = args[1].toReal()
            val endAction = args[2].toInt()
            val absolute = args[3].toBool()
            val runner = vm.runner

            self.pathIndex = pathIdx
            self.pathSpeed = speed
            self.pathEndAction = endAction
            self.pathPosition = 0.0

            val startPos = runner.interpolatePathPosition(pathIdx, 0.0)
            if (startPos != null) {
                if (absolute) {
                    self.pathXOffset = 0.0
                    self.pathYOffset = 0.0
                    self.x = startPos.first
                    self.y = startPos.second
                } else {
                    // Relative: offset so path starts at current instance position
                    self.pathXOffset = self.x - startPos.first
                    self.pathYOffset = self.y - startPos.second
                }
            }
        }
        GMLValue.ZERO
    }
    f["path_end"] = { v, _ ->
        val self = v.currentSelf
        if (self != null) {
            vm.runner.pathEnd(self)
        }
        GMLValue.ZERO
    }

    // ========== Room ==========
    f["room_goto"] = { _, args -> vm.runner.gotoRoom(args[0].toInt()); GMLValue.ZERO }
    f["room_goto_next"] = { _, _ -> vm.runner.gotoRoom(vm.runner.currentRoomIndex + 1); GMLValue.ZERO }
    f["room_goto_previous"] = { _, _ -> vm.runner.gotoRoom(vm.runner.currentRoomIndex - 1); GMLValue.ZERO }
    f["room_exists"] = { _, args -> GMLValue.of(args[0].toInt() in vm.gameData.rooms.indices) }
    f["room_next"] = { _, args ->
        val ind = args[0].toInt()
        val order = vm.gameData.gen8.roomOrder
        val pos = order.indexOf(ind)
        if (pos >= 0 && pos + 1 < order.size) GMLValue.of(order[pos + 1].toDouble())
        else GMLValue.of(-1.0)
    }
    f["room_previous"] = { _, args ->
        val ind = args[0].toInt()
        val order = vm.gameData.gen8.roomOrder
        val pos = order.indexOf(ind)
        if (pos > 0) GMLValue.of(order[pos - 1].toDouble())
        else GMLValue.of(-1.0)
    }

    // ========== Keyboard ==========
    f["keyboard_check"] = { _, args -> GMLValue.of(args[0].toInt() in vm.runner.keysHeld) }
    f["keyboard_check_pressed"] = { _, args -> GMLValue.of(args[0].toInt() in vm.runner.keysPressed) }
    f["keyboard_check_released"] = { _, args -> GMLValue.of(args[0].toInt() in vm.runner.keysReleased) }
    f["keyboard_clear"] = { _, args -> vm.runner.keysHeld.remove(args[0].toInt()); GMLValue.ZERO }

    // ========== Data structures ==========
    val dsMaps = mutableMapOf<Int, MutableMap<String, GMLValue>>()
    val dsLists = mutableMapOf<Int, MutableList<GMLValue>>()
    var nextDsId = 0

    f["ds_map_create"] = { _, _ -> val id = nextDsId++; dsMaps[id] = mutableMapOf(); GMLValue.of(id.toDouble()) }
    f["ds_map_destroy"] = { _, args -> dsMaps.remove(args[0].toInt()); GMLValue.ZERO }
    f["ds_map_add"] = { _, args ->
        dsMaps[args[0].toInt()]?.put(args[1].toStr(), args[2]); GMLValue.ZERO
    }
    f["ds_map_find_value"] = { _, args ->
        dsMaps[args[0].toInt()]?.get(args[1].toStr()) ?: GMLValue.Undefined
    }
    f["ds_map_replace"] = { _, args ->
        dsMaps[args[0].toInt()]?.put(args[1].toStr(), args[2]); GMLValue.ZERO
    }
    f["ds_map_exists"] = { _, args ->
        GMLValue.of(dsMaps[args[0].toInt()]?.containsKey(args[1].toStr()) == true)
    }
    f["ds_map_delete"] = { _, args ->
        dsMaps[args[0].toInt()]?.remove(args[1].toStr()); GMLValue.ZERO
    }
    f["ds_map_size"] = { _, args -> GMLValue.of(dsMaps[args[0].toInt()]?.size?.toDouble() ?: 0.0) }
    f["ds_map_find_first"] = { _, args ->
        val map = dsMaps[args[0].toInt()]
        if (map != null && map.isNotEmpty()) GMLValue.of(map.keys.first()) else GMLValue.Undefined
    }
    f["ds_map_find_next"] = { _, args ->
        val map = dsMaps[args[0].toInt()]
        val key = args[1].toStr()
        if (map != null) {
            val keys = map.keys.toList()
            val idx = keys.indexOf(key)
            if (idx >= 0 && idx < keys.size - 1) GMLValue.of(keys[idx + 1]) else GMLValue.Undefined
        } else GMLValue.Undefined
    }
    f["ds_map_find_last"] = { _, args ->
        val map = dsMaps[args[0].toInt()]
        if (map != null && map.isNotEmpty()) GMLValue.of(map.keys.last()) else GMLValue.Undefined
    }
    f["ds_map_find_previous"] = { _, args ->
        val map = dsMaps[args[0].toInt()]
        val key = args[1].toStr()
        if (map != null) {
            val keys = map.keys.toList()
            val idx = keys.indexOf(key)
            if (idx > 0) GMLValue.of(keys[idx - 1]) else GMLValue.Undefined
        } else GMLValue.Undefined
    }
    f["ds_map_set"] = { _, args ->
        dsMaps[args[0].toInt()]?.put(args[1].toStr(), args[2]); GMLValue.ZERO
    }
    f["ds_map_clear"] = { _, args -> dsMaps[args[0].toInt()]?.clear(); GMLValue.ZERO }
    f["ds_map_copy"] = { _, args ->
        val dest = dsMaps[args[0].toInt()]
        val src = dsMaps[args[1].toInt()]
        if (dest != null && src != null) { dest.clear(); dest.putAll(src) }
        GMLValue.ZERO
    }

    f["ds_list_create"] = { _, _ -> val id = nextDsId++; dsLists[id] = mutableListOf(); GMLValue.of(id.toDouble()) }
    f["ds_list_destroy"] = { _, args -> dsLists.remove(args[0].toInt()); GMLValue.ZERO }
    f["ds_list_add"] = { _, args ->
        val list = dsLists[args[0].toInt()]
        for (i in 1 until args.size) list?.add(args[i])
        GMLValue.ZERO
    }
    f["ds_list_find_value"] = { _, args ->
        val list = dsLists[args[0].toInt()]
        val idx = args[1].toInt()
        if (list != null && idx in list.indices) list[idx] else GMLValue.Undefined
    }
    f["ds_list_size"] = { _, args -> GMLValue.of(dsLists[args[0].toInt()]?.size?.toDouble() ?: 0.0) }

    // ========== File / INI ==========
    f["file_exists"] = { _, _ -> GMLValue.FALSE }
    f["ini_open"] = { _, _ -> GMLValue.ZERO }
    f["ini_close"] = { _, _ -> GMLValue.ZERO }
    f["ini_read_real"] = { _, args -> if (args.size >= 3) args[2] else GMLValue.ZERO }
    f["ini_read_string"] = { _, args -> if (args.size >= 3) args[2] else GMLValue.EMPTY_STRING }
    f["ini_write_real"] = { _, _ -> GMLValue.ZERO }
    f["ini_write_string"] = { _, _ -> GMLValue.ZERO }

    // ========== Audio stubs ==========
    for (name in listOf(
        "audio_play_sound", "audio_stop_sound", "audio_stop_all",
        "audio_is_playing", "audio_sound_gain", "audio_sound_pitch",
        "audio_group_load", "audio_group_is_loaded",
        "audio_create_stream", "audio_destroy_stream",
        "audio_master_gain",
        "sound_play", "sound_stop", "sound_stop_all",
        "sound_is_playing", "sound_volume", "sound_loop",
    )) {
        f[name] = { _, _ -> GMLValue.ZERO }
    }

    // Undertale-specific audio stubs
    for (name in listOf(
        "caster_load", "caster_play", "caster_stop", "caster_is_playing",
        "caster_loop", "caster_volume", "caster_position",
        "caster_free", "caster_set_volume", "caster_create",
    )) {
        f[name] = { _, _ -> GMLValue.ZERO }
    }

    // ========== Type checking ==========
    f["is_undefined"] = { _, args -> GMLValue.of(args.getOrNull(0) is GMLValue.Undefined) }
    f["is_string"] = { _, args -> GMLValue.of(args.getOrNull(0) is GMLValue.Str) }
    f["is_real"] = { _, args -> GMLValue.of(args.getOrNull(0) is GMLValue.Real) }
    f["is_array"] = { _, args -> GMLValue.of(args.getOrNull(0) is GMLValue.ArrayVal) }
    f["typeof"] = { _, args ->
        GMLValue.of(when (args.getOrNull(0)) {
            is GMLValue.Real -> "number"
            is GMLValue.Str -> "string"
            is GMLValue.ArrayVal -> "array"
            is GMLValue.Undefined, null -> "undefined"
        })
    }

    // ========== OS / System ==========
    f["os_get_language"] = { _, _ -> GMLValue.of("en") }
    f["os_get_region"] = { _, _ -> GMLValue.of("US") }
    f["os_get_config"] = { _, _ -> GMLValue.of("default") }
    f["os_get_info"] = { _, _ -> GMLValue.ZERO }
    f["get_timer"] = { _, _ -> GMLValue.of((System.nanoTime() / 1000).toDouble()) }
    f["current_time"] = { _, _ -> GMLValue.of(System.currentTimeMillis().toDouble()) }
    f["date_current_datetime"] = { _, _ -> GMLValue.of(System.currentTimeMillis().toDouble() / 86400000.0 + 25569.0) }
    f["environment_get_variable"] = { _, _ -> GMLValue.EMPTY_STRING }
    f["parameter_count"] = { _, _ -> GMLValue.ZERO }
    f["parameter_string"] = { _, _ -> GMLValue.EMPTY_STRING }

    // ========== Sprite prefetch ==========
    f["sprite_prefetch"] = { _, _ -> GMLValue.ZERO }
    f["sprite_prefetch_multi"] = { _, _ -> GMLValue.ZERO }
    f["background_prefetch"] = { _, _ -> GMLValue.ZERO }

    // ========== Window ==========
    f["window_set_caption"] = { _, _ -> GMLValue.ZERO }
    f["window_get_caption"] = { _, _ -> GMLValue.of("UNDERTALE") }
    f["window_set_fullscreen"] = { _, _ -> GMLValue.ZERO }
    f["window_get_fullscreen"] = { _, _ -> GMLValue.FALSE }

    // ========== Misc ==========
    f["show_debug_message"] = { _, args ->
        println("[DEBUG] ${args.getOrNull(0)?.toStr() ?: ""}")
        GMLValue.ZERO
    }
    f["show_message"] = { _, args ->
        println("[MESSAGE] ${args.getOrNull(0)?.toStr() ?: ""}")
        GMLValue.ZERO
    }
    f["game_end"] = { _, _ ->
        println("!!! game_end() called! Enabling call logging.")
        vm.runner.shouldQuit = true; GMLValue.ZERO
    }
    f["game_restart"] = { _, _ -> GMLValue.ZERO }
    f["randomize"] = { _, _ -> GMLValue.ZERO }
    f["random_set_seed"] = { _, _ -> GMLValue.ZERO }
    f["audio_channel_num"] = { _, _ -> GMLValue.ZERO }
    f["steam_initialised"] = { _, _ -> GMLValue.FALSE }
    f["steam_stats_ready"] = { _, _ -> GMLValue.FALSE }
    f["application_surface_enable"] = { _, _ -> GMLValue.ZERO }
    f["application_surface_draw_enable"] = { _, _ -> GMLValue.ZERO }
    f["joystick_exists"] = { _, _ -> GMLValue.FALSE }
    f["gamepad_get_device_count"] = { _, _ -> GMLValue.ZERO }
    f["gamepad_is_connected"] = { _, _ -> GMLValue.FALSE }
    f["keyboard_key_press"] = { _, _ -> GMLValue.ZERO }
    f["keyboard_key_release"] = { _, _ -> GMLValue.ZERO }
    f["keyboard_check_direct"] = { _, args -> GMLValue.of(args[0].toInt() in vm.runner.keysHeld) }

    f["script_execute"] = { v, args ->
        if (args.isNotEmpty()) {
            val scriptId = args[0].toInt()
            val scriptArgs = if (args.size > 1) args.subList(1, args.size) else emptyList()
            val script = vm.gameData.scripts.getOrNull(scriptId)
            val self = v.currentSelf ?: vm.runner.instances.firstOrNull()
            if (script != null && self != null) {
                v.callFunction(script.name, scriptArgs, self, self, mutableMapOf())
            } else GMLValue.ZERO
        } else GMLValue.ZERO
    }

    f["event_inherited"] = { v, _ ->
        val self = v.currentSelf
        if (self != null) {
            vm.runner.fireEventInherited(self)
        }
        GMLValue.ZERO
    }
    f["event_user"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            val userEventNum = args[0].toInt()
            vm.runner.fireEvent(self, GameRunner.EVENT_OTHER, 10 + userEventNum)
        }
        GMLValue.ZERO
    }
    f["event_perform"] = { v, args ->
        val self = v.currentSelf
        if (self != null) {
            val eventType = args[0].toInt()
            val eventNumb = args[1].toInt()
            vm.runner.fireEvent(self, eventType, eventNumb)
        }
        GMLValue.ZERO
    }

    f["variable_global_exists"] = { _, args ->
        GMLValue.of(args[0].toStr() in vm.runner.globalVariables)
    }
    f["variable_global_get"] = { _, args ->
        vm.runner.globalVariables[args[0].toStr()] ?: GMLValue.ZERO
    }
    f["variable_global_set"] = { _, args ->
        vm.runner.globalVariables[args[0].toStr()] = args[1]; GMLValue.ZERO
    }

    f["object_get_name"] = { _, args ->
        val idx = args[0].toInt()
        GMLValue.of(if (idx in vm.gameData.objects.indices) vm.gameData.objects[idx].name else "<undefined>")
    }
    f["sprite_get_name"] = { _, args ->
        val idx = args[0].toInt()
        GMLValue.of(if (idx in vm.gameData.sprites.indices) vm.gameData.sprites[idx].name else "<undefined>")
    }
    f["sprite_get_number"] = { _, args ->
        val idx = args[0].toInt()
        GMLValue.of(if (idx in vm.gameData.sprites.indices) vm.gameData.sprites[idx].tpagIndices.size.toDouble() else 0.0)
    }
    f["sprite_get_width"] = { _, args ->
        val idx = args[0].toInt()
        GMLValue.of(if (idx in vm.gameData.sprites.indices) vm.gameData.sprites[idx].width.toDouble() else 0.0)
    }
    f["sprite_get_height"] = { _, args ->
        val idx = args[0].toInt()
        GMLValue.of(if (idx in vm.gameData.sprites.indices) vm.gameData.sprites[idx].height.toDouble() else 0.0)
    }

    f["display_get_width"] = { _, _ -> GMLValue.of(1920.0) }
    f["display_get_height"] = { _, _ -> GMLValue.of(1080.0) }
    f["window_set_size"] = { _, _ -> GMLValue.ZERO }
    f["window_set_position"] = { _, _ -> GMLValue.ZERO }
    f["window_center"] = { _, _ -> GMLValue.ZERO }
    f["display_set_gui_size"] = { _, _ -> GMLValue.ZERO }

    // ========== View variables (accessed as functions in some cases) ==========
    f["view_set_visible"] = { _, args ->
        // Stub
        GMLValue.ZERO
    }

    // ========== Surface stubs ==========
    f["surface_create"] = { _, _ -> GMLValue.of(-1.0) }
    f["surface_free"] = { _, _ -> GMLValue.ZERO }
    f["surface_set_target"] = { _, _ -> GMLValue.ZERO }
    f["surface_reset_target"] = { _, _ -> GMLValue.ZERO }
    f["surface_exists"] = { _, _ -> GMLValue.FALSE }
    f["surface_get_width"] = { _, _ -> GMLValue.of(vm.gameData.gen8.windowWidth.toDouble()) }
    f["surface_get_height"] = { _, _ -> GMLValue.of(vm.gameData.gen8.windowHeight.toDouble()) }

    // ========== Array functions ==========
    f["array_length_1d"] = { _, args ->
        val arr = args[0]
        if (arr is GMLValue.ArrayVal) GMLValue.of(arr.data[0]?.size?.toDouble() ?: 0.0)
        else GMLValue.ZERO
    }
    f["array_length_2d"] = { _, args ->
        val arr = args[0]
        if (arr is GMLValue.ArrayVal) GMLValue.of(arr.data.size.toDouble())
        else GMLValue.ZERO
    }

    // ========== Collision ==========
    f["collision_point"] = { v, args ->
        val px = args[0].toReal()
        val py = args[1].toReal()
        val obj = args[2].toInt()
        // args[3] = prec (ignored, bbox-only)
        val notme = args[4].toBool()
        val self = v.currentSelf
        val runner = vm.runner
        var result = -4.0 // noone
        for (inst in runner.findInstancesByObjectOrId(obj)) {
            if (notme && inst === self) continue
            val bb = runner.computeBBox(inst) ?: continue
            if (px >= bb.left && px <= bb.right && py >= bb.top && py <= bb.bottom) {
                result = inst.id.toDouble()
                break
            }
        }
        GMLValue.of(result)
    }

    f["collision_rectangle"] = { v, args ->
        val qx1 = args[0].toReal(); val qy1 = args[1].toReal()
        val qx2 = args[2].toReal(); val qy2 = args[3].toReal()
        val obj = args[4].toInt()
        // args[5] = prec (ignored, bbox-only)
        val notme = args[6].toBool()
        val self = v.currentSelf
        val runner = vm.runner
        val ql = minOf(qx1, qx2); val qr = maxOf(qx1, qx2)
        val qt = minOf(qy1, qy2); val qb = maxOf(qy1, qy2)
        var result = -4.0 // noone
        for (inst in runner.findInstancesByObjectOrId(obj)) {
            if (notme && inst === self) continue
            val bb = runner.computeBBox(inst) ?: continue
            if (ql <= bb.right && qr >= bb.left && qt <= bb.bottom && qb >= bb.top) {
                result = inst.id.toDouble()
                break
            }
        }
        GMLValue.of(result)
    }

    f["collision_circle"] = { v, args ->
        val cx = args[0].toReal()
        val cy = args[1].toReal()
        val rad = args[2].toReal()
        val obj = args[3].toInt()
        // args[4] = prec (ignored, bbox-only)
        val notme = args[5].toBool()
        val self = v.currentSelf
        val runner = vm.runner
        val radSq = rad * rad
        var result = -4.0 // noone
        for (inst in runner.findInstancesByObjectOrId(obj)) {
            if (notme && inst === self) continue
            val bb = runner.computeBBox(inst) ?: continue
            val nearestX = cx.coerceIn(bb.left, bb.right)
            val nearestY = cy.coerceIn(bb.top, bb.bottom)
            val dx = cx - nearestX
            val dy = cy - nearestY
            if (dx * dx + dy * dy <= radSq) {
                result = inst.id.toDouble()
                break
            }
        }
        GMLValue.of(result)
    }

    f["collision_line"] = { v, args ->
        val lx1 = args[0].toReal(); val ly1 = args[1].toReal()
        val lx2 = args[2].toReal(); val ly2 = args[3].toReal()
        val obj = args[4].toInt()
        // args[5] = prec (ignored, bbox-only)
        val notme = args[6].toBool()
        val self = v.currentSelf
        val runner = vm.runner
        val dx = lx2 - lx1; val dy = ly2 - ly1
        var result = -4.0 // noone
        for (inst in runner.findInstancesByObjectOrId(obj)) {
            if (notme && inst === self) continue
            val bb = runner.computeBBox(inst) ?: continue
            // Liang-Barsky line-AABB intersection
            var tMin = 0.0; var tMax = 1.0
            val edges = doubleArrayOf(-dx, dx, -dy, dy)
            val sides = doubleArrayOf(lx1 - bb.left, bb.right - lx1, ly1 - bb.top, bb.bottom - ly1)
            var hit = true
            for (i in 0 until 4) {
                val p = edges[i]; val q = sides[i]
                if (p == 0.0) {
                    if (q < 0.0) { hit = false; break }
                } else {
                    val t = q / p
                    if (p < 0.0) { if (t > tMin) tMin = t } else { if (t < tMax) tMax = t }
                    if (tMin > tMax) { hit = false; break }
                }
            }
            if (hit) {
                result = inst.id.toDouble()
                break
            }
        }
        GMLValue.of(result)
    }

    // scr_gettext is handled by the real GML script (gml_Script_scr_gettext)
    // which reads from global.text_data_en ds_map populated by textdata_en script
}
