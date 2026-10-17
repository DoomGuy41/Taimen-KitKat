/* Subset of hardware/gralloc1.h (Android 11) and of the private_handle_t of the msm8998 QTI
 * gralloc1 (hardware/qcom/display/msm8998/libgralloc1), to drive gralloc.msm8998.so */
#ifndef GRALLOC1_A11_H
#define GRALLOC1_A11_H
#include <stdint.h>
#include <hardware/hardware.h>
#include <system/window.h>

typedef uint64_t gralloc1_buffer_descriptor_t;
typedef void (*gralloc1_function_pointer_t)(void);

typedef struct gralloc1_device {
    struct hw_device_t common;
    void (*getCapabilities)(struct gralloc1_device *dev, uint32_t *outCount, int32_t *outCaps);
    gralloc1_function_pointer_t (*getFunction)(struct gralloc1_device *dev, int32_t descriptor);
} gralloc1_device_t;

enum {
    G1_CREATE_DESCRIPTOR = 2, G1_DESTROY_DESCRIPTOR = 3, G1_SET_CONSUMER_USAGE = 4,
    G1_SET_DIMENSIONS = 5, G1_SET_FORMAT = 6, G1_SET_PRODUCER_USAGE = 7, G1_GET_STRIDE = 13,
    G1_ALLOCATE = 14, G1_RETAIN = 15, G1_RELEASE = 16, G1_LOCK = 18, G1_LOCK_FLEX = 19,
    G1_UNLOCK = 20, G1_SET_LAYER_COUNT = 21,
};

/* gralloc1 usages */
#define G1P_CPU_READ          (1ULL << 1)
#define G1P_CPU_READ_OFTEN    (1ULL << 2)
#define G1P_CPU_WRITE         (1ULL << 5)
#define G1P_CPU_WRITE_OFTEN   (1ULL << 6)
#define G1P_GPU_RENDER_TARGET (1ULL << 9)
#define G1P_PROTECTED         (1ULL << 14)
#define G1P_CAMERA            (1ULL << 17)
#define G1P_VIDEO_DECODER     (1ULL << 22)
#define G1C_CPU_READ          (1ULL << 1)
#define G1C_CPU_READ_OFTEN    (1ULL << 2)
#define G1C_GPU_TEXTURE       (1ULL << 8)
#define G1C_HWCOMPOSER        (1ULL << 11)
#define G1C_CLIENT_TARGET     (1ULL << 12)
#define G1C_VIDEO_ENCODER     (1ULL << 16)
#define G1C_CAMERA            (1ULL << 18)
#define G1C_RENDERSCRIPT      (1ULL << 20)

typedef struct { int32_t left, top, width, height; } gralloc1_rect_t;

typedef int32_t (*G1_PFN_CREATE_DESCRIPTOR)(gralloc1_device_t *, gralloc1_buffer_descriptor_t *);
typedef int32_t (*G1_PFN_DESTROY_DESCRIPTOR)(gralloc1_device_t *, gralloc1_buffer_descriptor_t);
typedef int32_t (*G1_PFN_SET_DIMENSIONS)(gralloc1_device_t *, gralloc1_buffer_descriptor_t, uint32_t, uint32_t);
typedef int32_t (*G1_PFN_SET_FORMAT)(gralloc1_device_t *, gralloc1_buffer_descriptor_t, int32_t);
typedef int32_t (*G1_PFN_SET_USAGE)(gralloc1_device_t *, gralloc1_buffer_descriptor_t, uint64_t);
typedef int32_t (*G1_PFN_SET_LAYER_COUNT)(gralloc1_device_t *, gralloc1_buffer_descriptor_t, uint32_t);
typedef int32_t (*G1_PFN_ALLOCATE)(gralloc1_device_t *, uint32_t, const gralloc1_buffer_descriptor_t *, buffer_handle_t *);
typedef int32_t (*G1_PFN_GET_STRIDE)(gralloc1_device_t *, buffer_handle_t, uint32_t *);
typedef int32_t (*G1_PFN_RETAIN)(gralloc1_device_t *, buffer_handle_t);
typedef int32_t (*G1_PFN_LOCK)(gralloc1_device_t *, buffer_handle_t, uint64_t, uint64_t,
                               const gralloc1_rect_t *, void **, int32_t);
typedef int32_t (*G1_PFN_UNLOCK)(gralloc1_device_t *, buffer_handle_t, int32_t *);

/* android_flex_layout (Android 11 system/graphics.h) */
enum { FLEX_COMPONENT_Y = 1, FLEX_COMPONENT_Cb = 2, FLEX_COMPONENT_Cr = 4 };
typedef struct {
    uint8_t *top_left;
    int32_t component, bits_per_component, bits_used, h_increment, v_increment,
            h_subsampling, v_subsampling;
} android_flex_plane_t;
typedef struct {
    int32_t format;
    uint32_t num_planes;
    android_flex_plane_t *planes;
} android_flex_layout_t;
typedef int32_t (*G1_PFN_LOCK_FLEX)(gralloc1_device_t *, buffer_handle_t, uint64_t, uint64_t,
                                    const gralloc1_rect_t *, android_flex_layout_t *, int32_t);

/* QTI private_handle_t (msm8998, Android 11): 2 fds, then ints */
struct qti_handle {
    native_handle_t nh;
    int fd, fd_metadata, magic, flags, width, height, unaligned_width, unaligned_height,
        format, buffer_type;
    unsigned int size, offset, offset_metadata;
    uint64_t base __attribute__((aligned(8)));
    uint64_t base_metadata __attribute__((aligned(8)));
    uint64_t gpuaddr __attribute__((aligned(8)));
    uint64_t id __attribute__((aligned(8)));
    uint64_t producer_usage __attribute__((aligned(8)));
    uint64_t consumer_usage __attribute__((aligned(8)));
    unsigned int layer_count;
};
#define QTI_HANDLE_MAGIC 0x676d736d /* 'gmsm' */
#endif
