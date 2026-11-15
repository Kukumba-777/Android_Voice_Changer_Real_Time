#pragma once

// avcrt_effects — the per-stream effect chain (Phase 6, Tier-1), built on avcrt_dsp.h primitives.
//
// One EffectChain per Engine (per capture stream). The Engine calls:
//   prepare(fs, channels)              once (and on rate/channel change) — allocates all buffers
//   updateParams(const DspConfig&)     when the config revision changes — sets coeffs/targets
//   processPre(float* x, size_t frames)   on the INPUT, before pitch (gate, EQ, telephone)
//   processPost(float* x, size_t frames)  on the OUTPUT, after pitch (ring-mod, tremolo, distortion,
//                                          echo, reverb)
// All buffers float, interleaved, normalized ~[-1,1]. Per-channel state lives in vectors sized to
// `channels`; shared modulators (LFO, ring carrier) advance ONCE PER FRAME so channels stay phase-
// aligned. When an effect's `enabled` is false it is skipped entirely (≈0 CPU) → a no-effect preset
// is byte-identical to the pre-Phase-6 pitch/formant/gain path.

#include <cmath>
#include <cstddef>
#include <vector>

#include "avcrt_config.h"
#include "avcrt_dsp.h"

namespace avcrt {

class EffectChain {
public:
    void prepare(uint32_t fs, uint32_t channels) {
        fs_ = static_cast<float>(fs);
        ch_ = channels < 1 ? 1 : channels;

        gateEnv_.assign(ch_, {});
        gateGain_.assign(ch_, {});
        eqLow_.assign(ch_, {});
        eqMid_.assign(ch_, {});
        eqHigh_.assign(ch_, {});
        tel_.assign(ch_, {});
        echo_.assign(ch_, {});
        reverb_.assign(ch_, {});
        // Tier-2 per-channel state
        filtHp_.assign(ch_, {});
        filtLp_.assign(ch_, {});
        vib_.assign(ch_, {});
        chor_.assign(ch_, {});
        flan_.assign(ch_, {});
        phaser_.assign(ch_, {});
        wah_.assign(ch_, {});
        crush_.assign(ch_, {});
        compEnv_.assign(ch_, {});
        // Tier-3 channel-vocoder banks (one per channel per effect)
        formantBank_.assign(ch_, {});
        vocBank_.assign(ch_, {});
        whisBank_.assign(ch_, {});
        // Phase 7 experimental STFT (one per channel)
        stft_.assign(ch_, {});

        // fs-scaled modulation-delay constants (samples)
        vibCenterS_ = fs_ * 0.004f; vibRangeS_  = fs_ * 0.003f;
        chorBaseS_  = fs_ * 0.018f; chorRangeS_ = fs_ * 0.008f;
        flanBaseS_  = fs_ * 0.001f; flanRangeS_ = fs_ * 0.006f;

        const size_t maxEcho = static_cast<size_t>(fs_ * 1.05f);  // 1000 ms max + margin
        const size_t maxVib  = static_cast<size_t>(fs_ * 0.02f) + 2;
        const size_t maxChor = static_cast<size_t>(fs_ * 0.06f) + 2;
        const size_t maxFlan = static_cast<size_t>(fs_ * 0.02f) + 2;
        for (uint32_t c = 0; c < ch_; ++c) {
            gateEnv_[c].prepare(fs_);
            gateGain_[c].setCoeff(0.01f);
            gateGain_[c].snap(1.f);
            echo_[c].prepare(maxEcho);
            reverb_[c].prepare(fs_);
            vib_[c].prepare(maxVib);
            chor_[c].prepare(maxChor);
            flan_[c].prepare(maxFlan);
            wah_[c].env.prepare(fs_);
            wah_[c].env.setTimes(5.f, 80.f);
            compEnv_[c].prepare(fs_);
            formantBank_[c].prepare(fs_);
            vocBank_[c].prepare(fs_);
            whisBank_[c].prepare(fs_);
            // Smaller STFT window at low capture rates so the latency in MS stays bounded:
            // 1024 @48k = 21 ms, but @16k that's 64 ms — use 512 (@16k = 32 ms) below 32 kHz.
            stft_[c].prepare(fs_ >= 32000.f ? 1024 : 512, fs_);
        }
        lfo_.prepare(fs_);
        carrier_.prepare(fs_);
        lfoVib_.prepare(fs_);
        lfoChor_.prepare(fs_);
        lfoFlan_.prepare(fs_);
        lfoPhas_.prepare(fs_);
        vocOsc_.prepare(fs_);

        // smoothed "amount" controls (one-pole, ~10 ms) — anti-click on preset switch
        for (dsp::OnePole* p : {&sTremDepth_, &sRingMix_, &sDistMix_, &sEchoMix_, &sEchoFb_,
                               &sVerbWet_, &sVerbDry_, &sVibDepth_, &sChorDepth_, &sChorMix_,
                               &sFlanDepth_, &sFlanFb_, &sFlanMix_, &sPhasDepth_, &sPhasFb_,
                               &sPhasMix_, &sWahMix_, &sCrushMix_, &sCompMakeup_,
                               &sVocMix_, &sWhisMix_}) {
            p->setCoeff(0.02f); p->snap(0.f);
        }
        configured_ = false;
    }

