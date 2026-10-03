#!/usr/bin/env python3
from pathlib import Path

ROOT = Path("ZalithLauncher2")

def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")

def write(rel, data):
    p = ROOT / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(data, encoding="utf-8")

# Version
p = "ZalithLauncher/gradle.properties"
s = read(p).replace("launcher_version_name=2.0.3-fg2", "launcher_version_name=2.0.3-fg3-winfg")
write(p, s)

# Make the UI accurately describe this backend.
for rel, pt in [
    ("ZalithLauncher/src/main/res/values/strings.xml", False),
    ("ZalithLauncher/src/main/res/values-pt-rBR/strings.xml", True),
    ("ZalithLauncher/src/main/res/values-pt/strings.xml", True),
]:
    s = read(rel)
    if pt:
        s = s.replace(
            "Interpolação interna de quadros para Zink/Turnip. Experimental e independente do VSync.",
            "Win-FG Native: optical flow Vulkan ARM64 integrado ao caminho Zink/Turnip. Não usa MediaProjection."
        )
        s = s.replace(
            "Desempenho é mais leve; Qualidade usa rejeição de artefatos mais forte.",
            "Controla a resolução do optical flow: Desempenho é mais leve, Balanceado é recomendado e Qualidade usa fluxo mais detalhado."
        )
        s = s.replace(
            "Evita esperas extras de sincronização. Recomendado para toque e controle.",
            "Em 2x usa um frame gerado mais próximo do frame anterior para reduzir a sensação de atraso."
        )
    else:
        s = s.replace(
            "Internal frame interpolation for Zink/Turnip. Experimental and independent from VSync.",
            "Win-FG Native: ARM64 Vulkan optical-flow frame generation integrated into the Zink/Turnip path. No MediaProjection."
        )
        s = s.replace(
            "Performance is fastest; Quality uses stronger artifact rejection.",
            "Controls optical-flow resolution: Performance is lightest, Balanced is recommended, Quality solves finer motion."
        )
        s = s.replace(
            "Avoid extra pacing waits. Recommended for touch and controller input.",
            "At 2x biases the generated frame earlier in time to reduce perceived input latency."
        )
    write(rel, s)

# Add a native library notice to assets.
assets = ROOT / "ZalithLauncher/src/main/assets"
assets.mkdir(parents=True, exist_ok=True)
(assets / "FRAMEGEN_BACKEND.txt").write_text(
    "Zalith FrameGen fg3 uses Win-FG Native (MIT) optical-flow frame synthesis.\n"
    "Upstream: The412Banner/win-fg commit fbc69ed6c5bf16b824d0a29bd8b171675101f9dd\n"
    "This is an unofficial modified Zalith Launcher build.\n",
    encoding="utf-8"
)

# Replace fg2 native blend/presenter bridge with the real Win-FG Native bridge.
p = "ZalithLauncher/src/main/jni/ctxbridges/osm_bridge.c"
s = read(p)
if "#include <dlfcn.h>" not in s:
    s = s.replace("#include <malloc.h>\n", "#include <malloc.h>\n#include <dlfcn.h>\n")

start = s.index("static pthread_mutex_t fg_mutex")
end = s.index("\nvoid osm_setup_window()", start)

