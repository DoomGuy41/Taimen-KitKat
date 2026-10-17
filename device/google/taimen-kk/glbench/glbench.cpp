/*
 * Standalone on-screen GPU benchmark, no interaction needed (adb shell /system/bin/glbench).
 *
 *  1. display: a 4-colour pattern drawn in a SurfaceFlinger window, then read back from what is
 *     actually shown (SurfaceFlinger screenshot). Cases: full-screen RGBA8888 window, and
 *     smaller RGB565 buffers scaled up by SurfaceFlinger (like a fixed-size SurfaceView).
 *  2. 3D scene: instanced, lit and textured cubes, full screen; end-to-end frames/s
 *     (app -> SurfaceFlinger -> display) and time per frame.
 *  3. 2D: textured sprites with blending, like a 2D game or a busy UI.
 */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayInfo.h>

using namespace android;

static int s_pass, s_fail;
static EGLDisplay s_dpy;
static EGLConfig s_cfg565, s_cfg8888;
static int s_dispW, s_dispH;

static double now()
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void result(const char *name, bool ok, const char *detail)
{
    printf("%-40s %s  %s\n", name, ok ? "OK" : "FAIL", detail ? detail : "");
    fflush(stdout);
    (ok ? s_pass : s_fail)++;
}

// ------------------------------------------------------------------ SurfaceFlinger window
struct Window {
    sp<SurfaceComposerClient> client;
    sp<SurfaceControl> ctrl;
    sp<Surface> surface;
    EGLSurface egl;
    EGLContext ctx;
};

static bool openWindow(Window &w, int bufW, int bufH, int format, EGLConfig cfg)
{
    w.client = new SurfaceComposerClient();
    // the layer covers the whole screen; the buffer may be smaller (scaled up by SurfaceFlinger)
    w.ctrl = w.client->createSurface(String8("glbench"), s_dispW, s_dispH, format, 0);
    if (w.ctrl == NULL || !w.ctrl->isValid())
        return false;
    SurfaceComposerClient::openGlobalTransaction();
    w.ctrl->setLayer(0x7ffffff0);
    w.ctrl->setPosition(0, 0);
    w.ctrl->show();
    SurfaceComposerClient::closeGlobalTransaction();
    w.surface = w.ctrl->getSurface();
    ANativeWindow *win = w.surface.get();
    if (bufW != s_dispW || bufH != s_dispH) {
        native_window_set_buffers_dimensions(win, bufW, bufH);
        native_window_set_scaling_mode(win, NATIVE_WINDOW_SCALING_MODE_SCALE_TO_WINDOW);
    }
    w.egl = eglCreateWindowSurface(s_dpy, cfg, win, NULL);
    EGLint attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    w.ctx = eglCreateContext(s_dpy, cfg, EGL_NO_CONTEXT, attr);
    return w.egl != EGL_NO_SURFACE && w.ctx != EGL_NO_CONTEXT &&
           eglMakeCurrent(s_dpy, w.egl, w.egl, w.ctx);
}

static void closeWindow(Window &w)
{
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(s_dpy, w.egl);
    eglDestroyContext(s_dpy, w.ctx);
    w.surface.clear();
    w.ctrl.clear();
    w.client->dispose();
    w.client.clear();
}

// ------------------------------------------------------------------ reading the screen back
// returns the colour actually displayed (screen coordinates)
// The displayed image is read back through a SurfaceFlinger screenshot: since zero-copy
// presentation (MDSS atomic commit by fbpresentd), fb0 is no longer fed.
static ScreenshotClient *s_shot;

static bool captureScreen()
{
    if (!s_shot)
        s_shot = new ScreenshotClient();
    sp<IBinder> display = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
    return s_shot->update(display) == NO_ERROR;
}

static bool screenPixel(int x, int y, int rgb[3])
{
    if (!s_shot || !s_shot->getPixels() || x >= (int)s_shot->getWidth() || y >= (int)s_shot->getHeight())
        return false;
    const unsigned char *px = (const unsigned char *)s_shot->getPixels() +
                              ((size_t)y * s_shot->getStride() + x) * 4;
    rgb[0] = px[0]; rgb[1] = px[1]; rgb[2] = px[2];
    return true;
}

