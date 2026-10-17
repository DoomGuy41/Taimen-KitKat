package org.taimen.diag;

import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES10;
import android.opengl.GLES20;
import android.opengl.GLES30;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/**
 * Queries EGL and GLES offscreen (pbuffer): strings, extensions, limits, and small real tests
 * (clear, ES 2 and ES 3 shader compilation, drawing, pixel read-back, throughput).
 * Call it outside the UI thread.
 */
final class GpuInfo {
    static final int EGL_OPENGL_ES3_BIT = 0x40;

    /** Summarized results for the report. */
    static boolean es1Ok, es2Ok, es3Ok, renderOk, es3ShaderOk;
    static String renderer = "?";

    static String probe() {
        StringBuilder sb = new StringBuilder();
        EGLDisplay dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] ver = new int[2];
        if (!EGL14.eglInitialize(dpy, ver, 0, ver, 1)) {
            return "eglInitialize failed: 0x" + Integer.toHexString(EGL14.eglGetError());
        }
        sb.append("== EGL ").append(ver[0]).append('.').append(ver[1]).append('\n');
        sb.append("EGL_VENDOR      ").append(EGL14.eglQueryString(dpy, EGL14.EGL_VENDOR)).append('\n');
        sb.append("EGL_VERSION     ").append(EGL14.eglQueryString(dpy, EGL14.EGL_VERSION)).append('\n');
        sb.append("EGL_CLIENT_APIS ").append(EGL14.eglQueryString(dpy, EGL14.EGL_CLIENT_APIS)).append('\n');
        sb.append("EGL_EXTENSIONS  ").append(EGL14.eglQueryString(dpy, EGL14.EGL_EXTENSIONS)).append('\n');
        int[] n = new int[1];
        EGL14.eglGetConfigs(dpy, new EGLConfig[64], 0, 64, n, 0);  // KitKat: 1 local JNI ref per config (max 512)
        sb.append("configs EGL     ").append(n[0]).append('\n');

