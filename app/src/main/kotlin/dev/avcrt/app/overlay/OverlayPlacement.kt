package dev.avcrt.app.overlay

import kotlin.math.roundToInt

internal data class OverlayBounds(val left: Int, val top: Int, val right: Int, val bottom: Int) {
    val width get() = right - left
    val height get() = bottom - top

    fun x(point: Float, width: Int) = left + (point * (this.width - width).coerceAtLeast(0)).roundToInt()
    fun y(point: Float, height: Int) = top + (point * (this.height - height).coerceAtLeast(0)).roundToInt()
    fun clampX(x: Float, width: Int) = x.coerceIn(left.toFloat(), (right - width).coerceAtLeast(left).toFloat())
    fun clampY(y: Float, height: Int) = y.coerceIn(top.toFloat(), (bottom - height).coerceAtLeast(top).toFloat())
}

/** The bubble anchor is independent of any temporary clamping/dragging of the expanded card. */
internal class OverlayPlacement {
    private var bubbleX = 0.96f
    private var bubbleY = 0.30f
    private var cardOffsetX = 0f
    private var cardOffsetY = 0f

    fun open() { cardOffsetX = 0f; cardOffsetY = 0f }

    fun position(bounds: OverlayBounds, bubbleSize: Int, width: Int, height: Int, expanded: Boolean): Pair<Int, Int> {
        val x = bounds.x(bubbleX, bubbleSize) + if (expanded) cardOffsetX else 0f
        val y = bounds.y(bubbleY, bubbleSize) + if (expanded) cardOffsetY else 0f
        return bounds.clampX(x, width).roundToInt() to bounds.clampY(y, height).roundToInt()
    }

    fun drag(bounds: OverlayBounds, bubbleSize: Int, width: Int, height: Int, expanded: Boolean, dx: Float, dy: Float) {
        val anchorX = bounds.x(bubbleX, bubbleSize).toFloat()
        val anchorY = bounds.y(bubbleY, bubbleSize).toFloat()
        if (expanded) {
            cardOffsetX = bounds.clampX(bounds.clampX(anchorX + cardOffsetX, width) + dx, width) - anchorX
            cardOffsetY = bounds.clampY(bounds.clampY(anchorY + cardOffsetY, height) + dy, height) - anchorY
        } else {
            val travelX = (bounds.width - bubbleSize).coerceAtLeast(0)
            val travelY = (bounds.height - bubbleSize).coerceAtLeast(0)
            if (travelX > 0) bubbleX = (bubbleX + dx / travelX).coerceIn(0f, 1f)
            if (travelY > 0) bubbleY = (bubbleY + dy / travelY).coerceIn(0f, 1f)
        }
    }
}
