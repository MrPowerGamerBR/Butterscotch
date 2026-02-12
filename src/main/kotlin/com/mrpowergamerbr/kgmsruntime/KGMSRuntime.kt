package com.mrpowergamerbr.kgmsruntime

import com.github.ajalt.clikt.core.CliktCommand
import com.github.ajalt.clikt.parameters.options.multiple
import com.github.ajalt.clikt.parameters.options.option
import com.github.ajalt.clikt.parameters.types.int
import com.mrpowergamerbr.kgmsruntime.builtin.registerBuiltins
import com.mrpowergamerbr.kgmsruntime.data.FormReader
import com.mrpowergamerbr.kgmsruntime.graphics.Renderer
import com.mrpowergamerbr.kgmsruntime.runtime.GameRunner
import com.mrpowergamerbr.kgmsruntime.vm.VM
import org.lwjgl.BufferUtils
import org.lwjgl.glfw.Callbacks
import org.lwjgl.glfw.GLFW
import org.lwjgl.glfw.GLFWErrorCallback
import org.lwjgl.opengl.GL
import org.lwjgl.opengl.GL11
import org.lwjgl.stb.STBImageWrite
import org.lwjgl.system.MemoryUtil

class KGMSRuntime(
    private val screenshotPattern: String? = null,
    private val screenshotAtFrames: Set<Int> = emptySet()
) {
    private var window: Long = 0
    private lateinit var runner: GameRunner
    private val headless = screenshotAtFrames.isNotEmpty()
    private var framebufferScaleX = 1.0f
    private var framebufferScaleY = 1.0f

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
        println("KGMSRuntime - GameMaker: Studio Bytecode 16 Runner")
        println("Loading game data...")

        val gameData = FormReader("undertale/game.unx").read()

        println("\nInitializing window...")
        initWindow(gameData.gen8.windowWidth, gameData.gen8.windowHeight, gameData.gen8.displayName)

        GL.createCapabilities()
        val renderer = Renderer(gameData)
        renderer.framebufferScaleX = framebufferScaleX
        renderer.framebufferScaleY = framebufferScaleY
        renderer.initialize()

        val vm = VM(gameData)
        registerBuiltins(vm)

        runner = GameRunner(gameData, vm, renderer)
        vm.initialize()
        runner.initialize()

        println("\nStarting game loop...")
        if (headless) {
            headlessLoop(gameData.gen8.windowWidth, gameData.gen8.windowHeight)
        } else {
            gameLoop()
        }

        renderer.dispose()
        Callbacks.glfwFreeCallbacks(window)
        GLFW.glfwDestroyWindow(window)
        GLFW.glfwTerminate()
        GLFW.glfwSetErrorCallback(null)?.free()
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

        window = GLFW.glfwCreateWindow(width, height, "KGMSRuntime - $title", MemoryUtil.NULL, MemoryUtil.NULL)
        if (window == MemoryUtil.NULL) throw RuntimeException("Failed to create GLFW window")

        if (!headless) {
            GLFW.glfwSetKeyCallback(window) { _, key, _, action, _ ->
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

            // Center window
            val vidmode = GLFW.glfwGetVideoMode(GLFW.glfwGetPrimaryMonitor())
            if (vidmode != null) {
                GLFW.glfwSetWindowPos(window, (vidmode.width() - width) / 2, (vidmode.height() - height) / 2)
            }
        }

        GLFW.glfwMakeContextCurrent(window)
        GLFW.glfwSwapInterval(if (headless) 0 else 1)

        // Query actual framebuffer size for HiDPI/Wayland scaling
        val fbW = IntArray(1)
        val fbH = IntArray(1)
        GLFW.glfwGetFramebufferSize(window, fbW, fbH)
        framebufferScaleX = fbW[0].toFloat() / width
        framebufferScaleY = fbH[0].toFloat() / height

        if (!headless) {
            GLFW.glfwShowWindow(window)
        }
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
        val targetFrameTime = 1.0 / roomSpeed
        var lastTime = GLFW.glfwGetTime()
        var accumulator = 0.0
        var frameCount = 0

        while (!GLFW.glfwWindowShouldClose(window) && !runner.shouldQuit) {
            frameCount++
            if (frameCount <= 3) println("  Frame $frameCount: room=${runner.currentRoom?.name}, instances=${runner.instances.size}")
            val currentTime = GLFW.glfwGetTime()
            val deltaTime = currentTime - lastTime
            lastTime = currentTime
            accumulator += deltaTime

            // Fixed timestep
            while (accumulator >= targetFrameTime) {
                runner.step()
                accumulator -= targetFrameTime
            }

            // Draw
            GL11.glClear(GL11.GL_COLOR_BUFFER_BIT)
            runner.draw()

            GLFW.glfwSwapBuffers(window)
            GLFW.glfwPollEvents()
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

    companion object {
        @JvmStatic
        fun main(args: Array<String>) {
            KGMSRuntimeCommand().main(args)
        }
    }
}

class KGMSRuntimeCommand : CliktCommand(name = "kgmsruntime") {
    private val screenshot by option("--screenshot", help = "Screenshot filename pattern (%s = frame number)")
    private val screenshotAtFrame by option("--screenshot-at-frame", help = "Frame number to capture screenshot").int().multiple()

    override fun run() {
        KGMSRuntime(
            screenshotPattern = screenshot,
            screenshotAtFrames = screenshotAtFrame.toSet()
        ).run()
    }
}
