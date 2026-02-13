package com.mrpowergamerbr.butterscotch.graphics

import com.mrpowergamerbr.butterscotch.data.GameData
import org.joml.Matrix4f
import org.lwjgl.BufferUtils
import org.lwjgl.opengl.GL11.*
import org.lwjgl.opengl.GL15.*
import org.lwjgl.opengl.GL20.*
import org.lwjgl.opengl.GL30.*
import org.lwjgl.stb.STBImage.*
import org.lwjgl.system.MemoryStack
import java.nio.ByteBuffer
import java.nio.FloatBuffer

class Renderer(val gameData: GameData) {
    // OpenGL textures for each TXTR page
    private val textures = IntArray(gameData.texturePages.size) { -1 }
    private val textureWidths = IntArray(gameData.texturePages.size)
    private val textureHeights = IntArray(gameData.texturePages.size)

    // Drawing state
    var drawColor: Int = 0xFFFFFF   // BGR format
    var drawAlpha: Double = 1.0
    var drawFont: Int = -1
    var drawHalign: Int = 0  // 0=left, 1=center, 2=right
    var drawValign: Int = 0  // 0=top, 1=middle, 2=bottom

    var currentView: Int = 0
    var framebufferScaleX: Float = 1.0f
    var framebufferScaleY: Float = 1.0f

    private var viewX = 0
    private var viewY = 0
    private var viewW = 640
    private var viewH = 480

    // Shader program and uniform locations
    private var shaderProgram = 0
    private var uProjection = 0
    private var uModel = 0
    private var uHasTexture = 0
    private var uTexture = 0

    // VAO/VBO
    private var vao = 0
    private var vbo = 0

    // Vertex data: 8 floats per vertex (pos.xy, tex.uv, color.rgba)
    // Max 256 quads = 1536 vertices = 12288 floats
    private val maxVertices = 1536
    private val floatsPerVertex = 8
    private val vertexData = FloatArray(maxVertices * floatsPerVertex)
    private val vertexBuffer: FloatBuffer = BufferUtils.createFloatBuffer(maxVertices * floatsPerVertex)
    private var vertexCount = 0

    // Matrix instances (reused to avoid allocation)
    private val projectionMatrix = Matrix4f()
    private val modelMatrix = Matrix4f()
    private val matrixBuffer: FloatBuffer = BufferUtils.createFloatBuffer(16)

    companion object {
        private const val VERTEX_SHADER_SOURCE = """
#version 330 core
layout (location = 0) in vec2 a_position;
layout (location = 1) in vec2 a_texCoord;
layout (location = 2) in vec4 a_color;

uniform mat4 u_projection;
uniform mat4 u_model;

out vec2 v_texCoord;
out vec4 v_color;

void main() {
    gl_Position = u_projection * u_model * vec4(a_position, 0.0, 1.0);
    v_texCoord = a_texCoord;
    v_color = a_color;
}
"""

        private const val FRAGMENT_SHADER_SOURCE = """
#version 330 core
in vec2 v_texCoord;
in vec4 v_color;

uniform bool u_hasTexture;
uniform sampler2D u_texture;

out vec4 fragColor;

void main() {
    if (u_hasTexture) {
        fragColor = texture(u_texture, v_texCoord) * v_color;
    } else {
        fragColor = v_color;
    }
}
"""
    }

