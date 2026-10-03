#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path("ZalithLauncher2")

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")

def write(rel, data):
    p = ROOT / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(data, encoding="utf-8")

def replace_once(text, old, new, label):
    if old not in text:
        raise RuntimeError(f"Patch marker not found: {label}")
    return text.replace(old, new, 1)

# 1) Give the unofficial build its own identity/package.
p = "ZalithLauncher/gradle.properties"
s = read(p)
s = s.replace("launcher_name=ZalithLauncher", "launcher_name=ZalithFrameGen")
s = s.replace("launcher_app_name=Zalith Launcher", "launcher_app_name=Zalith FrameGen (Unofficial)")
s = s.replace("launcher_short_name=ZL2", "launcher_short_name=ZLFG")
s = s.replace("url_home=https://github.com/ZalithLauncher/ZalithLauncher2", "url_home=https://github.com/Johnatafgfdgf/Frame-generation-android")
s = s.replace("launcher_version_name=2.0.3", "launcher_version_name=2.0.3-fg1")
write(p, s)

p = "ZalithLauncher/build.gradle.kts"
s = read(p)
s = replace_once(s, 'applicationIdSuffix = ".debug"', 'applicationIdSuffix = ".framegen"', "debug package suffix")
s = replace_once(s, 'versionNameSuffix = "-debug"', 'versionNameSuffix = "-framegen"', "debug version suffix")
write(p, s)

# 2) Persistent settings.
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/setting/AllSettings.kt"
s = read(p)
marker = '''    val vsyncInZink = boolSetting("vsyncInZink", false)

'''
addition = '''    val vsyncInZink = boolSetting("vsyncInZink", false)

    /**
     * Internal frame interpolation. This is an unofficial experimental feature.
     */
    val frameGenEnabled = boolSetting("frameGenEnabled", false)

    /**
     * Interpolation profile: performance, balanced or quality.
     */
    val frameGenProfile = stringSetting("frameGenProfile", "balanced")

    /**
     * Number of presented frames per real frame.
     */
    val frameGenMultiplier = intSetting("frameGenMultiplier", 2, 2..4)

    /**
     * Target display pacing used by the interpolator.
     */
    val frameGenTargetFps = intSetting("frameGenTargetFps", 60, 30..120)

    /**
     * Avoid deliberate pacing waits to minimize latency.
     */
    val frameGenLowLatency = boolSetting("frameGenLowLatency", true)

    /**
     * Do not interpolate large scene changes.
     */
    val frameGenSceneProtection = boolSetting("frameGenSceneProtection", true)

'''
s = replace_once(s, marker, addition, "AllSettings frame generation")
write(p, s)

# 3) Export settings to native renderer.
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/game/launch/Launcher.kt"
s = read(p)
marker = '''            if (AllSettings.vsyncInZink.getValue()) map["POJAV_VSYNC_IN_ZINK"] = "1"
            if (AllSettings.bigCoreAffinity.getValue()) map["POJAV_BIG_CORE_AFFINITY"] = "1"
'''
addition = '''            if (AllSettings.vsyncInZink.getValue()) map["POJAV_VSYNC_IN_ZINK"] = "1"

            map["POJAV_FRAMEGEN_ENABLED"] = if (AllSettings.frameGenEnabled.getValue()) "1" else "0"
            map["POJAV_FRAMEGEN_PROFILE"] = AllSettings.frameGenProfile.getValue()
            map["POJAV_FRAMEGEN_MULTIPLIER"] = AllSettings.frameGenMultiplier.getValue().toString()
            map["POJAV_FRAMEGEN_TARGET_FPS"] = AllSettings.frameGenTargetFps.getValue().toString()
            map["POJAV_FRAMEGEN_LOW_LATENCY"] = if (AllSettings.frameGenLowLatency.getValue()) "1" else "0"
            map["POJAV_FRAMEGEN_SCENE_PROTECTION"] = if (AllSettings.frameGenSceneProtection.getValue()) "1" else "0"

            if (AllSettings.bigCoreAffinity.getValue()) map["POJAV_BIG_CORE_AFFINITY"] = "1"
'''
s = replace_once(s, marker, addition, "Launcher env map")
write(p, s)

