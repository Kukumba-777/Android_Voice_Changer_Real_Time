#!/system/bin/sh
# Only remove AVCRT's playback files; leave the app's library and backups alone.
SOUND=/data/avcrt_sound
rm -f "$SOUND/clip.pcm" "$SOUND/playback" "$SOUND/.link-pending" "$SOUND/.playback-pending" "$SOUND"/.clip-*.pcm
rm -f "$SOUND/cache-v2"/*.pcm "$SOUND/cache-v2"/.build-*.pcm
rmdir "$SOUND/cache-v2" 2>/dev/null || true
rmdir "$SOUND" 2>/dev/null || true
