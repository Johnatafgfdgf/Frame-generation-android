package dev.framegen.android;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

public final class MainActivity extends Activity {
    static {
        System.loadLibrary("framegen_native");
    }

    private static native String nativeProbeVulkan();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        int pad = (int) (20 * getResources().getDisplayMetrics().density);

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
        title.setTextSize(24f);

        TextView subtitle = new TextView(this);
        subtitle.setText("M0 - Vulkan native probe");
        subtitle.setTextSize(15f);

        TextView status = new TextView(this);
        status.setText("Press the button to probe the device Vulkan driver.");
        status.setTextSize(14f);
        status.setPadding(0, pad, 0, pad);

        Button probe = new Button(this);
        probe.setText("Probe Vulkan");
        probe.setOnClickListener(v -> {
            try {
                status.setText(nativeProbeVulkan());
            } catch (Throwable t) {
                status.setText("Native probe failed: " + t);
            }
        });

        root.addView(title);
        root.addView(subtitle);
        root.addView(status);
        root.addView(probe);

        setContentView(root);
    }
}
