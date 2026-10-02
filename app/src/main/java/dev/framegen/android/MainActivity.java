package dev.framegen.android;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import java.io.File;

public final class MainActivity extends Activity
        implements SurfaceHolder.Callback {

    private static final int REQUEST_LOSSLESS_DLL = 2001;

    static {
        System.loadLibrary("framegen_native");
        System.loadLibrary("lossless_backend");
    }

    private static native String nativeProbeVulkan();
    private static native String nativeInitPresenter(
            android.view.Surface surface);
    private static native String nativeDrawFrame(float phase);
    private static native void nativeDestroyPresenter();

    private static native String nativeInstallVulkanHooks();
    private static native String nativeGetVulkanHookStats();
    private static native String nativeGetSwapchainRegistry();
    private static native String nativeProbeFrameBridge();

    private static native String nativeRunGuestVulkanTest();
    private static native String nativeRunGuestSurfaceTest(
            android.view.Surface surface,
            float phase);

    private static native String nativePrepareLosslessBackend(
            String dllPath,
            int multiplier,
            boolean performance);
    private static native String nativeGetLosslessBackendStatus();
    private static native void nativeReleaseLosslessBackend();

    private TextView runtimeStatus;
    private TextView losslessStatus;
    private TextView backendStatus;

    private SurfaceView guestSurfaceView;

    private LinearLayout runtimePage;
    private ScrollView frameGenPage;

    private Switch frameGenerationSwitch;
    private Switch performanceModeSwitch;
    private Spinner multiplierSpinner;

    private float phase = 0.0f;
    private float guestPhase = 0.0f;

    private boolean presenterReady = false;
    private boolean guestSurfaceReady = false;
    private boolean guestTestLoaded = false;
    private boolean losslessBackendPrepared = false;
    private boolean frameGenerationRequested = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = dp(10);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));

        TextView title = new TextView(this);
        title.setText("Frame Generation Android");
        title.setTextSize(21f);

        TextView subtitle = new TextView(this);
        subtitle.setText(
                "Guest Vulkan interception + native LSFG");
        subtitle.setTextSize(12f);

        LinearLayout tabs = new LinearLayout(this);
        tabs.setOrientation(LinearLayout.HORIZONTAL);
        tabs.setGravity(Gravity.CENTER);

        Button runtimeTab = new Button(this);
        runtimeTab.setText("Runtime");

        Button frameGenTab = new Button(this);
        frameGenTab.setText("Frame Generation");

        tabs.addView(runtimeTab);
        tabs.addView(frameGenTab);

        FrameLayout pages = new FrameLayout(this);
        pages.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1.0f));

        runtimePage = buildRuntimePage();
        frameGenPage = buildFrameGenPage();

        pages.addView(
                runtimePage,
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));

        pages.addView(
                frameGenPage,
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));

        runtimeTab.setOnClickListener(v -> showRuntimePage());
        frameGenTab.setOnClickListener(v -> showFrameGenerationPage());

        root.addView(title);
        root.addView(subtitle);
        root.addView(tabs);
        root.addView(pages);

        setContentView(root);

        showRuntimePage();
        refreshLosslessStatus();
    }

    private LinearLayout buildRuntimePage() {
        int pad = dp(8);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER_HORIZONTAL);

        SurfaceView hostSurfaceView = new SurfaceView(this);
        hostSurfaceView.getHolder().addCallback(this);

        LinearLayout.LayoutParams hostParams =
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        0,
                        1.0f);
        hostParams.topMargin = pad;
        hostSurfaceView.setLayoutParams(hostParams);

        guestSurfaceView = new SurfaceView(this);

        LinearLayout.LayoutParams guestParams =
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        0,
                        0.55f);
        guestParams.topMargin = pad / 2;
        guestParams.bottomMargin = pad;
        guestSurfaceView.setLayoutParams(guestParams);

        guestSurfaceView.getHolder().addCallback(
                new SurfaceHolder.Callback() {
                    @Override
                    public void surfaceCreated(
                            SurfaceHolder holder) {
                        guestSurfaceReady = true;
                    }

                    @Override
                    public void surfaceChanged(
                            SurfaceHolder holder,
                            int format,
                            int width,
                            int height) {
                        guestSurfaceReady = true;
                    }

                    @Override
                    public void surfaceDestroyed(
                            SurfaceHolder holder) {
                        guestSurfaceReady = false;
                    }
                });

        runtimeStatus = new TextView(this);
        runtimeStatus.setText("Criando surfaces Vulkan...");
        runtimeStatus.setTextSize(11f);
        runtimeStatus.setPadding(0, 0, 0, pad);

        LinearLayout row1 = horizontalRow();

        Button probe = new Button(this);
        probe.setText("GPU");
        probe.setOnClickListener(v -> {
            try {
                runtimeStatus.setText(nativeProbeVulkan());
            } catch (Throwable t) {
                runtimeStatus.setText(
                        "Falha no probe Vulkan: " + t);
            }
        });

        Button hostPresent = new Button(this);
        hostPresent.setText("Host present");
        hostPresent.setOnClickListener(v -> {
            if (!presenterReady) {
                runtimeStatus.setText(
                        "Compositor Vulkan do host não está pronto.");
                return;
            }

            phase = nextPhase(phase);

            try {
                runtimeStatus.setText(
                        nativeDrawFrame(phase));
            } catch (Throwable t) {
                runtimeStatus.setText(
                        "Falha ao apresentar frame do host: " + t);
            }
        });

        row1.addView(probe);
        row1.addView(hostPresent);

        LinearLayout row2 = horizontalRow();

        Button lookupTest = new Button(this);
        lookupTest.setText("M1A lookup");
        lookupTest.setOnClickListener(v -> runLookupTest());

        Button guestPresent = new Button(this);
        guestPresent.setText("M1B present");
        guestPresent.setOnClickListener(v -> runGuestPresentTest());

        row2.addView(lookupTest);
        row2.addView(guestPresent);

        LinearLayout row3 = horizontalRow();

        Button bridgeProbe = new Button(this);
        bridgeProbe.setText("M2 bridge");
        bridgeProbe.setOnClickListener(v -> {
            try {
                runtimeStatus.setText(
                        nativeProbeFrameBridge());
            } catch (Throwable t) {
                runtimeStatus.setText(
                        "Falha no probe do frame bridge: " + t);
            }
        });

        Button registry = new Button(this);
        registry.setText("M2 registry");
        registry.setOnClickListener(v -> {
            try {
                runtimeStatus.setText(
                        nativeGetSwapchainRegistry());
            } catch (Throwable t) {
                runtimeStatus.setText(
                        "Falha ao ler registry: " + t);
            }
        });

        row3.addView(bridgeProbe);
        row3.addView(registry);

        page.addView(hostSurfaceView);
        page.addView(guestSurfaceView);
        page.addView(runtimeStatus);
        page.addView(row1);
        page.addView(row2);
        page.addView(row3);

        return page;
    }

    private ScrollView buildFrameGenPage() {
        int pad = dp(12);

        ScrollView scroll = new ScrollView(this);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(0, pad, 0, pad);

        TextView sectionTitle = new TextView(this);
        sectionTitle.setText("Backend Lossless Scaling");
        sectionTitle.setTextSize(18f);

        TextView explanation = new TextView(this);
        explanation.setText(
                "Adicione sua Lossless.dll. "
                + "Ela fica no armazenamento privado do app e é usada "
                + "somente como fonte dos shaders LSFG. "
                + "O Android não executa a DLL Windows.");
        explanation.setTextSize(13f);
        explanation.setPadding(0, pad / 2, 0, pad);

        losslessStatus = new TextView(this);
        losslessStatus.setTextSize(12f);
        losslessStatus.setPadding(0, 0, 0, pad);

        LinearLayout dllButtons = horizontalRow();

        Button chooseDll = new Button(this);
        chooseDll.setText("Adicionar DLL");
        chooseDll.setOnClickListener(v -> chooseLosslessDll());

        Button removeDll = new Button(this);
        removeDll.setText("Remover DLL");
        removeDll.setOnClickListener(v -> {
            nativeReleaseLosslessBackend();
            losslessBackendPrepared = false;
            frameGenerationRequested = false;

            boolean removed =
                    LosslessDllManager.remove(this);

            if (!removed) {
                losslessStatus.setText(
                        "Não foi possível remover a DLL.");
            }

            refreshLosslessStatus();
        });

        dllButtons.addView(chooseDll);
        dllButtons.addView(removeDll);

        TextView settingsTitle = new TextView(this);
        settingsTitle.setText("Frame Generation");
        settingsTitle.setTextSize(18f);
        settingsTitle.setPadding(0, pad, 0, pad / 2);

        TextView multiplierLabel = new TextView(this);
        multiplierLabel.setText("Multiplicador");

        multiplierSpinner = new Spinner(this);
        String[] multipliers = {
                "2x",
                "3x",
                "4x",
                "5x",
                "6x",
                "7x",
                "8x"
        };

        ArrayAdapter<String> adapter =
                new ArrayAdapter<>(
                        this,
                        android.R.layout.simple_spinner_item,
                        multipliers);
        adapter.setDropDownViewResource(
                android.R.layout.simple_spinner_dropdown_item);
        multiplierSpinner.setAdapter(adapter);

        performanceModeSwitch = new Switch(this);
        performanceModeSwitch.setText(
                "Performance mode (LSFG 3.1P)");

        Button prepareBackend = new Button(this);
        prepareBackend.setText("Preparar / validar LSFG");
        prepareBackend.setOnClickListener(
                v -> prepareLosslessBackend());

        frameGenerationSwitch = new Switch(this);
        frameGenerationSwitch.setText(
                "Usar frame generation no pipeline");
        frameGenerationSwitch.setEnabled(false);
        frameGenerationSwitch.setOnCheckedChangeListener(
                this::onFrameGenerationToggle);

        backendStatus = new TextView(this);
        backendStatus.setTextSize(12f);
        backendStatus.setPadding(0, pad / 2, 0, pad);

        TextView architectureTitle = new TextView(this);
        architectureTitle.setText("Pipeline");
        architectureTitle.setTextSize(18f);
        architectureTitle.setPadding(0, pad, 0, pad / 2);

        TextView architecture = new TextView(this);
        architecture.setText(
                "Guest VkImage\n"
                + "  ↓ interceptor\n"
                + "AHardwareBuffer input A/B\n"
                + "  ↓ LSFG 3.1 / 3.1P Vulkan\n"
                + "AHardwareBuffer generated frames\n"
                + "  ↓ compositor Vulkan\n"
                + "Display\n\n"
                + "Sem MediaProjection no caminho planejado.");
        architecture.setTextSize(12f);

        content.addView(sectionTitle);
        content.addView(explanation);
        content.addView(losslessStatus);
        content.addView(dllButtons);

        content.addView(settingsTitle);
        content.addView(multiplierLabel);
        content.addView(multiplierSpinner);
        content.addView(performanceModeSwitch);
        content.addView(prepareBackend);
        content.addView(frameGenerationSwitch);
        content.addView(backendStatus);

        content.addView(architectureTitle);
        content.addView(architecture);

        scroll.addView(content);
        return scroll;
    }

    private void onFrameGenerationToggle(
            CompoundButton button,
            boolean checked) {
        if (!losslessBackendPrepared) {
            if (checked) {
                button.setChecked(false);
            }
            frameGenerationRequested = false;
            return;
        }

        frameGenerationRequested = checked;

        if (checked) {
            backendStatus.setText(
                    nativeGetLosslessBackendStatus()
                    + "\nFrame generation: ARMADO. "
                    + "O M3 agora deve conectar os AHardwareBuffers "
                    + "do guest a este backend.");
        } else {
            backendStatus.setText(
                    nativeGetLosslessBackendStatus()
                    + "\nFrame generation: desativado pelo usuário.");
        }
    }

    private LinearLayout horizontalRow() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER);
        return row;
    }

    private void showRuntimePage() {
        runtimePage.setVisibility(View.VISIBLE);
        frameGenPage.setVisibility(View.GONE);
    }

    private void showFrameGenerationPage() {
        runtimePage.setVisibility(View.GONE);
        frameGenPage.setVisibility(View.VISIBLE);
        refreshLosslessStatus();
    }

    private void chooseLosslessDll() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");

        startActivityForResult(
                Intent.createChooser(
                        intent,
                        "Selecione Lossless.dll"),
                REQUEST_LOSSLESS_DLL);
    }

    @Override
    protected void onActivityResult(
            int requestCode,
            int resultCode,
            Intent data) {
        super.onActivityResult(
                requestCode,
                resultCode,
                data);

        if (requestCode != REQUEST_LOSSLESS_DLL ||
            resultCode != RESULT_OK ||
            data == null) {
            return;
        }

        Uri uri = data.getData();
        if (uri == null) {
            losslessStatus.setText(
                    "Nenhum arquivo foi selecionado.");
            return;
        }

        try {
            nativeReleaseLosslessBackend();
            losslessBackendPrepared = false;
            frameGenerationRequested = false;

            LosslessDllManager.Info info =
                    LosslessDllManager.importFromUri(
                            this,
                            uri);

            losslessStatus.setText(
                    info.describe());

            prepareLosslessBackend();
        } catch (Throwable t) {
            losslessStatus.setText(
                    "Falha ao adicionar DLL: "
                    + t.getMessage());
            refreshLosslessStatus();
        }
    }

    private void prepareLosslessBackend() {
        LosslessDllManager.Info info =
                LosslessDllManager.inspect(this);

        if (!info.installed ||
            !info.validPe ||
            !info.x64) {
            losslessBackendPrepared = false;
            frameGenerationSwitch.setEnabled(false);
            frameGenerationSwitch.setChecked(false);
            backendStatus.setText(
                    "Adicione uma Lossless.dll x86-64 válida primeiro.");
            return;
        }

        File dll =
                LosslessDllManager.getDllFile(this);

        int multiplier =
                multiplierSpinner.getSelectedItemPosition() + 2;

        boolean performance =
                performanceModeSwitch.isChecked();

        try {
            String result =
                    nativePrepareLosslessBackend(
                            dll.getAbsolutePath(),
                            multiplier,
                            performance);

            losslessBackendPrepared =
                    result.startsWith(
                            "LSFG backend ready");

            frameGenerationSwitch.setChecked(false);
            frameGenerationSwitch.setEnabled(
                    losslessBackendPrepared);

            backendStatus.setText(result);
        } catch (Throwable t) {
            losslessBackendPrepared = false;
            frameGenerationSwitch.setChecked(false);
            frameGenerationSwitch.setEnabled(false);

            backendStatus.setText(
                    "Falha ao preparar LSFG: " + t);
        }
    }

    private void refreshLosslessStatus() {
        if (losslessStatus == null ||
            backendStatus == null ||
            frameGenerationSwitch == null) {
            return;
        }

        LosslessDllManager.Info info =
                LosslessDllManager.inspect(this);

        losslessStatus.setText(
                info.describe());

        if (!info.installed ||
            !info.validPe ||
            !info.x64) {
            losslessBackendPrepared = false;
            frameGenerationRequested = false;

            frameGenerationSwitch.setChecked(false);
            frameGenerationSwitch.setEnabled(false);

            backendStatus.setText(
                    "Backend indisponível até uma Lossless.dll "
                    + "x86-64 válida ser adicionada.");
            return;
        }

        try {
            String status =
                    nativeGetLosslessBackendStatus();

            losslessBackendPrepared =
                    status.startsWith(
                            "LSFG backend ready");

            frameGenerationSwitch.setEnabled(
                    losslessBackendPrepared);

            if (losslessBackendPrepared) {
                backendStatus.setText(status);
            } else {
                backendStatus.setText(
                        "DLL válida. Toque em "
                        + ""Preparar / validar LSFG" "
                        + "para extrair os shaders e testar a GPU.");
            }
        } catch (Throwable t) {
            losslessBackendPrepared = false;
            frameGenerationSwitch.setEnabled(false);
            backendStatus.setText(
                    "Backend nativo indisponível: " + t);
        }
    }

    private float nextPhase(float value) {
        value += 0.13f;
        return value >= 1.0f
                ? value - 1.0f
                : value;
    }

    private void ensureGuestHookRuntime() {
        nativeInstallVulkanHooks();

        if (!guestTestLoaded) {
            System.loadLibrary(
                    "framegen_guest_test");
            guestTestLoaded = true;
        }
    }

    private void runLookupTest() {
        try {
            String armed =
                    nativeInstallVulkanHooks();

            if (!guestTestLoaded) {
                System.loadLibrary(
                        "framegen_guest_test");
                guestTestLoaded = true;
            }

            String guest =
                    nativeRunGuestVulkanTest();

            String stats =
                    nativeGetVulkanHookStats();

            runtimeStatus.setText(
                    armed
                    + "\n\n"
                    + guest
                    + "\n\n"
                    + stats);
        } catch (Throwable t) {
            runtimeStatus.setText(
                    "M1A falhou: " + t);
        }
    }

    private void runGuestPresentTest() {
        if (!guestSurfaceReady) {
            runtimeStatus.setText(
                    "Guest Surface ainda não está pronta.");
            return;
        }

        try {
            ensureGuestHookRuntime();
            guestPhase = nextPhase(guestPhase);

            String result =
                    nativeRunGuestSurfaceTest(
                            guestSurfaceView
                                    .getHolder()
                                    .getSurface(),
                            guestPhase);

            String stats =
                    nativeGetVulkanHookStats();

            String registry =
                    nativeGetSwapchainRegistry();

            runtimeStatus.setText(
                    result
                    + "\n\n"
                    + stats
                    + "\n\n"
                    + registry);
        } catch (Throwable t) {
            runtimeStatus.setText(
                    "M1B guest present falhou: " + t);
        }
    }

    @Override
    public void surfaceCreated(
            SurfaceHolder holder) {
        initializePresenter(holder);
    }

    @Override
    public void surfaceChanged(
            SurfaceHolder holder,
            int format,
            int width,
            int height) {
        initializePresenter(holder);
    }

    @Override
    public void surfaceDestroyed(
            SurfaceHolder holder) {
        presenterReady = false;
        nativeDestroyPresenter();
    }

    private void initializePresenter(
            SurfaceHolder holder) {
        try {
            String result =
                    nativeInitPresenter(
                            holder.getSurface());

            presenterReady =
                    result.startsWith(
                            "Compositor ready");

            runtimeStatus.setText(result);

            if (presenterReady) {
                runtimeStatus.setText(
                        nativeDrawFrame(phase));
            }
        } catch (Throwable t) {
            presenterReady = false;
            runtimeStatus.setText(
                    "Falha ao iniciar compositor Vulkan: " + t);
        }
    }

    @Override
    protected void onDestroy() {
        presenterReady = false;

        nativeDestroyPresenter();
        nativeReleaseLosslessBackend();

        super.onDestroy();
    }

    private int dp(int value) {
        return (int) (
                value *
                getResources()
                        .getDisplayMetrics()
                        .density);
    }
}