// pattern: 4 quadrants (screen: top-left red, top-right green, bottom-left blue, bottom-right white)
static void drawPattern(int w, int h)
{
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    struct { int x, y; float r, g, b; } q[4] = {
        { 0, h / 2, 1, 0, 0 }, { w / 2, h / 2, 0, 1, 0 }, { 0, 0, 0, 0, 1 }, { w / 2, 0, 1, 1, 1 } };
    for (int i = 0; i < 4; i++) {
        glScissor(q[i].x, q[i].y, w / 2, h / 2);
        glClearColor(q[i].r, q[i].g, q[i].b, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glDisable(GL_SCISSOR_TEST);
}

// expected colours on screen per quadrant: top-left, top-right, bottom-left, bottom-right
static const int kRed[3] = { 255, 0, 0 }, kGreen[3] = { 0, 255, 0 }, kBlue[3] = { 0, 0, 255 },
                 kWhite[3] = { 255, 255, 255 };

static void checkDisplay(const char *name, int bufW, int bufH, int format, EGLConfig cfg,
                         int transform = 0, const int *tl = kRed, const int *tr = kGreen,
                         const int *bl = kBlue, const int *br = kWhite)
{
    Window w;
    if (!openWindow(w, bufW, bufH, format, cfg)) {
        result(name, false, "cannot create window");
        return;
    }
    if (transform)
        native_window_set_buffers_transform(w.surface.get(), transform);
    for (int i = 0; i < 4; i++) {        // several frames: buffer queue full, composition done
        drawPattern(bufW, bufH);
        eglSwapBuffers(s_dpy, w.egl);
    }
    usleep(500000);
    // centre of each quadrant, and 4 points near the edges (tiling would change the colour there)
    struct { int x, y; const int *c; } p[] = {
        { s_dispW / 4, s_dispH / 4, tl },     { 3 * s_dispW / 4, s_dispH / 4, tr },
        { s_dispW / 4, 3 * s_dispH / 4, bl }, { 3 * s_dispW / 4, 3 * s_dispH / 4, br },
        { 20, 20, tl },                       { s_dispW - 20, 20, tr },
        { 20, s_dispH - 20, bl },             { s_dispW - 20, s_dispH - 20, br },
        { s_dispW / 2 - 30, s_dispH / 2 - 30, tl },
    };
    // composition may take a little while (first window, wake-up): up to 2 s
    char detail[256] = "";
    bool ok = false;
    for (int attempt = 0; attempt < 10 && !ok; attempt++) {
        if (attempt) {
            drawPattern(bufW, bufH);
            eglSwapBuffers(s_dpy, w.egl);
            usleep(200000);
        }
        detail[0] = 0;
        ok = captureScreen();
        for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
            int c[3];
            if (!screenPixel(p[i].x, p[i].y, c) ||
                abs(c[0] - p[i].c[0]) > 8 || abs(c[1] - p[i].c[1]) > 8 || abs(c[2] - p[i].c[2]) > 8) {
                ok = false;
                snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail), "(%d,%d)=%d,%d,%d ",
                         p[i].x, p[i].y, c[0], c[1], c[2]);
            }
        }
    }
    result(name, ok, detail);
    closeWindow(w);
}

// ------------------------------------------------------------------ 3D scene
static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("  shader : %s\n", log);
    }
    return s;
}

static GLuint linkProgram(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    return p;
}

static GLuint checkerTexture()
{
    static unsigned char img[256 * 256 * 4];
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            unsigned char v = ((x / 32) ^ (y / 32)) & 1 ? 230 : 60;
            unsigned char *p = img + (y * 256 + x) * 4;
            p[0] = v; p[1] = (unsigned char)(v * 0.8f); p[2] = (unsigned char)(255 - v / 2); p[3] = 255;
        }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return t;
}

