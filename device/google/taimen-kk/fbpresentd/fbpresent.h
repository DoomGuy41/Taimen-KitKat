/* Protocol between gralloc.taimen (SurfaceFlinger, 32-bit) and fbpresentd (64-bit).
 * Abstract SOCK_SEQPACKET socket "fbpresent". The client sends one struct fbp_req per frame,
 * with the buffer descriptor (SCM_RIGHTS) the first time it presents that id; the daemon answers
 * with an int32 (0 or -errno) once the commit is done. */
#ifndef FBPRESENT_H
#define FBPRESENT_H
#include <stdint.h>
#define FBP_SOCKET "fbpresent"
struct fbp_req {
    uint64_t id;        /* QTI buffer id (private_handle_t::id) */
    uint32_t width;     /* aligned width (pixels) */
    uint32_t height;
    uint32_t offset;
    uint32_t ubwc;      /* UBWC compressed buffer */
    uint32_t xres, yres;
};
#endif
