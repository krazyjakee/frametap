package com.daccord.frametap.gui;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import com.daccord.frametap.FrametapProjection;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

public class FrametapGuiActivity extends Activity {

    static {
        System.loadLibrary("frametap");
    }

    private static native void nativeInit(Activity activity);

    private TextView textDisplayInfo;
    private TextView textStatus;
    private Button btnCapture;
    private Button btnSave;
    private ImageView imagePreview;

    private volatile boolean capturing = false;
    private Thread captureThread;
    private Bitmap lastBitmap;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        textDisplayInfo = findViewById(R.id.text_display_info);
        textStatus = findViewById(R.id.text_status);
        btnCapture = findViewById(R.id.btn_capture);
        btnSave = findViewById(R.id.btn_save);
        imagePreview = findViewById(R.id.image_preview);

        // Initialize the native frametap JNI bridge
        nativeInit(this);

        FrametapProjection fp = FrametapProjection.getInstance();
        int w = fp.getDisplayWidth();
        int h = fp.getDisplayHeight();
        textDisplayInfo.setText("Display: " + w + " x " + h);

        // Request notification permission early so it doesn't race with
        // the MediaProjection consent dialog when capture starts.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                    != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(
                        new String[]{Manifest.permission.POST_NOTIFICATIONS}, 2001);
            }
        }

        btnCapture.setOnClickListener(v -> toggleCapture());
        btnSave.setOnClickListener(v -> saveScreenshot());
    }

    private void toggleCapture() {
        if (!capturing) {
            startCapture();
        } else {
            stopCapture();
        }
    }

    private void startCapture() {
        textStatus.setText("Status: Requesting consent...");
        btnCapture.setEnabled(false);

        // Launch the consent dialog from the UI thread so it is not
        // blocked by Android 14+ background-activity restrictions.
        FrametapProjection fp = FrametapProjection.getInstance();
        fp.requestConsent();

        // Poll for projection readiness on a background thread.
        new Thread(() -> {
            // Wait for projection to become active (consent is async)
            for (int i = 0; i < 100; i++) {
                if (fp.isActive()) break;
                try { Thread.sleep(100); } catch (InterruptedException e) { break; }
            }

            if (!fp.isActive()) {
                runOnUiThread(() -> {
                    textStatus.setText("Status: Consent denied or timed out");
                    btnCapture.setEnabled(true);
                });
                return;
            }

            capturing = true;
            runOnUiThread(() -> {
                textStatus.setText("Status: Capturing");
                btnCapture.setText("Stop Capture");
                btnCapture.setEnabled(true);
                btnSave.setEnabled(true);
            });

            startPreviewLoop();
        }).start();
    }

    private void stopCapture() {
        capturing = false;
        if (captureThread != null) {
            captureThread.interrupt();
            captureThread = null;
        }

        FrametapProjection.getInstance().stopProjection();

        textStatus.setText("Status: Idle");
        btnCapture.setText("Start Capture");
        btnSave.setEnabled(false);
    }

    private void startPreviewLoop() {
        captureThread = new Thread(() -> {
            FrametapProjection fp = FrametapProjection.getInstance();
            int w = fp.getDisplayWidth();
            int h = fp.getDisplayHeight();

            while (capturing && !Thread.currentThread().isInterrupted()) {
                byte[] rgba = fp.captureFrame();
                if (rgba != null && rgba.length == w * h * 4) {
                    // Convert RGBA byte array to ARGB int array for Bitmap
                    int[] pixels = new int[w * h];
                    for (int i = 0; i < pixels.length; i++) {
                        int offset = i * 4;
                        int r = rgba[offset] & 0xFF;
                        int g = rgba[offset + 1] & 0xFF;
                        int b = rgba[offset + 2] & 0xFF;
                        int a = rgba[offset + 3] & 0xFF;
                        pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
                    }

                    Bitmap bmp = Bitmap.createBitmap(pixels, w, h,
                            Bitmap.Config.ARGB_8888);

                    runOnUiThread(() -> {
                        if (lastBitmap != null && lastBitmap != bmp) {
                            lastBitmap.recycle();
                        }
                        lastBitmap = bmp;
                        imagePreview.setImageBitmap(bmp);
                    });
                }

                try {
                    Thread.sleep(33); // ~30 FPS preview
                } catch (InterruptedException e) {
                    break;
                }
            }
        });
        captureThread.start();
    }

    private void saveScreenshot() {
        if (lastBitmap == null) {
            Toast.makeText(this, "No frame captured yet", Toast.LENGTH_SHORT).show();
            return;
        }

        String timestamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US)
                .format(new Date());
        File dir = Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_PICTURES);
        if (!dir.exists()) dir.mkdirs();
        File file = new File(dir, "frametap_" + timestamp + ".png");

        try (FileOutputStream out = new FileOutputStream(file)) {
            lastBitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
            Toast.makeText(this, "Saved: " + file.getName(), Toast.LENGTH_SHORT).show();
        } catch (IOException e) {
            Toast.makeText(this, "Save failed: " + e.getMessage(),
                    Toast.LENGTH_SHORT).show();
        }
    }

    @Override
    protected void onDestroy() {
        stopCapture();
        if (lastBitmap != null) {
            lastBitmap.recycle();
            lastBitmap = null;
        }
        super.onDestroy();
    }
}