    void updateParams(const DspConfig& c) {
        cfg_ = c;

        if (c.gate.enabled) {
            for (uint32_t i = 0; i < ch_; ++i)
                gateEnv_[i].setTimes(static_cast<float>(c.gate.attack_ms),
                                     static_cast<float>(c.gate.release_ms));
            gateThreshLin_ = dsp::dbToLin(static_cast<float>(c.gate.threshold_db));
        }
        if (c.eq.enabled) {
            for (uint32_t i = 0; i < ch_; ++i) {
                eqLow_[i].setLowShelf(fs_, 200.f,  static_cast<float>(c.eq.low_db));
                eqMid_[i].setPeaking(fs_, 1000.f, 1.0f, static_cast<float>(c.eq.mid_db));
                eqHigh_[i].setHighShelf(fs_, 4000.f, static_cast<float>(c.eq.high_db));
            }
        }
        if (c.telephone.enabled) {
            for (uint32_t i = 0; i < ch_; ++i) tel_[i].setBandpass(fs_, 1200.f, 0.7f);
        }
        carrier_.setFreq(static_cast<float>(c.ringmod.carrier_hz));
        lfo_.setRate(static_cast<float>(c.tremolo.rate_hz));
        for (uint32_t i = 0; i < ch_; ++i)
            reverb_[i].setParams(static_cast<float>(c.reverb.room_size),
                                 static_cast<float>(c.reverb.damping));
        echoDelaySamp_ = dsp::clampf(static_cast<float>(c.echo.time_ms) * 0.001f * fs_,
                                     1.f, static_cast<float>(fs_ * 1.0f));

        // ---- Tier-2 coeff / rate setup (scalar params; per-sample ones read cfg_ directly) ----
        if (c.filter.enabled) {
            for (uint32_t i = 0; i < ch_; ++i) {
                if (c.filter.hp_hz >= 20.0)
                    filtHp_[i].setHighpass(fs_, static_cast<float>(c.filter.hp_hz),
                                           static_cast<float>(c.filter.q));
                if (c.filter.lp_hz >= 20.0)
                    filtLp_[i].setLowpass(fs_, static_cast<float>(c.filter.lp_hz),
                                          static_cast<float>(c.filter.q));
            }
        }
        lfoVib_.setRate(static_cast<float>(c.vibrato.rate_hz));
        lfoChor_.setRate(static_cast<float>(c.chorus.rate_hz));
        lfoFlan_.setRate(static_cast<float>(c.flanger.rate_hz));
        lfoPhas_.setRate(static_cast<float>(c.phaser.rate_hz));
        if (c.compressor.enabled) {
            for (uint32_t i = 0; i < ch_; ++i)
                compEnv_[i].setTimes(static_cast<float>(c.compressor.attack_ms),
                                     static_cast<float>(c.compressor.release_ms));
        }
        {
            float bits = dsp::clampf(static_cast<float>(c.bitcrush.bits), 1.f, 16.f);
            crushLevels_ = std::pow(2.f, bits);
            crushStep_   = 2.f / crushLevels_;
            crushDs_     = c.bitcrush.downsample < 1.0 ? 1 : static_cast<int>(c.bitcrush.downsample);
        }
        // ---- Tier-3 ----
        if (c.formant.enabled) {
            for (uint32_t i = 0; i < ch_; ++i)
                formantBank_[i].setSynthShift(static_cast<float>(c.formant.shift));
        }
        vocOsc_.setFreq(static_cast<float>(c.vocoder.carrier_hz));
        {   // Phase 7 experimental: spectral + phase-vocoder params → each channel's STFT
            dsp::StftProcessor::Params sp;
            sp.robot = c.spectral.robot; sp.whisper = c.spectral.whisper;
            sp.freeze = c.spectral.freeze; sp.blur = c.spectral.blur;
            sp.blurAmt = static_cast<float>(c.spectral.blur_amt);
            sp.specMix = static_cast<float>(c.spectral.mix);
            sp.autotune = c.autotune.enabled; sp.atStrength = static_cast<float>(c.autotune.strength);
            sp.harmon = c.harmonizer.enabled; sp.hInt1 = c.harmonizer.interval1;
            sp.hInt2 = c.harmonizer.interval2; sp.hMix = static_cast<float>(c.harmonizer.mix);
            for (uint32_t i = 0; i < ch_; ++i) stft_[i].setParams(sp);
        }

        // smoother targets
        sTremDepth_.setTarget(c.tremolo.enabled    ? static_cast<float>(c.tremolo.depth)    : 0.f);
        sRingMix_.setTarget  (c.ringmod.enabled    ? static_cast<float>(c.ringmod.mix)      : 0.f);
        sDistMix_.setTarget  (c.distortion.enabled ? static_cast<float>(c.distortion.mix)   : 0.f);
        sEchoMix_.setTarget  (c.echo.enabled       ? static_cast<float>(c.echo.mix)         : 0.f);
        sEchoFb_.setTarget   (static_cast<float>(c.echo.feedback));
        sVerbWet_.setTarget  (c.reverb.enabled     ? static_cast<float>(c.reverb.wet)       : 0.f);
        sVerbDry_.setTarget  (c.reverb.enabled     ? 1.f : 1.f);  // dry kept at 1 (wet added on top)
        sVibDepth_.setTarget (c.vibrato.enabled    ? static_cast<float>(c.vibrato.depth)    : 0.f);
        sChorDepth_.setTarget(c.chorus.enabled     ? static_cast<float>(c.chorus.depth)     : 0.f);
        sChorMix_.setTarget  (c.chorus.enabled     ? static_cast<float>(c.chorus.mix)       : 0.f);
        sFlanDepth_.setTarget(c.flanger.enabled    ? static_cast<float>(c.flanger.depth)    : 0.f);
        sFlanFb_.setTarget   (static_cast<float>(c.flanger.feedback));
        sFlanMix_.setTarget  (c.flanger.enabled    ? static_cast<float>(c.flanger.mix)      : 0.f);
        sPhasDepth_.setTarget(c.phaser.enabled     ? static_cast<float>(c.phaser.depth)     : 0.f);
        sPhasFb_.setTarget   (static_cast<float>(c.phaser.feedback));
        sPhasMix_.setTarget  (c.phaser.enabled     ? static_cast<float>(c.phaser.mix)       : 0.f);
        sWahMix_.setTarget   (c.autowah.enabled    ? static_cast<float>(c.autowah.mix)      : 0.f);
        sCrushMix_.setTarget (c.bitcrush.enabled   ? static_cast<float>(c.bitcrush.mix)     : 0.f);
        sCompMakeup_.setTarget(static_cast<float>(c.compressor.makeup_db));
        sVocMix_.setTarget   (c.vocoder.enabled    ? static_cast<float>(c.vocoder.mix)      : 0.f);
        sWhisMix_.setTarget  (c.whisper.enabled    ? static_cast<float>(c.whisper.mix)      : 0.f);

        if (!configured_) {  // first config: snap smoothers to their targets (no ramp-up on buffer 1)
            sTremDepth_.snap(c.tremolo.enabled    ? static_cast<float>(c.tremolo.depth)  : 0.f);
            sRingMix_.snap  (c.ringmod.enabled    ? static_cast<float>(c.ringmod.mix)    : 0.f);
            sDistMix_.snap  (c.distortion.enabled ? static_cast<float>(c.distortion.mix) : 0.f);
            sEchoMix_.snap  (c.echo.enabled       ? static_cast<float>(c.echo.mix)       : 0.f);
            sEchoFb_.snap   (static_cast<float>(c.echo.feedback));
            sVerbWet_.snap  (c.reverb.enabled     ? static_cast<float>(c.reverb.wet)     : 0.f);
            sVerbDry_.snap  (1.f);
            sVibDepth_.snap (c.vibrato.enabled    ? static_cast<float>(c.vibrato.depth)  : 0.f);
            sChorDepth_.snap(c.chorus.enabled     ? static_cast<float>(c.chorus.depth)   : 0.f);
            sChorMix_.snap  (c.chorus.enabled     ? static_cast<float>(c.chorus.mix)     : 0.f);
            sFlanDepth_.snap(c.flanger.enabled    ? static_cast<float>(c.flanger.depth)  : 0.f);
            sFlanFb_.snap   (static_cast<float>(c.flanger.feedback));
            sFlanMix_.snap  (c.flanger.enabled    ? static_cast<float>(c.flanger.mix)    : 0.f);
            sPhasDepth_.snap(c.phaser.enabled     ? static_cast<float>(c.phaser.depth)   : 0.f);
            sPhasFb_.snap   (static_cast<float>(c.phaser.feedback));
            sPhasMix_.snap  (c.phaser.enabled     ? static_cast<float>(c.phaser.mix)     : 0.f);
            sWahMix_.snap   (c.autowah.enabled    ? static_cast<float>(c.autowah.mix)    : 0.f);
            sCrushMix_.snap (c.bitcrush.enabled   ? static_cast<float>(c.bitcrush.mix)   : 0.f);
            sCompMakeup_.snap(static_cast<float>(c.compressor.makeup_db));
            sVocMix_.snap   (c.vocoder.enabled    ? static_cast<float>(c.vocoder.mix)    : 0.f);
            sWhisMix_.snap  (c.whisper.enabled    ? static_cast<float>(c.whisper.mix)    : 0.f);
            configured_ = true;
        }
    }