static void bench3D(int instances, double seconds, bool separate = false)
{
    Window w;
    if (!openWindow(w, s_dispW, s_dispH, PIXEL_FORMAT_RGBA_8888, s_cfg8888)) {
        result("3D scene", false, "cannot create window");
        return;
    }
    // cube : 36 sommets (position, normale, uv)
    static const float F[6][4][3] = {
        {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}}, {{ 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},
        {{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1},{-1, 1,-1}}, {{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1}},
        {{ 1,-1, 1},{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1}}, {{-1,-1,-1},{-1,-1, 1},{-1, 1, 1},{-1, 1,-1}}};
    static const float N[6][3] = {{0,0,1},{0,0,-1},{0,1,0},{0,-1,0},{1,0,0},{-1,0,0}};
    static const float UV[4][2] = {{0,0},{1,0},{1,1},{0,1}};
    static const int idx[6] = {0, 1, 2, 0, 2, 3};
    float verts[36 * 8];
    int k = 0;
    for (int f = 0; f < 6; f++)
        for (int i = 0; i < 6; i++) {
            const float *v = F[f][idx[i]];
            float e[8] = { v[0], v[1], v[2], N[f][0], N[f][1], N[f][2], UV[idx[i]][0], UV[idx[i]][1] };
            memcpy(verts + k * 8, e, sizeof(e));
            k++;
        }
    // instance positions on a 3D grid
    float *inst = (float *)malloc(instances * 4 * sizeof(float));
    int side = (int)ceil(cbrt((double)instances));
    for (int i = 0; i < instances; i++) {
        inst[i * 4 + 0] = (i % side - side / 2.0f) * 3.0f;
        inst[i * 4 + 1] = ((i / side) % side - side / 2.0f) * 3.0f;
        inst[i * 4 + 2] = (i / (side * side) - side / 2.0f) * 3.0f;
        inst[i * 4 + 3] = (float)(i % 7);
    }
    GLuint vao, vbo, ibo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 32, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 32, (void *)12);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void *)24);
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ARRAY_BUFFER, ibo);
    glBufferData(GL_ARRAY_BUFFER, instances * 16, inst, GL_STATIC_DRAW);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glVertexAttribDivisor(3, 1);
    free(inst);

    GLuint prog = linkProgram(
        "#version 300 es\n"
        "layout(location=0) in vec3 pos; layout(location=1) in vec3 nrm; layout(location=2) in vec2 uv;\n"
        "layout(location=3) in vec4 off;\n"
        "uniform float t; uniform float aspect; uniform float base;\n"
        "out vec3 vn; out vec2 vuv; flat out float vk;\n"
        "mat3 rot(float a){ float c=cos(a), s=sin(a); return mat3(c,0.,-s, 0.,1.,0., s,0.,c); }\n"
        "void main(){\n"
        "  vec4 o4 = off; if (base > 0.0) { float b = base; float sd = 13.0;\n"
        "    o4 = vec4((mod(b, sd) - sd*0.5)*3.0, (mod(floor(b/sd), sd) - sd*0.5)*3.0, (floor(b/(sd*sd)) - sd*0.5)*3.0, mod(b, 7.0)); }\n"
        "  mat3 r = rot(t + o4.w);\n"
        "  vec3 p = rot(t*0.3) * (r * pos * 0.8 + o4.xyz);\n"
        "  p.z -= 60.0;\n"
        "  vn = rot(t*0.3) * r * nrm; vuv = uv; vk = o4.w;\n"
        "  gl_Position = vec4(p.x * 2.4 / aspect, p.y * 2.4, p.z * 0.01 * -p.z - 1.0, -p.z);\n"
        "}\n",
        "#version 300 es\nprecision mediump float;\n"
        "in vec3 vn; in vec2 vuv; flat in float vk; uniform sampler2D tex; out vec4 o;\n"
        "void main(){\n"
        "  float l = max(dot(normalize(vn), normalize(vec3(0.4,0.7,0.6))), 0.0) * 0.8 + 0.2;\n"
        "  vec3 tint = vec3(0.6 + 0.4*sin(vk), 0.6 + 0.4*sin(vk+2.0), 0.6 + 0.4*sin(vk+4.0));\n"
        "  o = vec4(texture(tex, vuv).rgb * tint * l, 1.0);\n"
        "}\n");
    glUseProgram(prog);
    GLuint tex = checkerTexture();
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glUniform1f(glGetUniformLocation(prog, "aspect"), (float)s_dispW / s_dispH);
    GLint ut = glGetUniformLocation(prog, "t");
    GLint ub = glGetUniformLocation(prog, "base");
    glEnable(GL_DEPTH_TEST);
    glViewport(0, 0, s_dispW, s_dispH);

    // warm-up (compilation, first frame)
    for (int i = 0; i < 5; i++) {
        glClearColor(0.05f, 0.05f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 36, instances);
        eglSwapBuffers(s_dpy, w.egl);
    }
    int frames = 0;
    double worst = 0, t0 = now(), last = t0;
    while (now() - t0 < seconds) {
        glUniform1f(ut, (float)(now() - t0));
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (separate) {
            // like a game engine: one call per object, each with its own parameters
            for (int i = 0; i < instances; i++) {
                glUniform1f(ub, (float)i);
                glDrawArraysInstanced(GL_TRIANGLES, 0, 36, 1);
            }
        } else {
            glDrawArraysInstanced(GL_TRIANGLES, 0, 36, instances);
        }
        eglSwapBuffers(s_dpy, w.egl);
        double n = now();
        if (n - last > worst)
            worst = n - last;
        last = n;
        frames++;
    }
    double dt = now() - t0;
    char d[160];
    snprintf(d, sizeof(d), "%.1f frames/s, worst frame %.0f ms (%d cubes, %d %s)",
             frames / dt, worst * 1000, instances, separate ? instances : 1,
             separate ? "draw calls" : "instanced call");
    result(separate ? "3D scene, separate calls" : "3D scene, instanced",
           frames > 0 && glGetError() == GL_NO_ERROR, d);
    glDeleteTextures(1, &tex);
    closeWindow(w);
}