# 4) Add a real Settings navigation tab.
p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/screens/NormalNavKey.kt"
s = read(p)
marker = '''        /** 渲染器设置屏幕 */
        @Serializable data object Renderer : Settings
'''
addition = '''        /** 渲染器设置屏幕 */
        @Serializable data object Renderer : Settings
        /** Frame Generation settings (unofficial experimental feature) */
        @Serializable data object FrameGeneration : Settings
'''
s = replace_once(s, marker, addition, "NormalNavKey frame generation")
write(p, s)

p = "ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/screens/content/SettingsScreen.kt"
s = read(p)
s = replace_once(
    s,
    "import com.movtery.zalithlauncher.ui.screens.content.settings.GameSettingsScreen\n",
    "import com.movtery.zalithlauncher.ui.screens.content.settings.GameSettingsScreen\nimport com.movtery.zalithlauncher.ui.screens.content.settings.FrameGenerationSettingsScreen\n",
    "SettingsScreen import"
)
s = replace_once(
    s,
    '''    CategoryItem(NormalNavKey.Settings.Renderer, { CategoryIcon(Icons.Outlined.VideoSettings, R.string.settings_tab_renderer) }, R.string.settings_tab_renderer),
''',
    '''    CategoryItem(NormalNavKey.Settings.Renderer, { CategoryIcon(Icons.Outlined.VideoSettings, R.string.settings_tab_renderer) }, R.string.settings_tab_renderer),
    CategoryItem(NormalNavKey.Settings.FrameGeneration, { CategoryIcon(Icons.Outlined.VideoSettings, R.string.settings_tab_frame_generation) }, R.string.settings_tab_frame_generation),
''',
    "SettingsScreen category"
)
s = replace_once(
    s,
    '''                entry<NormalNavKey.Settings.Renderer> {
                    RendererSettingsScreen(key, settingsScreenKey, mainScreenKey)
                }
''',
    '''                entry<NormalNavKey.Settings.Renderer> {
                    RendererSettingsScreen(key, settingsScreenKey, mainScreenKey)
                }
                entry<NormalNavKey.Settings.FrameGeneration> {
                    FrameGenerationSettingsScreen(key, settingsScreenKey, mainScreenKey)
                }
''',
    "SettingsScreen navigation entry"
)
write(p, s)

