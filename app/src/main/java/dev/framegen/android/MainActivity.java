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

    private TextView status;
    private float phase = 0.0f;
    private boolean presenterReady = false;
    private boolean guestTestLoaded = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = (int) (12 * getResources().getDisplayMetrics().density);

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
        title.setTextSize(22f);

        TextView subtitle = new TextView(this);
        subtitle.setText("M0.1 compositor + M1A guest Vulkan interception");
        subtitle.setTextSize(13f);

        SurfaceView surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(this);
        LinearLayout.LayoutParams surfaceParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1.0f
        );
        surfaceParams.topMargin = pad;
        surfaceParams.bottomMargin = pad;
        surfaceView.setLayoutParams(surfaceParams);

        status = new TextView(this);
        status.setText("Creating Vulkan output surface...");
        status.setTextSize(12f);
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

        Button render = new Button(this);
        render.setText("Present");
        render.setOnClickListener(v -> {
            if (!presenterReady) {
                status.setText("The Vulkan compositor is not ready.");
                return;
            }

            phase += 0.13f;
            if (phase >= 1.0f) {
                phase -= 1.0f;
            }

            try {
                status.setText(nativeDrawFrame(phase));
            } catch (Throwable t) {
                status.setText("Frame presentation failed: " + t);
            }
        });

        row1.addView(probe);
        row1.addView(render);

        Button hookTest = new Button(this);
        hookTest.setText("Run M1A Vulkan hook test");
        hookTest.setOnClickListener(v -> runHookTest());

        root.addView(title);
        root.addView(subtitle);
        root.addView(surfaceView);
        root.addView(status);
        root.addView(row1);
        root.addView(hookTest);

        setContentView(root);
    }

    private void runHookTest() {
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
            status.setText("M1A hook test failed: " + t);
        }
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        initializePresenter(holder);
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        initializePresenter(holder);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        presenterReady = false;
        nativeDestroyPresenter();
        status.setText("Output Surface destroyed.");
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
            status.setText("Vulkan compositor init failed: " + t);
        }
    }

    @Override
    protected void onDestroy() {
        presenterReady = false;
        nativeDestroyPresenter();
        super.onDestroy();
    }
}
