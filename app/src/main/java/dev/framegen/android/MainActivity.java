package dev.framegen.android;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity implements SurfaceHolder.Callback {
    static {
        System.loadLibrary("framegen_native");
    }

    private static native String nativeProbeVulkan();
    private static native String nativeInitPresenter(android.view.Surface surface);
    private static native String nativeDrawFrame(float phase);
    private static native void nativeDestroyPresenter();

    private static native String nativeInstallVulkanHooks();
    private static native String nativeGetVulkanHookStats();
    private static native String nativeRunGuestVulkanTest();
    private static native String nativeRunGuestSurfaceTest(
            android.view.Surface surface,
            float phase);

    private TextView status;
    private SurfaceView guestSurfaceView;
    private float phase = 0.0f;
    private float guestPhase = 0.0f;
    private boolean presenterReady = false;
    private boolean guestSurfaceReady = false;
    private boolean guestTestLoaded = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = (int) (10 * getResources().getDisplayMetrics().density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        root.setPadding(pad, pad, pad, pad);
        root.setLayoutParams(new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
        ));

        TextView title = new TextView(this);
        title.setText("Frame Generation Android");
        title.setTextSize(21f);

        TextView subtitle = new TextView(this);
        subtitle.setText("M0.1 host compositor + M1B intercepted guest present");
        subtitle.setTextSize(12f);

        SurfaceView hostSurfaceView = new SurfaceView(this);
        hostSurfaceView.getHolder().addCallback(this);
        LinearLayout.LayoutParams hostParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1.0f
        );
        hostParams.topMargin = pad;
        hostSurfaceView.setLayoutParams(hostParams);

        guestSurfaceView = new SurfaceView(this);
        LinearLayout.LayoutParams guestParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                0.55f
        );
        guestParams.topMargin = pad / 2;
        guestParams.bottomMargin = pad;
        guestSurfaceView.setLayoutParams(guestParams);
        guestSurfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
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
            public void surfaceDestroyed(SurfaceHolder holder) {
                guestSurfaceReady = false;
            }
        });

        status = new TextView(this);
        status.setText("Creating Vulkan surfaces...");
        status.setTextSize(11f);
        status.setPadding(0, 0, 0, pad);

        LinearLayout row1 = new LinearLayout(this);
        row1.setOrientation(LinearLayout.HORIZONTAL);
        row1.setGravity(Gravity.CENTER);

        Button probe = new Button(this);
        probe.setText("Probe");
        probe.setOnClickListener(v -> {
            try {
                status.setText(nativeProbeVulkan());
            } catch (Throwable t) {
                status.setText("Native probe failed: " + t);
            }
        });

        Button hostPresent = new Button(this);
        hostPresent.setText("Host present");
        hostPresent.setOnClickListener(v -> {
            if (!presenterReady) {
                status.setText("Host Vulkan compositor is not ready.");
                return;
            }

            phase = nextPhase(phase);
            try {
                status.setText(nativeDrawFrame(phase));
            } catch (Throwable t) {
                status.setText("Host presentation failed: " + t);
            }
        });

        row1.addView(probe);
        row1.addView(hostPresent);

        LinearLayout row2 = new LinearLayout(this);
        row2.setOrientation(LinearLayout.HORIZONTAL);
        row2.setGravity(Gravity.CENTER);

        Button lookupTest = new Button(this);
        lookupTest.setText("M1A lookup");
        lookupTest.setOnClickListener(v -> runLookupTest());

        Button guestPresent = new Button(this);
        guestPresent.setText("M1B guest present");
        guestPresent.setOnClickListener(v -> runGuestPresentTest());

        row2.addView(lookupTest);
        row2.addView(guestPresent);

        root.addView(title);
        root.addView(subtitle);
        root.addView(hostSurfaceView);
        root.addView(guestSurfaceView);
        root.addView(status);
        root.addView(row1);
        root.addView(row2);

        setContentView(root);
    }

    private float nextPhase(float value) {
        value += 0.13f;
        return value >= 1.0f ? value - 1.0f : value;
    }

    private void ensureGuestHookRuntime() {
        nativeInstallVulkanHooks();

        if (!guestTestLoaded) {
            System.loadLibrary("framegen_guest_test");
            guestTestLoaded = true;
        }
    }

    private void runLookupTest() {
        try {
            String armed = nativeInstallVulkanHooks();
            if (!guestTestLoaded) {
                System.loadLibrary("framegen_guest_test");
                guestTestLoaded = true;
            }

            String guest = nativeRunGuestVulkanTest();
            String stats = nativeGetVulkanHookStats();
            status.setText(armed + "\n\n" + guest + "\n\n" + stats);
        } catch (Throwable t) {
            status.setText("M1A test failed: " + t);
        }
    }

    private void runGuestPresentTest() {
        if (!guestSurfaceReady) {
            status.setText("Guest Surface is not ready yet.");
            return;
        }

        try {
            ensureGuestHookRuntime();
            guestPhase = nextPhase(guestPhase);

            String result = nativeRunGuestSurfaceTest(
                    guestSurfaceView.getHolder().getSurface(),
                    guestPhase);
            String stats = nativeGetVulkanHookStats();

            status.setText(result + "\n\n" + stats);
        } catch (Throwable t) {
            status.setText("M1B guest present failed: " + t);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
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
    public void surfaceDestroyed(SurfaceHolder holder) {
        presenterReady = false;
        nativeDestroyPresenter();
    }

    private void initializePresenter(SurfaceHolder holder) {
        try {
            String result = nativeInitPresenter(holder.getSurface());
            presenterReady = result.startsWith("Compositor ready");
            status.setText(result);

            if (presenterReady) {
                status.setText(nativeDrawFrame(phase));
            }
        } catch (Throwable t) {
            presenterReady = false;
            status.setText("Host Vulkan compositor init failed: " + t);
        }
    }

    @Override
    protected void onDestroy() {
        presenterReady = false;
        nativeDestroyPresenter();
        super.onDestroy();
    }
}