    bool anyPre()  const { return cfg_.gate.enabled || cfg_.eq.enabled || cfg_.telephone.enabled
                                  || cfg_.filter.enabled; }
    bool anyPost() const { return cfg_.ringmod.enabled || cfg_.tremolo.enabled || cfg_.distortion.enabled
                                  || cfg_.echo.enabled || cfg_.reverb.enabled || cfg_.vibrato.enabled
                                  || cfg_.chorus.enabled || cfg_.flanger.enabled || cfg_.phaser.enabled
                                  || cfg_.autowah.enabled || cfg_.bitcrush.enabled || cfg_.compressor.enabled
                                  || cfg_.formant.enabled || cfg_.vocoder.enabled || cfg_.whisper.enabled
                                  || cfg_.spectral.robot || cfg_.spectral.whisper || cfg_.spectral.freeze
                                  || cfg_.spectral.blur || cfg_.autotune.enabled || cfg_.harmonizer.enabled; }

    // ---- pre-pitch: gate → EQ → telephone (per channel) ----
    void processPre(float* x, size_t frames) {
        if (!anyPre()) return;
        for (size_t f = 0; f < frames; ++f) {
            for (uint32_t c = 0; c < ch_; ++c) {
                float* s = &x[f * ch_ + c];
                float v = *s;
                if (cfg_.gate.enabled) {
                    float env = gateEnv_[c].process(v);
                    gateGain_[c].setTarget(env >= gateThreshLin_ ? 1.f : 0.f);
                    v *= gateGain_[c].next();
                }
                if (cfg_.filter.enabled) {  // sweepable hp/lp (a 0 Hz cutoff = that leg off)
                    if (cfg_.filter.hp_hz >= 20.0) v = filtHp_[c].process(v);
                    if (cfg_.filter.lp_hz >= 20.0) v = filtLp_[c].process(v);
                }
                if (cfg_.eq.enabled) {
                    v = eqLow_[c].process(v);
                    v = eqMid_[c].process(v);
                    v = eqHigh_[c].process(v);
                }
                if (cfg_.telephone.enabled) v = tel_[c].process(v);
                *s = v;
            }
        }
    }

