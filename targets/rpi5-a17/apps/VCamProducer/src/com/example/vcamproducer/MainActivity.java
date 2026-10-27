package com.example.vcamproducer;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** User-visible entry point for the synthetic virtual-camera demo. */
public final class MainActivity extends Activity {
    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int padding = (int) (24 * getResources().getDisplayMetrics().density);
        root.setPadding(padding, padding, padding, padding);
        TextView status = new TextView(this);
        status.setText("Start the animated test pattern, then open VCam Viewer. No physical camera is used.");
        status.setTextSize(20);
        root.addView(status);
        Button start = new Button(this);
        start.setText("Start producer");
        start.setOnClickListener(view -> {
            try {
                startForegroundService(new Intent(this, VCamProducerService.class));
                status.setText("Starting producer. Open VCam Viewer to watch the test pattern.");
            } catch (RuntimeException error) {
                status.setText("Could not start producer: " + error.getMessage());
            }
        });
        root.addView(start);
        Button viewer = new Button(this);
        viewer.setText("Open viewer");
        viewer.setOnClickListener(view -> {
            Intent intent = getPackageManager().getLaunchIntentForPackage("com.example.vcamviewer");
            if (intent != null) startActivity(intent);
            else status.setText("VCam Viewer is not installed.");
        });
        root.addView(viewer);
        Button split = new Button(this);
        split.setText("Split-screen clock comparison");
        split.setOnClickListener(view -> startActivity(new Intent(this, SplitDemoActivity.class)));
        root.addView(split);
        Button split120 = new Button(this);
        split120.setText("120 fps split-screen test");
        split120.setOnClickListener(view -> startActivity(
                new Intent(this, SplitDemoActivity.class).putExtra("fps", 120)));
        root.addView(split120);
        Button split1080 = new Button(this);
        split1080.setText("1080p / 120 fps split-screen test");
        split1080.setOnClickListener(view -> startActivity(new Intent(this, SplitDemoActivity.class)
                .putExtra("fps", 120).putExtra("width", 1920)));
        root.addView(split1080);
        Button split4k = new Button(this);
        split4k.setText("4K UHD / 30 fps target split-screen test");
        split4k.setOnClickListener(view -> startActivity(new Intent(this, SplitDemoActivity.class)
                .putExtra("fps", 30).putExtra("width", 3840)));
        root.addView(split4k);
        Button stop = new Button(this);
        stop.setText("Stop producer");
        stop.setOnClickListener(view -> {
            stopService(new Intent(this, VCamProducerService.class));
            status.setText("Producer stopped.");
        });
        root.addView(stop);
        setContentView(root);
    }
}