# 5) Dedicated Frame Generation screen.
screen = r'''/*
 * Unofficial Frame Generation extension for Zalith Launcher 2.
 * This file is part of a modified build and is not affiliated with the official Zalith project.
 */
package com.movtery.zalithlauncher.ui.screens.content.settings

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.navigation3.runtime.NavKey
import com.movtery.zalithlauncher.R
import com.movtery.zalithlauncher.setting.AllSettings
import com.movtery.zalithlauncher.setting.unit.floatRange
import com.movtery.zalithlauncher.ui.base.BaseScreen
import com.movtery.zalithlauncher.ui.components.AnimatedColumn
import com.movtery.zalithlauncher.ui.screens.NestedNavKey
import com.movtery.zalithlauncher.ui.screens.NormalNavKey
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.CardPosition
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.IntSliderSettingsCard
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.ListSettingsCard
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.SettingsCardColumn
import com.movtery.zalithlauncher.ui.screens.content.settings.layouts.SwitchSettingsCard

@Composable
fun FrameGenerationSettingsScreen(
    key: NestedNavKey.Settings,
    settingsScreenKey: NavKey?,
    mainScreenKey: NavKey?
) {
    BaseScreen(
        Triple(key, mainScreenKey, false),
        Triple(NormalNavKey.Settings.FrameGeneration, settingsScreenKey, false)
    ) { isVisible ->
        AnimatedColumn(
            modifier = Modifier
                .fillMaxWidth()
                .verticalScroll(state = rememberScrollState())
                .padding(all = 12.dp),
            isVisible = isVisible
        ) { scope ->
            AnimatedItem(scope) { yOffset ->
                val enabled = AllSettings.frameGenEnabled.state

                SettingsCardColumn(
                    modifier = Modifier
                        .fillMaxWidth()
                        .offset { IntOffset(x = 0, y = yOffset.roundToPx()) }
                ) {
                    SwitchSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Top,
                        unit = AllSettings.frameGenEnabled,
                        title = stringResource(R.string.settings_framegen_enable_title),
                        summary = stringResource(R.string.settings_framegen_enable_summary)
                    )

                    ListSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Middle,
                        unit = AllSettings.frameGenProfile,
                        items = listOf("performance", "balanced", "quality"),
                        title = stringResource(R.string.settings_framegen_profile_title),
                        summary = stringResource(R.string.settings_framegen_profile_summary),
                        getItemText = {
                            when (it) {
                                "performance" -> stringResource(R.string.settings_framegen_profile_performance)
                                "quality" -> stringResource(R.string.settings_framegen_profile_quality)
                                else -> stringResource(R.string.settings_framegen_profile_balanced)
                            }
                        },
                        getItemId = { it },
                        enabled = enabled
                    )

                    IntSliderSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Middle,
                        unit = AllSettings.frameGenMultiplier,
                        title = stringResource(R.string.settings_framegen_multiplier_title),
                        summary = stringResource(R.string.settings_framegen_multiplier_summary),
                        valueRange = AllSettings.frameGenMultiplier.floatRange,
                        steps = 1,
                        suffix = "x",
                        enabled = enabled,
                        fineTuningControl = false
                    )

                    IntSliderSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Middle,
                        unit = AllSettings.frameGenTargetFps,
                        title = stringResource(R.string.settings_framegen_target_fps_title),
                        summary = stringResource(R.string.settings_framegen_target_fps_summary),
                        valueRange = AllSettings.frameGenTargetFps.floatRange,
                        suffix = " FPS",
                        enabled = enabled,
                        fineTuningControl = true
                    )

                    SwitchSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Middle,
                        unit = AllSettings.frameGenLowLatency,
                        title = stringResource(R.string.settings_framegen_low_latency_title),
                        summary = stringResource(R.string.settings_framegen_low_latency_summary),
                        enabled = enabled
                    )

                    SwitchSettingsCard(
                        modifier = Modifier.fillMaxWidth(),
                        position = CardPosition.Bottom,
                        unit = AllSettings.frameGenSceneProtection,
                        title = stringResource(R.string.settings_framegen_scene_protection_title),
                        summary = stringResource(R.string.settings_framegen_scene_protection_summary),
                        enabled = enabled
                    )
                }
            }
        }
    }
}
'''
write("ZalithLauncher/src/main/java/com/movtery/zalithlauncher/ui/screens/content/settings/FrameGenerationSettingsScreen.kt", screen)