        es1Ok = context(sb, dpy, 1, EGL14.EGL_OPENGL_ES_BIT);
        es2Ok = context(sb, dpy, 2, EGL14.EGL_OPENGL_ES2_BIT);
        es3Ok = context(sb, dpy, 3, EGL_OPENGL_ES3_BIT);
        EGL14.eglTerminate(dpy);
        return sb.toString();
    }

    private static boolean context(StringBuilder sb, EGLDisplay dpy, int version, int renderable) {
        sb.append("\n== OpenGL ES context ").append(version).append(".x\n");
        int[] attribs = {
            EGL14.EGL_RENDERABLE_TYPE, renderable, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_DEPTH_SIZE, 16, EGL14.EGL_NONE };
        EGLConfig[] cfg = new EGLConfig[1];
        int[] num = new int[1];
        if (!EGL14.eglChooseConfig(dpy, attribs, 0, cfg, 0, 1, num, 0) || num[0] == 0) {
            sb.append("FAIL: no EGL config (0x").append(Integer.toHexString(EGL14.eglGetError())).append(")\n");
            return false;
        }
        EGLContext ctx = EGL14.eglCreateContext(dpy, cfg[0], EGL14.EGL_NO_CONTEXT,
                new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, version, EGL14.EGL_NONE }, 0);
        if (ctx == null || ctx == EGL14.EGL_NO_CONTEXT) {
            sb.append("FAIL: eglCreateContext 0x").append(Integer.toHexString(EGL14.eglGetError())).append('\n');
            return false;
        }
        EGLSurface surf = EGL14.eglCreatePbufferSurface(dpy, cfg[0],
                new int[] { EGL14.EGL_WIDTH, 256, EGL14.EGL_HEIGHT, 256, EGL14.EGL_NONE }, 0);
        if (!EGL14.eglMakeCurrent(dpy, surf, surf, ctx)) {
            sb.append("FAIL: eglMakeCurrent 0x").append(Integer.toHexString(EGL14.eglGetError())).append('\n');
            EGL14.eglDestroyContext(dpy, ctx);
            return false;
        }
        try {
            if (version == 1) {
                sb.append("GL_VENDOR       ").append(GLES10.glGetString(GLES10.GL_VENDOR)).append('\n');
                sb.append("GL_RENDERER     ").append(GLES10.glGetString(GLES10.GL_RENDERER)).append('\n');
                sb.append("GL_VERSION      ").append(GLES10.glGetString(GLES10.GL_VERSION)).append('\n');
                sb.append("GL_EXTENSIONS   ").append(GLES10.glGetString(GLES10.GL_EXTENSIONS)).append('\n');
                sb.append(clearTest(true));
            } else {
                String r = GLES20.glGetString(GLES20.GL_RENDERER);
                if (r != null) renderer = r;
                sb.append("GL_VENDOR       ").append(GLES20.glGetString(GLES20.GL_VENDOR)).append('\n');
                sb.append("GL_RENDERER     ").append(r).append('\n');
                sb.append("GL_VERSION      ").append(GLES20.glGetString(GLES20.GL_VERSION)).append('\n');
                sb.append("GLSL            ").append(GLES20.glGetString(GLES20.GL_SHADING_LANGUAGE_VERSION)).append('\n');
                String ext = GLES20.glGetString(GLES20.GL_EXTENSIONS);
                int count = ext == null ? 0 : ext.trim().split(" +").length;
                sb.append("extensions (").append(count).append("):\n");
                if (ext != null) for (String e : ext.trim().split(" +")) sb.append("   ").append(e).append('\n');
                sb.append(limits(version));
                sb.append(clearTest(false));
                sb.append(drawTest(version));
                if (version == 2) sb.append(throughput());
            }
            int err = GLES20.glGetError();
            sb.append("final glGetError: ").append(err == 0 ? "none" : "0x" + Integer.toHexString(err)).append('\n');
        } catch (Throwable t) {
            sb.append("exception: ").append(t).append('\n');
        }
        EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(dpy, surf);
        EGL14.eglDestroyContext(dpy, ctx);
        return true;
    }

    private static String limits(int version) {
        StringBuilder sb = new StringBuilder("limits:\n");
        int[][] l2 = {
            { GLES20.GL_MAX_TEXTURE_SIZE }, { GLES20.GL_MAX_CUBE_MAP_TEXTURE_SIZE }, { GLES20.GL_MAX_RENDERBUFFER_SIZE },
            { GLES20.GL_MAX_VERTEX_ATTRIBS }, { GLES20.GL_MAX_VERTEX_UNIFORM_VECTORS }, { GLES20.GL_MAX_VARYING_VECTORS },
            { GLES20.GL_MAX_FRAGMENT_UNIFORM_VECTORS }, { GLES20.GL_MAX_TEXTURE_IMAGE_UNITS },
            { GLES20.GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS }, { GLES20.GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS } };
        String[] n2 = { "MAX_TEXTURE_SIZE", "MAX_CUBE_MAP_TEXTURE_SIZE", "MAX_RENDERBUFFER_SIZE", "MAX_VERTEX_ATTRIBS",
            "MAX_VERTEX_UNIFORM_VECTORS", "MAX_VARYING_VECTORS", "MAX_FRAGMENT_UNIFORM_VECTORS",
            "MAX_TEXTURE_IMAGE_UNITS", "MAX_VERTEX_TEXTURE_IMAGE_UNITS", "MAX_COMBINED_TEXTURE_IMAGE_UNITS" };
        int[] v = new int[2];
        for (int i = 0; i < l2.length; i++) {
            GLES20.glGetIntegerv(l2[i][0], v, 0);
            sb.append("   ").append(Util.pad(n2[i], 34)).append(v[0]).append('\n');
        }
        GLES20.glGetIntegerv(GLES20.GL_MAX_VIEWPORT_DIMS, v, 0);
        sb.append("   ").append(Util.pad("MAX_VIEWPORT_DIMS", 34)).append(v[0]).append(" x ").append(v[1]).append('\n');
        if (version >= 3) {
            int[] l3 = { GLES30.GL_MAX_3D_TEXTURE_SIZE, GLES30.GL_MAX_ARRAY_TEXTURE_LAYERS, GLES30.GL_MAX_DRAW_BUFFERS,
                GLES30.GL_MAX_COLOR_ATTACHMENTS, GLES30.GL_MAX_SAMPLES, GLES30.GL_MAX_UNIFORM_BUFFER_BINDINGS,
                GLES30.GL_MAX_UNIFORM_BLOCK_SIZE, GLES30.GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS,
                GLES30.GL_MAX_ELEMENTS_VERTICES, GLES30.GL_MAX_ELEMENTS_INDICES, GLES30.GL_NUM_EXTENSIONS,
                GLES30.GL_MAJOR_VERSION, GLES30.GL_MINOR_VERSION };
            String[] n3 = { "MAX_3D_TEXTURE_SIZE", "MAX_ARRAY_TEXTURE_LAYERS", "MAX_DRAW_BUFFERS", "MAX_COLOR_ATTACHMENTS",
                "MAX_SAMPLES", "MAX_UNIFORM_BUFFER_BINDINGS", "MAX_UNIFORM_BLOCK_SIZE",
                "MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS", "MAX_ELEMENTS_VERTICES", "MAX_ELEMENTS_INDICES",
                "NUM_EXTENSIONS", "MAJOR_VERSION", "MINOR_VERSION" };
            for (int i = 0; i < l3.length; i++) {
                v[0] = -1;
                GLES20.glGetIntegerv(l3[i], v, 0);
                sb.append("   ").append(Util.pad(n3[i], 34)).append(v[0]).append('\n');
            }
        }
        return sb.toString();
    }

    /** Clears to pure red and reads the centre pixel back. */
    private static String clearTest(boolean es1) {
        ByteBuffer px = ByteBuffer.allocateDirect(4);
        if (es1) {
            GLES10.glClearColor(1f, 0f, 0f, 1f);
            GLES10.glClear(GLES10.GL_COLOR_BUFFER_BIT);
            GLES10.glReadPixels(128, 128, 1, 1, GLES10.GL_RGBA, GLES10.GL_UNSIGNED_BYTE, px);
        } else {
            GLES20.glClearColor(1f, 0f, 0f, 1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            GLES20.glReadPixels(128, 128, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
        }
        int r = px.get(0) & 0xff, g = px.get(1) & 0xff, b = px.get(2) & 0xff;
        boolean ok = r > 250 && g < 5 && b < 5;
        return "red clear test: " + (ok ? "OK" : "FAIL") + " (read " + r + "," + g + "," + b + ")\n";
    }

    private static int shader(int type, String src, StringBuilder log) {
        int s = GLES20.glCreateShader(type);
        GLES20.glShaderSource(s, src);
        GLES20.glCompileShader(s);
        int[] st = new int[1];
        GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, st, 0);
        if (st[0] == 0) log.append("compilation FAILED: ").append(GLES20.glGetShaderInfoLog(s)).append('\n');
        return s;
    }

    /** Compiles a program (GLSL ES 1.00, or 3.00 with ES 3), draws a full-screen green triangle, reads back. */
    private static String drawTest(int version) {
        StringBuilder sb = new StringBuilder();
        String vs, fs;
        if (version >= 3) {
            vs = "#version 300 es\nin vec2 p;\nvoid main(){gl_Position=vec4(p,0.0,1.0);}\n";
            fs = "#version 300 es\nprecision mediump float;\nuniform vec4 c;\nout vec4 o;\nvoid main(){o=c;}\n";
        } else {
            vs = "attribute vec2 p;\nvoid main(){gl_Position=vec4(p,0.0,1.0);}\n";
            fs = "precision mediump float;\nuniform vec4 c;\nvoid main(){gl_FragColor=c;}\n";
        }
        int prog = GLES20.glCreateProgram();
        GLES20.glAttachShader(prog, shader(GLES20.GL_VERTEX_SHADER, vs, sb));
        GLES20.glAttachShader(prog, shader(GLES20.GL_FRAGMENT_SHADER, fs, sb));
        GLES20.glBindAttribLocation(prog, 0, "p");
        GLES20.glLinkProgram(prog);
        int[] st = new int[1];
        GLES20.glGetProgramiv(prog, GLES20.GL_LINK_STATUS, st, 0);
        if (st[0] == 0) {
            sb.append("link FAILED: ").append(GLES20.glGetProgramInfoLog(prog)).append('\n');
            return "GLSL shader test " + (version >= 3 ? "3.00" : "1.00") + ": FAIL\n" + sb;
        }
        FloatBuffer tri = ByteBuffer.allocateDirect(6 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        tri.put(new float[] { -1f, -1f, 3f, -1f, -1f, 3f }).position(0);
        GLES20.glViewport(0, 0, 256, 256);
        GLES20.glUseProgram(prog);
        GLES20.glUniform4f(GLES20.glGetUniformLocation(prog, "c"), 0f, 1f, 0f, 1f);
        GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, tri);
        GLES20.glEnableVertexAttribArray(0);
        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
        ByteBuffer px = ByteBuffer.allocateDirect(4);
        GLES20.glReadPixels(128, 128, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, px);
        int r = px.get(0) & 0xff, g = px.get(1) & 0xff, b = px.get(2) & 0xff;
        boolean ok = r < 5 && g > 250 && b < 5;
        if (version >= 3) es3ShaderOk = ok; else renderOk = ok;
        GLES20.glDisableVertexAttribArray(0);
        GLES20.glDeleteProgram(prog);
        return "GLSL shader test " + (version >= 3 ? "3.00" : "1.00") + " + draw: " + (ok ? "OK" : "FAIL")
                + " (read " + r + "," + g + "," + b + ")\n" + sb;
    }

    /** Rough throughput: 300 blended full-screen triangles at 256x256, then glFinish. */
    private static String throughput() {
        String vs = "attribute vec2 p;\nvoid main(){gl_Position=vec4(p,0.0,1.0);}\n";
        String fs = "precision mediump float;\nuniform vec4 c;\nvoid main(){gl_FragColor=c;}\n";
        StringBuilder log = new StringBuilder();
        int prog = GLES20.glCreateProgram();
        GLES20.glAttachShader(prog, shader(GLES20.GL_VERTEX_SHADER, vs, log));
        GLES20.glAttachShader(prog, shader(GLES20.GL_FRAGMENT_SHADER, fs, log));
        GLES20.glBindAttribLocation(prog, 0, "p");
        GLES20.glLinkProgram(prog);
        GLES20.glUseProgram(prog);
        int uc = GLES20.glGetUniformLocation(prog, "c");
        FloatBuffer tri = ByteBuffer.allocateDirect(6 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
        tri.put(new float[] { -1f, -1f, 3f, -1f, -1f, 3f }).position(0);
        GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, tri);
        GLES20.glEnableVertexAttribArray(0);
        GLES20.glEnable(GLES20.GL_BLEND);
        GLES20.glBlendFunc(GLES20.GL_SRC_ALPHA, GLES20.GL_ONE_MINUS_SRC_ALPHA);
        GLES20.glFinish();
        long t0 = System.nanoTime();
        final int calls = 300;
        for (int i = 0; i < calls; i++) {
            GLES20.glUniform4f(uc, (i % 7) / 7f, (i % 5) / 5f, (i % 3) / 3f, 0.5f);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
        }
        GLES20.glFinish();
        double ms = (System.nanoTime() - t0) / 1e6;
        GLES20.glDisable(GLES20.GL_BLEND);
        GLES20.glDeleteProgram(prog);
        return String.format("throughput: %d draws + glFinish in %.1f ms (%.1f us/draw)\n",
                calls, ms, ms * 1000 / calls);
    }
}
