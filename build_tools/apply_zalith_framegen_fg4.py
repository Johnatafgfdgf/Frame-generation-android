#!/usr/bin/env python3
from pathlib import Path

ROOT = Path("ZalithLauncher2")
TOOLS = Path("build_tools/fg4")

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")

def write(rel, data):
    p = ROOT / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(data, encoding="utf-8")

# Version identity.
p = "ZalithLauncher/gradle.properties"
s = read(p).replace("launcher_version_name=2.0.3-fg2", "launcher_version_name=2.0.3-fg4-lsfg")
write(p, s)

# Copy the Java DLL/cache bridge and the dedicated settings screen.
write(
    "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/framegen/LsfgNativeBridge.java",
    (TOOLS / "LsfgNativeBridge.java").read_text(encoding="utf-8")
)
write(
    "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/screens/content/settings/FrameGenerationSettingsScreen.kt",
    (TOOLS / "FrameGenerationSettingsScreenLSFG.kt").read_text(encoding="utf-8")
)

# Pass private LSFG paths into the game/native process.
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/game/launch/Launcher.kt"
s = read(p)
marker = '''            map["POJAV_FRAMEGEN_SCENE_PROTECTION"] = if (AllSettings.frameGenSceneProtection.getValue()) "1" else "0"

            if (AllSettings.bigCoreAffinity.getValue()) map["POJAV_BIG_CORE_AFFINITY"] = "1"
'''
replacement = '''            map["POJAV_FRAMEGEN_SCENE_PROTECTION"] = if (AllSettings.frameGenSceneProtection.getValue()) "1" else "0"
            map["POJAV_LSFG_DLL"] = File(PathManager.DIR_FILES_PRIVATE, "lsfg-vk/Lossless.dll").absolutePath
            map["POJAV_LSFG_CACHE"] = File(PathManager.DIR_FILES_PRIVATE, "lsfg-native/shaders.cache").absolutePath

            if (AllSettings.bigCoreAffinity.getValue()) map["POJAV_BIG_CORE_AFFINITY"] = "1"
'''
if marker not in s:
    raise RuntimeError("Launcher framegen env marker missing")
s = s.replace(marker, replacement, 1)
write(p, s)

# Make UI text accurate for LSFG Native.
for rel, pt in [
    ("ZalithLauncher/src/main/res/values/strings.xml", False),
    ("ZalithLauncher/src/main/res/values-pt-rBR/strings.xml", True),
    ("ZalithLauncher/src/main/res/values-pt/strings.xml", True),
]:
    s = read(rel)
    if pt:
        s = s.replace(
            "Interpolação interna de quadros para Zink/Turnip. Experimental e independente do VSync.",
            "LSFG Native usando os shaders da sua própria Lossless.dll, executados em Vulkan no Android. Não usa MediaProjection."
        )
        s = s.replace(
            "Desempenho é mais leve; Qualidade usa rejeição de artefatos mais forte.",
            "Controla a resolução de processamento do LSFG. Desempenho é mais leve, Balanceado é recomendado e Qualidade usa mais GPU."
        )
        s = s.replace(
            "Evita esperas extras de sincronização. Recomendado para toque e controle.",
            "Reduz esperas adicionais na apresentação dos frames gerados."
        )
        s = s.replace(
            "Pula a interpolação em mudanças grandes de imagem para reduzir flashes e ghosting.",
            "Evita gerar frames em mudanças bruscas de cena para reduzir artefatos."
        )
    else:
        s = s.replace(
            "Internal frame interpolation for Zink/Turnip. Experimental and independent from VSync.",
            "LSFG Native using shaders from your own Lossless.dll, executed through Vulkan on Android. No MediaProjection."
        )
        s = s.replace(
            "Performance is fastest; Quality uses stronger artifact rejection.",
            "Controls LSFG processing resolution. Performance is lighter, Balanced is recommended, Quality uses more GPU."
        )
        s = s.replace(
            "Avoid extra pacing waits. Recommended for touch and controller input.",
            "Reduces extra presentation waits for generated frames."
        )
        s = s.replace(
            "Skips interpolation on large image changes to reduce flashes and ghosting.",
            "Skips generation across abrupt scene changes to reduce artifacts."
        )
    write(rel, s)

# Brand in-game overlay as LSFG.
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/activities/FrameGenInGameOverlay.kt"
s = read(p)
s = s.replace('"FG " + (if (enabled) "ON" else "OFF")', '"LSFG " + (if (enabled) "ON" else "OFF")')
s = s.replace('Text(if (expanded) "Fechar FG" else "FG")', 'Text(if (expanded) "Fechar LSFG" else "LSFG")')
s = s.replace('Text("Frame Generation", modifier = Modifier.weight(1f))', 'Text("LSFG Native", modifier = Modifier.weight(1f))')
write(p, s)

# Asset notice. No proprietary shader or DLL is bundled.
assets = ROOT / "ZalithLauncher/src/main/assets"
assets.mkdir(parents=True, exist_ok=True)
(assets / "FRAMEGEN_BACKEND.txt").write_text(
    "Zalith FrameGen fg4 uses LSFG Native.\n"
    "The app bundles no Lossless Scaling DLL or proprietary LSFG shaders.\n"
    "The user imports their own Lossless.dll. It is parsed read-only as PE resources,\n"
    "the LSFG shader chain is converted/cached locally, and the cached modules run on Vulkan.\n"
    "LSFG Native integration derived from The412Banner/Bannerlator GPL-3.0 code.\n",
    encoding="utf-8"
)

# Replace fg2 CPU interpolator with LSFG Native presenter bridge.
p = "ZalithLauncher/src/main/jni/ctxbridges/osm_bridge.c"
s = read(p)
if "#include <dlfcn.h>" not in s:
    s = s.replace("#include <malloc.h>\n", "#include <malloc.h>\n#include <dlfcn.h>\n")

start = s.index("static pthread_mutex_t fg_mutex")
end = s.index("\nvoid osm_setup_window()", start)
native = (TOOLS / "osm_bridge_fg4_block.c.txt").read_text(encoding="utf-8")
s = s[:start] + native + s[end:]
write(p, s)

print("Frame Generation fg4 LSFG Native patch applied successfully.")
