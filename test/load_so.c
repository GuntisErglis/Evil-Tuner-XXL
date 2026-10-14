/* Evil Tuner XXL -- loads the built .so the way MPC does (dlopen + VSTPluginMain) and renders a few seconds:
 * run under qemu-arm against an armhf glibc to check the ARM build loads, starts its threads and renders.
 *   load_so <path/to/evil_tuner_xxl.so> [seconds] */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef struct AEffect AEffect;
typedef intptr_t (*cb)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
struct AEffect { int32_t magic; intptr_t (*d)(AEffect*, int32_t, int32_t, intptr_t, void*, float);
  void* p; void (*setP)(AEffect*, int32_t, float); float (*getP)(AEffect*, int32_t);
  int32_t np, npar, ni, no, flags; intptr_t r1, r2; int32_t a, b, c; float io; void *obj, *user; int32_t uid, ver;
  void (*pr)(AEffect*, float**, float**, int32_t); void* pdr; char f[56]; };
static double ti[16];
static intptr_t host(AEffect* e, int32_t op, int32_t i, intptr_t v, void* p, float o) {
  if (op == 7) { ti[4] = 120.0; ((int32_t*)&ti[8])[5] = 1 << 10; return (intptr_t)ti; }
  return 0;
}
int main(int argc, char** argv) {
  void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
  AEffect* (*mainf)(cb) = (AEffect * (*)(cb)) dlsym(h, "VSTPluginMain");
  AEffect* a = mainf(host);
  if (!a) { printf("no instance\n"); return 1; }
  char name[64] = {0};
  a->d(a, 45, 0, 0, name, 0);
  printf("loaded '%s': %d params, magic %08x\n", name, a->npar, a->magic);
  float L[128], R[128], *o[2] = {L, R};
  int secs = argc > 2 ? atoi(argv[2]) : 2;
  double e = 0;
  for (int b = 0; b < secs * 344; b++) { a->pr(a, 0, o, 128); for (int i = 0; i < 128; i++) e += L[i] * L[i]; }
  char d[64];
  for (int i = 0; i < a->npar; i++) { a->d(a, 7, i, 0, d, 0); }
  printf("rendered %d s, energy %.4f; all %d displays read\n", secs, e, a->npar);
  a->d(a, 1, 0, 0, 0, 0);   /* effClose */
  printf("closed\n");
  return 0;
}
