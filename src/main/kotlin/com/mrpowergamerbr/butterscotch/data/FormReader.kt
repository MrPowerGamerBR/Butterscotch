package com.mrpowergamerbr.butterscotch.data

import com.mrpowergamerbr.butterscotch.Butterscotch
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

class FormReader(private val filePath: String) {
    private lateinit var buf: ByteBuffer

    // Maps for resolving cross-references (absolute file offset -> index)
    private val stringsByOffset = mutableMapOf<Int, Int>()
    private val tpagByOffset = mutableMapOf<Int, Int>()
    private val codeByOffset = mutableMapOf<Int, Int>()

    fun read(): GameData {
        val channel = RandomAccessFile(filePath, "r").channel
        buf = channel.map(FileChannel.MapMode.READ_ONLY, 0, channel.size())
        buf.order(ByteOrder.LITTLE_ENDIAN)

        val formTag = readTag(0)
        check(formTag == "FORM") { "Not a FORM file: $formTag" }
        val formSize = buf.getInt(4)

        // Discover chunks
        val chunks = mutableMapOf<String, Pair<Int, Int>>() // tag -> (dataOffset, size)
        var offset = 8
        while (offset < 8 + formSize) {
            val tag = readTag(offset)
            val size = buf.getInt(offset + 4)
            chunks[tag] = Pair(offset + 8, size)
            offset += 8 + size
        }

        println("Found ${chunks.size} chunks: ${chunks.keys.joinToString()}")

        // Parse in dependency order
        val strings = parseStrg(chunks["STRG"]!!)
        val gen8 = parseGen8(chunks["GEN8"]!!, strings)
        val tpagItems = parseTpag(chunks["TPAG"]!!)
        val texturePages = parseTxtr(chunks["TXTR"]!!)
        val sprites = parseSprt(chunks["SPRT"]!!)
        val backgrounds = parseBgnd(chunks["BGND"]!!)
        val paths = parsePath(chunks["PATH"]!!)
        val fonts = parseFont(chunks["FONT"]!!)
        val code = parseCode(chunks["CODE"]!!)
        val objects = parseObjt(chunks["OBJT"]!!, code)
        val rooms = parseRoom(chunks["ROOM"]!!)
        val scripts = parseScpt(chunks["SCPT"]!!)
        val variables = parseVari(chunks["VARI"]!!)
        val functions = parseFunc(chunks["FUNC"]!!)

        println("Loaded: ${sprites.size} sprites, ${objects.size} objects, ${rooms.size} rooms, ${code.size} code entries")
        println("  ${variables.size} variables, ${functions.size} functions, ${scripts.size} scripts, ${fonts.size} fonts, ${paths.size} paths")

        return GameData(
            gen8 = gen8,
            strings = strings,
            sprites = sprites,
            backgrounds = backgrounds,
            texturePageItems = tpagItems,
            texturePages = texturePages,
            paths = paths,
            objects = objects,
            rooms = rooms,
            codeEntries = code,
            variables = variables,
            functions = functions,
            scripts = scripts,
            fonts = fonts,
            fileBuffer = buf,
        )
    }

    private fun readTag(offset: Int): String {
        val bytes = ByteArray(4)
        for (i in 0..3) bytes[i] = buf.get(offset + i)
        return String(bytes, Charsets.US_ASCII)
    }

    // Reads a length-prefixed string where offset points to the u32 length field
    private fun readStringAt(offset: Int): String {
        val len = buf.getInt(offset)
        val bytes = ByteArray(len)
        for (i in 0 until len) bytes[i] = buf.get(offset + 4 + i)
        return String(bytes, Charsets.UTF_8)
    }

    // Reads a string reference where ptr points to the string content (length is at ptr-4)
    // This is the format used by GEN8, SPRT, BGND, FONT, OBJT, ROOM, SCPT, VARI, FUNC, CODE
    private fun readStringRef(ptr: Int): String {
        if (ptr == 0) return ""
        val len = buf.getInt(ptr - 4)
        val bytes = ByteArray(len)
        for (i in 0 until len) bytes[i] = buf.get(ptr + i)
        return String(bytes, Charsets.UTF_8)
    }