native = r'''typedef int (*zfg_submit_frame_fn)(
    ANativeWindow*, const void*, int, int, int, int, int, int, int, int
);
typedef void (*zfg_release_surface_fn)(void);
typedef void (*zfg_get_stats_fn)(uint64_t out[4]);
typedef const char* (*zfg_backend_name_fn)(void);

static void *zfg_handle = NULL;
static zfg_submit_frame_fn zfg_submit_frame_p = NULL;
static zfg_release_surface_fn zfg_release_surface_p = NULL;
static zfg_get_stats_fn zfg_get_stats_p = NULL;
static zfg_backend_name_fn zfg_backend_name_p = NULL;

static uint8_t *fg3_render_buffer = NULL;
static size_t fg3_render_capacity = 0;
static int fg3_width = 0;
static int fg3_height = 0;
static int fg3_stride = 0;
static bool fg3_backend_active = false;

static bool fg3_env_bool(const char *name, bool fallback) {
    const char *value = getenv(name);
    if (value == NULL) return fallback;
    return strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0;
}

static int fg3_env_int(const char *name, int fallback, int minimum, int maximum) {
    const char *value = getenv(name);
    if (value == NULL) return fallback;
    long parsed = strtol(value, NULL, 10);
    if (parsed < minimum) parsed = minimum;
    if (parsed > maximum) parsed = maximum;
    return (int)parsed;
}

static int fg3_perf_preset(void) {
    const char *profile = getenv("POJAV_FRAMEGEN_PROFILE");
    if (profile == NULL) return 1;
    if (strcmp(profile, "quality") == 0) return 0;
    if (strcmp(profile, "performance") == 0) return 2;
    return 1;
}

static bool fg3_enabled(void) {
    return pojav_environ->config_renderer == RENDERER_VK_ZINK &&
           fg3_env_bool("POJAV_FRAMEGEN_ENABLED", false);
}

static bool fg3_load_backend(void) {
    if (zfg_handle != NULL) {
        return zfg_submit_frame_p != NULL &&
               zfg_release_surface_p != NULL &&
               zfg_get_stats_p != NULL;
    }

    zfg_handle = dlopen("libzalith_winfg.so", RTLD_NOW | RTLD_LOCAL);
    if (zfg_handle == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, "ZalithWinFG",
                            "dlopen libzalith_winfg.so failed: %s", dlerror());
        return false;
    }

    zfg_submit_frame_p = (zfg_submit_frame_fn)dlsym(zfg_handle, "zfg_submit_frame");
    zfg_release_surface_p = (zfg_release_surface_fn)dlsym(zfg_handle, "zfg_release_surface");
    zfg_get_stats_p = (zfg_get_stats_fn)dlsym(zfg_handle, "zfg_get_stats");
    zfg_backend_name_p = (zfg_backend_name_fn)dlsym(zfg_handle, "zfg_backend_name");

    if (!zfg_submit_frame_p || !zfg_release_surface_p || !zfg_get_stats_p) {
        __android_log_print(ANDROID_LOG_ERROR, "ZalithWinFG",
                            "Win-FG native symbols missing");
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, "ZalithWinFG", "Loaded backend: %s",
                        zfg_backend_name_p ? zfg_backend_name_p() : "Win-FG Native");
    return true;
}

static void fg3_release_backend_surface(void) {
    if (zfg_release_surface_p != NULL) {
        zfg_release_surface_p();
    }
    fg3_backend_active = false;
}

static bool fg3_prepare_render_buffer(osm_render_window_t *bundle) {
    if (bundle == NULL || bundle->nativeSurface == NULL) return false;

    int width = ANativeWindow_getWidth(bundle->nativeSurface);
    int height = ANativeWindow_getHeight(bundle->nativeSurface);
    if (width <= 0 || height <= 0) return false;

    int stride = width;
    size_t bytes = (size_t)stride * (size_t)height * 4u;
    if (bytes > fg3_render_capacity) {
        uint8_t *new_buffer = (uint8_t *)realloc(fg3_render_buffer, bytes);
        if (new_buffer == NULL) return false;
        fg3_render_buffer = new_buffer;
        fg3_render_capacity = bytes;
    }

    fg3_width = width;
    fg3_height = height;
    fg3_stride = stride;

    bundle->buffer.bits = fg3_render_buffer;
    bundle->buffer.width = width;
    bundle->buffer.height = height;
    bundle->buffer.stride = stride;
    bundle->buffer.format = WINDOW_FORMAT_RGBX_8888;
    return true;
}

static bool fg3_present_fallback(osm_render_window_t *bundle) {
    if (!bundle || !bundle->nativeSurface || !fg3_render_buffer) return false;

    ANativeWindow_Buffer out;
    if (ANativeWindow_lock(bundle->nativeSurface, &out, NULL) != 0) return false;

    int copy_width = fg3_width < out.width ? fg3_width : out.width;
    int copy_height = fg3_height < out.height ? fg3_height : out.height;
    for (int y = 0; y < copy_height; ++y) {
        memcpy(
            (uint8_t *)out.bits + ((size_t)y * (size_t)out.stride * 4u),
            fg3_render_buffer + ((size_t)y * (size_t)fg3_stride * 4u),
            (size_t)copy_width * 4u
        );
    }

    return ANativeWindow_unlockAndPost(bundle->nativeSurface) == 0;
}

void osm_swap_buffers() {
    if(currentBundle->state == STATE_RENDERER_NEW_WINDOW) {
        if (fg3_backend_active) fg3_release_backend_surface();
        osm_swap_surfaces(currentBundle);
        currentBundle->state = STATE_RENDERER_ALIVE;
    }

    if (!fg3_enabled()) {
        if (fg3_backend_active) fg3_release_backend_surface();

        if(currentBundle->nativeSurface != NULL && !currentBundle->disable_rendering)
            if(ANativeWindow_lock(currentBundle->nativeSurface, &currentBundle->buffer, NULL) != 0)
                osm_release_window();

        osm_apply_current_ll();
        glFinish_p();

        if(currentBundle->nativeSurface != NULL && !currentBundle->disable_rendering)
            if(ANativeWindow_unlockAndPost(currentBundle->nativeSurface) != 0)
                osm_release_window();
        return;
    }

    if (!fg3_prepare_render_buffer(currentBundle)) {
        return;
    }

    osm_apply_current_ll();
    glFinish_p();

    bool submitted = false;
    if (fg3_load_backend()) {
        int multiplier = fg3_env_int("POJAV_FRAMEGEN_MULTIPLIER", 2, 2, 4);
        int target_fps = fg3_env_int("POJAV_FRAMEGEN_TARGET_FPS", 60, 30, 120);
        int low_latency = fg3_env_bool("POJAV_FRAMEGEN_LOW_LATENCY", true) ? 1 : 0;
        int scene_protection = fg3_env_bool("POJAV_FRAMEGEN_SCENE_PROTECTION", true) ? 1 : 0;

        submitted = zfg_submit_frame_p(
            currentBundle->nativeSurface,
            fg3_render_buffer,
            fg3_width,
            fg3_height,
            fg3_stride,
            multiplier,
            fg3_perf_preset(),
            target_fps,
            low_latency,
            scene_protection
        ) != 0;
    }

    if (submitted) {
        fg3_backend_active = true;
        return;
    }

    if (fg3_backend_active) fg3_release_backend_surface();

    // Safe fallback: never leave the user with a black screen if Win-FG init fails.
    if (!fg3_present_fallback(currentBundle)) {
        osm_release_window();
    }
}

JNIEXPORT jlongArray JNICALL
Java_com_movtery_zalithlauncher_bridge_ZLBridge_getFrameGenStats(JNIEnv *env, jclass clazz) {
    uint64_t values64[4] = {0, 0, 0, 0};
    if (fg3_load_backend() && zfg_get_stats_p) {
        zfg_get_stats_p(values64);
    }

    jlong values[4] = {
        (jlong)values64[0],
        (jlong)values64[1],
        (jlong)values64[2],
        (jlong)values64[3]
    };
    jlongArray result = (*env)->NewLongArray(env, 4);
    if (result != NULL) {
        (*env)->SetLongArrayRegion(env, result, 0, 4, values);
    }
    return result;
}
'''

s = s[:start] + native + s[end:]
write(p, s)

print("Frame Generation fg3 Win-FG bridge applied successfully.")
