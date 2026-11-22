#!/system/bin/sh
# service.sh — Magisk/KSU late_start service. Launches the AVCRT cellular
# voice-changer daemon supervisor and returns immediately (never blocks boot).
#
# The daemon (bin/avcrtd) is self-managing: it idles until the app enables the
# engine (active_preset.json "enabled":true) AND a cellular call is up, then
# engages a clean voice replace, and disengages + restores on call-end/disable.
# The supervisor attempts to clear the dedicated AVCRT mute control via
# `avcrtd --unmute`. It intentionally does not reconstruct HAL routing after a crash:
# mixer edits are ownership-journaled only in daemon memory, so blind restoration could
# overwrite a route concurrently claimed by the audio HAL. A device-level audio-service
# restart/reboot is the safe way to force a full HAL rebuild after a daemon crash mid-route.

MODDIR=${0%/*}
BIN="$MODDIR/bin/avcrtd"
LOG=/data/adb/avcrt_daemon.log
BOOT_FILE=/data/adb/avcrt_service_boot

[ -x "$BIN" ] || exit 0
BOOT=$(cat /proc/sys/kernel/random/boot_id) || exit 1
[ -n "$BOOT" ] || exit 1
if [ "$(cat "$BOOT_FILE" 2>/dev/null)" != "$BOOT" ]; then
    umask 077
    : > "$LOG" || exit 1
    printf '%s\n' "$BOOT" > "$BOOT_FILE" || exit 1
fi

# Derive the optional shared-HAL payload. Only the installed hook can claim
# the backend. A daemon attempt or restart must not overwrite that claim.
AVCRT_AF_PAYLOAD=
for payload in "$MODDIR"/system/lib*/libavcrt_afhook.so; do
    [ -f "$payload" ] || continue
    if [ -n "$AVCRT_AF_PAYLOAD" ]; then AVCRT_AF_PAYLOAD=; break; fi
    AVCRT_AF_PAYLOAD=${payload#"$MODDIR"}
done
export AVCRT_AF_PAYLOAD

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
