package com.daccord.frametap;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Foreground service required for MediaProjection on Android 10+ (API 29).
 *
 * The system requires an active foreground service with
 * FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION before MediaProjection.createVirtualDisplay()
 * can be called from a background context.
 *
 * Uses IMPORTANCE_LOW for a silent notification.
 */
public class FrametapCaptureService extends Service {

    private static final String CHANNEL_ID = "frametap_capture";
    private static final int NOTIFICATION_ID = 1;

    @Override
    public void onCreate() {
        super.onCreate();
        createNotificationChannel();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Notification notification = buildNotification();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }

        // Start the actual projection now that the foreground service is active
        FrametapProjection.getInstance().startProjectionFromService();

        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        stopForeground(STOP_FOREGROUND_REMOVE);
    }

    private void createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID,
                    "Screen Capture",
                    NotificationManager.IMPORTANCE_LOW);
            channel.setDescription("Active while capturing the screen");

            NotificationManager nm = getSystemService(NotificationManager.class);
            if (nm != null) {
                nm.createNotificationChannel(channel);
            }
        }
    }

    private Notification buildNotification() {
        Notification.Builder builder;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            builder = new Notification.Builder(this, CHANNEL_ID);
        } else {
            builder = new Notification.Builder(this);
        }

        return builder
                .setContentTitle("Screen Capture")
                .setContentText("frametap is capturing the screen")
                .setSmallIcon(android.R.drawable.ic_menu_camera)
                .build();
    }
}
