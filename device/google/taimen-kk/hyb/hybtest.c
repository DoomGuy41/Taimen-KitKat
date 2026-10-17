/* hybtest: checks libhyb: loads the real Android 11 Adreno driver into a KitKat process,
 * creates an offscreen OpenGL ES 3 context, draws and reads pixels back.
 * usage: hybtest [Android 11 root] */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "hyb.h"

#define F(type, name) __typeof__(type) name = (type)sym(#name)
static void *s_egl, *s_gles;

static void *sym(const char *n)
{
    void *p = hyb_dlsym(n[0] == 'e' ? s_egl : s_gles, n);
    if (!p) {
        printf("missing symbol: %s\n", n);
        exit(1);
    }
    return p;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        hyb_set_root(argv[1]);
    double t0 = now();
    s_egl = hyb_dlopen("libEGL_adreno.so", 0);
    if (!s_egl) { printf("libEGL_adreno: %s\n", hyb_dlerror()); return 1; }
    s_gles = hyb_dlopen("libGLESv2_adreno.so", 0);
    if (!s_gles) { printf("libGLESv2_adreno: %s\n", hyb_dlerror()); return 1; }
    printf("driver loaded in %.0f ms\n", (now() - t0) * 1000);

    F(EGLDisplay (*)(EGLNativeDisplayType), eglGetDisplay);
    F(EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *), eglInitialize);
    F(const char *(*)(EGLDisplay, EGLint), eglQueryString);
    F(EGLBoolean (*)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *), eglChooseConfig);
    F(EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint *), eglCreateContext);
    F(EGLSurface (*)(EGLDisplay, EGLConfig, const EGLint *), eglCreatePbufferSurface);
    F(EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext), eglMakeCurrent);
    F(EGLint (*)(void), eglGetError);
    F(EGLBoolean (*)(EGLenum), eglBindAPI);

    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize: 0x%x\n", eglGetError()); return 1; }
    printf("EGL %d.%d — %s / %s\n", maj, min, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
    printf("EGL_EXTENSIONS: %s\n", eglQueryString(dpy, EGL_EXTENSIONS));
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint attr[] = { EGL_RENDERABLE_TYPE, 0x40 /* ES3 */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || !n) { printf("eglChooseConfig: 0x%x\n", eglGetError()); return 1; }
    EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr);
    EGLint sattr[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, sattr);
    if (!ctx || !surf || !eglMakeCurrent(dpy, surf, surf, ctx)) { printf("context: 0x%x\n", eglGetError()); return 1; }

    F(const GLubyte *(*)(GLenum), glGetString);
    F(void (*)(GLfloat, GLfloat, GLfloat, GLfloat), glClearColor);
    F(void (*)(GLbitfield), glClear);
    F(void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *), glReadPixels);
    F(void (*)(void), glFinish);
    F(GLenum (*)(void), glGetError);
    printf("GL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\nGLSL        %s\n", glGetString(GL_VENDOR),
           glGetString(GL_RENDERER), glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    int next = 0;
    for (const char *p = ext; p && *p; p++) next += *p == ' ';
    printf("%d GL extensions\n", next);

    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char px[4] = { 0 };
    glReadPixels(128, 128, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("green clear: read %d,%d,%d,%d -> %s (GL error 0x%x)\n", px[0], px[1], px[2], px[3],
           px[0] < 5 && px[1] > 250 && px[2] < 5 ? "OK" : "KO", glGetError());

    t0 = now();
    for (int i = 0; i < 2000; i++) {
        glClearColor((i & 1) * 1.0f, 0.5f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glFinish();
    printf("2000 clears + glFinish: %.1f ms\n", (now() - t0) * 1000);
    return 0;
}
