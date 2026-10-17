package org.taimen.diag;

import android.app.Activity;
import android.hardware.Camera;
import android.os.Bundle;
import android.os.Environment;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;

/** Camera preview in a SurfaceView (composed by SurfaceFlinger, no hwui), mean luminance of the
 *  preview frames, photo. Tells "no frames" apart from "frames not displayed". */
public class CameraPreviewActivity extends Activity implements SurfaceHolder.Callback {
    private Camera mCamera;
    private int mId;
    private SurfaceHolder mHolder;
    private TextView mInfo;
    private int mFrames;
    private long mStart;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        FrameLayout root = new FrameLayout(this);
        SurfaceView sv = new SurfaceView(this);
        sv.getHolder().addCallback(this);
        root.addView(sv);
        mInfo = new TextView(this);
        mInfo.setTextColor(0xffffff00);
        mInfo.setBackgroundColor(0x80000000);
        mInfo.setTextSize(14);
        root.addView(mInfo, new FrameLayout.LayoutParams(-1, -2, Gravity.TOP));
        LinearLayout bar = new LinearLayout(this);
        Button sw = new Button(this);
        sw.setText("Switch camera");
        sw.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) {
                close();
                mId = (mId + 1) % Math.max(1, Camera.getNumberOfCameras());
                open();
            }
        });
        Button shot = new Button(this);
        shot.setText("Photo");
        shot.setOnClickListener(new android.view.View.OnClickListener() {
            public void onClick(android.view.View v) { takePicture(); }
        });
        bar.addView(sw);
        bar.addView(shot);
        root.addView(bar, new FrameLayout.LayoutParams(-2, -2, Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL));
        setContentView(root);
    }

    @Override
    protected void onPause() {
        super.onPause();
        close();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (mHolder != null)
            open();
    }

    public void surfaceCreated(SurfaceHolder h) {
        mHolder = h;
        open();
    }

    public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}

    public void surfaceDestroyed(SurfaceHolder h) {
        close();
        mHolder = null;
    }

    private void open() {
        if (mCamera != null || mHolder == null)
            return;
        try {
            mCamera = Camera.open(mId);
            Camera.CameraInfo ci = new Camera.CameraInfo();
            Camera.getCameraInfo(mId, ci);
            mCamera.setDisplayOrientation(ci.facing == Camera.CameraInfo.CAMERA_FACING_FRONT
                    ? (360 - ci.orientation) % 360 : ci.orientation);
            mCamera.setPreviewDisplay(mHolder);
            mFrames = 0;
            mStart = System.currentTimeMillis();
            mCamera.setPreviewCallback(new Camera.PreviewCallback() {
              public void onPreviewFrame(byte[] data, Camera cam) {
                mFrames++;
                if (mFrames % 10 != 1)
                    return;
                Camera.Size s = cam.getParameters().getPreviewSize();
                long sum = 0;
                int n = s.width * s.height, step = 97;
                for (int i = 0; i < n; i += step)
                    sum += data[i] & 0xff;
                double fps = mFrames * 1000.0 / Math.max(1, System.currentTimeMillis() - mStart);
                mInfo.setText("camera " + mId + ": " + s.width + "x" + s.height + ", " + mFrames
                        + " frames (" + String.format("%.1f", fps) + " fps), mean luminance "
                        + (sum * step / n));
              }
            });
            mCamera.startPreview();
            mInfo.setText("camera " + mId + ": preview started, waiting for frames...");
        } catch (Exception e) {
            mInfo.setText("camera " + mId + ": " + e);
            close();
        }
    }

    private void close() {
        if (mCamera == null)
            return;
        mCamera.setPreviewCallback(null);
        mCamera.stopPreview();
        mCamera.release();
        mCamera = null;
    }

    private void takePicture() {
        if (mCamera == null)
            return;
        mCamera.setPreviewCallback(null);
        mCamera.takePicture(null, null, new Camera.PictureCallback() {
            public void onPictureTaken(byte[] data, Camera cam) {
                File f = new File(Environment.getExternalStorageDirectory(), "taimen-photo-" + mId + ".jpg");
                try {
                    FileOutputStream o = new FileOutputStream(f);
                    o.write(data);
                    o.close();
                    mInfo.setText("photo: " + f + " (" + data.length + " bytes)");
                } catch (Exception e) {
                    mInfo.setText("photo: " + e);
                }
                close();
                open();
            }
        });
    }
}
