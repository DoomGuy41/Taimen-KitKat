package org.taimen.diag;

import android.app.Activity;
import android.graphics.Color;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * On-screen 3D test: grid of rotating objects (ES 3 when available, ES 2 otherwise), frames per
 * second counter. Touching the screen doubles the number of objects (back to 16 after 4096).
 */
public class GlTestActivity extends Activity {
    private GLSurfaceView view;
    private TextView info;
    private volatile int objects = 16;
    private volatile String status = "";

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        FrameLayout root = new FrameLayout(this);
        view = new GLSurfaceView(this);
        int es = getIntent().getIntExtra("es", 3);
        view.setEGLContextClientVersion(es);
        view.setRenderer(new R(es));
        root.addView(view);
        info = new TextView(this);
        info.setTextColor(Color.WHITE);
        info.setBackgroundColor(0x80000000);
        info.setTypeface(android.graphics.Typeface.MONOSPACE);
        info.setPadding(16, 16, 16, 16);
        root.addView(info, new FrameLayout.LayoutParams(-1, -2));
        setContentView(root);
        view.setOnTouchListener(new View.OnTouchListener() {
            public boolean onTouch(View v, MotionEvent e) {
                if (e.getAction() == MotionEvent.ACTION_DOWN) objects = objects >= 4096 ? 16 : objects * 2;
                return true;
            }
        });
        tick();
    }

    private void tick() {
        info.setText(status + "\ntouch the screen: x2 objects");
        info.postDelayed(new Runnable() { public void run() { if (!isFinishing()) tick(); } }, 500);
    }

    @Override protected void onPause() { super.onPause(); view.onPause(); }
    @Override protected void onResume() { super.onResume(); view.onResume(); }

    private class R implements GLSurfaceView.Renderer {
        private final int es;
        private int prog, uMvp, uColor;
        private FloatBuffer cube;
        private long t0, frames, lastFps;
        private float fps;
        private String renderer = "", version = "";
        private int w = 1, h = 1;

        R(int es) { this.es = es; }

        public void onSurfaceCreated(GL10 gl, EGLConfig c) {
            renderer = GLES20.glGetString(GLES20.GL_RENDERER);
            version = GLES20.glGetString(GLES20.GL_VERSION);
            String vs, fs;
            if (es >= 3) {
                vs = "#version 300 es\nuniform mat4 m;in vec3 p;out vec3 n;void main(){n=p*0.5+0.5;gl_Position=m*vec4(p,1.0);}";
                fs = "#version 300 es\nprecision mediump float;uniform vec4 c;in vec3 n;out vec4 o;void main(){o=vec4(c.rgb*n,1.0);}";
            } else {
                vs = "uniform mat4 m;attribute vec3 p;varying vec3 n;void main(){n=p*0.5+0.5;gl_Position=m*vec4(p,1.0);}";
                fs = "precision mediump float;uniform vec4 c;varying vec3 n;void main(){gl_FragColor=vec4(c.rgb*n,1.0);}";
            }
            prog = GLES20.glCreateProgram();
            GLES20.glAttachShader(prog, sh(GLES20.GL_VERTEX_SHADER, vs));
            GLES20.glAttachShader(prog, sh(GLES20.GL_FRAGMENT_SHADER, fs));
            GLES20.glBindAttribLocation(prog, 0, "p");
            GLES20.glLinkProgram(prog);
            uMvp = GLES20.glGetUniformLocation(prog, "m");
            uColor = GLES20.glGetUniformLocation(prog, "c");
            float[] v = new float[36 * 3];
            int[][] faces = { {0,1,2,3}, {4,5,6,7}, {0,1,5,4}, {2,3,7,6}, {0,3,7,4}, {1,2,6,5} };
            float[][] p = { {-1,-1,-1}, {1,-1,-1}, {1,1,-1}, {-1,1,-1}, {-1,-1,1}, {1,-1,1}, {1,1,1}, {-1,1,1} };
            int k = 0;
            for (int[] f : faces) {
                int[] idx = { f[0], f[1], f[2], f[0], f[2], f[3] };
                for (int i : idx) { v[k++] = p[i][0]; v[k++] = p[i][1]; v[k++] = p[i][2]; }
            }
            cube = ByteBuffer.allocateDirect(v.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
            cube.put(v).position(0);
            GLES20.glEnable(GLES20.GL_DEPTH_TEST);
            t0 = System.nanoTime();
            lastFps = t0;
        }

        private int sh(int type, String src) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, src);
            GLES20.glCompileShader(s);
            return s;
        }

        public void onSurfaceChanged(GL10 gl, int w, int h) {
            this.w = w; this.h = h;
            GLES20.glViewport(0, 0, w, h);
        }

        public void onDrawFrame(GL10 gl) {
            long now = System.nanoTime();
            float t = (now - t0) / 1e9f;
            GLES20.glClearColor(0.05f, 0.05f, 0.1f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT | GLES20.GL_DEPTH_BUFFER_BIT);
            GLES20.glUseProgram(prog);
            GLES20.glVertexAttribPointer(0, 3, GLES20.GL_FLOAT, false, 0, cube);
            GLES20.glEnableVertexAttribArray(0);
            int n = objects;
            int side = (int) Math.ceil(Math.sqrt(n));
            float aspect = (float) w / h;
            float s = 0.6f / side;
            float[] m = new float[16];
            for (int i = 0; i < n; i++) {
                float x = -1 + (2f * (i % side) + 1) / side, y = -1 + (2f * (i / side) + 1) / side;
                float a = t * (1 + (i % 5) * 0.3f), c = (float) Math.cos(a), sn = (float) Math.sin(a);
                float c2 = (float) Math.cos(a * 0.7f), s2 = (float) Math.sin(a * 0.7f);
                // Y then X rotation, scale, translation; z brought into [-1,1]
                m[0] = s * c / aspect;       m[1] = s * sn * s2;  m[2] = -s * sn * c2 * 0.5f; m[3] = 0;
                m[4] = 0;                    m[5] = s * c2;       m[6] = s * s2 * 0.5f;       m[7] = 0;
                m[8] = s * sn / aspect;      m[9] = -s * c * s2;  m[10] = s * c * c2 * 0.5f;  m[11] = 0;
                m[12] = x;                   m[13] = y;           m[14] = 0;                  m[15] = 1;
                GLES20.glUniformMatrix4fv(uMvp, 1, false, m, 0);
                GLES20.glUniform4f(uColor, 0.4f + (i % 3) * 0.3f, 0.4f + (i % 4) * 0.2f, 1f - (i % 5) * 0.15f, 1f);
                GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 36);
            }
            frames++;
            if (now - lastFps > 1000000000L) {
                fps = frames * 1e9f / (now - lastFps);
                frames = 0;
                lastFps = now;
                int err = GLES20.glGetError();
                status = String.format("%.1f fps  %d objects (%d triangles, %d draws/frame)\n%s\n%s  %dx%d%s",
                        fps, n, n * 12, n, renderer, version, w, h, err != 0 ? "  GL error 0x" + Integer.toHexString(err) : "");
            }
        }
    }
}
