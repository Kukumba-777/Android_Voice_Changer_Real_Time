#!/system/bin/sh
# post-fs-data.sh — early boot. Data dir + sentinels, and mirror the active preset
# into avcrt.dsp.* system properties so the Zygisk app-capture hook has the config
# from boot (app processes can't read the JSON; props don't survive reboot).
MODDIR=${0%/*}
DATA=/data/local/tmp/avcrt
PRESET="$DATA/active_preset.json"

# Distinguish a skipped module from one that reached early boot only.
if BOOT=$(cat /proc/sys/kernel/random/boot_id) && [ -n "$BOOT" ]; then
    (umask 077; printf '%s\n' "$BOOT" > /data/adb/avcrt_early_boot)
fi

mkdir -p "$DATA"; chmod 0755 "$DATA"
[ -f "$DATA/engine_state" ] || { echo off > "$DATA/engine_state"; chmod 0644 "$DATA/engine_state"; }
rm -f /data/adb/avcrt_install_pending 2>/dev/null

# Transient PCM only. The directory is not writable by capture clients.
SOUND=/dev/avcrt_sound
mkdir -p "$SOUND" && chmod 0755 "$SOUND"
rm -f "$SOUND/clip.pcm"
if [ -d "$MODDIR/zygisk" ] || [ -f "$MODDIR/bin/avcrt_inject" ]; then
    chcon u:object_r:avcrt_sound_file:s0 "$SOUND"
fi

# The AF claim is non-persistent; only an installed hook publishes it.

# Clear only AVCRT's dedicated mute control. Topology is deliberately not restored
# from stale values because ownership cannot be proven across a daemon crash.
"$MODDIR/bin/avcrtd" --unmute >/dev/null 2>&1 &

# mirror active_preset.json -> props (single-line JSON; grep the fields)
if [ -f "$PRESET" ]; then
    jget() { grep -o "\"$1\":[-0-9.]*" "$PRESET" 2>/dev/null | head -1 | cut -d: -f2; }
    en=$(grep -o '"enabled":[a-z]*' "$PRESET" | head -1 | cut -d: -f2); [ "$en" = true ] && en=1 || en=0
    pf=$(grep -o '"preserveFormants":[a-z]*' "$PRESET" | head -1 | cut -d: -f2); [ "$pf" = false ] && pf=0 || pf=1
    setprop avcrt.dsp.enabled "$en"
    bypass=$(grep -o '"bypassVoice":[a-z]*' "$PRESET" | head -1 | cut -d: -f2)
    [ "$bypass" = true ] && bypass=1 || bypass=0
    setprop avcrt.dsp.bypass_voice "$bypass"
    setprop avcrt.dsp.pitch "$(jget pitchSemitones)"
    setprop avcrt.dsp.formant "$(jget formantSemitones)"
    setprop avcrt.dsp.gain "$(jget gainDb)"
    setprop avcrt.dsp.preserve_formants "$pf"
    setprop avcrt.config.revision "$(jget revision)"
fi
