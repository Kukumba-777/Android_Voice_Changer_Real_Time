package dev.avcrt.feature.converter

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import dev.avcrt.core.common.dispatcher.DispatcherProvider
import dev.avcrt.core.common.result.AvcrtResult
import dev.avcrt.core.common.result.userText
import dev.avcrt.domain.converter.ConversionParams
import dev.avcrt.domain.converter.SharedModels
import dev.avcrt.domain.converter.VoiceConverter
import dev.avcrt.domain.converter.VoiceModel
import dev.avcrt.domain.media.MediaFile
import dev.avcrt.domain.media.MediaFolder
import dev.avcrt.domain.media.MediaRepository
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import timber.log.Timber
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import javax.inject.Inject

enum class ConverterPhase { IDLE, RECORDING, CONVERTING, PLAYING, LIVE }

data class VoiceOption(val id: String, val name: String, val path: String)

data class ConverterUiState(
    val sharedReady: Boolean = false,
    val voices: List<VoiceOption> = emptyList(),
    val selectedVoiceId: String? = null,
    val transpose: Int = 0,
    val modelSampleRate: Int = 40000,
    val phase: ConverterPhase = ConverterPhase.IDLE,
    val hasRecording: Boolean = false,
    val hasResult: Boolean = false,
    val message: String? = null,
)

