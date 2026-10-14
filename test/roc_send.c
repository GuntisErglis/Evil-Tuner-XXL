/* Evil Tuner XXL -- test sender: a 440 Hz tone over Roc, the way Roc Droid sends (rtp+rs8m 10001, rs8m 10002, 44.1 kHz
 * stereo float, internal clock).   roc_send <ip> <port> <seconds> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <roc/context.h>
#include <roc/endpoint.h>
#include <roc/log.h>
#include <roc/sender.h>
static void conn(roc_sender *s, roc_interface i, roc_protocol p, const char *ip, int port) {
  roc_endpoint *e; roc_endpoint_allocate(&e); roc_endpoint_set_protocol(e, p); roc_endpoint_set_host(e, ip);
  roc_endpoint_set_port(e, port); if (roc_sender_connect(s, ROC_SLOT_DEFAULT, i, e)) printf("connect failed\n"); roc_endpoint_deallocate(e);
}
int main(int argc, char **argv) {
  roc_context_config cc; memset(&cc, 0, sizeof cc); roc_context *ctx; roc_context_open(&cc, &ctx);
  roc_sender_config sc; memset(&sc, 0, sizeof sc);
  sc.frame_encoding.rate = 44100; sc.frame_encoding.format = ROC_FORMAT_PCM_FLOAT32; sc.frame_encoding.channels = ROC_CHANNEL_LAYOUT_STEREO;
  sc.clock_source = ROC_CLOCK_SOURCE_INTERNAL; sc.fec_encoding = ROC_FEC_ENCODING_RS8M;
  roc_sender *s; if (roc_sender_open(ctx, &sc, &s)) { printf("open failed\n"); return 1; }
  int port = atoi(argv[2]);
  conn(s, ROC_INTERFACE_AUDIO_SOURCE, ROC_PROTO_RTP_RS8M_SOURCE, argv[1], port);
  conn(s, ROC_INTERFACE_AUDIO_REPAIR, ROC_PROTO_RS8M_REPAIR, argv[1], port + 1);
  static float buf[512]; double ph = 0; long n = (long)(atof(argv[3]) * 44100 / 256);
  for (long b = 0; b < n; b++) {
    for (int i = 0; i < 256; i++) { float v = 0.5f * sinf(ph); buf[2 * i] = buf[2 * i + 1] = v; ph += 2 * M_PI * 440 / 44100; }
    roc_frame f; f.samples = buf; f.samples_size = sizeof buf; roc_sender_write(s, &f);
  }
  roc_sender_close(s); roc_context_close(ctx); return 0;
}
