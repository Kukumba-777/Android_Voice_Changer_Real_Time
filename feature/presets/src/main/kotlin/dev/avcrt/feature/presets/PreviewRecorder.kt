package dev.avcrt.feature.presets

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import timber.log.Timber
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.coroutines.coroutineContext

/**
 * In-app capture → playback used by the editor's "Test" button so the user can hear an unsaved
 * preset on their own voice WITHOUT it going out on calls/other apps.
 *
 * HOW THE EFFECT GETS APPLIED: this app is an ordinary app process, so the system-wide Zygisk hook
 * (which pitches every AudioRecord capture) runs HERE too. While a preview preset is live in the
 * `avcrt.dsp.*` props, the mic buffer this recorder reads is already voice-changed in-process — no
 * separate DSP path needed; it exercises the exact same engine the real feature uses. We then play
 * the captured (already-effected) PCM straight back.
 *
 * Mono 48 kHz PCM16 — the engine is rate-relative, and 48 k is the device's native capture rate.
 */
@Singleton
class PreviewRecorder @Inject constructor(
    private val dispatchers: DispatcherProvider,
) {
    @Volatile private var record: AudioRecord? = null
    @Volatile private var track: AudioTrack? = null
    @Volatile private var stopRequested = false

    /**
     * Soft-stop the in-flight [record]: the capture loop finishes and RETURNS what it has so far (the
     * user tapped "Stop" — they still want to hear the few seconds they recorded). Distinct from
     * cancelling the coroutine / [stop], which discards everything.
     */
    fun requestStop() {
        stopRequested = true
    }

    /**
     * Captures up to [seconds] of mic audio, or until [requestStop] — returns the (possibly shorter)
     * PCM16 mono samples. Cancelling the coroutine or calling [stop] aborts with no result.
     */
    suspend fun record(seconds: Int): ShortArray = withContext(dispatchers.io) {
        stopRequested = false
        val minBuf = AudioRecord.getMinBufferSize(
            SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
        ).coerceAtLeast(SAMPLE_RATE)  // ≥1 s of headroom
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
            // Read in small chunks (~0.1 s) — a single read() sized to the whole clip blocks the
            // thread until that much audio arrives, so Stop would only register seconds later. Small
            // reads return ~10×/s, making the stopRequested / cancellation checks responsive.
            val chunk = SAMPLE_RATE / 10
            while (off < total) {
                if (stopRequested) break          // soft stop → keep & return what we captured
                coroutineContext.ensureActive()   // hard cancel (leave editor) → throw, no result
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

    /** Plays back captured PCM16 mono. Suspends until playback finishes or is cancelled/[stop]ped. */
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
            // MODE_STATIC plays the written buffer once; wait for the head to reach the end.
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

    /** Force-stops any in-flight record/playback (e.g. user left the editor). */
    fun stop() {
        runCatching { record?.stop() }
        runCatching { record?.release() }
        record = null
        runCatching { track?.stop() }
        runCatching { track?.release() }
        track = null
    }

    private companion object {
        const val SAMPLE_RATE = 48000
    }
}
