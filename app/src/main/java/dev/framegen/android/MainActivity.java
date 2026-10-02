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

    private TextView status;
    private SurfaceView surfaceView;
    private float phase = 0.0f;
    private boolean presenterReady = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = (int) (16 * getResources().getDisplayMetrics().density);

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
        title.setTextSize(23f);

        TextView subtitle = new TextView(this);
        subtitle.setText("M0.1 - host-owned Vulkan compositor");
        subtitle.setTextSize(14f);

        surfaceView = new SurfaceView(this);
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
        status.setTextSize(13f);
        status.setPadding(0, 0, 0, pad);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.setGravity(Gravity.CENTER);

        Button probe = new Button(this);
        probe.setText("Probe GPU");
        probe.setOnClickListener(v -> {
            try {
                status.setText(nativeProbeVulkan());
            } catch (Throwable t) {
                status.setText("Native probe failed: " + t);
            }
        });

        Button render = new Button(this);
        render.setText("Present frame");
        render.setOnClickListener(v -> {
            if (!presenterReady) {
                status.setText("The Vulkan compositor is not ready yet.");
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

        buttons.addView(probe);
        buttons.addView(render);

        root.addView(title);
        root.addView(subtitle);
        root.addView(surfaceView);
        root.addView(status);
        root.addView(buttons);

        setContentView(root);
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
