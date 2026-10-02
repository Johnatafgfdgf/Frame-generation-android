#!/usr/bin/env python3
from pathlib import Path

ROOT = Path("ZalithLauncher2")
TOOLS = Path("build_tools")

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")

def write(rel, data):
    p = ROOT / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(data, encoding="utf-8")

def replace_once(text, old, new, label):
    if old not in text:
        raise RuntimeError("Patch marker not found: " + label)
    return text.replace(old, new, 1)

# Version identity
p = "ZalithLauncher/gradle.properties"
s = read(p).replace("launcher_version_name=2.0.3-fg1", "launcher_version_name=2.0.3-fg2")
write(p, s)

# HUD setting
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/setting/AllSettings.kt"
s = read(p)
if "frameGenHud" not in s:
    s = replace_once(
        s,
        '    val frameGenSceneProtection = boolSetting("frameGenSceneProtection", true)\n',
        '    val frameGenSceneProtection = boolSetting("frameGenSceneProtection", true)\n\n'
        '    /** Show real/generated/presented FPS over the running game. */\n'
        '    val frameGenHud = boolSetting("frameGenHud", true)\n',
        "frameGenHud setting"
    )
write(p, s)

# Main settings screen HUD switch
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/screens/content/settings/FrameGenerationSettingsScreen.kt"
s = read(p)
if "settings_framegen_hud_title" not in s:
    old = '''                    SwitchSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Bottom,
                        unit = AllSettings.frameGenSceneProtection,
                        title = stringResource(R.string.settings_framegen_scene_protection_title),
                        summary = stringResource(R.string.settings_framegen_scene_protection_summary),
                        enabled = enabled
                    )'''
    new = '''                    SwitchSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Middle,
                        unit = AllSettings.frameGenSceneProtection,
                        title = stringResource(R.string.settings_framegen_scene_protection_title),
                        summary = stringResource(R.string.settings_framegen_scene_protection_summary),
                        enabled = enabled
                    )

                    SwitchSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Bottom,
                        unit = AllSettings.frameGenHud,
                        title = stringResource(R.string.settings_framegen_hud_title),
                        summary = stringResource(R.string.settings_framegen_hud_summary)
                    )'''
    s = replace_once(s, old, new, "HUD launcher option")
write(p, s)

# Strings
for rel, portuguese in [
    ("ZalithLauncher/src/main/res/values/strings.xml", False),
    ("ZalithLauncher/src/main/res/values-pt-rBR/strings.xml", True),
    ("ZalithLauncher/src/main/res/values-pt/strings.xml", True),
]:
    s = read(rel)
    if "settings_framegen_hud_title" not in s:
        if portuguese:
            block = '''
    <string name="settings_framegen_hud_title">HUD do FrameGen no jogo</string>
    <string name="settings_framegen_hud_summary">Mostra FPS real, gerado e realmente apresentado enquanto você joga.</string>
'''
        else:
            block = '''
    <string name="settings_framegen_hud_title">In-game FrameGen HUD</string>
    <string name="settings_framegen_hud_summary">Shows real, generated and actually presented FPS while playing.</string>
'''
        s = s.replace("</resources>", block + "</resources>")
    write(rel, s)

# JNI stats declaration
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/bridge/ZLBridge.java"
s = read(p)
if "getFrameGenStats" not in s:
    s = replace_once(
        s,
        "    @Keep public static native int[] renderAWTScreenFrame();\n",
        "    @Keep public static native int[] renderAWTScreenFrame();\n"
        "    @Keep public static native long[] getFrameGenStats();\n",
        "ZLBridge stats"
    )
write(p, s)

# Overlay hook + file
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/activities/VMActivity.kt"
s = read(p)
if "FrameGenInGameOverlay()" not in s:
    s = replace_once(
        s,
        "            content()\n",
        "            content()\n            FrameGenInGameOverlay()\n",
        "VMActivity overlay"
    )
write(p, s)

overlay = (TOOLS / "FrameGenInGameOverlay.kt").read_text(encoding="utf-8")
write("ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/activities/FrameGenInGameOverlay.kt", overlay)

# Native fg2
p = "ZalithLauncher/src/main/jni/ctxbridges/osm_bridge.c"
s = read(p)
if "#include <pthread.h>" not in s:
    s = s.replace("#include <unistd.h>\n", "#include <unistd.h>\n#include <pthread.h>\n")

old_cleanup = '''        if(!bundle->disable_rendering) {
            __android_log_print(ANDROID_LOG_INFO, g_LogTag, "Unlocking for cleanup...");
            ANativeWindow_unlockAndPost(bundle->nativeSurface);
        }'''
new_cleanup = '''        const char *fg_cleanup_env = getenv("POJAV_FRAMEGEN_ENABLED");
        const bool fg_cleanup_active = fg_cleanup_env != NULL && strcmp(fg_cleanup_env, "1") == 0;
        if(!bundle->disable_rendering && !fg_cleanup_active) {
            __android_log_print(ANDROID_LOG_INFO, g_LogTag, "Unlocking for cleanup...");
            ANativeWindow_unlockAndPost(bundle->nativeSurface);
        }'''
if old_cleanup in s:
    s = s.replace(old_cleanup, new_cleanup, 1)

start = s.index("static uint8_t *fg_previous")
end = s.index("\nvoid osm_setup_window()", start)
native = (TOOLS / "osm_bridge_fg2_block.c.txt").read_text(encoding="utf-8")
s = s[:start] + native + s[end:]
write(p, s)

print("Frame Generation fg2 patch applied successfully.")
