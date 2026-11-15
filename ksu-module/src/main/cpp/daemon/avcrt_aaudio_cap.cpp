// avcrt_aaudio_cap — AHAL-managed capture probe.
//
// Unlike raw tinyalsa (which can only tap a DMIC the call already drives), AAudio
// goes through AudioFlinger -> the audio HAL, so the AHAL orchestrates a proper
// capture usecase + powers up a mic. This tool records N seconds from an AAudio
// INPUT stream to a WAV, so we can test whether an AHAL capture still hears the
// user while the cellular call's uplink mux is hijacked to DMIC0 (silent).
//
//   avcrt_aaudio_cap <out.wav> <seconds> <preset>
//     preset: vc | mic | unproc | camcorder | voicerec  (default vc)
//
// Build: see build_aaudio_cap.sh (NDK27, -laaudio).

#include <aaudio/AAudio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

namespace {

constexpr int32_t kRate     = 16000;
constexpr int32_t kChannels = 1;

aaudio_input_preset_t parse_preset(const char* s, const char** name) {
    if (!strcmp(s, "mic"))        { *name = "GENERIC(mic)";          return AAUDIO_INPUT_PRESET_GENERIC; }
    if (!strcmp(s, "unproc"))     { *name = "UNPROCESSED";           return AAUDIO_INPUT_PRESET_UNPROCESSED; }
    if (!strcmp(s, "camcorder"))  { *name = "CAMCORDER";             return AAUDIO_INPUT_PRESET_CAMCORDER; }
    if (!strcmp(s, "voicerec"))   { *name = "VOICE_RECOGNITION";     return AAUDIO_INPUT_PRESET_VOICE_RECOGNITION; }
    *name = "VOICE_COMMUNICATION";
    return AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION;
}

void write_wav(const char* path, const int16_t* s, size_t n, int32_t rate, int32_t ch) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[cap] cannot open %s\n", path); return; }
    uint32_t dataBytes = (uint32_t)(n * sizeof(int16_t));
    uint32_t byteRate  = rate * ch * 2;
    uint32_t riff = 36 + dataBytes;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t fmtlen = 16; uint16_t fmt = 1, chs = (uint16_t)ch, bits = 16;
    uint32_t sr = (uint32_t)rate; uint16_t blockAlign = (uint16_t)(ch * 2);
    fwrite(&fmtlen,4,1,f); fwrite(&fmt,2,1,f); fwrite(&chs,2,1,f);
    fwrite(&sr,4,1,f); fwrite(&byteRate,4,1,f); fwrite(&blockAlign,2,1,f); fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&dataBytes,4,1,f);
    fwrite(s, sizeof(int16_t), n, f);
    fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    const char* out     = (argc > 1) ? argv[1] : "/data/local/tmp/aaudio_cap.wav";
    int         seconds = (argc > 2) ? atoi(argv[2]) : 8;
    const char* presetName = "VOICE_COMMUNICATION";
    aaudio_input_preset_t preset = parse_preset((argc > 3) ? argv[3] : "vc", &presetName);

    AAudioStreamBuilder* b = nullptr;
    aaudio_result_t r = AAudio_createStreamBuilder(&b);
    if (r != AAUDIO_OK) { fprintf(stderr, "[cap] createStreamBuilder: %s\n", AAudio_convertResultToText(r)); return 1; }

    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, kChannels);
    AAudioStreamBuilder_setSampleRate(b, kRate);
    AAudioStreamBuilder_setInputPreset(b, preset);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);

    AAudioStream* s = nullptr;
    r = AAudioStreamBuilder_openStream(b, &s);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) { fprintf(stderr, "[cap] openStream: %s\n", AAudio_convertResultToText(r)); return 2; }

    int32_t devId = AAudioStream_getDeviceId(s);
    int32_t aRate = AAudioStream_getSampleRate(s);
    int32_t aCh   = AAudioStream_getChannelCount(s);
    fprintf(stderr, "[cap] preset=%s deviceId=%d rate=%d ch=%d -> recording %ds to %s\n",
            presetName, devId, aRate, aCh, seconds, out);

    r = AAudioStream_requestStart(s);
    if (r != AAUDIO_OK) { fprintf(stderr, "[cap] requestStart: %s\n", AAudio_convertResultToText(r)); AAudioStream_close(s); return 3; }

    const int32_t want = aRate * seconds * aCh;
    std::vector<int16_t> buf;
    buf.reserve(want);
    int16_t chunk[512];
    int64_t got = 0;
    long peak = 0; double sumsq = 0;
    while (got < want) {
        int32_t frames = AAudioStream_read(s, chunk, 512 / aCh, 200LL * 1000 * 1000);
        if (frames < 0) { fprintf(stderr, "[cap] read err: %s\n", AAudio_convertResultToText(frames)); break; }
        if (frames == 0) continue;
        int32_t samples = frames * aCh;
        for (int32_t i = 0; i < samples; ++i) {
            int16_t v = chunk[i];
            buf.push_back(v);
            long a = v < 0 ? -v : v; if (a > peak) peak = a;
            sumsq += (double)v * v;
        }
        got += samples;
    }
    AAudioStream_requestStop(s);
    AAudioStream_close(s);

    double rms = buf.empty() ? 0 : __builtin_sqrt(sumsq / buf.size());
    fprintf(stderr, "[cap] captured %zu samples, peak=%ld rms=%.0f\n", buf.size(), peak, rms);
    if (!buf.empty()) write_wav(out, buf.data(), buf.size(), aRate, aCh);
    fprintf(stderr, "[cap] done.\n");
    return 0;
}
