// Hardware video encoder test through MediaCodec (adb shell enctest <mime> <output> [w h frames])
// Encodes an animated NV12 test pattern and writes the raw stream (IVF for VP8/VP9) for checking on a PC.
#include <binder/ProcessState.h>
#include <gui/Surface.h>
#include <media/ICrypto.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <OMX_IVCommon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

using namespace android;

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// pattern: 8 vertical colour bars, scrolling white square
static void fill_frame(uint8_t *dst, int w, int h, int n)
{
    static const uint8_t bars[8][3] = {   // Y U V
        {235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
        {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    int sq = h / 4, sx = (n * 8) % (w - sq), sy = h / 2 - sq / 2;
    uint8_t *uv = dst + w * h;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            bool in = x >= sx && x < sx + sq && y >= sy && y < sy + sq;
            dst[y * w + x] = in ? 235 : bars[x * 8 / w][0];
        }
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            bool in = x * 2 >= sx && x * 2 < sx + sq && y * 2 >= sy && y * 2 < sy + sq;
            const uint8_t *c = bars[x * 2 * 8 / w];
            uv[y * w + x * 2] = in ? 128 : c[1];
            uv[y * w + x * 2 + 1] = in ? 128 : c[2];
        }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: enctest <mime> <output> [width height frames]\n");
        return 2;
    }
    const char *mime = argv[1];
    int w = argc > 4 ? atoi(argv[3]) : 640, h = argc > 4 ? atoi(argv[4]) : 480;
    int frames = argc > 5 ? atoi(argv[5]) : 150;
    bool ivf = !strcmp(mime, "video/x-vnd.on2.vp8") || !strcmp(mime, "video/x-vnd.on2.vp9");

    ProcessState::self()->startThreadPool();
    sp<ALooper> looper = new ALooper;
    looper->setName("enctest");
    looper->start();

    sp<MediaCodec> codec = MediaCodec::CreateByType(looper, mime, true /* encoder */);
    if (codec == NULL) {
        fprintf(stderr, "no encoder for %s\n", mime);
        return 1;
    }
    sp<AMessage> fmt = new AMessage;
    fmt->setString("mime", mime);
    fmt->setInt32("width", w);
    fmt->setInt32("height", h);
    fmt->setInt32("stride", w);
    fmt->setInt32("slice-height", h);
    fmt->setInt32("color-format", OMX_COLOR_FormatYUV420SemiPlanar);
    fmt->setInt32("bitrate", 2000000);
    fmt->setInt32("frame-rate", 30);
    fmt->setInt32("i-frame-interval", 1);
    status_t err = codec->configure(fmt, NULL, NULL, MediaCodec::CONFIGURE_FLAG_ENCODE);
    if (err != OK) {
        fprintf(stderr, "configure: %d\n", err);
        return 1;
    }
    AString name;
    codec->getName(&name);
    if ((err = codec->start()) != OK) {
        fprintf(stderr, "start: %d\n", err);
        return 1;
    }
    Vector<sp<ABuffer> > in, out;
    codec->getInputBuffers(&in);
    codec->getOutputBuffers(&out);

    FILE *f = fopen(argv[2], "wb");
    if (f == NULL) {
        perror(argv[2]);
        return 1;
    }
    if (ivf) {
        uint8_t hd[32] = {'D', 'K', 'I', 'F'};
        put16(hd + 6, 32);
        memcpy(hd + 8, strcmp(mime, "video/x-vnd.on2.vp8") ? "VP90" : "VP80", 4);
        put16(hd + 12, w);
        put16(hd + 14, h);
        put32(hd + 16, 30);
        put32(hd + 20, 1);
        put32(hd + 24, frames);
        fwrite(hd, 1, 32, f);
    }

    int sent = 0, got = 0, keys = 0;
    size_t bytes = 0, frame_size = w * h * 3 / 2;
    bool eos = false;
    int64_t t0 = now_us();
    while (!eos) {
        size_t idx;
        if (sent <= frames && codec->dequeueInputBuffer(&idx, 5000) == OK) {
            if (sent == frames) {
                codec->queueInputBuffer(idx, 0, 0, sent * 33333LL, MediaCodec::BUFFER_FLAG_EOS);
            } else {
                if (in[idx]->capacity() < frame_size) {
                    fprintf(stderr, "input buffer too small: %zu < %zu\n", in[idx]->capacity(), frame_size);
                    return 1;
                }
                fill_frame(in[idx]->base(), w, h, sent);
                codec->queueInputBuffer(idx, 0, frame_size, sent * 33333LL, 0);
            }
            sent++;
        }
        size_t off, size;
        int64_t pts;
        uint32_t flags;
        err = codec->dequeueOutputBuffer(&idx, &off, &size, &pts, &flags, 5000);
        if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
            codec->getOutputBuffers(&out);
        } else if (err == OK) {
            if (size > 0) {
                if (ivf && !(flags & MediaCodec::BUFFER_FLAG_CODECCONFIG)) {
                    uint8_t fh[12];
                    put32(fh, size);
                    put32(fh + 4, got);
                    put32(fh + 8, 0);
                    fwrite(fh, 1, 12, f);
                }
                if (!ivf || !(flags & MediaCodec::BUFFER_FLAG_CODECCONFIG))
                    fwrite(out[idx]->base() + off, 1, size, f);
                bytes += size;
                if (!(flags & MediaCodec::BUFFER_FLAG_CODECCONFIG)) {
                    got++;
                    if (flags & MediaCodec::BUFFER_FLAG_SYNCFRAME)
                        keys++;
                }
            }
            eos = flags & MediaCodec::BUFFER_FLAG_EOS;
            codec->releaseOutputBuffer(idx);
        } else if (err != -EAGAIN && err != INFO_FORMAT_CHANGED) {
            fprintf(stderr, "dequeueOutputBuffer: %d\n", err);
            break;
        }
        if (now_us() - t0 > 30000000LL) {
            fprintf(stderr, "timed out\n");
            break;
        }
    }
    double dt = (now_us() - t0) / 1e6;
    fclose(f);
    codec->stop();
    codec->release();
    looper->stop();
    printf("%s: %s %dx%d, %d frames in, %d out (%d key frames), %zu bytes, %.0f frames/s\n",
           mime, name.c_str(), w, h, frames, got, keys, bytes, got / dt);
    return eos && got >= frames ? 0 : 1;
}
