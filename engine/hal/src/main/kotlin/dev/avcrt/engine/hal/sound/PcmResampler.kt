package dev.avcrt.engine.hal.sound

import java.io.OutputStream
import kotlin.math.roundToInt

/** Stateful linear resampling; a codec buffer boundary never resets the phase. */
internal class PcmResampler(
    private val inputRate: Int,
    private val outputRate: Int,
    private val output: OutputStream,
) {
    private val buffer = ByteArray(32 * 1024)
    private var used = 0
    private var frames = 0L
    private var inputPosition = 0L
    private var nextOutputPosition = 0L
    private var previous = 0f

    fun append(sample: Float) {
        while (nextOutputPosition < inputPosition) {
            val fraction = (nextOutputPosition - (inputPosition - outputRate)).toFloat() / outputRate
            emit(previous + (sample - previous) * fraction)
        }
        previous = sample
        inputPosition += outputRate
    }

    fun finish(): Long {
        check(inputPosition != 0L) { "Empty decoded audio" }
        while (nextOutputPosition < inputPosition) emit(previous)
        if (used > 0) flush()
        return frames
    }

    private fun emit(sample: Float) {
        val pcm = (sample * 32768).roundToInt().coerceIn(-32768, 32767)
        buffer[used++] = pcm.toByte()
        buffer[used++] = (pcm shr 8).toByte()
        if (used == buffer.size) flush()
        frames++
        nextOutputPosition += inputRate
    }

    private fun flush() {
        output.write(buffer, 0, used)
        used = 0
    }
}
