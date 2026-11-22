package dev.avcrt.engine.hal.sound

import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.os.SystemClock
import dev.avcrt.domain.media.SOUNDBOARD_MAX_SECONDS
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.roundToInt

/** Decode a short clip using platform codecs, then resample/downmix for the native mixer. */
internal object SoundDecoder {
    const val SAMPLE_RATE = 48_000 // Shared with avcrt_soundboard.h.
    private const val RAW_AUDIO = "audio/raw"
    private const val TIMEOUT_US = 10_000L

    suspend fun decode(path: String, clipGain: Float): ByteArray {
        val scope = currentCoroutineContext()
        val extractor = MediaExtractor()
        try {
            extractor.setDataSource(path)
            val track = (0 until extractor.trackCount).first { index ->
                extractor.getTrackFormat(index).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            }
            extractor.selectTrack(track)
            val format = extractor.getTrackFormat(track)
            val mime = checkNotNull(format.getString(MediaFormat.KEY_MIME))
            val pcm = PcmCollector().apply { configure(format) }
            if (mime == RAW_AUDIO) {
                val chunk = ByteBuffer.allocate(128 * 1024)
                while (true) {
                    scope.ensureActive()
                    chunk.clear()
                    val count = extractor.readSampleData(chunk, 0)
                    if (count < 0) break
                    chunk.position(0)
                    chunk.limit(count)
                    pcm.append(chunk)
                    extractor.advance()
                }
            } else {
                val decoder = MediaCodec.createDecoderByType(mime)
                try {
                    decoder.configure(format, null, null, 0)
                    decoder.start()
                    val info = MediaCodec.BufferInfo()
                    var inputDone = false
                    val deadline = SystemClock.elapsedRealtime() + 30_000L
                    while (true) {
                        scope.ensureActive()
                        check(SystemClock.elapsedRealtime() < deadline) { "Audio decoder timed out" }
                        if (!inputDone) {
                            val index = decoder.dequeueInputBuffer(TIMEOUT_US)
                            if (index >= 0) {
                                val input = checkNotNull(decoder.getInputBuffer(index))
                                val size = extractor.readSampleData(input, 0)
                                inputDone = size < 0
                                if (inputDone) decoder.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                                else {
                                    decoder.queueInputBuffer(index, 0, size, extractor.sampleTime, 0)
                                    extractor.advance()
                                }
                            }
                        }
                        when (val index = decoder.dequeueOutputBuffer(info, TIMEOUT_US)) {
                            MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> pcm.configure(decoder.outputFormat)
                            else -> if (index >= 0) {
                                if (info.size > 0 && info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
                                    val output = checkNotNull(decoder.getOutputBuffer(index))
                                    output.limit(info.offset + info.size)
                                    output.position(info.offset)
                                    pcm.append(output)
                                }
                                decoder.releaseOutputBuffer(index, false)
                                if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                            }
                        }
                    }
                } finally {
                    decoder.release()
                }
            }
            scope.ensureActive()
            return pcm.toPcm16(clipGain)
        } finally {
            extractor.release()
        }
    }

    internal class TooLong : Exception()

    private class PcmCollector {
        private var rate = 0
        private var channels = 0
        private var encoding = AudioFormat.ENCODING_PCM_16BIT
        private var mono = FloatArray(0)
        private var count = 0

        fun configure(format: MediaFormat) {
            val newRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            val newChannels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            val newEncoding = format.getInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
            require(newRate in 8_000..192_000 && newChannels in 1..8)
            check(count == 0 || (newRate == rate && newChannels == channels && newEncoding == encoding))
            rate = newRate
            channels = newChannels
            encoding = newEncoding
        }

        fun append(buffer: ByteBuffer) {
            val bytes = when (encoding) {
                AudioFormat.ENCODING_PCM_8BIT -> 1
                AudioFormat.ENCODING_PCM_16BIT -> 2
                AudioFormat.ENCODING_PCM_24BIT_PACKED -> 3
                AudioFormat.ENCODING_PCM_32BIT, AudioFormat.ENCODING_PCM_FLOAT -> 4
                else -> error("Unsupported PCM encoding")
            }
            val frameSize = bytes * channels
            check(buffer.remaining() % frameSize == 0)
            val frames = buffer.remaining() / frameSize
            val limit = rate * SOUNDBOARD_MAX_SECONDS
            if (frames > limit - count) throw TooLong()
            if (count + frames > mono.size) {
                mono = mono.copyOf(maxOf(count + frames, rate, mono.size * 2).coerceAtMost(limit))
            }
            buffer.order(ByteOrder.LITTLE_ENDIAN)
            repeat(frames) {
                var sum = 0f
                repeat(channels) {
                    val sample = when (encoding) {
                        AudioFormat.ENCODING_PCM_8BIT -> ((buffer.get().toInt() and 255) - 128) / 128f
                        AudioFormat.ENCODING_PCM_16BIT -> buffer.short / 32768f
                        AudioFormat.ENCODING_PCM_24BIT_PACKED -> {
                            val lo = buffer.get().toInt() and 255
                            val mid = buffer.get().toInt() and 255
                            (lo or (mid shl 8) or (buffer.get().toInt() shl 16)) / 8388608f
                        }
                        AudioFormat.ENCODING_PCM_32BIT -> buffer.int / 2147483648f
                        else -> buffer.float
                    }
                    if (sample.isFinite()) sum += sample.coerceIn(-1f, 1f)
                }
                mono[count++] = sum / channels
            }
        }

        fun toPcm16(clipGain: Float): ByteArray {
            check(count > 0) { "Empty decoded audio" }
            val frames = (count.toLong() * SAMPLE_RATE / rate).toInt().coerceAtLeast(1)
            val output = ByteBuffer.allocate(frames * Short.SIZE_BYTES).order(ByteOrder.LITTLE_ENDIAN)
            repeat(frames) { frame ->
                val offset = frame.toLong() * rate
                val start = (offset / SAMPLE_RATE).toInt()
                val fraction = (offset % SAMPLE_RATE).toFloat() / SAMPLE_RATE
                val a = mono[start]
                val b = mono[(start + 1).coerceAtMost(count - 1)]
                val sample = (a + fraction * (b - a)) * clipGain
                output.putShort((sample * 32768).roundToInt().coerceIn(-32768, 32767).toShort())
            }
            return output.array()
        }
    }
}
