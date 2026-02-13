package com.mrpowergamerbr.kgmsruntime

import com.github.ajalt.clikt.core.CliktCommand
import com.github.ajalt.clikt.parameters.options.default
import com.github.ajalt.clikt.parameters.options.flag
import com.github.ajalt.clikt.parameters.options.multiple
import com.github.ajalt.clikt.parameters.options.option
import com.github.ajalt.clikt.parameters.types.double
import com.github.ajalt.clikt.parameters.types.int
import com.mrpowergamerbr.kgmsruntime.data.FormReader

class KGMSRuntimeCommand : CliktCommand(name = "kgmsruntime") {
    private val screenshot by option("--screenshot", help = "Screenshot filename pattern (%s = frame number)")
    private val screenshotAtFrame by option("--screenshot-at-frame", help = "Frame number to capture screenshot").int().multiple()
    private val room by option("--room", help = "Start at a specific room (name or index)")
    private val listRooms by option("--list-rooms", help = "List all rooms and exit").flag()
    private val debugObj by option("--debug-obj", help = "Debug object name").multiple()
    private val traceCalls by option("--trace-calls", help = "Print all function calls of a specific object, can be set to \"*\" to log all objects").multiple()
    private val ignoreFunctionTracedCalls by option("--ignore-function-traced-calls", help = "Ignore specific function when tracing calls, useful to trim down logs").multiple()
    private val debug by option("--debug", help = "Enable debug mode").flag()
    private val speed by option("--speed", help = "Game speed multiplier (e.g. 2.0 = twice as fast)").double().default(1.0)
    private val recordInputs by option("--record-inputs", help = "Record inputs to JSON file")
    private val playbackInputs by option("--playback-inputs", help = "Playback inputs from JSON file")

    override fun run() {
        KGMSRuntime.debugObj = debugObj.toSet()
        KGMSRuntime.traceCalls = traceCalls.toSet()
        KGMSRuntime.ignoreFunctionTracedCalls = ignoreFunctionTracedCalls.toSet()
        KGMSRuntime.debug = debug

        if (listRooms) {
            val gameData = FormReader("undertale/game.unx").read()
            for ((i, room) in gameData.rooms.withIndex()) {
                println("$i: ${room.name} (${room.width}x${room.height})")
            }
            return
        }

        KGMSRuntime(
            screenshotPattern = screenshot,
            screenshotAtFrames = screenshotAtFrame.toSet(),
            startRoom = room,
            speedMultiplier = speed,
            recordInputsPath = recordInputs,
            playbackInputsPath = playbackInputs
        ).run()
    }
}

fun main(args: Array<String>) {
    KGMSRuntimeCommand().main(args)
}