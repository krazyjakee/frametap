package com.daccord.frametap;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.ImageFormat;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.util.DisplayMetrics;
import android.view.WindowManager;

import java.nio.ByteBuffer;

/**
 * Singleton managing the MediaProjection lifecycle for frametap.
 *
 * Called from C++ via JNI. Manages consent flow, VirtualDisplay,
 * ImageReader, and frame capture.
 */
public final class FrametapProjection {

    private static FrametapProjection sInstance;

    private Activity activity;
    private MediaProjectionManager projectionManager;
    private MediaProjection projection;
    private VirtualDisplay virtualDisplay;
    private ImageReader imageReader;

    private int displayWidth;
    private int displayHeight;
    private int displayDensity;

    // Stored consent result for deferred projection start (after service starts)
    private int consentResultCode;
    private Intent consentData;

    private FrametapProjection() {}

    public static synchronized FrametapProjection getInstance() {
        if (sInstance == null) {
            sInstance = new FrametapProjection();
        }
        return sInstance;
    }

    /**
     * Initialize with the host Activity. Must be called before any other method.
     */
    public void init(Activity activity) {
        this.activity = activity;
        this.projectionManager = (MediaProjectionManager)
                activity.getSystemService(Context.MEDIA_PROJECTION_SERVICE);

        // Query display metrics
        DisplayMetrics metrics = new DisplayMetrics();
        WindowManager wm = (WindowManager) activity.getSystemService(Context.WINDOW_SERVICE);
        wm.getDefaultDisplay().getRealMetrics(metrics);
        displayWidth = metrics.widthPixels;
        displayHeight = metrics.heightPixels;
        displayDensity = metrics.densityDpi;
    }

    /**
     * Launch the consent activity to request screen capture permission.
     * The result arrives via onConsentGranted/onConsentDenied.
     */
    public void requestConsent() {
        Intent intent = new Intent(activity, FrametapConsentActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        activity.startActivity(intent);
    }

    /**
     * Called by FrametapConsentActivity when the user grants permission.
     */
    public void onConsentGranted(int resultCode, Intent data) {
        consentResultCode = resultCode;
        consentData = data;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // API 29+: must start projection from a foreground service
            Intent serviceIntent = new Intent(activity, FrametapCaptureService.class);
            activity.startForegroundService(serviceIntent);
        } else {
            startProjectionDirect();
        }

        // Notify C++ that consent was granted
        nativeOnConsentResult(true);
    }

    /**
     * Called by FrametapConsentActivity when the user denies permission.
     */
    public void onConsentDenied() {
        nativeOnConsentResult(false);
    }

    /**
     * Create the MediaProjection directly (pre-API 29 path, or called from service).
     */
    public void startProjectionDirect() {
        if (projectionManager == null || consentData == null) return;

        projection = projectionManager.getMediaProjection(consentResultCode, consentData);
        if (projection == null) return;

        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                cleanupDisplay();
            }
        }, null);

        createVirtualDisplay();
    }

    /**
     * Called from FrametapCaptureService after startForeground().
     */
    public void startProjectionFromService() {
        startProjectionDirect();
    }

    private void createVirtualDisplay() {
        if (projection == null) return;

        imageReader = ImageReader.newInstance(
                displayWidth, displayHeight, PixelFormat.RGBA_8888, 2);

        virtualDisplay = projection.createVirtualDisplay(
                "frametap",
                displayWidth, displayHeight, displayDensity,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                imageReader.getSurface(),
                null, null);
    }

    /**
     * Capture the latest frame as an RGBA byte array.
     * Returns null if no frame is available yet.
     */
    public byte[] captureFrame() {
        if (imageReader == null) return null;

        Image image = imageReader.acquireLatestImage();
        if (image == null) return null;

        try {
            Image.Plane plane = image.getPlanes()[0];
            ByteBuffer buffer = plane.getBuffer();
            int rowStride = plane.getRowStride();
            int pixelStride = plane.getPixelStride();
            int width = image.getWidth();
            int height = image.getHeight();

            byte[] rgba = new byte[width * height * 4];

            if (rowStride == width * pixelStride) {
                // No padding — direct copy
                buffer.get(rgba);
            } else {
                // Row stride has padding — copy row by row
                int rowLength = width * pixelStride;
                byte[] rowData = new byte[rowStride];
                for (int y = 0; y < height; y++) {
                    int remaining = buffer.remaining();
                    int toRead = Math.min(rowStride, remaining);
                    buffer.get(rowData, 0, toRead);
                    System.arraycopy(rowData, 0, rgba, y * rowLength, rowLength);
                }
            }

            return rgba;
        } finally {
            image.close();
        }
    }

    public boolean isActive() {
        return projection != null && virtualDisplay != null;
    }

    public int getDisplayWidth() {
        return displayWidth;
    }

    public int getDisplayHeight() {
        return displayHeight;
    }

    public void stopProjection() {
        cleanupDisplay();
        if (projection != null) {
            projection.stop();
            projection = null;
        }
        consentData = null;

        // Stop foreground service if running
        if (activity != null) {
            Intent serviceIntent = new Intent(activity, FrametapCaptureService.class);
            activity.stopService(serviceIntent);
        }
    }

    private void cleanupDisplay() {
        if (virtualDisplay != null) {
            virtualDisplay.release();
            virtualDisplay = null;
        }
        if (imageReader != null) {
            imageReader.close();
            imageReader = null;
        }
    }

    /**
     * Get the MediaProjectionManager's screen capture intent for consent.
     */
    public Intent getScreenCaptureIntent() {
        if (projectionManager == null) return null;
        return projectionManager.createScreenCaptureIntent();
    }

    // Native callback — implemented in jni_bridge.cpp
    private static native void nativeOnConsentResult(boolean granted);
}