// ------------------------------------------------------------------ 2D : sprites
static void bench2D(int sprites, double seconds)
{
    Window w;
    if (!openWindow(w, s_dispW, s_dispH, PIXEL_FORMAT_RGBA_8888, s_cfg8888)) {
        result("2D sprites", false, "cannot create window");
        return;
    }
    GLuint prog = linkProgram(
        "#version 300 es\nlayout(location=0) in vec2 corner; layout(location=1) in vec4 spr;\n"
        "uniform float t; out vec2 uv;\n"
        "void main(){ uv = corner * 0.5 + 0.5;\n"
        "  vec2 c = vec2(sin(t * spr.z + spr.x * 6.28), cos(t * spr.w + spr.y * 6.28)) * 0.85;\n"
        "  gl_Position = vec4(c + corner * 0.06, 0.0, 1.0); }\n",
        "#version 300 es\nprecision mediump float;\nin vec2 uv; uniform sampler2D tex; out vec4 o;\n"
        "void main(){ vec4 c = texture(tex, uv); float r = length(uv - 0.5) * 2.0;\n"
        "  o = vec4(c.rgb, (1.0 - smoothstep(0.8, 1.0, r)) * 0.7); }\n");
    glUseProgram(prog);
    GLuint tex = checkerTexture();
    static const float corners[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    float *spr = (float *)malloc(sprites * 16);
    for (int i = 0; i < sprites; i++) {
        spr[i * 4 + 0] = (float)rand() / RAND_MAX; spr[i * 4 + 1] = (float)rand() / RAND_MAX;
        spr[i * 4 + 2] = 0.3f + (float)rand() / RAND_MAX; spr[i * 4 + 3] = 0.3f + (float)rand() / RAND_MAX;
    }
    GLuint vao, vb[2];
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(2, vb);
    glBindBuffer(GL_ARRAY_BUFFER, vb[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vb[1]);
    glBufferData(GL_ARRAY_BUFFER, sprites * 16, spr, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
    glVertexAttribDivisor(1, 1);
    free(spr);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GLint ut = glGetUniformLocation(prog, "t");
    glViewport(0, 0, s_dispW, s_dispH);
    int frames = 0;
    double worst = 0, t0 = now(), last = t0;
    while (now() - t0 < seconds) {
        glUniform1f(ut, (float)(now() - t0));
        glClearColor(0.1f, 0.1f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, sprites);
        eglSwapBuffers(s_dpy, w.egl);
        double n = now();
        if (frames > 2 && n - last > worst)
            worst = n - last;
        last = n;
        frames++;
    }
    double dt = now() - t0;
    char d[160];
    snprintf(d, sizeof(d), "%.1f frames/s, worst frame %.0f ms (%d blended sprites)", frames / dt,
             worst * 1000, sprites);
    result("full-screen 2D sprites", frames > 0 && glGetError() == GL_NO_ERROR, d);
    glDeleteTextures(1, &tex);
    closeWindow(w);
}

static EGLConfig chooseConfig(int r, int g, int b, int a)
{
    EGLint attr[] = { EGL_RENDERABLE_TYPE, 0x40 /* ES3 */, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                      EGL_RED_SIZE, r, EGL_GREEN_SIZE, g, EGL_BLUE_SIZE, b, EGL_ALPHA_SIZE, a,
                      EGL_DEPTH_SIZE, 16, EGL_NONE };
    EGLConfig cfgs[64];
    EGLint n = 0;
    eglChooseConfig(s_dpy, attr, cfgs, 64, &n);
    for (int i = 0; i < n; i++) {   // exact channel sizes
        EGLint cr, cg, cb, ca;
        eglGetConfigAttrib(s_dpy, cfgs[i], EGL_RED_SIZE, &cr);
        eglGetConfigAttrib(s_dpy, cfgs[i], EGL_GREEN_SIZE, &cg);
        eglGetConfigAttrib(s_dpy, cfgs[i], EGL_BLUE_SIZE, &cb);
        eglGetConfigAttrib(s_dpy, cfgs[i], EGL_ALPHA_SIZE, &ca);
        if (cr == r && cg == g && cb == b && ca == a)
            return cfgs[i];
    }
    return n ? cfgs[0] : NULL;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 8.0;
    // "glbench <s> calls <n>": only the separate-calls scene (profiling)
    bool onlyDraws = argc > 3 && !strcmp(argv[2], "calls");
    sp<IBinder> display = SurfaceComposerClient::getBuiltInDisplay(ISurfaceComposer::eDisplayIdMain);
    DisplayInfo info;
    SurfaceComposerClient::getDisplayInfo(display, &info);
    s_dispW = info.w;
    s_dispH = info.h;
    printf("screen %dx%d\n", s_dispW, s_dispH);
    // screen off = no composition: wake up with the Power key (KEYCODE_WAKEUP does not exist
    // on KitKat), only when it is off, then a simple unlock (menu)
    system("dumpsys power | grep -q 'mScreenOn=true' || input keyevent 26");
    usleep(800000);
    system("input keyevent 82");
    usleep(300000);

    s_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(s_dpy, NULL, NULL);
    s_cfg8888 = chooseConfig(8, 8, 8, 8);
    s_cfg565 = chooseConfig(5, 6, 5, 0);

    if (onlyDraws) {
        bench3D(atoi(argv[3]), secs, true);
        return 0;
    }
    printf("\ndisplay (pattern read back):\n");
    checkDisplay("full screen RGBA8888", s_dispW, s_dispH, PIXEL_FORMAT_RGBA_8888, s_cfg8888);
    checkDisplay("1/2 RGB565 buffer scaled up", s_dispW / 2, s_dispH / 2, PIXEL_FORMAT_RGB_565, s_cfg565);
    checkDisplay("3/4 RGBA8888 buffer scaled up", s_dispW * 3 / 4, s_dispH * 3 / 4, PIXEL_FORMAT_RGBA_8888,
                 s_cfg8888);
    // like Unity in landscape: 1920x1080 RGB565 buffer rotated 90 degrees and scaled to the screen.
    // clockwise rotation: top-left of the buffer -> top-right of the screen, etc.
    checkDisplay("landscape RGB565 rotated 90, scaled up", s_dispH * 2 / 3, s_dispW * 3 / 4,
                 PIXEL_FORMAT_RGB_565, s_cfg565, NATIVE_WINDOW_TRANSFORM_ROT_90,
                 kBlue, kRed, kWhite, kGreen);

    printf("\non-screen performance:\n");
    bench3D(2000, secs);
    bench3D(20000, secs);
    bench3D(300, secs, true);
    bench3D(1500, secs, true);
    bench2D(2000, secs);

    printf("\nresult: %d OK, %d FAIL\n", s_pass, s_fail);
    eglTerminate(s_dpy);
    return s_fail ? 2 : 0;
}