    fun initialize() {
        shaderProgram = createShaderProgram()
        glUseProgram(shaderProgram)

        uProjection = glGetUniformLocation(shaderProgram, "u_projection")
        uModel = glGetUniformLocation(shaderProgram, "u_model")
        uHasTexture = glGetUniformLocation(shaderProgram, "u_hasTexture")
        uTexture = glGetUniformLocation(shaderProgram, "u_texture")

        // Texture unit 0
        glUniform1i(uTexture, 0)

        // Set model matrix to identity initially
        modelMatrix.identity()
        uploadModelMatrix()

        // Create VAO/VBO
        vao = glGenVertexArrays()
        glBindVertexArray(vao)

        vbo = glGenBuffers()
        glBindBuffer(GL_ARRAY_BUFFER, vbo)
        glBufferData(GL_ARRAY_BUFFER, (maxVertices * floatsPerVertex * 4).toLong(), GL_DYNAMIC_DRAW)

        val stride = floatsPerVertex * 4 // 32 bytes
        // a_position: location 0, vec2
        glVertexAttribPointer(0, 2, GL_FLOAT, false, stride, 0L)
        glEnableVertexAttribArray(0)
        // a_texCoord: location 1, vec2
        glVertexAttribPointer(1, 2, GL_FLOAT, false, stride, (2 * 4).toLong())
        glEnableVertexAttribArray(1)
        // a_color: location 2, vec4
        glVertexAttribPointer(2, 4, GL_FLOAT, false, stride, (4 * 4).toLong())
        glEnableVertexAttribArray(2)

        glEnable(GL_BLEND)
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)
    }

    fun dispose() {
        if (shaderProgram != 0) {
            glDeleteProgram(shaderProgram)
            shaderProgram = 0
        }
        if (vbo != 0) {
            glDeleteBuffers(vbo)
            vbo = 0
        }
        if (vao != 0) {
            glDeleteVertexArrays(vao)
            vao = 0
        }
        for (i in textures.indices) {
            if (textures[i] >= 0) {
                glDeleteTextures(textures[i])
                textures[i] = -1
            }
        }
    }

    fun setView(vx: Int, vy: Int, vw: Int, vh: Int, px: Int, py: Int, pw: Int, ph: Int) {
        viewX = vx; viewY = vy; viewW = vw; viewH = vh

        glViewport(
            (px * framebufferScaleX).toInt(),
            (py * framebufferScaleY).toInt(),
            (pw * framebufferScaleX).toInt(),
            (ph * framebufferScaleY).toInt()
        )

        // y-down orthographic: top=vy, bottom=vy+vh
        projectionMatrix.identity().ortho(
            vx.toFloat(), (vx + vw).toFloat(),
            (vy + vh).toFloat(), vy.toFloat(),
            -1f, 1f
        )
        uploadProjectionMatrix()

        // Reset model matrix
        modelMatrix.identity()
        uploadModelMatrix()
    }

    fun clear(bgColor: Int) {
        val r = (bgColor and 0xFF) / 255f
        val g = ((bgColor shr 8) and 0xFF) / 255f
        val b = ((bgColor shr 16) and 0xFF) / 255f
        glClearColor(r, g, b, 1f)
        glClear(GL_COLOR_BUFFER_BIT)
    }

    fun drawSprite(spriteIndex: Int, subImage: Int, x: Double, y: Double,
                   xscale: Double = 1.0, yscale: Double = 1.0, angle: Double = 0.0,
                   blend: Int = 0xFFFFFF, alpha: Double = 1.0) {
        if (spriteIndex < 0 || spriteIndex >= gameData.sprites.size) return
        val sprite = gameData.sprites[spriteIndex]
        val frameIdx = if (sprite.tpagIndices.isEmpty()) return else {
            subImage.mod(sprite.tpagIndices.size)
        }
        val tpagIdx = sprite.tpagIndices[frameIdx]
        if (tpagIdx < 0) return
        val tpag = gameData.texturePageItems[tpagIdx]

        val texId = ensureTexture(tpag.texturePageId)
        if (texId < 0) return

        val texW = textureWidths[tpag.texturePageId].toFloat()
        val texH = textureHeights[tpag.texturePageId].toFloat()
        if (texW == 0f || texH == 0f) return

        val u0 = tpag.sourceX / texW
        val v0 = tpag.sourceY / texH
        val u1 = (tpag.sourceX + tpag.sourceWidth) / texW
        val v1 = (tpag.sourceY + tpag.sourceHeight) / texH

        val ox = sprite.originX.toDouble()
        val oy = sprite.originY.toDouble()

        val dx = x - ox * xscale + tpag.targetX * xscale
        val dy = y - oy * yscale + tpag.targetY * yscale
        val dw = tpag.sourceWidth * xscale
        val dh = tpag.sourceHeight * yscale

        val cr = (blend and 0xFF) / 255f
        val cg = ((blend shr 8) and 0xFF) / 255f
        val cb = ((blend shr 16) and 0xFF) / 255f
        val ca = alpha.toFloat()

        // Set model matrix for rotation
        if (angle != 0.0) {
            modelMatrix.identity()
                .translate(x.toFloat(), y.toFloat(), 0f)
                .rotateZ(Math.toRadians(-angle).toFloat())
                .translate(-x.toFloat(), -y.toFloat(), 0f)
            uploadModelMatrix()
        }

        glBindTexture(GL_TEXTURE_2D, texId)
        glUniform1i(uHasTexture, 1)

        vertexCount = 0
        putQuad(dx, dy, dx + dw, dy + dh, u0, v0, u1, v1, cr, cg, cb, ca)
        flushVertices(GL_TRIANGLES)

        if (angle != 0.0) {
            modelMatrix.identity()
            uploadModelMatrix()
        }
    }

    fun drawSpritePart(spriteIndex: Int, subImage: Int, left: Int, top: Int, partWidth: Int, partHeight: Int, x: Double, y: Double) {
        if (spriteIndex < 0 || spriteIndex >= gameData.sprites.size) return
        val sprite = gameData.sprites[spriteIndex]
        val frameIdx = if (sprite.tpagIndices.isEmpty()) return else {
            subImage.mod(sprite.tpagIndices.size)
        }
        val tpagIdx = sprite.tpagIndices[frameIdx]
        if (tpagIdx < 0) return
        val tpag = gameData.texturePageItems[tpagIdx]

        val texId = ensureTexture(tpag.texturePageId)
        if (texId < 0) return

        val texW = textureWidths[tpag.texturePageId].toFloat()
        val texH = textureHeights[tpag.texturePageId].toFloat()
        if (texW == 0f || texH == 0f) return

        // Clip the requested rect against the TPAG target region (handles trimmed sprites)
        val clipLeft = maxOf(left, tpag.targetX)
        val clipTop = maxOf(top, tpag.targetY)
        val clipRight = minOf(left + partWidth, tpag.targetX + tpag.sourceWidth)
        val clipBottom = minOf(top + partHeight, tpag.targetY + tpag.sourceHeight)

        if (clipLeft >= clipRight || clipTop >= clipBottom) return

        // Map clipped region to texture coordinates
        val texX = tpag.sourceX + (clipLeft - tpag.targetX)
        val texY = tpag.sourceY + (clipTop - tpag.targetY)
        val clippedW = clipRight - clipLeft
        val clippedH = clipBottom - clipTop

        val u0 = texX / texW
        val v0 = texY / texH
        val u1 = (texX + clippedW) / texW
        val v1 = (texY + clippedH) / texH

        // Draw at (x + offset from clipping), no sprite origin applied
        val dx = x + (clipLeft - left).toDouble()
        val dy = y + (clipTop - top).toDouble()

        glBindTexture(GL_TEXTURE_2D, texId)
        glUniform1i(uHasTexture, 1)

        vertexCount = 0
        putQuad(dx, dy, dx + clippedW, dy + clippedH, u0, v0, u1, v1, 1f, 1f, 1f, 1f)
        flushVertices(GL_TRIANGLES)
    }

    fun drawBackground(tpagIndex: Int, x: Int, y: Int, tileX: Boolean, tileY: Boolean) {
        if (tpagIndex < 0 || tpagIndex >= gameData.texturePageItems.size) return
        val tpag = gameData.texturePageItems[tpagIndex]

        val texId = ensureTexture(tpag.texturePageId)
        if (texId < 0) return

        val texW = textureWidths[tpag.texturePageId].toFloat()
        val texH = textureHeights[tpag.texturePageId].toFloat()
        if (texW == 0f || texH == 0f) return

        val u0 = tpag.sourceX / texW
        val v0 = tpag.sourceY / texH
        val u1 = (tpag.sourceX + tpag.sourceWidth) / texW
        val v1 = (tpag.sourceY + tpag.sourceHeight) / texH

        glBindTexture(GL_TEXTURE_2D, texId)
        glUniform1i(uHasTexture, 1)

        vertexCount = 0

        if (tileX || tileY) {
            val w = tpag.sourceWidth
            val h = tpag.sourceHeight
            val startX = if (tileX) ((x % w) - w) else x
            val startY = if (tileY) ((y % h) - h) else y
            val endX = if (tileX) (viewX + viewW) else (x + w)
            val endY = if (tileY) (viewY + viewH) else (y + h)

            var cy = startY
            while (cy < endY) {
                var cx = startX
                while (cx < endX) {
                    // Flush if we're getting close to capacity
                    if (vertexCount + 6 > maxVertices) {
                        flushVertices(GL_TRIANGLES)
                        vertexCount = 0
                    }
                    putQuad(
                        cx.toDouble(), cy.toDouble(),
                        (cx + w).toDouble(), (cy + h).toDouble(),
                        u0, v0, u1, v1, 1f, 1f, 1f, 1f
                    )
                    cx += w
                }
                cy += h
            }
        } else {
            putQuad(
                x.toDouble(), y.toDouble(),
                (x + tpag.sourceWidth).toDouble(), (y + tpag.sourceHeight).toDouble(),
                u0, v0, u1, v1, 1f, 1f, 1f, 1f
            )
        }

        flushVertices(GL_TRIANGLES)
    }

    fun drawTile(tpagIndex: Int, x: Int, y: Int, sourceX: Int, sourceY: Int,
                 width: Int, height: Int, scaleX: Float, scaleY: Float, color: Int) {
        if (tpagIndex < 0 || tpagIndex >= gameData.texturePageItems.size) return
        val tpag = gameData.texturePageItems[tpagIndex]

        val texId = ensureTexture(tpag.texturePageId)
        if (texId < 0) return

        val texW = textureWidths[tpag.texturePageId].toFloat()
        val texH = textureHeights[tpag.texturePageId].toFloat()
        if (texW == 0f || texH == 0f) return

        // The tile's sourceX/Y is an offset within the background image.
        // The background image starts at tpag.sourceX/Y in the texture page.
        val u0 = (tpag.sourceX + sourceX) / texW
        val v0 = (tpag.sourceY + sourceY) / texH
        val u1 = (tpag.sourceX + sourceX + width) / texW
        val v1 = (tpag.sourceY + sourceY + height) / texH

        // Extract color components (ARGB)
        val a = ((color ushr 24) and 0xFF) / 255f
        val r = ((color ushr 16) and 0xFF) / 255f
        val g = ((color ushr 8) and 0xFF) / 255f
        val b = (color and 0xFF) / 255f

        glBindTexture(GL_TEXTURE_2D, texId)
        glUniform1i(uHasTexture, 1)

        vertexCount = 0
        val dx = x.toDouble()
        val dy = y.toDouble()
        val dw = (width * scaleX).toDouble()
        val dh = (height * scaleY).toDouble()
        putQuad(dx, dy, dx + dw, dy + dh, u0, v0, u1, v1, r, g, b, a)
        flushVertices(GL_TRIANGLES)
    }

    fun drawRectangle(x1: Double, y1: Double, x2: Double, y2: Double, outline: Boolean) {
        glBindTexture(GL_TEXTURE_2D, 0)
        glUniform1i(uHasTexture, 0)

        val cr = (drawColor and 0xFF) / 255f
        val cg = ((drawColor shr 8) and 0xFF) / 255f
        val cb = ((drawColor shr 16) and 0xFF) / 255f
        val ca = drawAlpha.toFloat()

        vertexCount = 0

        if (outline) {
            // GL_LINE_LOOP: 4 vertices
            putVertex(x1, y1, 0f, 0f, cr, cg, cb, ca)
            putVertex(x2, y1, 0f, 0f, cr, cg, cb, ca)
            putVertex(x2, y2, 0f, 0f, cr, cg, cb, ca)
            putVertex(x1, y2, 0f, 0f, cr, cg, cb, ca)
            flushVertices(GL_LINE_LOOP)
        } else {
            putQuad(x1, y1, x2, y2, 0f, 0f, 0f, 0f, cr, cg, cb, ca)
            flushVertices(GL_TRIANGLES)
        }
    }

    fun drawLineStrip(points: List<Pair<Double, Double>>, r: Float, g: Float, b: Float, a: Float) {
        if (points.size < 2) return

        glBindTexture(GL_TEXTURE_2D, 0)
        glUniform1i(uHasTexture, 0)

        vertexCount = 0
        for (i in points.indices) {
            if (vertexCount + 1 > maxVertices) {
                flushVertices(GL_LINE_STRIP)
                // Restart strip from previous point to keep continuity
                vertexCount = 0
                val prev = points[i - 1]
                putVertex(prev.first, prev.second, 0f, 0f, r, g, b, a)
            }
            val (px, py) = points[i]
            putVertex(px, py, 0f, 0f, r, g, b, a)
        }
        flushVertices(GL_LINE_STRIP)
    }

    fun drawText(x: Double, y: Double, text: String) {
        if (drawFont < 0 || drawFont >= gameData.fonts.size) return
        val font = gameData.fonts[drawFont]
        if (font.tpagIndex < 0) return
        val tpag = gameData.texturePageItems[font.tpagIndex]

        val texId = ensureTexture(tpag.texturePageId)
        if (texId < 0) return

        val texW = textureWidths[tpag.texturePageId].toFloat()
        val texH = textureHeights[tpag.texturePageId].toFloat()
        if (texW == 0f || texH == 0f) return

        val cr = (drawColor and 0xFF) / 255f
        val cg = ((drawColor shr 8) and 0xFF) / 255f
        val cb = ((drawColor shr 16) and 0xFF) / 255f
        val ca = drawAlpha.toFloat()

        glBindTexture(GL_TEXTURE_2D, texId)
        glUniform1i(uHasTexture, 1)

        val lines = text.split("\n")
        val lineHeight = font.emSize.toDouble()
        val textHeight = lines.size * lineHeight

        var startY = when (drawValign) {
            1 -> y - textHeight / 2
            2 -> y - textHeight
            else -> y
        }

        vertexCount = 0

        for (line in lines) {
            val lineWidth = measureLineWidth(font, line)
            var cx = when (drawHalign) {
                1 -> x - lineWidth / 2
                2 -> x - lineWidth
                else -> x
            }

            for (ch in line) {
                val glyph = font.glyphs.find { it.character == ch.code } ?: continue
                val gx = tpag.sourceX + glyph.x
                val gy = tpag.sourceY + glyph.y
                val gu0 = gx / texW
                val gv0 = gy / texH
                val gu1 = (gx + glyph.width) / texW
                val gv1 = (gy + glyph.height) / texH

                val dx = cx + glyph.offset
                val dy = startY

                if (vertexCount + 6 > maxVertices) {
                    flushVertices(GL_TRIANGLES)
                    vertexCount = 0
                }

                putQuad(dx, dy, dx + glyph.width, dy + glyph.height, gu0, gv0, gu1, gv1, cr, cg, cb, ca)
                cx += glyph.shift
            }

            startY += lineHeight
        }

        flushVertices(GL_TRIANGLES)
    }

    fun drawTextTransformed(x: Double, y: Double, text: String, xscale: Double, yscale: Double, angle: Double) {
        if (drawFont < 0 || drawFont >= gameData.fonts.size) return
        val font = gameData.fonts[drawFont]
        if (font.tpagIndex < 0) return
        val tpag = gameData.texturePageItems[font.tpagIndex]

        val texId = ensureTexture(tpag.texturePageId)
        if (texId < 0) return

        val texW = textureWidths[tpag.texturePageId].toFloat()
        val texH = textureHeights[tpag.texturePageId].toFloat()
        if (texW == 0f || texH == 0f) return

        val cr = (drawColor and 0xFF) / 255f
        val cg = ((drawColor shr 8) and 0xFF) / 255f
        val cb = ((drawColor shr 16) and 0xFF) / 255f
        val ca = drawAlpha.toFloat()

        glBindTexture(GL_TEXTURE_2D, texId)
        glUniform1i(uHasTexture, 1)

        if (angle != 0.0 || xscale != 1.0 || yscale != 1.0) {
            modelMatrix.identity()
                .translate(x.toFloat(), y.toFloat(), 0f)
            if (angle != 0.0) modelMatrix.rotateZ(Math.toRadians(-angle).toFloat())
            if (xscale != 1.0 || yscale != 1.0) modelMatrix.scale(xscale.toFloat(), yscale.toFloat(), 1f)
            modelMatrix.translate(-x.toFloat(), -y.toFloat(), 0f)
            uploadModelMatrix()
        }

        vertexCount = 0
        var cx = x
        for (ch in text) {
            val glyph = font.glyphs.find { it.character == ch.code } ?: continue
            val gx = tpag.sourceX + glyph.x
            val gy = tpag.sourceY + glyph.y
            val gu0 = gx / texW
            val gv0 = gy / texH
            val gu1 = (gx + glyph.width) / texW
            val gv1 = (gy + glyph.height) / texH

            val dx = cx + glyph.offset
            val dy = y

            if (vertexCount + 6 > maxVertices) {
                flushVertices(GL_TRIANGLES)
                vertexCount = 0
            }

            putQuad(dx, dy, dx + glyph.width, dy + glyph.height, gu0, gv0, gu1, gv1, cr, cg, cb, ca)
            cx += glyph.shift
        }

        flushVertices(GL_TRIANGLES)

        if (angle != 0.0 || xscale != 1.0 || yscale != 1.0) {
            modelMatrix.identity()
            uploadModelMatrix()
        }
    }

    fun measureStringWidth(text: String): Double {
        if (drawFont < 0 || drawFont >= gameData.fonts.size) return 0.0
        val font = gameData.fonts[drawFont]
        return text.split("\n").maxOfOrNull { measureLineWidth(font, it) } ?: 0.0
    }

    fun measureStringHeight(text: String): Double {
        if (drawFont < 0 || drawFont >= gameData.fonts.size) return 0.0
        val font = gameData.fonts[drawFont]
        return text.split("\n").size * font.emSize.toDouble()
    }

    private fun measureLineWidth(font: com.mrpowergamerbr.butterscotch.data.FontData, line: String): Double {
        var w = 0.0
        for (ch in line) {
            val glyph = font.glyphs.find { it.character == ch.code }
            w += glyph?.shift?.toDouble() ?: 0.0
        }
        return w
    }

    // --- Vertex helpers ---

    private fun putVertex(x: Double, y: Double, u: Float, v: Float, r: Float, g: Float, b: Float, a: Float) {
        val offset = vertexCount * floatsPerVertex
        vertexData[offset + 0] = x.toFloat()
        vertexData[offset + 1] = y.toFloat()
        vertexData[offset + 2] = u
        vertexData[offset + 3] = v
        vertexData[offset + 4] = r
        vertexData[offset + 5] = g
        vertexData[offset + 6] = b
        vertexData[offset + 7] = a
        vertexCount++
    }

    private fun putQuad(x1: Double, y1: Double, x2: Double, y2: Double,
                        u0: Float, v0: Float, u1: Float, v1: Float,
                        r: Float, g: Float, b: Float, a: Float) {
        // Triangle 1: top-left, top-right, bottom-right
        putVertex(x1, y1, u0, v0, r, g, b, a)
        putVertex(x2, y1, u1, v0, r, g, b, a)
        putVertex(x2, y2, u1, v1, r, g, b, a)
        // Triangle 2: top-left, bottom-right, bottom-left
        putVertex(x1, y1, u0, v0, r, g, b, a)
        putVertex(x2, y2, u1, v1, r, g, b, a)
        putVertex(x1, y2, u0, v1, r, g, b, a)
    }

    private fun flushVertices(mode: Int) {
        if (vertexCount == 0) return

        vertexBuffer.clear()
        vertexBuffer.put(vertexData, 0, vertexCount * floatsPerVertex)
        vertexBuffer.flip()

        glBindBuffer(GL_ARRAY_BUFFER, vbo)
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertexBuffer)
        glDrawArrays(mode, 0, vertexCount)

        vertexCount = 0
    }

    // --- Matrix upload helpers ---

    private fun uploadProjectionMatrix() {
        matrixBuffer.clear()
        projectionMatrix.get(matrixBuffer)
        glUniformMatrix4fv(uProjection, false, matrixBuffer)
    }

    private fun uploadModelMatrix() {
        matrixBuffer.clear()
        modelMatrix.get(matrixBuffer)
        glUniformMatrix4fv(uModel, false, matrixBuffer)
    }

    // --- Shader compilation ---

    private fun createShaderProgram(): Int {
        val vertexShader = compileShader(GL_VERTEX_SHADER, VERTEX_SHADER_SOURCE)
        val fragmentShader = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER_SOURCE)

        val program = glCreateProgram()
        glAttachShader(program, vertexShader)
        glAttachShader(program, fragmentShader)
        glLinkProgram(program)

        if (glGetProgrami(program, GL_LINK_STATUS) == GL_FALSE) {
            val log = glGetProgramInfoLog(program)
            glDeleteProgram(program)
            glDeleteShader(vertexShader)
            glDeleteShader(fragmentShader)
            throw RuntimeException("Shader program linking failed:\n$log")
        }

        // Shaders are linked, can delete the individual shader objects
        glDeleteShader(vertexShader)
        glDeleteShader(fragmentShader)

        return program
    }

    private fun compileShader(type: Int, source: String): Int {
        val shader = glCreateShader(type)
        glShaderSource(shader, source)
        glCompileShader(shader)

        if (glGetShaderi(shader, GL_COMPILE_STATUS) == GL_FALSE) {
            val log = glGetShaderInfoLog(shader)
            glDeleteShader(shader)
            val typeName = if (type == GL_VERTEX_SHADER) "vertex" else "fragment"
            throw RuntimeException("$typeName shader compilation failed:\n$log")
        }

        return shader
    }

    // --- Texture loading ---

    private fun ensureTexture(pageId: Int): Int {
        if (pageId < 0 || pageId >= textures.size) return -1
        if (textures[pageId] >= 0) return textures[pageId]

        val page = gameData.texturePages[pageId]
        val pngData = ByteArray(page.pngLength)
        val fileBuf = gameData.fileBuffer
        for (i in 0 until page.pngLength) {
            pngData[i] = fileBuf.get(page.pngOffset + i)
        }

        val directBuf = ByteBuffer.allocateDirect(pngData.size)
        directBuf.put(pngData)
        directBuf.flip()

        MemoryStack.stackPush().use { stack ->
            val w = stack.mallocInt(1)
            val h = stack.mallocInt(1)
            val comp = stack.mallocInt(1)

            val pixels = stbi_load_from_memory(directBuf, w, h, comp, 4)
            if (pixels == null) {
                System.err.println("Failed to decode texture page $pageId: ${stbi_failure_reason()}")
                return -1
            }

            val texId = glGenTextures()
            glBindTexture(GL_TEXTURE_2D, texId)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w.get(0), h.get(0), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels)

            textureWidths[pageId] = w.get(0)
            textureHeights[pageId] = h.get(0)
            textures[pageId] = texId

            stbi_image_free(pixels)
            println("  Loaded texture page $pageId: ${w.get(0)}x${h.get(0)}")
            return texId
        }
    }
}