    // ---- post-pitch: ring-mod → tremolo → vibrato → chorus → flanger → phaser → auto-wah →
    //      distortion → bitcrush → echo → reverb → compressor ----
    void processPost(float* x, size_t frames) {
        if (!anyPost()) return;
        const float drive = static_cast<float>(cfg_.distortion.drive);
        for (size_t f = 0; f < frames; ++f) {
            // shared modulators advance once per frame (channel-aligned)
            float carrier = cfg_.ringmod.enabled  ? carrier_.nextSine() : 0.f;
            float trem    = cfg_.tremolo.enabled  ? lfo_.nextUni(cfg_.tremolo.shape) : 0.f;
            float vibLfo  = cfg_.vibrato.enabled  ? lfoVib_.nextBiSine()  : 0.f;
            float chorLfo = cfg_.chorus.enabled   ? lfoChor_.nextBiSine() : 0.f;
            float flanLfo = cfg_.flanger.enabled  ? lfoFlan_.nextBiSine() : 0.f;
            float phasLfo = cfg_.phaser.enabled   ? lfoPhas_.nextBiSine() : 0.f;
            float ringMix = sRingMix_.next();
            float tremDep = sTremDepth_.next();
            float vibDep  = sVibDepth_.next();
            float chorDep = sChorDepth_.next();
            float chorMix = sChorMix_.next();
            float flanDep = sFlanDepth_.next();
            float flanFb  = sFlanFb_.next();
            float flanMix = sFlanMix_.next();
            float phasDep = sPhasDepth_.next();
            float phasFb  = sPhasFb_.next();
            float phasMix = sPhasMix_.next();
            float distMix = sDistMix_.next();
            float crushMix = sCrushMix_.next();
            float wahMix  = sWahMix_.next();
            float compMk  = sCompMakeup_.next();
            float echoMix = sEchoMix_.next();
            float echoFb  = sEchoFb_.next();
            float verbWet = sVerbWet_.next();
            float verbDry = sVerbDry_.next();
            float vocMix  = sVocMix_.next();
            float whisMix = sWhisMix_.next();
            // phaser all-pass coeff (modulated, shared across channels) — once per frame
            float phasCoeff = 0.f;
            if (cfg_.phaser.enabled) {
                float fc = kPhMinHz + (0.5f + 0.5f * phasLfo) * (kPhMaxHz - kPhMinHz) * phasDep;
                phasCoeff = dsp::allpassCoeff(fs_, fc);
            }
            // Tier-3 shared excitation (per frame, channel-aligned): vocoder carrier + whisper noise
            float vocCarrier = 0.f;
            if (cfg_.vocoder.enabled) {
                vocCarrier = (cfg_.vocoder.carrier_type == 2) ? rng_.next()
                                                              : vocOsc_.next(cfg_.vocoder.carrier_type);
            }
            float whisNoise = cfg_.whisper.enabled ? rng_.next() : 0.f;

            for (uint32_t c = 0; c < ch_; ++c) {
                float* s = &x[f * ch_ + c];
                float v = *s;
                if (cfg_.ringmod.enabled)  v = v * (1.f - ringMix) + (v * carrier) * ringMix;
                if (cfg_.tremolo.enabled)  v *= (1.f - tremDep + tremDep * trem);
                if (cfg_.vibrato.enabled) {  // 100% wet pitch-wobble via a modulated delay tap
                    vib_[c].write(v);
                    v = vib_[c].readFrac(vibCenterS_ + vibLfo * vibRangeS_ * vibDep);
                }
                if (cfg_.chorus.enabled) {  // two LFO-offset voices mixed with dry
                    chor_[c].write(v);
                    float d1 = chorBaseS_ + (0.5f + 0.5f * chorLfo) * chorRangeS_ * chorDep;
                    float d2 = chorBaseS_ * 1.6f + (0.5f - 0.5f * chorLfo) * chorRangeS_ * chorDep;
                    float wet = 0.5f * (chor_[c].readFrac(d1) + chor_[c].readFrac(d2));
                    v = v * (1.f - chorMix) + wet * chorMix;
                }
                if (cfg_.flanger.enabled) {  // short modulated delay + feedback
                    float d = flanBaseS_ + (0.5f + 0.5f * flanLfo) * flanRangeS_ * flanDep;
                    float delayed = flan_[c].readFrac(d);
                    flan_[c].write(v + delayed * flanFb);
                    v = v * (1.f - flanMix) + delayed * flanMix;
                }
                if (cfg_.phaser.enabled) {  // N all-pass stages + feedback
                    PhaserCh& p = phaser_[c];
                    float xin = v + p.fb * phasFb;
                    for (int st = 0; st < kPhaserStages; ++st) {
                        p.ap[st].setCoeff(phasCoeff);
                        xin = p.ap[st].process(xin);
                    }
                    p.fb = xin;
                    v = v * (1.f - phasMix) + xin * phasMix;
                }
                if (cfg_.autowah.enabled) {  // envelope-swept bandpass (coeff updated at control rate)
                    WahCh& w = wah_[c];
                    float env = w.env.process(v);
                    if (--w.cnt <= 0) {
                        float fc = static_cast<float>(cfg_.autowah.base_hz) +
                                   env * static_cast<float>(cfg_.autowah.sensitivity) *
                                   static_cast<float>(cfg_.autowah.range_hz);
                        w.bp.setBandpass(fs_, dsp::clampf(fc, 60.f, fs_ * 0.45f), 2.5f);
                        w.cnt = 8;
                    }
                    float wet = w.bp.process(v);
                    v = v * (1.f - wahMix) + wet * wahMix;
                }
                if (cfg_.distortion.enabled) v = v * (1.f - distMix) + dsp::softClip(v, drive) * distMix;
                if (cfg_.bitcrush.enabled) {  // sample-hold (rate reduce) + quantize, mixed with dry
                    CrushCh& cr = crush_[c];
                    if (--cr.cnt <= 0) {
                        cr.held = std::round(v / crushStep_) * crushStep_;
                        cr.cnt = crushDs_;
                    }
                    v = v * (1.f - crushMix) + cr.held * crushMix;
                }
                if (cfg_.echo.enabled) {
                    float echoed = echo_[c].readFrac(echoDelaySamp_);
                    echo_[c].write(v + echoed * echoFb);
                    v = v + echoed * echoMix;
                }
                if (cfg_.reverb.enabled) {
                    float wet = reverb_[c].process(v);
                    v = v * verbDry + wet * verbWet;
                }
                if (cfg_.compressor.enabled) {  // peak-detect downward compressor + makeup
                    float env_db = dsp::linToDb(compEnv_[c].process(v));
                    float over   = env_db - static_cast<float>(cfg_.compressor.threshold_db);
                    float ratio  = cfg_.compressor.ratio < 1.0 ? 1.f
                                                               : static_cast<float>(cfg_.compressor.ratio);
                    float gr     = over > 0.f ? over * (1.f - 1.f / ratio) : 0.f;  // dB reduction
                    v *= dsp::dbToLin(compMk - gr);
                }
                // ---- Tier-3 channel-vocoder family (formant shift / vocoder / whisper) ----
                if (cfg_.formant.enabled) {  // re-impose the shifted spectral envelope; full wet.
                    // Excitation = the raw input (broadband enough for voiced speech); the bank
                    // auto-gains to the input level. (A pre-emphasis "whitening" excitation was tried
                    // and removed — it killed the low-frequency voice energy → near silence.)
                    v = formantBank_[c].process(v, v);
                }
                if (cfg_.vocoder.enabled) {  // envelope-gated carrier (robot/synth)
                    float wet = vocBank_[c].process(v, vocCarrier);
                    v = v * (1.f - vocMix) + wet * vocMix;
                }
                if (cfg_.whisper.enabled) {  // envelope-gated noise (breath)
                    float wet = whisBank_[c].process(v, whisNoise);
                    v = v * (1.f - whisMix) + wet * whisMix;
                }
                // ---- Phase 7 experimental spectral (LAST; adds ~21 ms latency, bypassed when off) ----
                if (stft_[c].active()) v = stft_[c].tick(v);
                *s = v;
            }
        }
    }

private:
    float fs_ = 48000.f;
    uint32_t ch_ = 1;
    bool configured_ = false;
    DspConfig cfg_;