    // ========== STRG ==========
    private fun parseStrg(chunk: Pair<Int, Int>): List<String> {
        val (dataOffset, _) = chunk
        val count = buf.getInt(dataOffset)
        val strings = ArrayList<String>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(dataOffset + 4 + i * 4)
            stringsByOffset[ptr] = i
            strings.add(readStringAt(ptr))
        }

        println("  STRG: $count strings")
        return strings
    }

    // ========== GEN8 ==========
    private fun parseGen8(chunk: Pair<Int, Int>, strings: List<String>): Gen8Info {
        val (d, _) = chunk
        val bytecodeVersion = buf.get(d + 1).toInt() and 0xFF
        val gameNamePtr = buf.getInt(d + 0x28)
        val displayNamePtr = buf.getInt(d + 0x64)
        val gameId = buf.getInt(d + 0x14)
        val windowWidth = buf.getInt(d + 0x3C)
        val windowHeight = buf.getInt(d + 0x40)
        val roomOrderCount = buf.getInt(d + 0x80)
        val roomOrder = (0 until roomOrderCount).map { buf.getInt(d + 0x84 + it * 4) }

        val info = Gen8Info(
            bytecodeVersion = bytecodeVersion,
            gameName = readStringRef(gameNamePtr),
            displayName = readStringRef(displayNamePtr),
            gameId = gameId,
            windowWidth = windowWidth,
            windowHeight = windowHeight,
            roomOrder = roomOrder,
        )
        println("  GEN8: '${info.gameName}' BC${info.bytecodeVersion} ${info.windowWidth}x${info.windowHeight} ${roomOrder.size} rooms")
        return info
    }

    // ========== TPAG ==========
    private fun parseTpag(chunk: Pair<Int, Int>): List<TexturePageItemData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val items = ArrayList<TexturePageItemData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            tpagByOffset[ptr] = i
            items.add(
                TexturePageItemData(
                    sourceX = buf.getShort(ptr).toInt() and 0xFFFF,
                    sourceY = buf.getShort(ptr + 2).toInt() and 0xFFFF,
                    sourceWidth = buf.getShort(ptr + 4).toInt() and 0xFFFF,
                    sourceHeight = buf.getShort(ptr + 6).toInt() and 0xFFFF,
                    targetX = buf.getShort(ptr + 8).toInt() and 0xFFFF,
                    targetY = buf.getShort(ptr + 10).toInt() and 0xFFFF,
                    targetWidth = buf.getShort(ptr + 12).toInt() and 0xFFFF,
                    targetHeight = buf.getShort(ptr + 14).toInt() and 0xFFFF,
                    boundingWidth = buf.getShort(ptr + 16).toInt() and 0xFFFF,
                    boundingHeight = buf.getShort(ptr + 18).toInt() and 0xFFFF,
                    texturePageId = buf.getShort(ptr + 20).toInt() and 0xFFFF,
                )
            )
        }

        println("  TPAG: $count items")
        return items
    }

    // ========== TXTR ==========
    private fun parseTxtr(chunk: Pair<Int, Int>): List<TexturePageData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val pages = ArrayList<TexturePageData>(count)

        // Read all entries first to compute PNG sizes
        val offsets = mutableListOf<Int>()
        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val pngOffset = buf.getInt(ptr + 4)
            offsets.add(pngOffset)
        }

        for (i in 0 until count) {
            val pngOffset = offsets[i]
            val pngEnd = if (i < count - 1) offsets[i + 1] else (d + chunk.second)
            pages.add(TexturePageData(pngOffset = pngOffset, pngLength = pngEnd - pngOffset))
        }

        println("  TXTR: $count texture pages")
        return pages
    }

    // ========== SPRT ==========
    private fun parseSprt(chunk: Pair<Int, Int>): List<SpriteData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val sprites = ArrayList<SpriteData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val namePtr = buf.getInt(ptr)
            val width = buf.getInt(ptr + 4)
            val height = buf.getInt(ptr + 8)
            val marginLeft = buf.getInt(ptr + 0x0C)
            val marginRight = buf.getInt(ptr + 0x10)
            val marginBottom = buf.getInt(ptr + 0x14)
            val marginTop = buf.getInt(ptr + 0x18)
            val originX = buf.getInt(ptr + 0x30)
            val originY = buf.getInt(ptr + 0x34)
            val sepMasks = buf.getInt(ptr + 0x2C) // 0=AxisAlignedRect, 1=Precise, 2=RotatedRect
            val subImageCount = buf.getInt(ptr + 0x38)

            val tpagIndices = (0 until subImageCount).map { j ->
                val tpagPtr = buf.getInt(ptr + 0x3C + j * 4)
                tpagByOffset[tpagPtr] ?: -1
            }

            // Parse collision mask data after TPAG pointers
            val maskDataOffset = ptr + 0x3C + subImageCount * 4
            val maskCount = buf.getInt(maskDataOffset)
            val stride = (width + 7) / 8
            val maskLen = stride * height
            val masks = ArrayList<ByteArray>(maskCount)
            var maskOff = maskDataOffset + 4
            for (m in 0 until maskCount) {
                val maskData = ByteArray(maskLen)
                for (b in 0 until maskLen) {
                    maskData[b] = buf.get(maskOff + b)
                }
                masks.add(maskData)
                maskOff += maskLen
            }

            sprites.add(
                SpriteData(
                    name = readStringRef(namePtr),
                    width = width,
                    height = height,
                    marginLeft = marginLeft,
                    marginRight = marginRight,
                    marginTop = marginTop,
                    marginBottom = marginBottom,
                    originX = originX,
                    originY = originY,
                    tpagIndices = tpagIndices,
                    collisionMaskType = sepMasks,
                    masks = masks,
                )
            )
        }

        println("  SPRT: $count sprites")
        return sprites
    }

    // ========== BGND ==========
    private fun parseBgnd(chunk: Pair<Int, Int>): List<BackgroundData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val bgs = ArrayList<BackgroundData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val namePtr = buf.getInt(ptr)
            val tpagPtr = buf.getInt(ptr + 0x10)
            bgs.add(
                BackgroundData(
                    name = readStringRef(namePtr),
                    tpagIndex = tpagByOffset[tpagPtr] ?: -1,
                )
            )
        }

        println("  BGND: $count backgrounds")
        return bgs
    }

    // ========== PATH ==========
    private fun parsePath(chunk: Pair<Int, Int>): List<PathData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val paths = ArrayList<PathData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val namePtr = buf.getInt(ptr)
            val isSmooth = buf.getInt(ptr + 4) != 0
            val isClosed = buf.getInt(ptr + 8) != 0
            val precision = buf.getInt(ptr + 12)

            // Points are stored inline (UndertaleSimpleList): count + N * 12 bytes
            val pointCount = buf.getInt(ptr + 16)
            val points = ArrayList<PathPointData>(pointCount)
            for (j in 0 until pointCount) {
                val pp = ptr + 20 + j * 12
                points.add(
                    PathPointData(
                        x = buf.getFloat(pp),
                        y = buf.getFloat(pp + 4),
                        speed = buf.getFloat(pp + 8),
                    )
                )
            }

            paths.add(
                PathData(
                    name = readStringRef(namePtr),
                    isSmooth = isSmooth,
                    isClosed = isClosed,
                    precision = precision,
                    points = points,
                )
            )
        }

        println("  PATH: $count paths")
        return paths
    }

    // ========== FONT ==========
    private fun parseFont(chunk: Pair<Int, Int>): List<FontData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val fonts = ArrayList<FontData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val namePtr = buf.getInt(ptr)
            val displayNamePtr = buf.getInt(ptr + 4)
            val emSize = buf.getInt(ptr + 8)
            // bold at +12, italic at +16
            // rangeStart(u16) at +20, charset(u8) at +22, aaLevel(u8) at +23
            // rangeEnd at +24
            val tpagPtr = buf.getInt(ptr + 28)
            val scaleX = buf.getFloat(ptr + 32)
            val scaleY = buf.getFloat(ptr + 36)

            // Glyphs follow at ptr + 40
            val glyphCount = buf.getInt(ptr + 40)
            val glyphs = ArrayList<FontGlyphData>(glyphCount)
            for (j in 0 until glyphCount) {
                val gPtr = buf.getInt(ptr + 44 + j * 4)
                glyphs.add(
                    FontGlyphData(
                        character = buf.getShort(gPtr).toInt() and 0xFFFF,
                        x = buf.getShort(gPtr + 2).toInt() and 0xFFFF,
                        y = buf.getShort(gPtr + 4).toInt() and 0xFFFF,
                        width = buf.getShort(gPtr + 6).toInt() and 0xFFFF,
                        height = buf.getShort(gPtr + 8).toInt() and 0xFFFF,
                        shift = buf.getShort(gPtr + 10).toInt() and 0xFFFF,
                        offset = buf.getShort(gPtr + 12).toInt() and 0xFFFF,
                    )
                )
            }

            fonts.add(
                FontData(
                    name = readStringRef(namePtr),
                    displayName = readStringRef(displayNamePtr),
                    emSize = emSize,
                    tpagIndex = tpagByOffset[tpagPtr] ?: -1,
                    scaleX = scaleX,
                    scaleY = scaleY,
                    glyphs = glyphs,
                )
            )
        }

        println("  FONT: $count fonts")
        return fonts
    }

    // ========== CODE ==========
    private fun parseCode(chunk: Pair<Int, Int>): List<CodeEntryData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val entries = ArrayList<CodeEntryData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            codeByOffset[ptr] = i

            val namePtr = buf.getInt(ptr)
            val length = buf.getInt(ptr + 4)
            val localsCount = buf.getShort(ptr + 8).toInt() and 0xFFFF
            val argsCount = buf.getShort(ptr + 10).toInt() and 0x7FFF
            val relativeOffset = buf.getInt(ptr + 12)
            // Bytecode absolute address = (ptr + 12) + relativeOffset
            val bytecodeAddr = (ptr + 12) + relativeOffset

            val bytecode = ByteArray(length)
            for (j in 0 until length) {
                bytecode[j] = buf.get(bytecodeAddr + j)
            }

            entries.add(
                CodeEntryData(
                    name = readStringRef(namePtr),
                    localsCount = localsCount,
                    argumentsCount = argsCount,
                    bytecodeAbsoluteOffset = bytecodeAddr,
                    bytecodeLength = length,
                    bytecode = bytecode,
                )
            )
        }

        println("  CODE: $count entries")
        return entries
    }

    // ========== OBJT ==========
    private fun parseObjt(chunk: Pair<Int, Int>, codeEntries: List<CodeEntryData>): List<GameObjectData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val objects = ArrayList<GameObjectData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val namePtr = buf.getInt(ptr)
            val spriteIndex = buf.getInt(ptr + 4)
            val visible = buf.getInt(ptr + 8) != 0
            val solid = buf.getInt(ptr + 0x0C) != 0
            val depth = buf.getInt(ptr + 0x10)
            val persistent = buf.getInt(ptr + 0x14) != 0
            val parentId = buf.getInt(ptr + 0x18)
            val maskId = buf.getInt(ptr + 0x1C)

            // Physics: enabled(4) + sensor(4) + shape(4) + density(4) + restitution(4) +
            //   group(4) + linearDamping(4) + angularDamping(4) + vertexCount(4) +
            //   friction(4) + awake(4) + kinematic(4) = 52 bytes at +0x20
            // Then vertex data (vertexCount * 8 bytes, 2 floats each)
            val physicsVertexCount = buf.getInt(ptr + 0x40)
            val eventsStart = ptr + 0x50 + physicsVertexCount * 8

            // Events are stored as nested pointer lists:
            // Outer: count(u32) + ptrs[count] (one per event category, typically 13)
            // Each category ptr -> sub-event list: count(u32) + ptrs[count]
            // Each sub-event ptr -> subtype(u32) + action pointer list
            // Action pointer list: count(u32) + ptrs[count]
            // Each action ptr -> 52-byte action data (codeId at +0x20 is a direct CODE index)
            val outerCount = buf.getInt(eventsStart)
            val events = ArrayList<List<EventEntry>>(outerCount)
            val objName = readStringRef(namePtr)
            val isDebugObj = objName in Butterscotch.debugObj || i.toString() in Butterscotch.debugObj

            if (isDebugObj) {
                println("  DEBUG OBJT '$objName' (index=$i): ptr=0x${ptr.toString(16)}, spriteIndex=$spriteIndex, solid=$solid, parentId=$parentId, maskId=$maskId, depth=$depth, persistent=$persistent")
                println("    physicsEnabled=${buf.getInt(ptr+0x20)}, sensor=${buf.getInt(ptr+0x24)}, shape=${buf.getInt(ptr+0x28)}")
                println("    density=${buf.getFloat(ptr+0x2C)}, restitution=${buf.getFloat(ptr+0x30)}, group=${buf.getInt(ptr+0x34)}")
                println("    linearDamp=${buf.getFloat(ptr+0x38)}, angularDamp=${buf.getFloat(ptr+0x3C)}")
                println("    +0x40=0x${buf.getInt(ptr+0x40).toString(16)}, +0x44=0x${buf.getInt(ptr+0x44).toString(16)}")
                println("    +0x48=0x${buf.getInt(ptr+0x48).toString(16)}, +0x4C=0x${buf.getInt(ptr+0x4C).toString(16)}")
                println("    physicsVertexCount=$physicsVertexCount, eventsStart=0x${eventsStart.toString(16)}")
                println("    outerCount=$outerCount")
            }

            for (eventTypeIdx in 0 until outerCount) {
                val categoryPtr = buf.getInt(eventsStart + 4 + eventTypeIdx * 4)
                val subEventCount = buf.getInt(categoryPtr)
                val eventList = ArrayList<EventEntry>(subEventCount)

                if (isDebugObj && subEventCount > 0) {
                    println("    Category $eventTypeIdx: $subEventCount sub-events at ptr=0x${categoryPtr.toString(16)}")
                }

                for (e in 0 until subEventCount) {
                    val eventPtr = buf.getInt(categoryPtr + 4 + e * 4)
                    val subtype = buf.getInt(eventPtr)

                    val actionCount = buf.getInt(eventPtr + 4)
                    val actions = ArrayList<EventAction>(actionCount)
                    for (a in 0 until actionCount) {
                        val actionPtr = buf.getInt(eventPtr + 8 + a * 4)
                        val rawCodeId = buf.getInt(actionPtr + 0x20)
                        // Check if rawCodeId is a file offset (pointer to CODE entry) or direct index
                        val codeId = codeByOffset[rawCodeId] ?: rawCodeId
                        if (isDebugObj) {
                            val resolvedViaOffset = codeByOffset.containsKey(rawCodeId)
                            val codeName = if (codeId in codeEntries.indices) codeEntries[codeId].name else "INVALID"
                            println("      sub=$subtype action=$a: rawCodeId=0x${rawCodeId.toString(16)} resolved=$resolvedViaOffset -> idx=$codeId ($codeName)")
                        }
                        actions.add(EventAction(codeId = codeId))
                    }

                    eventList.add(EventEntry(subtype = subtype, actions = actions))
                }

                events.add(eventList)
            }

            objects.add(
                GameObjectData(
                    name = readStringRef(namePtr),
                    spriteIndex = spriteIndex,
                    visible = visible,
                    solid = solid,
                    depth = depth,
                    persistent = persistent,
                    parentId = parentId,
                    maskId = maskId,
                    events = events,
                )
            )
        }

        println("  OBJT: $count objects")
        return objects
    }

    // ========== ROOM ==========
    private fun parseRoom(chunk: Pair<Int, Int>): List<RoomData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val rooms = ArrayList<RoomData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val namePtr = buf.getInt(ptr)
            val captionPtr = buf.getInt(ptr + 4)
            val width = buf.getInt(ptr + 8)
            val height = buf.getInt(ptr + 0x0C)
            val speed = buf.getInt(ptr + 0x10)
            val persistent = buf.getInt(ptr + 0x14) != 0
            val bgColor = buf.getInt(ptr + 0x18)
            val drawBgColor = buf.getInt(ptr + 0x1C) != 0
            val creationCodeId = buf.getInt(ptr + 0x20)
            val flags = buf.getInt(ptr + 0x24)
            val bgListPtr = buf.getInt(ptr + 0x28)
            val viewListPtr = buf.getInt(ptr + 0x2C)
            val objListPtr = buf.getInt(ptr + 0x30)
            val tileListPtr = buf.getInt(ptr + 0x34)

            // Parse backgrounds
            val bgCount = buf.getInt(bgListPtr)
            val backgrounds = (0 until bgCount).map { j ->
                val bp = buf.getInt(bgListPtr + 4 + j * 4)
                RoomBackgroundData(
                    enabled = buf.getInt(bp) != 0,
                    foreground = buf.getInt(bp + 4) != 0,
                    bgDefIndex = buf.getInt(bp + 8),
                    x = buf.getInt(bp + 12),
                    y = buf.getInt(bp + 16),
                    tileX = buf.getInt(bp + 20) != 0,
                    tileY = buf.getInt(bp + 24) != 0,
                    speedX = buf.getInt(bp + 28),
                    speedY = buf.getInt(bp + 32),
                    stretch = buf.getInt(bp + 36) != 0,
                )
            }

            // Parse views
            val viewCount = buf.getInt(viewListPtr)
            val views = (0 until viewCount).map { j ->
                val vp = buf.getInt(viewListPtr + 4 + j * 4)
                RoomViewData(
                    enabled = buf.getInt(vp) != 0,
                    viewX = buf.getInt(vp + 4),
                    viewY = buf.getInt(vp + 8),
                    viewW = buf.getInt(vp + 12),
                    viewH = buf.getInt(vp + 16),
                    portX = buf.getInt(vp + 20),
                    portY = buf.getInt(vp + 24),
                    portW = buf.getInt(vp + 28),
                    portH = buf.getInt(vp + 32),
                    borderH = buf.getInt(vp + 36),
                    borderV = buf.getInt(vp + 40),
                    speedH = buf.getInt(vp + 44),
                    speedV = buf.getInt(vp + 48),
                    followObjectId = buf.getInt(vp + 52),
                )
            }

            // Parse instances
            val objCount = buf.getInt(objListPtr)
            val instances = (0 until objCount).map { j ->
                val op = buf.getInt(objListPtr + 4 + j * 4)
                RoomInstanceData(
                    x = buf.getInt(op),
                    y = buf.getInt(op + 4),
                    objectDefId = buf.getInt(op + 8),
                    instanceId = buf.getInt(op + 12),
                    creationCodeId = buf.getInt(op + 16),
                    scaleX = buf.getFloat(op + 20),
                    scaleY = buf.getFloat(op + 24),
                    color = buf.getInt(op + 28),
                    rotation = buf.getFloat(op + 32),
                )
            }

            // Parse tiles
            val tileCount = buf.getInt(tileListPtr)
            val tiles = (0 until tileCount).map { j ->
                val tp = buf.getInt(tileListPtr + 4 + j * 4)
                RoomTileData(
                    x = buf.getInt(tp),
                    y = buf.getInt(tp + 4),
                    bgDefIndex = buf.getInt(tp + 8),
                    sourceX = buf.getInt(tp + 12),
                    sourceY = buf.getInt(tp + 16),
                    width = buf.getInt(tp + 20),
                    height = buf.getInt(tp + 24),
                    depth = buf.getInt(tp + 28),
                    instanceId = buf.getInt(tp + 32),
                    scaleX = buf.getFloat(tp + 36),
                    scaleY = buf.getFloat(tp + 40),
                    color = buf.getInt(tp + 44),
                )
            }

            // Resolve creation code to CODE index
            val resolvedCreationCodeId = if (creationCodeId >= 0) {
                codeByOffset[creationCodeId] ?: -1
            } else -1

            rooms.add(
                RoomData(
                    name = readStringRef(namePtr),
                    caption = readStringRef(captionPtr),
                    width = width,
                    height = height,
                    speed = speed,
                    persistent = persistent,
                    bgColor = bgColor,
                    drawBgColor = drawBgColor,
                    creationCodeId = resolvedCreationCodeId,
                    flags = flags,
                    backgrounds = backgrounds,
                    views = views,
                    instances = instances.map { inst ->
                        RoomInstanceData(
                            x = inst.x,
                            y = inst.y,
                            objectDefId = inst.objectDefId,
                            instanceId = inst.instanceId,
                            creationCodeId = if (inst.creationCodeId >= 0) codeByOffset[inst.creationCodeId] ?: -1 else -1,
                            scaleX = inst.scaleX,
                            scaleY = inst.scaleY,
                            color = inst.color,
                            rotation = inst.rotation,
                        )
                    },
                    tiles = tiles,
                )
            )
        }

        println("  ROOM: $count rooms")
        return rooms
    }

    // ========== SCPT ==========
    private fun parseScpt(chunk: Pair<Int, Int>): List<ScriptData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val scripts = ArrayList<ScriptData>(count)

        for (i in 0 until count) {
            val ptr = buf.getInt(d + 4 + i * 4)
            val namePtr = buf.getInt(ptr)
            val codeId = buf.getInt(ptr + 4)
            scripts.add(ScriptData(name = readStringRef(namePtr), codeId = codeId))
        }

        println("  SCPT: $count scripts")
        return scripts
    }

    // ========== VARI ==========
    private fun parseVari(chunk: Pair<Int, Int>): List<VariableData> {
        val (d, size) = chunk
        // Header: VarCount1 (u32), VarCount2 (u32), MaxLocalVarCount (u32)
        val varCount1 = buf.getInt(d)
        val varCount2 = buf.getInt(d + 4)
        val maxLocalVarCount = buf.getInt(d + 8)

        val variables = ArrayList<VariableData>()
        var offset = d + 12
        val end = d + size
        while (offset + 20 <= end) {
            val namePtr = buf.getInt(offset)
            val instanceType = buf.getInt(offset + 4)
            val varId = buf.getInt(offset + 8)
            val occurrenceCount = buf.getInt(offset + 12)
            val firstOcc = buf.getInt(offset + 16)
            variables.add(
                VariableData(
                    name = readStringRef(namePtr),
                    instanceType = instanceType,
                    varId = varId,
                    occurrenceCount = occurrenceCount,
                    firstOccurrenceOffset = firstOcc,
                )
            )
            offset += 20
        }

        println("  VARI: ${variables.size} variables (count1=$varCount1, maxLocal=$maxLocalVarCount)")
        return variables
    }

    // ========== FUNC ==========
    private fun parseFunc(chunk: Pair<Int, Int>): List<FunctionData> {
        val (d, _) = chunk
        val count = buf.getInt(d)
        val functions = ArrayList<FunctionData>(count)

        var offset = d + 4
        for (i in 0 until count) {
            val namePtr = buf.getInt(offset)
            val occurrenceCount = buf.getInt(offset + 4)
            val firstOcc = buf.getInt(offset + 8)
            functions.add(
                FunctionData(
                    name = readStringRef(namePtr),
                    occurrenceCount = occurrenceCount,
                    firstOccurrenceOffset = firstOcc,
                )
            )
            offset += 12
        }

        println("  FUNC: $count functions")
        return functions
    }
}