@HiltViewModel
class ConverterViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val converter: VoiceConverter,
    private val dispatchers: DispatcherProvider,
    private val library: MediaRepository,
) : ViewModel() {

    private val audio = ConverterAudio(dispatchers)
    private val outputDir = File(context.getExternalFilesDir(null), "output").apply { mkdirs() }

    private var recorded: ShortArray? = null
    private var result: ShortArray? = null
    private var recordJob: Job? = null
    private var playJob: Job? = null
    private var liveJob: Job? = null
    @Volatile private var liveActive = false

    private var sharedModels: SharedModels? = null
    private val _state = MutableStateFlow(ConverterUiState())
    val state = _state.asStateFlow()

    init {
        viewModelScope.launch { library.observe(MediaFolder.MODELS).collect(::updateModels) }
    }

    /** Re-reads the models folder: detects the shared pair + lists voice (net_g) models. */
    fun scan() { viewModelScope.launch { library.refresh(MediaFolder.MODELS) } }

    private fun updateModels(files: List<MediaFile>) {
        val content = files.firstOrNull { it.id.equals(CONTENTVEC, ignoreCase = true) }
        val pitch = files.firstOrNull { it.id.equals(RMVPE, ignoreCase = true) }
        sharedModels = if (content != null && pitch != null) SharedModels(content.absolutePath, pitch.absolutePath) else null
        val voices = files
            .filter { it.id.lowercase() !in SHARED_NAMES }
            .sortedBy { it.id.lowercase() }
            .map { VoiceOption(id = it.id, name = it.displayName, path = it.absolutePath) }
        val selectedId = _state.value.selectedVoiceId?.takeIf { id -> voices.any { it.id == id } }
            ?: voices.firstOrNull()?.id
        _state.value = _state.value.copy(
            sharedReady = sharedModels != null,
            voices = voices,
            selectedVoiceId = selectedId,
            modelSampleRate = if (selectedId == _state.value.selectedVoiceId) _state.value.modelSampleRate
            else voices.find { it.id == selectedId }?.let { guessSampleRate(it.name) } ?: _state.value.modelSampleRate,
        )
    }

    fun selectVoice(id: String) {
        val sr = _state.value.voices.find { it.id == id }?.let { guessSampleRate(it.name) }
            ?: _state.value.modelSampleRate
        _state.value = _state.value.copy(selectedVoiceId = id, modelSampleRate = sr)
    }

    fun setSampleRate(sampleRate: Int) {
        _state.value = _state.value.copy(modelSampleRate = sampleRate)
    }

    fun setTranspose(value: Int) {
        _state.value = _state.value.copy(transpose = value.coerceIn(-12, 12))
    }

    fun record() {
        if (_state.value.phase != ConverterPhase.IDLE) return
        recordJob = viewModelScope.launch {
            _state.value = _state.value.copy(phase = ConverterPhase.RECORDING, hasResult = false)
            result = null
            runCatching { audio.record(MAX_RECORD_SECONDS) }
                .onSuccess { pcm ->
                    recorded = pcm
                    _state.value = _state.value.copy(
                        phase = ConverterPhase.IDLE,
                        hasRecording = pcm.isNotEmpty(),
                    )
                }
                .onFailure { e ->
                    Timber.tag(TAG).e(e, "record failed")
                    _state.value = _state.value.copy(
                        phase = ConverterPhase.IDLE,
                        message = context.getString(R.string.converter_record_failed),
                    )
                }
        }
    }

    fun stopRecord() = audio.requestStop()

    fun onPermissionDenied() {
        _state.value = _state.value.copy(message = context.getString(R.string.converter_permission_denied))
    }

    fun convert() {
        val s = _state.value
        val voice = s.voices.find { it.id == s.selectedVoiceId }
        val shared = sharedModels
        if (shared == null) {
            _state.value = s.copy(message = context.getString(R.string.converter_models_missing_short))
            return
        }
        if (voice == null) {
            _state.value = s.copy(message = context.getString(R.string.converter_need_voice))
            return
        }
        val input = recorded
        if (input == null || input.isEmpty()) {
            _state.value = s.copy(message = context.getString(R.string.converter_need_recording))
            return
        }
        viewModelScope.launch {
            _state.value = _state.value.copy(phase = ConverterPhase.CONVERTING)
            val model = VoiceModel(
                id = voice.id,
                name = voice.name,
                netGPath = voice.path,
                sampleRate = s.modelSampleRate,
                vecChannels = 768,
            )
            val outcome = when (val prep = converter.prepare(shared, model)) {
                is AvcrtResult.Failure -> prep
                is AvcrtResult.Success<*> -> converter.convert(
                    inputPcm = input,
                    inputSampleRate = ConverterAudio.SAMPLE_RATE,
                    outputSampleRate = ConverterAudio.SAMPLE_RATE,
                    params = ConversionParams(transposeSemitones = s.transpose),
                )
            }
            when (outcome) {
                is AvcrtResult.Success<*> -> {
                    result = outcome.value as? ShortArray
                    _state.value = _state.value.copy(
                        phase = ConverterPhase.IDLE,
                        hasResult = result?.isNotEmpty() == true,
                    )
                }
                is AvcrtResult.Failure -> _state.value = _state.value.copy(
                    phase = ConverterPhase.IDLE,
                    message = outcome.error.userText(context),
                )
            }
        }
    }

    fun playOriginal() = play(recorded)

    fun playResult() = play(result)

    private fun play(pcm: ShortArray?) {
        if (pcm == null || pcm.isEmpty() || _state.value.phase != ConverterPhase.IDLE) return
        playJob = viewModelScope.launch {
            _state.value = _state.value.copy(phase = ConverterPhase.PLAYING)
            runCatching { audio.play(pcm) }
            _state.value = _state.value.copy(phase = ConverterPhase.IDLE)
        }
    }

    fun stopPlayback() {
        audio.stop()
        playJob?.cancel()
        if (_state.value.phase == ConverterPhase.PLAYING) {
            _state.value = _state.value.copy(phase = ConverterPhase.IDLE)
        }
    }

    /** Realtime live loopback: mic → streaming convert → playback. Toggle to start/stop. */
    fun toggleLive() {
        if (_state.value.phase == ConverterPhase.LIVE) {
            liveActive = false
            liveJob?.cancel()
            audio.stop()
            return
        }
        val s = _state.value
        if (s.phase != ConverterPhase.IDLE) return
        val voice = s.voices.find { it.id == s.selectedVoiceId }
        val shared = sharedModels
        if (shared == null) {
            _state.value = s.copy(message = context.getString(R.string.converter_models_missing_short)); return
        }
        if (voice == null) {
            _state.value = s.copy(message = context.getString(R.string.converter_need_voice)); return
        }
        liveActive = true
        liveJob = viewModelScope.launch {
            _state.value = _state.value.copy(phase = ConverterPhase.LIVE, hasResult = false)
            val model = VoiceModel(voice.id, voice.name, voice.path, s.modelSampleRate, 768)
            val prep = converter.prepare(shared, model)
            if (prep is AvcrtResult.Failure) {
                liveActive = false
                _state.value = _state.value.copy(
                    phase = ConverterPhase.IDLE,
                    message = prep.error.userText(context),
                )
                return@launch
            }
            converter.streamReset()
            val transpose = s.transpose
            runCatching {
                audio.liveLoop(
                    convert = { pcm -> converter.streamFeed(pcm, 16000, 48000, ConversionParams(transpose)) },
                    isActive = { liveActive },
                )
            }.onFailure { e ->
                if (e !is CancellationException) {
                    Timber.tag(TAG).e(e, "live loop failed")
                    _state.value = _state.value.copy(message = context.getString(R.string.converter_live_failed))
                }
            }
            liveActive = false
            _state.value = _state.value.copy(phase = ConverterPhase.IDLE)
        }
    }

    fun saveResult() {
        val pcm = result ?: return
        viewModelScope.launch {
            val path = withContext(dispatchers.io) {
                val file = File(outputDir, "avcrt_${System.currentTimeMillis()}.wav")
                writeWav(file, pcm, ConverterAudio.SAMPLE_RATE)
                file.absolutePath
            }
            _state.value = _state.value.copy(
                message = context.getString(R.string.converter_saved, path),
            )
        }
    }

    fun consumeMessage() {
        _state.value = _state.value.copy(message = null)
    }

    override fun onCleared() {
        liveActive = false
        liveJob?.cancel()
        audio.stop()
        converter.release()
    }

    private fun guessSampleRate(name: String): Int {
        val n = name.lowercase()
        return when {
            "48k" in n -> 48000
            "40k" in n -> 40000
            "32k" in n -> 32000
            // RVC v2 models commonly use 40 kHz, but filenames are not reliable
            // metadata. Keep the conventional default explicit in the UI rather
            // than silently claiming it was detected from the model.
            else -> 40000
        }
    }

    /** Minimal 16-bit mono PCM → RIFF/WAVE writer. */
    private fun writeWav(file: File, pcm: ShortArray, sampleRate: Int) {
        val dataBytes = pcm.size * 2
        RandomAccessFile(file, "rw").use { raf ->
            raf.setLength(0)
            val header = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
            header.put("RIFF".toByteArray(Charsets.US_ASCII))
            header.putInt(36 + dataBytes)
            header.put("WAVE".toByteArray(Charsets.US_ASCII))
            header.put("fmt ".toByteArray(Charsets.US_ASCII))
            header.putInt(16)                 // PCM fmt chunk size
            header.putShort(1)                // PCM
            header.putShort(1)                // mono
            header.putInt(sampleRate)
            header.putInt(sampleRate * 2)     // byte rate (mono 16-bit)
            header.putShort(2)                // block align
            header.putShort(16)               // bits per sample
            header.put("data".toByteArray(Charsets.US_ASCII))
            header.putInt(dataBytes)
            raf.write(header.array())
            val body = ByteBuffer.allocate(dataBytes).order(ByteOrder.LITTLE_ENDIAN)
            for (s in pcm) body.putShort(s)
            raf.write(body.array())
        }
    }

    private companion object {
        const val TAG = "avcrt-converter"
        const val MAX_RECORD_SECONDS = 8
        const val CONTENTVEC = "contentvec.onnx"
        const val RMVPE = "rmvpe.onnx"
        val SHARED_NAMES = setOf(CONTENTVEC, RMVPE)
    }
}
