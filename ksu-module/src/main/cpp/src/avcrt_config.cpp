#include "avcrt_config.h"

#include <fstream>
#include <sstream>
#include <string>

#include <sys/system_properties.h>
#include <nlohmann/json.hpp>

#include "avcrt_log.h"
#include "avcrt_paths.h"

namespace avcrt {

namespace {
int64_t read_property_revision() {
    char buf[PROP_VALUE_MAX] = {0};
    int len = __system_property_get(paths::kReloadProperty, buf);
    if (len <= 0) return 0;
    try {
        return std::stoll(buf);
    } catch (...) {
        return 0;
    }
}

std::string slurp(const char* path) {
    std::ifstream in(path);
    if (!in.is_open()) return {};
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}
}  // namespace

ConfigStore::ConfigStore() {
    reload();
}

ConfigStore::~ConfigStore() = default;

DspConfig ConfigStore::snapshot() {
    int64_t propRev = read_property_revision();
    if (propRev == last_prop_revision_) {
        std::lock_guard<std::mutex> lock(mu_);
        return current_;
    }
    last_prop_revision_ = propRev;
    reload();
    std::lock_guard<std::mutex> lock(mu_);
    return current_;
}

void ConfigStore::reload() {
    DspConfig parsed = parse_file();
    std::lock_guard<std::mutex> lock(mu_);
    current_ = parsed;
    log::i("config reloaded revision=%lld enabled=%d pitch=%.2f formant=%.2f",
           static_cast<long long>(parsed.revision),
           parsed.enabled, parsed.pitch_semitones, parsed.formant_semitones);
}

DspConfig ConfigStore::parse_file() {
    DspConfig cfg;

    std::string text = slurp(paths::kActivePreset);
    if (text.empty()) {
        log::w("active preset file empty or missing: %s", paths::kActivePreset);
        return cfg;
    }
    try {
        auto json = nlohmann::json::parse(text);
        cfg.enabled  = json.value("enabled", false);
        cfg.revision = json.value("revision", 0LL);

        if (json.contains("dsp") && !json["dsp"].is_null()) {
            const auto& d = json["dsp"];
            cfg.bypass_voice     = d.value("bypassVoice", false);
            cfg.pitch_semitones   = d.value("pitchSemitones",   0.0);
            cfg.formant_semitones = d.value("formantSemitones", 0.0);
            cfg.time_ratio        = d.value("timeRatio",        1.0);
            cfg.gain_db           = d.value("gainDb",           0.0);
            cfg.crispness         = d.value("crispness",        5);
            cfg.preserve_formants = d.value("preserveFormants", true);

            // ---- Phase 6 effect chain (dsp.fx.*). Missing object → all effects stay disabled. ----
            if (d.contains("fx") && d["fx"].is_object()) {
                const auto& fx = d["fx"];
                auto obj = [&](const char* k) { return fx.contains(k) && fx[k].is_object(); };
                if (obj("gate")) { const auto& g = fx["gate"];
                    cfg.gate.enabled      = g.value("enabled", false);
                    cfg.gate.threshold_db = g.value("thresholdDb", -45.0);
                    cfg.gate.attack_ms    = g.value("attackMs", 5.0);
                    cfg.gate.release_ms   = g.value("releaseMs", 80.0); }
                if (obj("eq")) { const auto& e = fx["eq"];
                    cfg.eq.enabled = e.value("enabled", false);
                    cfg.eq.low_db  = e.value("lowDb", 0.0);
                    cfg.eq.mid_db  = e.value("midDb", 0.0);
                    cfg.eq.high_db = e.value("highDb", 0.0); }
                if (obj("telephone")) { cfg.telephone.enabled = fx["telephone"].value("enabled", false); }
                if (obj("ringmod")) { const auto& r = fx["ringmod"];
                    cfg.ringmod.enabled    = r.value("enabled", false);
                    cfg.ringmod.carrier_hz = r.value("carrierHz", 120.0);
                    cfg.ringmod.mix        = r.value("mix", 1.0); }
                if (obj("tremolo")) { const auto& t = fx["tremolo"];
                    cfg.tremolo.enabled = t.value("enabled", false);
                    cfg.tremolo.rate_hz = t.value("rateHz", 5.0);
                    cfg.tremolo.depth   = t.value("depth", 0.6);
                    cfg.tremolo.shape   = t.value("shape", 0); }
                if (obj("distortion")) { const auto& s = fx["distortion"];
                    cfg.distortion.enabled = s.value("enabled", false);
                    cfg.distortion.drive   = s.value("drive", 4.0);
                    cfg.distortion.mix     = s.value("mix", 1.0); }
                if (obj("echo")) { const auto& h = fx["echo"];
                    cfg.echo.enabled  = h.value("enabled", false);
                    cfg.echo.time_ms  = h.value("timeMs", 180.0);
                    cfg.echo.feedback = h.value("feedback", 0.35);
                    cfg.echo.mix      = h.value("mix", 0.30); }
                if (obj("reverb")) { const auto& v = fx["reverb"];
                    cfg.reverb.enabled   = v.value("enabled", false);
                    cfg.reverb.room_size = v.value("roomSize", 0.5);
                    cfg.reverb.damping   = v.value("damping", 0.5);
                    cfg.reverb.wet       = v.value("wet", 0.3);
                    cfg.reverb.width     = v.value("width", 1.0); }
                // ---- Tier-2 ----
                if (obj("filter")) { const auto& f = fx["filter"];
                    cfg.filter.enabled = f.value("enabled", false);
                    cfg.filter.hp_hz   = f.value("hpHz", 0.0);
                    cfg.filter.lp_hz   = f.value("lpHz", 0.0);
                    cfg.filter.q       = f.value("q", 0.707); }
                if (obj("vibrato")) { const auto& v = fx["vibrato"];
                    cfg.vibrato.enabled = v.value("enabled", false);
                    cfg.vibrato.rate_hz = v.value("rateHz", 5.0);
                    cfg.vibrato.depth   = v.value("depth", 0.5); }
                if (obj("chorus")) { const auto& c = fx["chorus"];
                    cfg.chorus.enabled = c.value("enabled", false);
                    cfg.chorus.rate_hz = c.value("rateHz", 1.5);
                    cfg.chorus.depth   = c.value("depth", 0.5);
                    cfg.chorus.mix     = c.value("mix", 0.5); }
                if (obj("flanger")) { const auto& f = fx["flanger"];
                    cfg.flanger.enabled  = f.value("enabled", false);
                    cfg.flanger.rate_hz  = f.value("rateHz", 0.5);
                    cfg.flanger.depth    = f.value("depth", 0.5);
                    cfg.flanger.feedback = f.value("feedback", 0.5);
                    cfg.flanger.mix      = f.value("mix", 0.5); }
                if (obj("phaser")) { const auto& p = fx["phaser"];
                    cfg.phaser.enabled  = p.value("enabled", false);
                    cfg.phaser.rate_hz  = p.value("rateHz", 0.5);
                    cfg.phaser.depth    = p.value("depth", 0.7);
                    cfg.phaser.feedback = p.value("feedback", 0.4);
                    cfg.phaser.mix      = p.value("mix", 0.5); }
                if (obj("autowah")) { const auto& w = fx["autowah"];
                    cfg.autowah.enabled     = w.value("enabled", false);
                    cfg.autowah.sensitivity = w.value("sensitivity", 0.5);
                    cfg.autowah.base_hz     = w.value("baseHz", 300.0);
                    cfg.autowah.range_hz    = w.value("rangeHz", 2000.0);
                    cfg.autowah.mix         = w.value("mix", 1.0); }
                if (obj("bitcrush")) { const auto& b = fx["bitcrush"];
                    cfg.bitcrush.enabled    = b.value("enabled", false);
                    cfg.bitcrush.bits       = b.value("bits", 8.0);
                    cfg.bitcrush.downsample = b.value("downsample", 4.0);
                    cfg.bitcrush.mix        = b.value("mix", 1.0); }
                if (obj("compressor")) { const auto& m = fx["compressor"];
                    cfg.compressor.enabled     = m.value("enabled", false);
                    cfg.compressor.threshold_db = m.value("thresholdDb", -18.0);
                    cfg.compressor.ratio       = m.value("ratio", 3.0);
                    cfg.compressor.attack_ms   = m.value("attackMs", 10.0);
                    cfg.compressor.release_ms  = m.value("releaseMs", 120.0);
                    cfg.compressor.makeup_db   = m.value("makeupDb", 0.0); }
                // ---- Tier-3 ----
                if (obj("formant")) { const auto& f = fx["formant"];
                    cfg.formant.enabled = f.value("enabled", false);
                    cfg.formant.shift   = f.value("shift", 1.0); }
                if (obj("vocoder")) { const auto& v = fx["vocoder"];
                    cfg.vocoder.enabled      = v.value("enabled", false);
                    cfg.vocoder.carrier_hz   = v.value("carrierHz", 110.0);
                    cfg.vocoder.carrier_type = v.value("carrierType", 0);
                    cfg.vocoder.mix          = v.value("mix", 1.0); }
                if (obj("whisper")) { const auto& w = fx["whisper"];
                    cfg.whisper.enabled = w.value("enabled", false);
                    cfg.whisper.mix     = w.value("mix", 1.0); }
                // ---- Phase 7 experimental ----
                if (obj("spectral")) { const auto& s = fx["spectral"];
                    cfg.spectral.robot   = s.value("robot", false);
                    cfg.spectral.whisper = s.value("whisper", false);
                    cfg.spectral.freeze  = s.value("freeze", false);
                    cfg.spectral.blur    = s.value("blur", false);
                    cfg.spectral.blur_amt = s.value("blurAmt", 0.5);
                    cfg.spectral.mix     = s.value("mix", 1.0); }
                if (obj("autotune")) { const auto& a = fx["autotune"];
                    cfg.autotune.enabled  = a.value("enabled", false);
                    cfg.autotune.strength = a.value("strength", 1.0); }
                if (obj("harmonizer")) { const auto& h = fx["harmonizer"];
                    cfg.harmonizer.enabled   = h.value("enabled", false);
                    cfg.harmonizer.interval1 = h.value("interval1", 4);
                    cfg.harmonizer.interval2 = h.value("interval2", 7);
                    cfg.harmonizer.mix       = h.value("mix", 0.5); }
            }
        }
    } catch (const std::exception& ex) {
        log::e("failed to parse %s: %s", paths::kActivePreset, ex.what());
    }
    return cfg;
}

}  // namespace avcrt
