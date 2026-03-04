package com.daccord.frametap;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Transparent activity that shows the MediaProjection consent dialog.
 *
 * This activity is invisible — it only exists to call startActivityForResult()
 * for the screen capture permission dialog, since that API requires an Activity.
 *
 * Must be declared in AndroidManifest with:
 *   - theme: @android:style/Theme.Translucent.NoTitleBar
 *   - excludeFromRecents: true
 *   - launchMode: singleInstance
 */
public class FrametapConsentActivity extends Activity {

    private static final int REQUEST_MEDIA_PROJECTION = 1001;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        FrametapProjection fp = FrametapProjection.getInstance();
        Intent captureIntent = fp.getScreenCaptureIntent();
        if (captureIntent != null) {
            startActivityForResult(captureIntent, REQUEST_MEDIA_PROJECTION);
        } else {
            fp.onConsentDenied();
            finish();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);

        FrametapProjection fp = FrametapProjection.getInstance();
        if (requestCode == REQUEST_MEDIA_PROJECTION && resultCode == RESULT_OK && data != null) {
            fp.onConsentGranted(resultCode, data);
        } else {
            fp.onConsentDenied();
        }

        finish();
    }
}
