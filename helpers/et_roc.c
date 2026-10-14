/* Evil Tuner XXL -- et_roc: the PHONE source's receiver (a separate process, so a fault here never takes MPC down).
 *
 * Receives a Roc stream (roc-toolkit 0.3, the protocol Roc Droid 0.2 sends: RTP + Reed-Solomon FEC) on
 *   source  udp 10001 (rtp+rs8m)      repair  udp 10002 (rs8m)      control  udp 10003 (rtcp)
 * and writes it to stdout as raw 44100 Hz interleaved stereo signed 16-bit (the format every Evil Tuner source
 * delivers), paced in real time by roc's own clock. Silence while no phone sends.
 *
 *   et_roc [source_port]      (repair = source + 1, control = source + 2)
 *
 * Nothing leaves the device: the receiver only listens. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <roc/context.h>
#include <roc/endpoint.h>
#include <roc/log.h>
#include <roc/receiver.h>

#define FRAMES 256

static int bind_one(roc_receiver *r, roc_interface iface, roc_protocol proto, int port)
{
    roc_endpoint *ep = NULL;
    int rc = -1;
    if (roc_endpoint_allocate(&ep) != 0) return -1;
    if (roc_endpoint_set_protocol(ep, proto) == 0 && roc_endpoint_set_host(ep, "0.0.0.0") == 0 &&
        roc_endpoint_set_port(ep, port) == 0)
        rc = roc_receiver_bind(r, ROC_SLOT_DEFAULT, iface, ep);
    roc_endpoint_deallocate(ep);
    return rc;
}

static int write_all(const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t w = write(1, c, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const int port = argc > 1 ? atoi(argv[1]) : 10001;
    roc_context_config cc;
    roc_receiver_config rc;
    roc_context *ctx = NULL;
    roc_receiver *recv = NULL;
    static float buf[FRAMES * 2];
    static short out[FRAMES * 2];

    signal(SIGPIPE, SIG_DFL);   /* the plugin closed the pipe: end */
    roc_log_set_level(ROC_LOG_ERROR);
    memset(&cc, 0, sizeof cc);
    if (roc_context_open(&cc, &ctx) != 0) { fprintf(stderr, "et_roc: context\n"); return 1; }
    memset(&rc, 0, sizeof rc);
    rc.frame_encoding.rate = 44100;
    rc.frame_encoding.format = ROC_FORMAT_PCM_FLOAT32;
    rc.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
    rc.clock_source = ROC_CLOCK_SOURCE_INTERNAL;
    if (roc_receiver_open(ctx, &rc, &recv) != 0) { fprintf(stderr, "et_roc: receiver\n"); return 1; }
    if (bind_one(recv, ROC_INTERFACE_AUDIO_SOURCE, ROC_PROTO_RTP_RS8M_SOURCE, port) != 0 ||
        bind_one(recv, ROC_INTERFACE_AUDIO_REPAIR, ROC_PROTO_RS8M_REPAIR, port + 1) != 0) {
        fprintf(stderr, "et_roc: cannot listen on udp %d/%d (in use?)\n", port, port + 1);
        return 2;
    }
    bind_one(recv, ROC_INTERFACE_AUDIO_CONTROL, ROC_PROTO_RTCP, port + 2);   /* optional: senders without RTCP still play */
    fprintf(stderr, "et_roc: listening on udp %d (repair %d)\n", port, port + 1);
    for (;;) {
        roc_frame f;
        int i;
        f.samples = buf;
        f.samples_size = sizeof buf;
        if (roc_receiver_read(recv, &f) != 0) break;
        for (i = 0; i < FRAMES * 2; i++) {
            float v = buf[i] * 32767.f;
            out[i] = (short)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
        }
        if (write_all(out, sizeof out) != 0) break;
    }
    roc_receiver_close(recv);
    roc_context_close(ctx);
    return 0;
}