# 6) UI strings.
english = r'''
    <!-- Unofficial Frame Generation extension -->
    <string name="settings_tab_frame_generation">Frame Generation</string>
    <string name="settings_framegen_enable_title">Frame Generation</string>
    <string name="settings_framegen_enable_summary">Internal frame interpolation for Zink/Turnip. Experimental and independent from VSync.</string>
    <string name="settings_framegen_profile_title">Interpolation profile</string>
    <string name="settings_framegen_profile_summary">Performance is fastest; Quality uses stronger artifact rejection.</string>
    <string name="settings_framegen_profile_performance">Performance</string>
    <string name="settings_framegen_profile_balanced">Balanced</string>
    <string name="settings_framegen_profile_quality">Quality</string>
    <string name="settings_framegen_multiplier_title">Frame multiplier</string>
    <string name="settings_framegen_multiplier_summary">2x is recommended for mobile GPUs. 3x and 4x cost more CPU/GPU time.</string>
    <string name="settings_framegen_target_fps_title">Target display FPS</string>
    <string name="settings_framegen_target_fps_summary">Used for optional presentation pacing; it does not increase the game simulation rate.</string>
    <string name="settings_framegen_low_latency_title">Low latency</string>
    <string name="settings_framegen_low_latency_summary">Avoid extra pacing waits. Recommended for touch and controller input.</string>
    <string name="settings_framegen_scene_protection_title">Scene-change protection</string>
    <string name="settings_framegen_scene_protection_summary">Skips interpolation on large image changes to reduce flashes and ghosting.</string>
'''
portuguese = r'''
    <!-- Extensão não oficial de geração de quadros -->
    <string name="settings_tab_frame_generation">Frame Generation</string>
    <string name="settings_framegen_enable_title">Frame Generation</string>
    <string name="settings_framegen_enable_summary">Interpolação interna de quadros para Zink/Turnip. Experimental e independente do VSync.</string>
    <string name="settings_framegen_profile_title">Perfil de interpolação</string>
    <string name="settings_framegen_profile_summary">Desempenho é mais leve; Qualidade usa rejeição de artefatos mais forte.</string>
    <string name="settings_framegen_profile_performance">Desempenho</string>
    <string name="settings_framegen_profile_balanced">Balanceado</string>
    <string name="settings_framegen_profile_quality">Qualidade</string>
    <string name="settings_framegen_multiplier_title">Multiplicador de quadros</string>
    <string name="settings_framegen_multiplier_summary">2x é recomendado em GPU móvel. 3x e 4x exigem mais processamento.</string>
    <string name="settings_framegen_target_fps_title">FPS alvo da tela</string>
    <string name="settings_framegen_target_fps_summary">Usado para o ritmo opcional de apresentação; não aumenta a velocidade da simulação do jogo.</string>
    <string name="settings_framegen_low_latency_title">Baixa latência</string>
    <string name="settings_framegen_low_latency_summary">Evita esperas extras de sincronização. Recomendado para toque e controle.</string>
    <string name="settings_framegen_scene_protection_title">Proteção de troca de cena</string>
    <string name="settings_framegen_scene_protection_summary">Pula a interpolação em mudanças grandes de imagem para reduzir flashes e ghosting.</string>
'''

for rel, block in [
    ("ZalithLauncher/src/main/res/values/strings.xml", english),
    ("ZalithLauncher/src/main/res/values-pt-rBR/strings.xml", portuguese),
    ("ZalithLauncher/src/main/res/values-pt/strings.xml", portuguese),
]:
    s = read(rel)
    if "settings_tab_frame_generation" not in s:
        s = s.replace("</resources>", block + "\n</resources>")
    write(rel, s)

# 7) Native in-process frame interpolator in the Zink/Turnip OSMesa presentation path.
p = "ZalithLauncher/src/main/jni/ctxbridges/osm_bridge.c"
s = read(p)
s = s.replace("#include <malloc.h>\n", "#include <malloc.h>\n#include <stdlib.h>\n#include <stdint.h>\n#include <strings.h>\n#include <unistd.h>\n")
if '#include "renderer_config.h"' not in s:
    s = s.replace('#include "osm_bridge.h"\n', '#include "osm_bridge.h"\n#include "renderer_config.h"\n')

start = s.index("void osm_swap_buffers() {")
end = s.index("\nvoid osm_setup_window()", start)

