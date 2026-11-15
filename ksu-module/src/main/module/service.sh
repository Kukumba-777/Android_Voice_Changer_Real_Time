#!/system/bin/sh
# service.sh — Magisk/KSU late_start service. Launches the AVCRT cellular
# voice-changer daemon supervisor and returns immediately (never blocks boot).
#
# The daemon (bin/avcrtd) is self-managing: it idles until the app enables the
# engine (active_preset.json "enabled":true) AND a cellular call is up, then
# engages a clean voice replace, and disengages + restores on call-end/disable.
# The supervisor guarantees the codec mute is cleared if the daemon ever exits
# for any reason (crash, OOM-kill) via `avcrtd --unmute`.

MODDIR=${0%/*}
BIN="$MODDIR/bin/avcrtd"
LOG=/data/adb/avcrt_daemon.log

[ -x "$BIN" ] || exit 0

supervise() {
    # Wait for the ALSA control node so mixer_open() succeeds.
    i=0
    while [ ! -e /dev/snd/controlC0 ] && [ $i -lt 60 ]; do sleep 2; i=$((i+1)); done
    # Clear any stale mute from a previous unclean shutdown.
    "$BIN" --unmute >> "$LOG" 2>&1
    while true; do
        echo "$(date '+%H:%M:%S') supervisor: starting avcrtd" >> "$LOG"
        "$BIN" >> "$LOG" 2>&1
        # Daemon exited (clean stop, crash, kill) — always restore, then respawn.
        "$BIN" --unmute >> "$LOG" 2>&1
        sleep 3
    done
}

supervise &
