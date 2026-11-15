package dev.avcrt.domain.converter

import dev.avcrt.core.common.result.AvcrtResult

/**
 * AI (neural) voice conversion — Phase A: an OFFLINE "impersonate a character" converter.
 *
 * This is the any-to-one RVC path: the user picks a target voice (a downloaded community model)
 * and a recorded/imported clip is converted to sound like that character. It is APP-ONLY (no root,
 * no daemon, no Zygisk) — it runs entirely in our process via ONNX Runtime, so it cannot affect the
 * existing GSM/Zygisk voice-change paths.
 *
 * The realtime call/app paths (Phase B/C) will reuse the same engine behind this interface.
 */
interface VoiceConverter {

    /**
     * Loads (or re-uses) the shared models + the chosen target [voice]. Cheap to call repeatedly
     * with the same arguments (fingerprinted — only reloads when the model set changes). Heavy
     * (loads ~100 MB of ONNX) → call off the main thread; runs on IO internally.
     */
    suspend fun prepare(shared: SharedModels, voice: VoiceModel): AvcrtResult<Unit>

    /**
     * Converts [inputPcm] (mono PCM16 at [inputSampleRate]) to the prepared target voice and returns
     * mono PCM16 at [outputSampleRate]. Requires a successful [prepare] first.
     */
    suspend fun convert(
        inputPcm: ShortArray,
        inputSampleRate: Int,
        outputSampleRate: Int,
        params: ConversionParams = ConversionParams(),
    ): AvcrtResult<ShortArray>

    /** Resets streaming state — call before starting a new live session (models come from [prepare]). */
    fun streamReset()

    /**
     * Realtime path: push input PCM16 (mono at [inRate]); returns converted PCM16 (mono at [outRate])
     * for whatever blocks became ready (may be empty until enough is buffered). Requires [prepare]
     * first, and a DYNAMIC-T model (e.g. our SpongeBob export; a baked-@200 model won't stream).
     */
    fun streamFeed(
        inPcm16: ShortArray,
        inRate: Int,
        outRate: Int,
        params: ConversionParams = ConversionParams(),
    ): ShortArray

    /** Frees the native ORT sessions. Safe to call when idle. */
    fun release()
}

/**
 * The two SHARED ONNX models — identical for every voice, downloaded once:
 *  - [contentVecPath]  ContentVec "vec-768-layer-12.onnx" (speaker-invariant content features)
 *  - [f0Path]          pitch extractor, e.g. "rmvpe.onnx"
 */
data class SharedModels(
    val contentVecPath: String,
    val f0Path: String,
)

/**
 * One target "character" = a per-voice RVC generator exported to ONNX (net_g).
 *
 * @param sampleRate the model's OUTPUT rate (32000 / 40000 / 48000 — it's a property of the model,
 *   usually in its name e.g. "...-40k"). We resample the result to the playback rate.
 * @param vecChannels content-feature width: 768 (RVC v2) or 256 (RVC v1).
 */
data class VoiceModel(
    val id: String,
    val name: String,
    val netGPath: String,
    val sampleRate: Int = 40000,
    val vecChannels: Int = 768,
)

/** Per-conversion tweaks. [transposeSemitones] shifts the pitch/key (e.g. +12 for a higher voice). */
data class ConversionParams(
    val transposeSemitones: Int = 0,
)