native = r'''static uint8_t *fg_previous = NULL;
static uint8_t *fg_current = NULL;
static size_t fg_capacity = 0;
static int fg_last_width = 0;
static int fg_last_height = 0;
static int fg_last_stride = 0;
static bool fg_has_previous = false;

static bool fg_env_bool(const char *name, bool fallback) {
    const char *value = getenv(name);
    if (value == NULL) return fallback;
    return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
}

static int fg_env_int(const char *name, int fallback, int minimum, int maximum) {
    const char *value = getenv(name);
    if (value == NULL) return fallback;
    long parsed = strtol(value, NULL, 10);
    if (parsed < minimum) parsed = minimum;
    if (parsed > maximum) parsed = maximum;
    return (int)parsed;
}

static const char *fg_env_profile(void) {
    const char *profile = getenv("POJAV_FRAMEGEN_PROFILE");
    return profile == NULL ? "balanced" : profile;
}

static bool fg_enabled(void) {
    return pojav_environ->config_renderer == RENDERER_VK_ZINK &&
           fg_env_bool("POJAV_FRAMEGEN_ENABLED", false);
}

static void fg_reset_history(void) {
    fg_has_previous = false;
}

static bool fg_prepare_buffers(const ANativeWindow_Buffer *buffer) {
    if (buffer == NULL || buffer->bits == NULL || buffer->width <= 0 ||
        buffer->height <= 0 || buffer->stride <= 0) {
        fg_reset_history();
        return false;
    }

    const size_t bytes = (size_t)buffer->stride * (size_t)buffer->height * 4u;
    const bool geometry_changed =
        buffer->width != fg_last_width ||
        buffer->height != fg_last_height ||
        buffer->stride != fg_last_stride;

    if (bytes > fg_capacity) {
        uint8_t *new_previous = (uint8_t *)malloc(bytes);
        uint8_t *new_current = (uint8_t *)malloc(bytes);
        if (new_previous == NULL || new_current == NULL) {
            free(new_previous);
            free(new_current);
            fg_reset_history();
            return false;
        }
        free(fg_previous);
        free(fg_current);
        fg_previous = new_previous;
        fg_current = new_current;
        fg_capacity = bytes;
        fg_reset_history();
    }

    if (geometry_changed) {
        fg_last_width = buffer->width;
        fg_last_height = buffer->height;
        fg_last_stride = buffer->stride;
        fg_reset_history();
    }
    return true;
}

static double fg_frame_difference(
    const uint8_t *a,
    const uint8_t *b,
    int width,
    int height,
    int stride
) {
    uint64_t difference = 0;
    uint64_t samples = 0;
    const int y_step = 6;
    const int x_step = 12;

    for (int y = 0; y < height; y += y_step) {
        const uint8_t *row_a = a + ((size_t)y * (size_t)stride * 4u);
        const uint8_t *row_b = b + ((size_t)y * (size_t)stride * 4u);
        for (int x = 0; x < width; x += x_step) {
            const int i = x * 4;
            difference += (uint64_t)abs((int)row_a[i + 0] - (int)row_b[i + 0]);
            difference += (uint64_t)abs((int)row_a[i + 1] - (int)row_b[i + 1]);
            difference += (uint64_t)abs((int)row_a[i + 2] - (int)row_b[i + 2]);
            samples += 3;
        }
    }

    if (samples == 0) return 0.0;
    return (double)difference / ((double)samples * 255.0);
}

static void fg_blend(
    uint8_t *dst,
    const uint8_t *previous,
    const uint8_t *current,
    int width,
    int height,
    int stride,
    int numerator,
    int denominator,
    const char *profile
) {
    const size_t bytes = (size_t)stride * (size_t)height * 4u;
    memcpy(dst, current, bytes);

    int artifact_threshold = 255;
    if (strcmp(profile, "quality") == 0) artifact_threshold = 52;
    else if (strcmp(profile, "balanced") == 0) artifact_threshold = 104;

    for (int y = 0; y < height; ++y) {
        uint8_t *out = dst + ((size_t)y * (size_t)stride * 4u);
        const uint8_t *a = previous + ((size_t)y * (size_t)stride * 4u);
        const uint8_t *b = current + ((size_t)y * (size_t)stride * 4u);

        for (int x = 0; x < width; ++x) {
            const int i = x * 4;
            const int d0 = abs((int)a[i + 0] - (int)b[i + 0]);
            const int d1 = abs((int)a[i + 1] - (int)b[i + 1]);
            const int d2 = abs((int)a[i + 2] - (int)b[i + 2]);
            const int max_delta = d0 > d1 ? (d0 > d2 ? d0 : d2) : (d1 > d2 ? d1 : d2);

            if (artifact_threshold < 255 && max_delta > artifact_threshold) {
                const uint8_t *chosen = (numerator * 2 >= denominator) ? b : a;
                out[i + 0] = chosen[i + 0];
                out[i + 1] = chosen[i + 1];
                out[i + 2] = chosen[i + 2];
            } else {
                out[i + 0] = (uint8_t)(((int)a[i + 0] * (denominator - numerator) + (int)b[i + 0] * numerator) / denominator);
                out[i + 1] = (uint8_t)(((int)a[i + 1] * (denominator - numerator) + (int)b[i + 1] * numerator) / denominator);
                out[i + 2] = (uint8_t)(((int)a[i + 2] * (denominator - numerator) + (int)b[i + 2] * numerator) / denominator);
            }
            out[i + 3] = b[i + 3];
        }
    }
}

static void fg_optional_pacing_sleep(void) {
    if (fg_env_bool("POJAV_FRAMEGEN_LOW_LATENCY", true)) return;

    const int target_fps = fg_env_int("POJAV_FRAMEGEN_TARGET_FPS", 60, 30, 120);
    long ns = 1000000000L / target_fps / 4L;
    if (ns > 4000000L) ns = 4000000L;
    if (ns < 250000L) ns = 250000L;

    usleep((unsigned int)(ns / 1000L));
}

static bool fg_post_and_lock_next(osm_render_window_t *bundle) {
    if (ANativeWindow_unlockAndPost(bundle->nativeSurface) != 0) {
        fg_reset_history();
        return false;
    }

    fg_optional_pacing_sleep();

    if (ANativeWindow_lock(bundle->nativeSurface, &bundle->buffer, NULL) != 0) {
        fg_reset_history();
        return false;
    }
    return true;
}

void osm_swap_buffers() {
    if(currentBundle->state == STATE_RENDERER_NEW_WINDOW) {
        osm_swap_surfaces(currentBundle);
        currentBundle->state = STATE_RENDERER_ALIVE;
        fg_reset_history();
    }

    bool locked = false;
    if(currentBundle->nativeSurface != NULL && !currentBundle->disable_rendering) {
        if(ANativeWindow_lock(currentBundle->nativeSurface, &currentBundle->buffer, NULL) != 0) {
            osm_release_window();
        } else {
            locked = true;
        }
    }

    osm_apply_current_ll();
    glFinish_p();

    if (!locked || currentBundle->nativeSurface == NULL || currentBundle->disable_rendering) {
        return;
    }

    if (!fg_enabled() || !fg_prepare_buffers(&currentBundle->buffer)) {
        fg_reset_history();
        if(ANativeWindow_unlockAndPost(currentBundle->nativeSurface) != 0)
            osm_release_window();
        return;
    }

    const size_t bytes = (size_t)currentBundle->buffer.stride *
                         (size_t)currentBundle->buffer.height * 4u;
    memcpy(fg_current, currentBundle->buffer.bits, bytes);

    const int multiplier = fg_env_int("POJAV_FRAMEGEN_MULTIPLIER", 2, 2, 4);
    const char *profile = fg_env_profile();
    const bool scene_protection = fg_env_bool("POJAV_FRAMEGEN_SCENE_PROTECTION", true);

    bool interpolate = fg_has_previous;
    if (interpolate) {
        const double difference = fg_frame_difference(
            fg_previous,
            fg_current,
            currentBundle->buffer.width,
            currentBundle->buffer.height,
            currentBundle->buffer.stride
        );

        // Exact/near duplicate frames do not need interpolation.
        if (difference < 0.0025) interpolate = false;

        // Large cuts are safer without generated frames.
        if (scene_protection && difference > 0.24) interpolate = false;
    }

    if (interpolate) {
        for (int step = 1; step < multiplier; ++step) {
            fg_blend(
                (uint8_t *)currentBundle->buffer.bits,
                fg_previous,
                fg_current,
                currentBundle->buffer.width,
                currentBundle->buffer.height,
                currentBundle->buffer.stride,
                step,
                multiplier,
                profile
            );

            if (!fg_post_and_lock_next(currentBundle)) {
                return;
            }
        }
    }

    // Always finish with the real current frame.
    memcpy(currentBundle->buffer.bits, fg_current, bytes);
    if (ANativeWindow_unlockAndPost(currentBundle->nativeSurface) != 0) {
        fg_reset_history();
        return;
    }

    memcpy(fg_previous, fg_current, bytes);
    fg_has_previous = true;
}
'''
s = s[:start] + native + s[end:]
write(p, s)

print("Frame Generation patch applied successfully.")
