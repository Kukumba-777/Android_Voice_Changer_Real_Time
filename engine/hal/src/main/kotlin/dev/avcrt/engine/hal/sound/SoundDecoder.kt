package dev.avcrt.engine.hal.sound

import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.os.SystemClock
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** Decode and write mono PCM incrementally; memory use does not depend on duration. */
internal object SoundDecoder {
    const val SAMPLE_RATE = 48_000 // Shared with avcrt_soundboard.h.
    private const val RAW_AUDIO = "audio/raw"
    private const val TIMEOUT_US = 10_000L
    private const val STALL_TIMEOUT_MS = 30_000L

    suspend fun decode(path: String, output: OutputStream): Long {
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
            val pcm = PcmWriter(output)
            if (mime == RAW_AUDIO) {
                pcm.configure(format)
                var chunk = ByteBuffer.allocate(128 * 1024)
                while (true) {
                    scope.ensureActive()
                    val size = extractor.sampleSize
                    if (size < 0) break
                    // Raw Android extractors yield small frames, not an entire audio file.
                    require(size <= 8 * 1024 * 1024) { "Oversized raw audio packet" }
                    if (size > chunk.capacity()) chunk = ByteBuffer.allocate(size.toInt())
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
                    var lastProgress = SystemClock.elapsedRealtime()
                    while (true) {
                        scope.ensureActive()
                        check(SystemClock.elapsedRealtime() - lastProgress < STALL_TIMEOUT_MS) { "Audio decoder stalled" }
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
                                lastProgress = SystemClock.elapsedRealtime()
                            }
                        }
                        when (val index = decoder.dequeueOutputBuffer(info, TIMEOUT_US)) {
                            MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> {
                                pcm.configure(decoder.outputFormat)
                                lastProgress = SystemClock.elapsedRealtime()
                            }
                            else -> if (index >= 0) {
                                if (info.size > 0 && info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0) {
                                    if (!pcm.configured) pcm.configure(decoder.outputFormat)
                                    val output = checkNotNull(decoder.getOutputBuffer(index))
                                    output.limit(info.offset + info.size)
                                    output.position(info.offset)
                                    pcm.append(output)
                                    lastProgress = SystemClock.elapsedRealtime()
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
            return pcm.finish()
        } finally {
            extractor.release()
        }
    }

    private class PcmWriter(private val output: OutputStream) {
        private var rate = 0
        private var channels = 0
        private var encoding = AudioFormat.ENCODING_PCM_16BIT
        private lateinit var resampler: PcmResampler
        val configured get() = rate != 0

        fun configure(format: MediaFormat) {
            val newRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            val newChannels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            val newEncoding = format.getInteger(MediaFormat.KEY_PCM_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
            require(newRate in 1..384_000 && newChannels in 1..32)
            check(!configured || (newRate == rate && newChannels == channels && newEncoding == encoding))
            if (!configured) resampler = PcmResampler(newRate, SAMPLE_RATE, output)
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
                resampler.append(sum / channels)
            }
        }

        fun finish(): Long {
            check(configured) { "Empty decoded audio" }
            return resampler.finish()
        }
    }
}
