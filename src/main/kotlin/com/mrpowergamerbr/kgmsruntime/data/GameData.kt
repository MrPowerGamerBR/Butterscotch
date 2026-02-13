package com.mrpowergamerbr.kgmsruntime.data

class Gen8Info(
    val bytecodeVersion: Int,
    val gameName: String,
    val displayName: String,
    val gameId: Int,
    val windowWidth: Int,
    val windowHeight: Int,
    val roomOrder: List<Int>,
)

class SpriteData(
    val name: String,
    val width: Int,
    val height: Int,
    val marginLeft: Int,
    val marginRight: Int,
    val marginTop: Int,
    val marginBottom: Int,
    val originX: Int,
    val originY: Int,
    val tpagIndices: List<Int>,
)

class BackgroundData(
    val name: String,
    val tpagIndex: Int,
)

class TexturePageItemData(
    val sourceX: Int,
    val sourceY: Int,
    val sourceWidth: Int,
    val sourceHeight: Int,
    val targetX: Int,
    val targetY: Int,
    val targetWidth: Int,
    val targetHeight: Int,
    val boundingWidth: Int,
    val boundingHeight: Int,
    val texturePageId: Int,
)

class TexturePageData(
    val pngOffset: Int,
    val pngLength: Int,
)

class EventAction(
    val codeId: Int,
)

class EventEntry(
    val subtype: Int,
    val actions: List<EventAction>,
)

class GameObjectData(
    val name: String,
    val spriteIndex: Int,
    val visible: Boolean,
    val solid: Boolean,
    val depth: Int,
    val persistent: Boolean,
    val parentId: Int,
    val maskId: Int,
    val events: List<List<EventEntry>>,
)

class RoomBackgroundData(
    val enabled: Boolean,
    val foreground: Boolean,
    val bgDefIndex: Int,
    val x: Int,
    val y: Int,
    val tileX: Boolean,
    val tileY: Boolean,
    val speedX: Int,
    val speedY: Int,
    val stretch: Boolean,
)

class RoomViewData(
    var enabled: Boolean,
    var viewX: Int,
    var viewY: Int,
    var viewW: Int,
    var viewH: Int,
    var portX: Int,
    var portY: Int,
    var portW: Int,
    var portH: Int,
    var borderH: Int,
    var borderV: Int,
    var speedH: Int,
    var speedV: Int,
    var followObjectId: Int,
)

class RoomInstanceData(
    val x: Int,
    val y: Int,
    val objectDefId: Int,
    val instanceId: Int,
    val creationCodeId: Int,
    val scaleX: Float,
    val scaleY: Float,
    val color: Int,
    val rotation: Float,
)

class RoomTileData(
    val x: Int,
    val y: Int,
    val bgDefIndex: Int,
    val sourceX: Int,
    val sourceY: Int,
    val width: Int,
    val height: Int,
    val depth: Int,
    val instanceId: Int,
    val scaleX: Float,
    val scaleY: Float,
    val color: Int,
)

class RoomData(
    val name: String,
    val caption: String,
    val width: Int,
    val height: Int,
    val speed: Int,
    val persistent: Boolean,
    val bgColor: Int,
    val drawBgColor: Boolean,
    val creationCodeId: Int,
    val flags: Int,
    val backgrounds: List<RoomBackgroundData>,
    val views: List<RoomViewData>,
    val instances: List<RoomInstanceData>,
    val tiles: List<RoomTileData>,
)

class CodeEntryData(
    val name: String,
    val localsCount: Int,
    val argumentsCount: Int,
    val bytecodeAbsoluteOffset: Int,
    val bytecodeLength: Int,
    val bytecode: ByteArray,
)

class VariableData(
    val name: String,
    val instanceType: Int,
    val varId: Int,
    val occurrenceCount: Int,
    val firstOccurrenceOffset: Int,
)

class FunctionData(
    val name: String,
    val occurrenceCount: Int,
    val firstOccurrenceOffset: Int,
)

class ScriptData(
    val name: String,
    val codeId: Int,
)

class FontGlyphData(
    val character: Int,
    val x: Int,
    val y: Int,
    val width: Int,
    val height: Int,
    val shift: Int,
    val offset: Int,
)

class FontData(
    val name: String,
    val displayName: String,
    val emSize: Int,
    val tpagIndex: Int,
    val scaleX: Float,
    val scaleY: Float,
    val glyphs: List<FontGlyphData>,
)

class PathPointData(
    val x: Float,
    val y: Float,
    val speed: Float,
)

class PathData(
    val name: String,
    val isSmooth: Boolean,
    val isClosed: Boolean,
    val precision: Int,
    val points: List<PathPointData>,
)

class GameData(
    val gen8: Gen8Info,
    val strings: List<String>,
    val sprites: List<SpriteData>,
    val backgrounds: List<BackgroundData>,
    val texturePageItems: List<TexturePageItemData>,
    val texturePages: List<TexturePageData>,
    val paths: List<PathData>,
    val objects: List<GameObjectData>,
    val rooms: List<RoomData>,
    val codeEntries: List<CodeEntryData>,
    val variables: List<VariableData>,
    val functions: List<FunctionData>,
    val scripts: List<ScriptData>,
    val fonts: List<FontData>,
    val fileBuffer: java.nio.ByteBuffer,
)
