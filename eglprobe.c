/* eglprobe.c — does a MUSL process survive dlopen()ing the device's GLIBC Adreno libEGL.so and
 * calling eglInitialize? This is the make-or-break question for the musl WPE build on this device. */
#include <stdio.h>
#include <dlfcn.h>

#define EGL_VERSION    0x3054
#define EGL_VENDOR     0x3053
#define EGL_EXTENSIONS 0x3055

int main(void)
{
    fprintf(stderr, "eglprobe: musl process up; dlopen /usr/lib/libEGL.so ...\n");
    void* h = dlopen("/usr/lib/libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "  dlopen FAILED: %s\n", dlerror()); return 1; }
    fprintf(stderr, "  dlopen OK (loaded the glibc driver into a musl process)\n");

    void*       (*eglGetDisplay)(void*)            = dlsym(h, "eglGetDisplay");
    unsigned    (*eglInitialize)(void*, int*, int*)= dlsym(h, "eglInitialize");
    const char* (*eglQueryString)(void*, int)      = dlsym(h, "eglQueryString");
    if (!eglGetDisplay || !eglInitialize) { fprintf(stderr, "  dlsym FAILED\n"); return 2; }

    void* dpy = eglGetDisplay((void*)0);   /* EGL_DEFAULT_DISPLAY */
    fprintf(stderr, "  eglGetDisplay -> %p\n", dpy);

    int maj = 0, min = 0;
    unsigned r = eglInitialize(dpy, &maj, &min);
    fprintf(stderr, "  eglInitialize -> %u  (EGL %d.%d)\n", r, maj, min);
    if (eglQueryString) {
        const char* vn = eglQueryString(dpy, EGL_VENDOR);
        const char* vr = eglQueryString(dpy, EGL_VERSION);
        fprintf(stderr, "  vendor=%s  version=%s\n", vn ? vn : "(null)", vr ? vr : "(null)");
    }
    fprintf(stderr, "eglprobe: SURVIVED — musl can drive the glibc Adreno EGL.\n");
    return 0;
}
