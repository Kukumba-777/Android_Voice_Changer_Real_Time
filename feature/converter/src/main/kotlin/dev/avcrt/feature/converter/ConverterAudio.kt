package dev.avcrt.feature.converter

import android.annotation.SuppressLint
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import kotlin.coroutines.coroutineContext

/**
 * Mic capture → playback for the AI converter (mono 48 kHz PCM16). A trimmed sibling of the
 * preset editor's PreviewRecorder; the converter feature can't depend on another feature module,
 * so it carries its own copy. No effect is applied here — the recorded mic is the RAW input the
 * RVC engine converts.
 */
class ConverterAudio(private val dispatchers: DispatcherProvider) {

    @Volatile private var record: AudioRecord? = null
    @Volatile private var track: AudioTrack? = null
    @Volatile private var stopRequested = false

    /** Soft-stop the in-flight capture: the loop returns what it has so far. */
    fun requestStop() {
        stopRequested = true
    }

    @SuppressLint("MissingPermission") // the screen requests RECORD_AUDIO before calling this
    suspend fun record(seconds: Int): ShortArray = withContext(dispatchers.io) {
        stopRequested = false
        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
        ).coerceAtLeast(SAMPLE_RATE)
        val rec = AudioRecord(
            MediaRecorder.AudioSource.MIC,
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
            minBuf,
        )
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            throw IllegalStateException("AudioRecord init failed")
        }
        record = rec
        val total = SAMPLE_RATE * seconds
        val out = ShortArray(total)
        try {
            rec.startRecording()
            var off = 0
            val chunk = SAMPLE_RATE / 10
            while (off < total) {
                if (stopRequested) break
                coroutineContext.ensureActive()
                val want = minOf(chunk, total - off)
                val n = rec.read(out, off, want)
                if (n <= 0) break
                off += n
            }
            if (off < total) out.copyOf(off) else out
        } finally {
            runCatching { rec.stop() }
            rec.release()
            record = null
        }
    }

    suspend fun play(pcm: ShortArray) = withContext(dispatchers.io) {
        if (pcm.isEmpty()) return@withContext
        val minBuf = AudioTrack.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT,
        ).coerceAtLeast(pcm.size * 2)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(SAMPLE_RATE)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
            )
            .setBufferSizeInBytes(minBuf)
            .setTransferMode(AudioTrack.MODE_STATIC)
            .build()
        track = t
        try {
            t.write(pcm, 0, pcm.size)
            t.play()
            while (t.playbackHeadPosition < pcm.size && t.playState == AudioTrack.PLAYSTATE_PLAYING) {
                coroutineContext.ensureActive()
                delay(50)
            }
        } finally {
            runCatching { t.stop() }
            t.release()
            track = null
        }
    }

    /**
     * Continuous duplex loop for realtime: capture 16 kHz mic in ~100 ms chunks, run each through
     * [convert] (the streaming converter → 48 kHz PCM16), and stream the result to the speaker. Runs
     * until [isActive] returns false or the coroutine is cancelled. Use headphones to avoid feedback.
     */
    @SuppressLint("MissingPermission")
    suspend fun liveLoop(convert: (ShortArray) -> ShortArray, isActive: () -> Boolean) = withContext(dispatchers.io) {
        val inRate = 16000
        val outRate = 48000
        val recMin = AudioRecord.getMinBufferSize(inRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            .coerceAtLeast(inRate)
        val rec = AudioRecord(
            MediaRecorder.AudioSource.MIC, inRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, recMin,
        )
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release(); throw IllegalStateException("AudioRecord init failed")
        }
        record = rec
        val trkMin = AudioTrack.getMinBufferSize(outRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
            .coerceAtLeast(outRate)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(outRate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
            )
            .setBufferSizeInBytes(trkMin)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track = t
        val inChunk = ShortArray(inRate / 10)   // ~100 ms
        try {
            rec.startRecording()
            t.play()
            while (isActive()) {
                coroutineContext.ensureActive()
                val n = rec.read(inChunk, 0, inChunk.size)
                if (n <= 0) continue
                val out = convert(if (n == inChunk.size) inChunk else inChunk.copyOf(n))
                if (out.isNotEmpty()) t.write(out, 0, out.size)
            }
        } finally {
            runCatching { rec.stop() }; rec.release(); record = null
            runCatching { t.stop() }; t.release(); track = null
        }
    }

    fun stop() {
        runCatching { record?.stop() }
        runCatching { record?.release() }
        record = null
        runCatching { track?.stop() }
        runCatching { track?.release() }
        track = null
    }

    companion object {
        const val SAMPLE_RATE = 48000
    }
}
