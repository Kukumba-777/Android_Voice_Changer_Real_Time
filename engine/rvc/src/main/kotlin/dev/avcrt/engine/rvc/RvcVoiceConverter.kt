package dev.avcrt.engine.rvc

import ai.onnxruntime.OnnxTensor
import ai.onnxruntime.OrtEnvironment
import ai.onnxruntime.OrtSession
import ai.onnxruntime.TensorInfo
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.R
import dev.avcrt.core.common.result.AvcrtError
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.UserMessage
import dev.avcrt.domain.converter.ConversionParams
import dev.avcrt.domain.converter.SharedModels
import dev.avcrt.domain.converter.VoiceConverter
import dev.avcrt.domain.converter.VoiceModel
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import timber.log.Timber
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.nio.LongBuffer
import java.util.Random
import javax.inject.Inject
import javax.inject.Singleton
import kotlin.math.ln
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Phase-A RVC voice conversion via ONNX Runtime. Pipeline (= RVC `vc_infer`, FAISS index skipped):
 *
 *   inputPCM → resample 16k → ContentVec(vec-768-l12) → feats[T0,C]
 *            → upsample ×2 → feats[T,C]
 *   16k audio → RMVPE → f0[Hz] → resize to T → transpose(key) → coarse pitch[1..255] + pitchf
 *   net_g( phone=feats, phone_lengths=T, pitch, pitchf, ds=0, rnd=randn[1,192,T] ) → audio[N]
 *            → resample model-SR → outputSR → PCM16
 *
 * net_g ONNX contract is fixed by RVC's exporter (opset 18): inputs phone[1,T,C] f32 / phone_lengths[1]
 * i64 / pitch[1,T] i64 / pitchf[1,T] f32 / ds[1] i64 / rnd[1,192,T] f32, output = audio[1,N] f32.
 *
 * ── ON-DEVICE VERIFY POINTS (the 3 things only a real run nails down; flagged in code) ──
 *  (V1) ContentVec input rank (we adapt [1,n] vs [1,1,n]) + that output[0] is the [1,T,C] features.
 *  (V2) RMVPE output is f0 in Hz at ~100 Hz frames (some exports emit a salience map instead → would
 *       need argmax→cents decode). If pitch sounds wrong/flat, this is the suspect.
 *  (V3) VoiceModel.sampleRate must match the model (32k/40k/48k) — wrong SR = chipmunk/slow output.
 */