    float gateThreshLin_ = 0.f;
    float echoDelaySamp_ = 1.f;

    std::vector<dsp::EnvFollower> gateEnv_;
    std::vector<dsp::OnePole>     gateGain_;
    std::vector<dsp::Biquad>      eqLow_, eqMid_, eqHigh_, tel_;
    std::vector<dsp::DelayLine>   echo_;
    std::vector<dsp::Freeverb>    reverb_;
    dsp::Lfo    lfo_;
    dsp::Phasor carrier_;

    dsp::OnePole sTremDepth_, sRingMix_, sDistMix_, sEchoMix_, sEchoFb_, sVerbWet_, sVerbDry_;

    // ---- Tier-2 ----
    static constexpr int kPhaserStages = 4;
    static constexpr float kPhMinHz = 200.f, kPhMaxHz = 2000.f;
    struct PhaserCh { dsp::ModAllpass ap[kPhaserStages]; float fb = 0.f; };
    struct CrushCh  { float held = 0.f; int cnt = 0; };
    struct WahCh    { dsp::EnvFollower env; dsp::Biquad bp; int cnt = 0; };

    std::vector<dsp::Biquad>      filtHp_, filtLp_;
    std::vector<dsp::DelayLine>   vib_, chor_, flan_;
    std::vector<PhaserCh>         phaser_;
    std::vector<WahCh>            wah_;
    std::vector<CrushCh>          crush_;
    std::vector<dsp::EnvFollower> compEnv_;
    dsp::Lfo lfoVib_, lfoChor_, lfoFlan_, lfoPhas_;

    // fs-scaled modulation delays (samples), set in prepare()
    float vibCenterS_ = 0.f, vibRangeS_ = 0.f;
    float chorBaseS_  = 0.f, chorRangeS_ = 0.f;
    float flanBaseS_  = 0.f, flanRangeS_ = 0.f;
    // bitcrush scalars precomputed in updateParams()
    float crushLevels_ = 256.f, crushStep_ = 2.f / 256.f;
    int   crushDs_ = 4;

    dsp::OnePole sVibDepth_, sChorDepth_, sChorMix_, sFlanDepth_, sFlanFb_, sFlanMix_,
                 sPhasDepth_, sPhasFb_, sPhasMix_, sWahMix_, sCrushMix_, sCompMakeup_;

    // ---- Tier-3 (channel-vocoder family) ----
    std::vector<dsp::VocoderBank> formantBank_, vocBank_, whisBank_;
    dsp::Osc     vocOsc_;
    dsp::Rng     rng_;
    dsp::OnePole sVocMix_, sWhisMix_;

    // ---- Phase 7 experimental (FFT spectral; adds latency, bypassed when off) ----
    std::vector<dsp::StftProcessor> stft_;
};

}  // namespace avcrt
