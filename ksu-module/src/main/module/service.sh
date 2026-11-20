#!/system/bin/sh
# service.sh — Magisk/KSU late_start service. Launches the AVCRT cellular
# voice-changer daemon supervisor and returns immediately (never blocks boot).
#
# The daemon (bin/avcrtd) is self-managing: it idles until the app enables the
# engine (active_preset.json "enabled":true) AND a cellular call is up, then
# engages a clean voice replace, and disengages + restores on call-end/disable.
# The supervisor attempts to clear the dedicated AVCRT mute control via
# `avcrtd --unmute`. It does not reconstruct HAL routing after a crash.

MODDIR=${0%/*}
BIN="$MODDIR/bin/avcrtd"
LOG=/data/adb/avcrt_daemon.log

[ -x "$BIN" ] || exit 0

supervise() {
    # Clear any stale mute from a previous unclean shutdown.
    "$BIN" --unmute >> "$LOG" 2>&1
    while true; do
        echo "$(date '+%H:%M:%S') supervisor: starting avcrtd" >> "$LOG"
        "$BIN" >> "$LOG" 2>&1
        # Daemon exited (clean stop, crash, kill) — clear AVCRT mute, then respawn.
        "$BIN" --unmute >> "$LOG" 2>&1
        sleep 3
    done
}

supervise &
