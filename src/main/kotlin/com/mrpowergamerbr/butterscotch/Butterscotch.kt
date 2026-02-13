package com.mrpowergamerbr.butterscotch

import com.mrpowergamerbr.butterscotch.builtin.registerBuiltins
import com.mrpowergamerbr.butterscotch.console.DebugConsole
import com.mrpowergamerbr.butterscotch.data.FormReader
import com.mrpowergamerbr.butterscotch.data.GameData
import com.mrpowergamerbr.butterscotch.graphics.Renderer
import com.mrpowergamerbr.butterscotch.runtime.GameRunner
import com.mrpowergamerbr.butterscotch.vm.VM
import org.lwjgl.BufferUtils
import org.lwjgl.glfw.Callbacks
import org.lwjgl.glfw.GLFW
import org.lwjgl.glfw.GLFWErrorCallback
import org.lwjgl.opengl.GL
import org.lwjgl.opengl.GL11
import org.lwjgl.stb.STBImageWrite
import org.lwjgl.system.MemoryUtil
import kotlinx.serialization.json.*
import java.io.File

class Butterscotch(
    private val screenshotPattern: String? = null,
    private val screenshotAtFrames: Set<Int> = emptySet(),
    private val startRoom: String? = null,
    private val speedMultiplier: Double,
    private val recordInputsPath: String? = null,
    private val playbackInputsPath: String? = null,
    private val vsync: Boolean,
    private val rngSeed: Long?
) {
    companion object {
        // yay static abuse
        lateinit var debugObj: Set<String>
        lateinit var traceCalls: Set<String>
        lateinit var ignoreFunctionTracedCalls: Set<String>
        lateinit var traceFireEvents: Set<String>
        lateinit var traceInstructions: Set<String>
        lateinit var traceGlobalVars: Set<String>
        lateinit var traceInstanceVars: Set<String>
        lateinit var tracePaths: Set<String>
        var drawPaths = false
        var alwaysLogUnknownInstructions = false
        var debug = false
    }

    private var window: Long = 0
    private lateinit var renderer: Renderer
    private lateinit var runner: GameRunner
    private val headless = screenshotAtFrames.isNotEmpty()
    private var framebufferScaleX = 1.0f
    private var framebufferScaleY = 1.0f
    private var windowWidth = 0
    private var windowHeight = 0
    private var debugPaused = false
    private var debugStepOneFrame = false
    var console: DebugConsole? = null

    // GM key code mapping (VK codes)
    private val glfwToGMKey = mapOf(
        GLFW.GLFW_KEY_LEFT to 37,
        GLFW.GLFW_KEY_UP to 38,
        GLFW.GLFW_KEY_RIGHT to 39,
        GLFW.GLFW_KEY_DOWN to 40,
        GLFW.GLFW_KEY_ENTER to 13,
        GLFW.GLFW_KEY_ESCAPE to 27,
        GLFW.GLFW_KEY_SPACE to 32,
        GLFW.GLFW_KEY_BACKSPACE to 8,
        GLFW.GLFW_KEY_TAB to 9,
        GLFW.GLFW_KEY_LEFT_SHIFT to 16,
        GLFW.GLFW_KEY_RIGHT_SHIFT to 16,
        GLFW.GLFW_KEY_LEFT_CONTROL to 17,
        GLFW.GLFW_KEY_RIGHT_CONTROL to 17,
        GLFW.GLFW_KEY_LEFT_ALT to 18,
        GLFW.GLFW_KEY_RIGHT_ALT to 18,
        GLFW.GLFW_KEY_F1 to 112,
        GLFW.GLFW_KEY_F2 to 113,
        GLFW.GLFW_KEY_F3 to 114,
        GLFW.GLFW_KEY_F4 to 115,
        GLFW.GLFW_KEY_Z to 90,
        GLFW.GLFW_KEY_X to 88,
        GLFW.GLFW_KEY_C to 67,
    )

    fun run() {
        println("Butterscotch - GameMaker: Studio (Bytecode 16) Runner")
        println("Loading game data...")

        val gameData = FormReader("undertale/game.unx").read()

        println("\nInitializing window...")
        windowWidth = gameData.gen8.windowWidth
        windowHeight = gameData.gen8.windowHeight
        initWindow(windowWidth, windowHeight, gameData.gen8.displayName)

        GL.createCapabilities()
        renderer = Renderer(gameData)
        renderer.framebufferScaleX = framebufferScaleX
        renderer.framebufferScaleY = framebufferScaleY
        renderer.initialize()

        val vm = VM(gameData)
        registerBuiltins(vm)

        // "Just to add on to this...
        // Game Maker only uses the same random seed when running the game through the IDE. Once you compile it, the seed will be truly random."
        // https://www.reddit.com/r/gamemaker/comments/9btry9/random_object_generation_always_gives_same_result/
        runner = GameRunner(gameData, vm, renderer, rngSeed ?: System.nanoTime(), rngSeed != null)
        if (debug) {
            console = DebugConsole(runner, windowWidth, windowHeight)
        }
        vm.initialize()

        // Set up input playback/recording
        if (playbackInputsPath != null) {
            runner.inputPlayback = readInputJson(playbackInputsPath)
            println("Input playback loaded from $playbackInputsPath (${runner.inputPlayback!!.size} frames with input)")
        }
        if (recordInputsPath != null) {
            runner.inputRecording = mutableMapOf()
            println("Input recording enabled, will save to $recordInputsPath")
        }

        val startRoomIndex = resolveStartRoom(gameData)
        runner.initialize(startRoomIndex)

        println("\nStarting game loop...")
        if (headless) {
            headlessLoop(gameData.gen8.windowWidth, gameData.gen8.windowHeight)
        } else {
            gameLoop()
        }

        // Save recorded inputs
        if (recordInputsPath != null && runner.inputRecording != null) {
            writeInputJson(recordInputsPath, runner.inputRecording!!)
            println("Input recording saved to $recordInputsPath (${runner.inputRecording!!.size} frames with input)")
        }

        console?.dispose()
        renderer.dispose()
        Callbacks.glfwFreeCallbacks(window)
        GLFW.glfwDestroyWindow(window)
        GLFW.glfwTerminate()
        GLFW.glfwSetErrorCallback(null)?.free()
    }

    private fun resolveStartRoom(gameData: GameData): Int? {
        val room = startRoom ?: return null
        // Try as integer index first
        room.toIntOrNull()?.let { index ->
            if (index in gameData.rooms.indices) {
                println("Starting at room index $index (${gameData.rooms[index].name})")
                return index
            }
            error("Room index $index is out of range (0..${gameData.rooms.size - 1})")
        }
        // Try as room name
        val index = gameData.rooms.indexOfFirst { it.name == room }
        if (index >= 0) {
            println("Starting at room '$room' (index $index)")
            return index
        }
        error("Room '$room' not found. Use --list-rooms to see available rooms.")
    }

    private fun initWindow(width: Int, height: Int, title: String) {
        GLFWErrorCallback.createPrint(System.err).set()
        check(GLFW.glfwInit()) { "Unable to initialize GLFW" }

        GLFW.glfwDefaultWindowHints()
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MAJOR, 3)
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MINOR, 3)
        GLFW.glfwWindowHint(GLFW.GLFW_OPENGL_PROFILE, GLFW.GLFW_OPENGL_CORE_PROFILE)
        GLFW.glfwWindowHint(GLFW.GLFW_OPENGL_FORWARD_COMPAT, GLFW.GLFW_TRUE)
        GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_FALSE)
        GLFW.glfwWindowHint(GLFW.GLFW_RESIZABLE, GLFW.GLFW_FALSE)

        window = GLFW.glfwCreateWindow(width, height, "Butterscotch - $title", MemoryUtil.NULL, MemoryUtil.NULL)
        if (window == MemoryUtil.NULL) throw RuntimeException("Failed to create GLFW window")

        if (!headless) {
            GLFW.glfwSetKeyCallback(window) { _, key, _, action, _ ->
                if (debug) {
                    // The console must NEVER be null here because debug is enabled
                    val console = console!!

                    // Toggle console with grave accent
                    if (key == GLFW.GLFW_KEY_GRAVE_ACCENT && action == GLFW.GLFW_PRESS) {
                        // We ONLY want to be able to close the console if the input buffer is empty
                        // Because if not, we aren't able to type the grave accent key without closing the console!
                        if (console.inputBuffer.isEmpty())
                            console.toggle()
                        return@glfwSetKeyCallback
                    }

                    // When console is open, route keys to it and suppress game input
                    if (console.isOpen == true) {
                        if (action == GLFW.GLFW_PRESS || action == GLFW.GLFW_REPEAT) {
                            console.onKey(key)
                        }
                        return@glfwSetKeyCallback
                    }

                    // Debug keys: room navigation
                    if (action == GLFW.GLFW_PRESS) {
                        when (key) {
                            GLFW.GLFW_KEY_PAGE_UP -> {
                                val next = runner.currentRoomIndex + 1
                                if (next < runner.gameData.rooms.size) runner.gotoRoom(next)
                                return@glfwSetKeyCallback
                            }

                            GLFW.GLFW_KEY_PAGE_DOWN -> {
                                val prev = runner.currentRoomIndex - 1
                                if (prev >= 0) runner.gotoRoom(prev)
                                return@glfwSetKeyCallback
                            }

                            GLFW.GLFW_KEY_P -> {
                                debugPaused = !debugPaused
                                println("[DEBUG] ${if (debugPaused) "Paused" else "Unpaused"}")
                                return@glfwSetKeyCallback
                            }

                            GLFW.GLFW_KEY_O -> {
                                if (debugPaused) {
                                    println("[DEBUG] Frame Stepped (Frame: ${runner.frameCount})")
                                    debugStepOneFrame = true
                                }
                                return@glfwSetKeyCallback
                            }
                        }
                    }
                }

                // Skip real input during playback mode
                if (runner.inputPlayback != null) return@glfwSetKeyCallback

                val gmKey = glfwToGMKey[key] ?: run {
                    // Letters A-Z: GLFW uses ASCII codes (65-90), GM uses same
                    if (key in GLFW.GLFW_KEY_A..GLFW.GLFW_KEY_Z) key
                    // Numbers 0-9: GLFW 48-57, GM same
                    else if (key in GLFW.GLFW_KEY_0..GLFW.GLFW_KEY_9) key
                    else return@glfwSetKeyCallback
                }

                when (action) {
                    GLFW.GLFW_PRESS -> runner.onKeyDown(gmKey)
                    GLFW.GLFW_RELEASE -> runner.onKeyUp(gmKey)
                }
            }

            // Character callback for console text input
            val console = this.console
            if (console != null) {
                GLFW.glfwSetCharCallback(window) { _, codepoint ->
                    if (console.isOpen && !console.toggledConsoleOnThisFrame) {
                        console.onChar(codepoint)
                    }
                }
            }

            // Center window
            val vidmode = GLFW.glfwGetVideoMode(GLFW.glfwGetPrimaryMonitor())
            if (vidmode != null) {
                GLFW.glfwSetWindowPos(window, (vidmode.width() - width) / 2, (vidmode.height() - height) / 2)
            }
        }

        // Set up framebuffer size callback to handle Wayland/HiDPI scaling changes
        GLFW.glfwSetFramebufferSizeCallback(window) { _, fbWidth, fbHeight ->
            framebufferScaleX = fbWidth.toFloat() / windowWidth
            framebufferScaleY = fbHeight.toFloat() / windowHeight
            if (::renderer.isInitialized) {
                renderer.framebufferScaleX = framebufferScaleX
                renderer.framebufferScaleY = framebufferScaleY
            }
        }

        GLFW.glfwMakeContextCurrent(window)
        GLFW.glfwSwapInterval(if (headless || !vsync) 0 else 1)

        if (!headless) {
            GLFW.glfwShowWindow(window)
        }

        // Query framebuffer size AFTER showing the window - on Wayland the actual
        // scale isn't known until the window is mapped to a surface
        GLFW.glfwPollEvents()
        val fbW = IntArray(1)
        val fbH = IntArray(1)
        GLFW.glfwGetFramebufferSize(window, fbW, fbH)
        framebufferScaleX = fbW[0].toFloat() / width
        framebufferScaleY = fbH[0].toFloat() / height
    }

    private fun headlessLoop(windowWidth: Int, windowHeight: Int) {
        val maxFrame = screenshotAtFrames.max()
        val pattern = screenshotPattern ?: "screenshot-%s.png"
        var frameCount = 0

        println("Headless mode: capturing screenshots at frames $screenshotAtFrames")

        while (frameCount < maxFrame && !runner.shouldQuit) {
            runner.step()
            frameCount++

            GL11.glClear(GL11.GL_COLOR_BUFFER_BIT)
            runner.draw()

            // Clear per-frame input AFTER both step and draw
            runner.clearPerFrameInput()

            if (frameCount in screenshotAtFrames) {
                val filename = pattern.replace("%s", frameCount.toString())
                captureScreenshot(filename, windowWidth, windowHeight)
                println("Screenshot saved: $filename (frame $frameCount)")
            }

            GLFW.glfwSwapBuffers(window)
            GLFW.glfwPollEvents()
        }

        println("Headless mode complete: $frameCount frames processed")
    }

    private fun gameLoop() {
        val roomSpeed = 30 // Undertale uses 30 FPS
        val targetFrameTime = 1.0 / (roomSpeed * speedMultiplier)
        var lastTime = GLFW.glfwGetTime()
        var accumulator = 0.0
        var frameCount = 0

        while (!GLFW.glfwWindowShouldClose(window) && !runner.shouldQuit) {
            val currentTime = GLFW.glfwGetTime()
            val deltaTime = currentTime - lastTime
            lastTime = currentTime
            accumulator += deltaTime

            // Cap to prevent spiral of death after lag
            if (accumulator > targetFrameTime * 3) {
                accumulator = targetFrameTime
            }

            if (debugPaused && !debugStepOneFrame) {
                // While paused, just poll for input events without advancing the game
                GLFW.glfwWaitEventsTimeout(0.05)
                lastTime = GLFW.glfwGetTime()
                accumulator = 0.0
                continue
            }

            if (accumulator >= targetFrameTime || debugStepOneFrame) {
                if (!debugStepOneFrame) {
                    accumulator -= targetFrameTime
                } else {
                    accumulator = 0.0
                    debugStepOneFrame = false
                }
                frameCount++
                if (frameCount <= 3) println("  Frame $frameCount: room=${runner.currentRoom?.name}, instances=${runner.instances.size}")

                runner.step()

                GL11.glClear(GL11.GL_COLOR_BUFFER_BIT)
                runner.draw()

                // Draw console overlay on top of everything
                console?.render(renderer)

                // Clear per-frame input AFTER both step and draw
                runner.clearPerFrameInput()

                GLFW.glfwSwapBuffers(window)
                GLFW.glfwPollEvents()
            } else {
                // Wait efficiently for next frame or input event
                val waitTime = targetFrameTime - accumulator
                GLFW.glfwWaitEventsTimeout(waitTime)
            }
        }
        println("Game loop ended: windowShouldClose=${GLFW.glfwWindowShouldClose(window)}, shouldQuit=${runner.shouldQuit}, frames=$frameCount")
    }

    private fun captureScreenshot(filename: String, width: Int, height: Int) {
        val fbW = (width * framebufferScaleX).toInt()
        val fbH = (height * framebufferScaleY).toInt()
        val buffer = BufferUtils.createByteBuffer(fbW * fbH * 4)
        GL11.glReadPixels(0, 0, fbW, fbH, GL11.GL_RGBA, GL11.GL_UNSIGNED_BYTE, buffer)
        STBImageWrite.stbi_flip_vertically_on_write(true)
        STBImageWrite.stbi_write_png(filename, fbW, fbH, 4, buffer, fbW * 4)
    }

    private fun writeInputJson(path: String, data: Map<Int, List<Int>>) {
        val jsonObj = JsonObject(
            data.entries.sortedBy { it.key }.associate { (frame, keys) ->
                frame.toString() to JsonArray(keys.map { JsonPrimitive(it) })
            }
        )
        val json = Json { prettyPrint = true }
        File(path).writeText(json.encodeToString(JsonObject.serializer(), jsonObj))
    }

    private fun readInputJson(path: String): Map<Int, List<Int>> {
        val text = File(path).readText()
        val jsonObj = Json.parseToJsonElement(text).jsonObject
        return jsonObj.entries.associate { (key, value) ->
            key.toInt() to value.jsonArray.map { it.jsonPrimitive.int }
        }
    }
}