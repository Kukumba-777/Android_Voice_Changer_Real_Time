#!/system/bin/sh
# AVCRT module installer. App capture requires a compatible Zygisk runtime and
# audio ABI. Cellular clean-replace requires the expected kernel controls and
# a supported route; the daemon verifies these at runtime.

ui_print "  AVCRT — system-wide voice changer"
[ ! -d "$MODPATH/zygisk" ] || ui_print "  • App capture: requires a compatible Zygisk runtime"
if [ -f "$MODPATH/bin/avcrt_inject" ]; then
    ui_print "  • Shared-HAL backend: selected only after successful runtime hook install"
    ui_print "  • Zygisk app hooks remain as fallback when the shared backend is unavailable"
    if [ "${KSU:-}" = true ] && [ -L /data/adb/metamodule ] && [ ! -d /data/adb/metamodule ]; then
        ui_print "  ! KernelSU metamodule link points to a missing directory"
        ui_print "  ! Keep the regular AVCRT ZIP until module mounting is restored"
    fi
fi
ui_print "  • Cellular routing is experimental and requires a compatible audio route"
ui_print "  • Built-in microphone replacement requires a matching AVCRT kernel patch"

set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/bin/avcrtd"        0 0 0755
[ ! -f "$MODPATH/bin/avcrt_inject" ] || set_perm "$MODPATH/bin/avcrt_inject" 0 0 0755
set_perm "$MODPATH/service.sh"        0 0 0755
set_perm "$MODPATH/post-fs-data.sh"   0 0 0755
set_perm "$MODPATH/uninstall.sh"      0 0 0755

# Fresh install = engine OFF until the app explicitly enables it. Reset the
# active-preset config to disabled (a leftover enabled=true would make the daemon
# engage on the first boot before the app has synced its state → voice changed
# even though the app shows it off). The app rewrites this when the user picks a
# preset / toggles the engine.
mkdir -p /data/local/tmp/avcrt
echo '{"schema":1,"enabled":false,"presetId":null,"dsp":null,"revision":0}' \
    > /data/local/tmp/avcrt/active_preset.json
echo "off" > /data/local/tmp/avcrt/engine_state
chmod 0755 /data/local/tmp/avcrt
chmod 0644 /data/local/tmp/avcrt/active_preset.json /data/local/tmp/avcrt/engine_state

cat /proc/sys/kernel/random/boot_id > /data/adb/avcrt_install_pending
ui_print "  Reboot to activate (engine starts OFF — enable it in the app)."