@Singleton
class RvcVoiceConverter @Inject constructor(
    private val dispatchers: DispatcherProvider,
) : VoiceConverter {

    private val env: OrtEnvironment by lazy { OrtEnvironment.getEnvironment() }
    private val lock = Mutex()

    private var contentVec: OrtSession? = null
    private var f0Session: OrtSession? = null
    private var netG: OrtSession? = null
    private var voice: VoiceModel? = null
    private data class ModelFile(val path: String, val bytes: Long, val modifiedAt: Long) {
        constructor(file: File) : this(file.path, file.length(), file.lastModified())
    }

    private var fingerprint: List<ModelFile>? = null

    override suspend fun prepare(shared: SharedModels, voice: VoiceModel): AvcrtResult<Unit> =
        withContext(dispatchers.io) {
            lock.withLock {
                // The Files screen can delete and re-import a model under the same name.
                val fp = listOf(shared.contentVecPath, shared.f0Path, voice.netGPath)
                    .map { path -> ModelFile(File(path)) }
                if (fp == fingerprint && netG != null) {
                    this@RvcVoiceConverter.voice = voice
                    return@withLock AvcrtResult.Success(Unit)
                }
                runCatching {
                    releaseLocked()
                    // NNAPI gave ~0 gain on VITS/NSF-HiFiGAN (ops fall back to CPU) + slower load → CPU+threads.
                    contentVec = makeSession(shared.contentVecPath, tryNnapi = false)
                    f0Session = makeSession(shared.f0Path, tryNnapi = false)
                    netG = makeSession(voice.netGPath, tryNnapi = false)
                    this@RvcVoiceConverter.voice = voice
                    fingerprint = fp
                    Timber.tag(TAG).i(
                        "prepared netg in=%s out=%s (sr=%d ch=%d); cv in=%s; f0 in=%s",
                        netG!!.inputNames, netG!!.outputNames, voice.sampleRate, voice.vecChannels,
                        contentVec!!.inputNames, f0Session!!.inputNames,
                    )
                    if (NET_G_INPUTS.any { it !in netG!!.inputNames }) {
                        Timber.tag(TAG).w(
                            "net_g input names %s differ from expected %s — run may fail",
                            netG!!.inputNames, NET_G_INPUTS,
                        )
                    }
                }.fold(
                    onSuccess = { AvcrtResult.Success(Unit) },
                    onFailure = { e ->
                        Timber.tag(TAG).e(e, "prepare failed")
                        releaseLocked()
                        AvcrtResult.Failure(AvcrtError.Io("Failed to load models: ${e.message}", e))
                    },
                )
            }
        }

    override suspend fun convert(
        inputPcm: ShortArray,
        inputSampleRate: Int,
        outputSampleRate: Int,
        params: ConversionParams,
    ): AvcrtResult<ShortArray> = withContext(dispatchers.default) {
        lock.withLock {
            val cv = contentVec
            val f0s = f0Session
            val ng = netG
            val v = voice
            if (cv == null || f0s == null || ng == null || v == null) {
                return@withLock AvcrtResult.Failure(
                    AvcrtError.Unknown("Converter is not prepared; call prepare() first"),
                )
            }
            if (inputPcm.isEmpty()) {
                return@withLock AvcrtResult.Failure(AvcrtError.Unknown("Input recording is empty"))
            }
            runCatching {
                val tStart = System.nanoTime()
                val durMs = (inputPcm.size * 1000L / inputSampleRate).coerceAtLeast(1)
                val mono16k = resample(toFloat(inputPcm), inputSampleRate, FEATURE_RATE)
                val tA = System.nanoTime()
                val feats = upsample2x(runContentVec(cv, mono16k, v.vecChannels))   // (V1)
                val cvMs = (System.nanoTime() - tA) / 1_000_000
                val t = feats.size
                require(t > 0) { "Content features are empty" }

                val tB = System.nanoTime()
                val f0 = estimateF0(mono16k, t)                                     // (V2) pure-DSP YIN
                val yinMs = (System.nanoTime() - tB) / 1_000_000
                if (params.transposeSemitones != 0) {
                    val shift = Math.pow(2.0, params.transposeSemitones / 12.0).toFloat()
                    for (i in f0.indices) f0[i] *= shift
                }
                val pitch = LongArray(t) { coarseF0(f0[it]) }

                val tC = System.nanoTime()
                val audio = runNetGChunked(ng, feats, f0, pitch, v.vecChannels)
                val netgMs = (System.nanoTime() - tC) / 1_000_000
                val rendered = resample(audio, v.sampleRate, outputSampleRate)       // (V3)
                val pcm = toPcm16(normalizePeak(rendered))
                val totalMs = (System.nanoTime() - tStart) / 1_000_000
                val blocks = (t + NETG_BLOCK - 1) / NETG_BLOCK
                Timber.tag(TAG).i(
                    "BENCH audio=%dms T=%d blocks=%d | contentvec=%dms yin=%dms netg=%dms (per-block~%dms) total=%dms | RTF=%.2fx",
                    durMs, t, blocks, cvMs, yinMs, netgMs, if (blocks > 0) netgMs / blocks else netgMs,
                    totalMs, totalMs.toFloat() / durMs,
                )
                pcm
            }.fold(
                onSuccess = { AvcrtResult.Success(it) },
                onFailure = { e ->
                    Timber.tag(TAG).e(e, "convert failed")
                    AvcrtResult.Failure(
                        AvcrtError.Io(
                            "Voice conversion failed: ${e.message}",
                            e,
                            UserMessage(R.string.error_converter_failed),
                        ),
                    )
                },
            )
        }
    }

    // ───────────────────────── streaming (realtime) ─────────────────────────
    // Sliding-window streaming: keep CTX frames of context + process BLOCK new frames per step,
    // output only the BLOCK, crossfade the seam. Needs a dynamic-T model (runNetG at the window T).
    private val streamLock = Any()
    private var strmBuf = FloatArray(0)   // recent 16k mono input
    private var strmPending = 0           // new samples since the last block emitted
    private var strmTail = FloatArray(0)  // previous block's output tail (for crossfade)

    override fun streamReset() {
        synchronized(streamLock) {
            strmBuf = FloatArray(0); strmPending = 0; strmTail = FloatArray(0)
        }
    }

    override fun streamFeed(inPcm16: ShortArray, inRate: Int, outRate: Int, params: ConversionParams): ShortArray {
        val cv = contentVec; val ng = netG; val v = voice
        if (cv == null || ng == null || v == null || inPcm16.isEmpty()) return ShortArray(0)
        synchronized(streamLock) {
            val add = resample(toFloat(inPcm16), inRate, FEATURE_RATE)
            strmBuf = if (strmBuf.isEmpty()) add else strmBuf + add
            strmPending += add.size
            val blockSamples = STREAM_BLOCK_FRAMES * STREAM_HOP
            val windowSamples = (STREAM_CTX_FRAMES + STREAM_BLOCK_FRAMES) * STREAM_HOP
            val parts = ArrayList<ShortArray>()
            while (strmPending >= blockSamples && strmBuf.size >= windowSamples) {
                try {
                    val window = strmBuf.copyOfRange(strmBuf.size - windowSamples, strmBuf.size)
                    val feats = upsample2x(runContentVec(cv, window, v.vecChannels))
                    val t = feats.size
                    require(t > 0)
                    val f0 = estimateF0(window, t)
                    if (params.transposeSemitones != 0) {
                        val sh = Math.pow(2.0, params.transposeSemitones / 12.0).toFloat()
                        for (i in f0.indices) f0[i] *= sh
                    }
                    val pitch = LongArray(t) { coarseF0(f0[it]) }
                    val audioFull = runNetG(ng, feats, f0, pitch, v.vecChannels)
                    val sPerFrame = (audioFull.size / t).coerceAtLeast(1)
                    val blockLen = (STREAM_BLOCK_FRAMES * sPerFrame).coerceAtMost(audioFull.size)
                    val blockOut = audioFull.copyOfRange(audioFull.size - blockLen, audioFull.size)
                    val xf = minOf(STREAM_XFADE_FRAMES * sPerFrame, blockOut.size, strmTail.size)
                    for (i in 0 until xf) {
                        val w = (i + 1f) / (xf + 1f)
                        blockOut[i] = strmTail[strmTail.size - xf + i] * (1f - w) + blockOut[i] * w
                    }
                    val keepTail = (STREAM_XFADE_FRAMES * sPerFrame).coerceAtMost(blockOut.size)
                    strmTail = blockOut.copyOfRange(blockOut.size - keepTail, blockOut.size)
                    parts.add(toPcm16(resample(normalizePeak(blockOut), v.sampleRate, outRate)))
                    strmPending -= blockSamples
                    if (strmBuf.size > windowSamples) {
                        strmBuf = strmBuf.copyOfRange(strmBuf.size - windowSamples, strmBuf.size)
                    }
                } catch (e: Throwable) {
                    Timber.tag(TAG).e(e, "stream block failed (baked model needs T=200? use a dynamic model)")
                    strmPending = 0
                    break
                }
            }
            if (parts.isEmpty()) return ShortArray(0)
            val total = parts.sumOf { it.size }
            val out = ShortArray(total)
            var off = 0
            for (p in parts) {
                System.arraycopy(p, 0, out, off, p.size)
                off += p.size
            }
            return out
        }
    }

    override fun release() = releaseLocked()

    private fun releaseLocked() {
        runCatching { contentVec?.close() }; contentVec = null
        runCatching { f0Session?.close() }; f0Session = null
        runCatching { netG?.close() }; netG = null
        voice = null
        fingerprint = null
    }

    /**
     * Builds an ORT session. tryNnapi → attempt the NNAPI execution provider (offloads supported ops
     * to the Adreno GPU / Hexagon NPU); falls back to multi-threaded CPU if NNAPI is unavailable or the
     * graph can't be partitioned. The BENCH log reveals whether it actually helped (it can also regress
     * if too many ops fall back across the NNAPI/CPU boundary).
     */
    private fun makeSession(path: String, tryNnapi: Boolean): OrtSession {
        val name = path.substringAfterLast('/')
        if (tryNnapi) {
            try {
                val o = OrtSession.SessionOptions()
                o.addNnapi()
                val s = env.createSession(path, o)
                Timber.tag(TAG).i("session %s → NNAPI (GPU/NPU)", name)
                return s
            } catch (e: Throwable) {
                Timber.tag(TAG).w("NNAPI unavailable for %s (%s) → CPU", name, e.message)
            }
        }
        val o = OrtSession.SessionOptions()
        o.setIntraOpNumThreads(Runtime.getRuntime().availableProcessors())
        return env.createSession(path, o)
    }

    // ───────────────────────── ORT calls ─────────────────────────

    /** ContentVec: 16k mono → features [T, C]. Adapts to [1,n] or [1,1,n] input rank (V1). */
    private fun runContentVec(s: OrtSession, audio16k: FloatArray, ch: Int): Array<FloatArray> {
        val inName = s.inputNames.first()
        val shape = if (inputRank(s, inName) == 3) {
            longArrayOf(1, 1, audio16k.size.toLong())
        } else {
            longArrayOf(1, audio16k.size.toLong())
        }
        return OnnxTensor.createTensor(env, directFloat(audio16k), shape).use { input ->
            s.run(mapOf(inName to input)).use { r ->
                read3d(r.get(0) as OnnxTensor, ch)
            }
        }
    }

    /** RMVPE/pitch: 16k mono → f0 contour in Hz [T]. Feeds an optional threshold scalar (V2). */
    private fun runF0(s: OrtSession, audio16k: FloatArray): FloatArray {
        val ins = s.inputNames.toList()
        val shape = if (inputRank(s, ins[0]) == 3) {
            longArrayOf(1, 1, audio16k.size.toLong())   // [batch, channel, samples]
        } else {
            longArrayOf(1, audio16k.size.toLong())
        }
        val feeds = LinkedHashMap<String, OnnxTensor>()
        feeds[ins[0]] = OnnxTensor.createTensor(env, directFloat(audio16k), shape)
        if (ins.size > 1) {
            feeds[ins[1]] = OnnxTensor.createTensor(env, directFloat(floatArrayOf(F0_THRESHOLD)), longArrayOf(1))
        }
        try {
            return s.run(feeds).use { r -> read1d(r.get(0) as OnnxTensor) }
        } finally {
            feeds.values.forEach { runCatching { it.close() } }
        }
    }

    private fun runNetG(
        s: OrtSession,
        feats: Array<FloatArray>,
        pitchf: FloatArray,
        pitch: LongArray,
        ch: Int,
    ): FloatArray {
        val t = feats.size
        val phone = FloatArray(t * ch)
        var k = 0
        for (row in feats) {
            System.arraycopy(row, 0, phone, k, ch)
            k += ch
        }
        val rnd = FloatArray(192 * t)
        val rng = Random()
        for (i in rnd.indices) rnd[i] = rng.nextGaussian().toFloat()

        val feeds = LinkedHashMap<String, OnnxTensor>()
        feeds["phone"] = OnnxTensor.createTensor(env, directFloat(phone), longArrayOf(1, t.toLong(), ch.toLong()))
        feeds["phone_lengths"] = OnnxTensor.createTensor(env, directLong(longArrayOf(t.toLong())), longArrayOf(1))
        feeds["pitch"] = OnnxTensor.createTensor(env, directLong(pitch), longArrayOf(1, t.toLong()))
        feeds["pitchf"] = OnnxTensor.createTensor(env, directFloat(pitchf), longArrayOf(1, t.toLong()))
        feeds["ds"] = OnnxTensor.createTensor(env, directLong(longArrayOf(0L)), longArrayOf(1))
        feeds["rnd"] = OnnxTensor.createTensor(env, directFloat(rnd), longArrayOf(1, 192, t.toLong()))
        try {
            return s.run(feeds).use { r -> read1d(r.get(0) as OnnxTensor) }
        } finally {
            feeds.values.forEach { runCatching { it.close() } }
        }
    }

    /**
     * net_g must be fed a FIXED sequence length: GuraTalkV2 (and standard RVC ONNX exports) bake the
     * relative-attention pad from the export dummy T=200, so the encoder only reshapes correctly at
     * exactly [NETG_BLOCK] frames. We run net_g in 200-frame blocks (last block zero-padded, its output
     * trimmed) and concatenate. Independent blocks → minor seams at boundaries; fine for the spike.
     */
    private fun runNetGChunked(
        s: OrtSession,
        feats: Array<FloatArray>,
        pitchf: FloatArray,
        pitch: LongArray,
        ch: Int,
    ): FloatArray {
        val block = NETG_BLOCK
        val t = feats.size
        if (t <= block) {
            // single (possibly short) block, padded to 200 then trimmed
            val fChunk = Array(block) { i -> if (i < t) feats[i] else FloatArray(ch) }
            val pfChunk = FloatArray(block) { i -> if (i < t) pitchf[i] else 0f }
            val pChunk = LongArray(block) { i -> if (i < t) pitch[i] else 1L }
            val outChunk = runNetG(s, fChunk, pfChunk, pChunk, ch)
            val keep = if (t == block) outChunk.size else (outChunk.size.toLong() * t / block).toInt()
            return if (keep < outChunk.size) outChunk.copyOf(keep) else outChunk
        }
        // Overlapping blocks + linear crossfade in the overlap region → no seam clicks.
        val overlap = NETG_OVERLAP
        val stride = block - overlap
        var out = FloatArray(0)
        var cursor = 0
        var sPerFrame = 0
        var start = 0
        var firstBlock = true
        while (start < t) {
            val n = minOf(block, t - start)
            val fChunk = Array(block) { i -> if (start + i < t) feats[start + i] else FloatArray(ch) }
            val pfChunk = FloatArray(block) { i -> if (start + i < t) pitchf[start + i] else 0f }
            val pChunk = LongArray(block) { i -> if (start + i < t) pitch[start + i] else 1L }
            val chunk = runNetG(s, fChunk, pfChunk, pChunk, ch)
            if (firstBlock) {
                sPerFrame = (chunk.size / block).coerceAtLeast(1)
                out = FloatArray((t + 2 * block) * sPerFrame)   // generous; trimmed at the end
            }
            val realSamples = if (n == block) chunk.size else (sPerFrame * n).coerceAtMost(chunk.size)
            if (firstBlock) {
                System.arraycopy(chunk, 0, out, 0, realSamples)
                cursor = realSamples
                firstBlock = false
            } else {
                val cf = minOf(overlap * sPerFrame, realSamples, cursor)
                for (i in 0 until cf) {
                    val w = (i + 1f) / (cf + 1f)
                    val idx = cursor - cf + i
                    out[idx] = out[idx] * (1f - w) + chunk[i] * w
                }
                val rest = realSamples - cf
                if (rest > 0) {
                    System.arraycopy(chunk, cf, out, cursor, rest)
                    cursor += rest
                }
            }
            if (n < block) break
            start += stride
        }
        return out.copyOf(cursor)
    }

    // ───────────────────────── tensor reading ─────────────────────────

    private fun inputRank(s: OrtSession, name: String): Int =
        (s.inputInfo[name]?.info as? TensorInfo)?.shape?.size ?: 2

    /** Reads a [1, T, C] float tensor → [T, C], padding/truncating the channel dim to [ch]. */
    private fun read3d(t: OnnxTensor, ch: Int): Array<FloatArray> {
        @Suppress("UNCHECKED_CAST")
        val batch = t.value as Array<Array<FloatArray>>
        val rows = batch[0]
        return Array(rows.size) { i ->
            val src = rows[i]
            if (src.size == ch) src else FloatArray(ch) { j -> if (j < src.size) src[j] else 0f }
        }
    }

    /** Reads a float tensor shaped [..,N] (rank 1/2/3, leading dims = 1, e.g. net_g audio [1,1,N]) → FloatArray[N]. */
    private fun read1d(t: OnnxTensor): FloatArray {
        var v: Any? = t.value
        while (v is Array<*>) v = v.firstOrNull()   // drill [1][1][N] → [N]
        return v as? FloatArray
            ?: throw IllegalStateException("Unexpected output type ${t.value?.javaClass}")
    }

    // ───────────────────────── DSP helpers ─────────────────────────

    private fun toFloat(pcm: ShortArray) = FloatArray(pcm.size) { pcm[it] / 32768f }

    private fun toPcm16(f: FloatArray) = ShortArray(f.size) {
        (f[it].coerceIn(-1f, 1f) * 32767f).roundToInt().toShort()
    }

    /** Soft anti-clip: scale down only if the peak exceeds 0.99 (leaves quieter audio untouched). */
    private fun normalizePeak(x: FloatArray): FloatArray {
        var peak = 0f
        for (v in x) {
            val a = if (v < 0f) -v else v
            if (a > peak) peak = a
        }
        if (peak <= 0.99f) return x
        val g = 0.99f / peak
        return FloatArray(x.size) { x[it] * g }
    }

    /** Linear resampler. */
    private fun resample(input: FloatArray, inRate: Int, outRate: Int): FloatArray {
        if (inRate == outRate || input.size < 2) return input
        val ratio = outRate.toDouble() / inRate
        val outLen = (input.size * ratio).toInt().coerceAtLeast(1)
        val out = FloatArray(outLen)
        val last = input.size - 1
        for (i in 0 until outLen) {
            val src = i / ratio
            val i0 = src.toInt()
            val frac = (src - i0).toFloat()
            val a = input[i0.coerceIn(0, last)]
            val b = input[(i0 + 1).coerceIn(0, last)]
            out[i] = a + (b - a) * frac
        }
        return out
    }

    /** Doubles the frame count with linear interpolation (RVC interpolates feats ×2 → ~100 Hz). */
    private fun upsample2x(feats: Array<FloatArray>): Array<FloatArray> {
        if (feats.isEmpty()) return feats
        val t = feats.size
        val c = feats[0].size
        return Array(t * 2) { idx ->
            val i = idx / 2
            if (idx % 2 == 0) {
                feats[i]
            } else {
                val cur = feats[i]
                val nxt = feats[min(i + 1, t - 1)]
                FloatArray(c) { j -> (cur[j] + nxt[j]) * 0.5f }
            }
        }
    }

    /** Resizes an f0 contour to [target] frames (linear). Length-based, rate-agnostic. */
    private fun resampleTo(f0: FloatArray, target: Int): FloatArray {
        if (f0.isEmpty()) return FloatArray(target)
        if (f0.size == target) return f0
        val out = FloatArray(target)
        val ratio = f0.size.toDouble() / target
        val last = f0.size - 1
        for (i in 0 until target) {
            val src = i * ratio
            val i0 = src.toInt()
            val frac = (src - i0).toFloat()
            val a = f0[i0.coerceIn(0, last)]
            val b = f0[(i0 + 1).coerceIn(0, last)]
            out[i] = a + (b - a) * frac
        }
        return out
    }

    /**
     * Pure-DSP pitch tracker (YIN) → f0 in Hz per frame, length [frames] (0 = unvoiced).
     *
     * Why not rmvpe.onnx here: that model's input is a 128-bin MEL spectrogram ([1,128,T]) and its
     * output is a salience map needing an argmax→cents→Hz decode — i.e. an exact-match mel front-end
     * AND a decoder. For the Phase-A spike we estimate f0 directly (the same call the known on-device
     * RVC/ONNX port made with DIO). Good enough to drive net_g; real RMVPE can replace this later.
     */
    private fun estimateF0(audio: FloatArray, frames: Int): FloatArray {
        val out = FloatArray(frames)
        if (audio.size < 256 || frames <= 0) return out
        val tauMin = (FEATURE_RATE / F0_MAX).toInt().coerceAtLeast(2)
        val tauMax = (FEATURE_RATE / F0_MIN).toInt().coerceAtMost(audio.size / 2)
        val window = tauMax                       // YIN difference window per frame
        val hop = audio.size.toFloat() / frames
        val cmnd = FloatArray(tauMax + 1)
        val threshold = 0.15f
        for (fi in 0 until frames) {
            val start = (fi * hop).toInt()
            var running = 0f
            cmnd[0] = 1f
            for (tau in 1..tauMax) {
                var sum = 0f
                var n = 0
                while (n < window) {
                    val i0 = start + n
                    val i1 = i0 + tau
                    val a = if (i0 < audio.size) audio[i0] else 0f
                    val b = if (i1 < audio.size) audio[i1] else 0f
                    val d = a - b
                    sum += d * d
                    n++
                }
                running += sum
                cmnd[tau] = if (running > 0f) sum * tau / running else 1f
            }
            var tau = tauMin
            var tauEst = -1
            while (tau <= tauMax) {
                if (cmnd[tau] < threshold) {
                    while (tau + 1 <= tauMax && cmnd[tau + 1] < cmnd[tau]) tau++
                    tauEst = tau
                    break
                }
                tau++
            }
            out[fi] = if (tauEst > 0) {
                // parabolic interpolation around the dip → sub-sample tau precision (smoother pitch)
                val refined = if (tauEst in (tauMin + 1) until tauMax) {
                    val a = cmnd[tauEst - 1]
                    val b = cmnd[tauEst]
                    val c = cmnd[tauEst + 1]
                    val denom = a + c - 2f * b
                    tauEst + if (denom != 0f) (0.5f * (a - c) / denom).coerceIn(-1f, 1f) else 0f
                } else {
                    tauEst.toFloat()
                }
                FEATURE_RATE / refined
            } else {
                0f
            }
        }
        return medianSmooth(out, 3)
    }

    /** Median filter — knocks out single-frame octave/voicing spikes for a steadier f0 contour. */
    private fun medianSmooth(f0: FloatArray, win: Int): FloatArray {
        if (f0.size < win || win < 3) return f0
        val half = win / 2
        val out = FloatArray(f0.size)
        val w = FloatArray(win)
        for (i in f0.indices) {
            for (j in 0 until win) w[j] = f0[(i + j - half).coerceIn(0, f0.size - 1)]
            w.sort()
            out[i] = w[half]
        }
        return out
    }

    /** RVC f0 → coarse pitch bin (1..255) via the mel scale; unvoiced (f0≤0) → 1. */
    private fun coarseF0(f0: Float): Long {
        if (f0 <= 0f) return 1L
        val mel = 1127.0 * ln(1.0 + f0 / 700.0)
        val scaled = (mel - F0_MEL_MIN) * 254.0 / (F0_MEL_MAX - F0_MEL_MIN) + 1.0
        return scaled.roundToInt().coerceIn(1, 255).toLong()
    }

    private fun directFloat(a: FloatArray): FloatBuffer {
        val fb = ByteBuffer.allocateDirect(a.size * 4).order(ByteOrder.nativeOrder()).asFloatBuffer()
        fb.put(a); fb.rewind()
        return fb
    }

    private fun directLong(a: LongArray): LongBuffer {
        val lb = ByteBuffer.allocateDirect(a.size * 8).order(ByteOrder.nativeOrder()).asLongBuffer()
        lb.put(a); lb.rewind()
        return lb
    }

    private companion object {
        const val TAG = "avcrt-rvc"
        const val FEATURE_RATE = 16000   // ContentVec + RMVPE input rate
        const val F0_THRESHOLD = 0.3f
        const val F0_MIN = 50.0
        const val F0_MAX = 1100.0
        val F0_MEL_MIN = 1127.0 * ln(1.0 + F0_MIN / 700.0)
        val F0_MEL_MAX = 1127.0 * ln(1.0 + F0_MAX / 700.0)
        val NET_G_INPUTS = setOf("phone", "phone_lengths", "pitch", "pitchf", "ds", "rnd")
        const val NETG_BLOCK = 200   // RVC ONNX export dummy T — net_g reshapes only work at this length
        const val NETG_OVERLAP = 16  // frames of block overlap, crossfaded to hide seams
        // Streaming (realtime) window: BLOCK = output/latency granularity, CTX = lookback context.
        const val STREAM_HOP = 160          // 16k samples per 100 Hz frame
        const val STREAM_BLOCK_FRAMES = 80  // 0.8 s output block (latency floor)
        const val STREAM_CTX_FRAMES = 32    // 0.32 s context for net_g/pitch continuity
        const val STREAM_XFADE_FRAMES = 8   // seam crossfade
    }
}
