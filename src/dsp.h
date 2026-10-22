// Evil Tuner XXL -- the sound: every processor of the chain, header-only (engine.cc owns one of each).
//
// All run at 44.1 kHz on blocks of up to 128 frames, on MPC's audio thread: no allocation after init(), no locks.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "et.h"

namespace et {

static const float kSR = (float)ET_SR;
static const float kTwoPi = 6.28318530718f;

inline float db2g(float db) { return powf(10.f, db / 20.f); }
inline float expmap(float v, float lo, float hi) { return lo * powf(hi / lo, clampf(v, 0.f, 1.f)); }
inline float onepole(float ms) { return 1.f - expf(-1.f / (fmaxf(ms, 0.01f) * 0.001f * kSR)); }

struct Rng {
  uint32_t s = 0x9E3779B9u;
  inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  inline float uni() { return (next() >> 8) * (1.f / 16777216.f); }       // [0, 1)
  inline float bi() { return uni() * 2.f - 1.f; }                         // [-1, 1)
};

// ------------------------------------------------------------------------------------------------ tempo (from MPC)
struct Tempo {
  double bpm = 120.0;
  double ppq = 0.0;          // beats at the block start (MPC's song position while playing, else our own count)
  bool playing = false;
  bool started = false;      // the transport started on this block
  double beat_per_frame() const { return bpm / 60.0 / kSR; }
};

// ------------------------------------------------------------------------------------------------ envelopes, LFOs
struct Adsr {
  enum { IDLE, A, D, S, R } st = IDLE;
  float v = 0.f;
  inline void gate(bool on) { if (on) st = A; else if (st != IDLE) st = R; }
  inline float run(float a, float d, float s, float r) {   // a, d, r: per-sample coefficients; s: level
    switch (st) {
      case A: v += a * (1.08f - v); if (v >= 1.f) { v = 1.f; st = D; } break;
      case D: v += d * (s - v); if (fabsf(v - s) < 1e-4f) st = S; break;
      case S: v = s; break;
      case R: v += r * (0.f - v); if (v < 1e-5f) { v = 0.f; st = IDLE; } break;
      default: break;
    }
    return v;
  }
};

struct Lfo {
  double ph = 0.0;
  float sh = 0.f, drift = 0.f, drift_t = 0.f;
  Rng rng;
  // advances by `inc` (cycles) over a block and returns the value (-1..1) at the block start
  inline float step(int wave, double inc) {
    float p = (float)ph, y;
    switch (wave) {
      case 0: y = sinf(kTwoPi * p); break;
      case 1: y = 1.f - 4.f * fabsf(p - 0.5f); break;
      case 2: y = 2.f * p - 1.f; break;
      case 3: y = 1.f - 2.f * p; break;
      case 4: y = p < 0.5f ? 1.f : -1.f; break;
      case 5: y = sh; break;
      default: y = drift; break;
    }
    ph += inc;
    if (ph >= 1.0) {
      ph -= floor(ph);
      sh = rng.bi();
      drift_t = rng.bi();
    }
    drift += (drift_t - drift) * 0.02f;
    return y;
  }
};

struct Follower {
  float env = 0.f;
  inline void run(const float* l, const float* r, int n, float speed) {
    float atk = onepole(expmap(1.f - speed, 1.f, 60.f)), rel = onepole(expmap(1.f - speed, 30.f, 900.f));
    for (int i = 0; i < n; ++i) {
      float x = fmaxf(fabsf(l[i]), fabsf(r[i]));
      env += (x > env ? atk : rel) * (x - env);
    }
  }
};

// ------------------------------------------------------------------------------------------------ the filter
// Zero-delay-feedback state-variable filter (LP / HP / BP), a feedback comb, and DUAL: one knob for both, a DJ filter
// (below the middle a low-pass closing, above it a high-pass opening; open in the middle).
struct Svf {
  float ic1[2] = {0, 0}, ic2[2] = {0, 0};
  inline void tick(int c, float x, float g, float k, float& lp, float& bp, float& hp) {
    float a1 = 1.f / (1.f + g * (g + k)), a2 = g * a1, a3 = g * a2;
    float v3 = x - ic2[c];
    float v1 = a1 * ic1[c] + a2 * v3;
    float v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
    ic1[c] = 2.f * v1 - ic1[c];
    ic2[c] = 2.f * v2 - ic2[c];
    lp = v2;
    bp = v1;
    hp = x - k * v1 - v2;
  }
  void reset() { ic1[0] = ic1[1] = ic2[0] = ic2[1] = 0.f; }
};

struct Filter {
  Svf a, b;
  std::vector<float> comb[2];
  int cw = 0;
  float cfb[2] = {0, 0};
  void init() {
    comb[0].assign(4096, 0.f);
    comb[1].assign(4096, 0.f);
  }
  static inline float g_of(float hz) { return tanf(3.14159265f * clampf(hz, 16.f, 20000.f) / kSR); }
  // mode 0 LP, 1 HP, 2 BP, 3 COMB, 4 DUAL; cut 0..1 (with modulation, octaves already applied), res 0..1
  void process(float* l, float* r, int n, int mode, float cut, float res) {
    float* io[2] = {l, r};
    if (mode == 4) {
      // DUAL: the middle (0.47..0.53) is open; the halves sweep a resonant low-pass down / high-pass up
      float k = 2.f - 1.8f * res;
      if (cut > 0.47f && cut < 0.53f) return;
      bool low = cut <= 0.47f;
      float t = low ? cut / 0.47f : (cut - 0.53f) / 0.47f;
      float hz = low ? expmap(t, 60.f, 20000.f) : expmap(t, 20.f, 9000.f);
      float g = g_of(hz);
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) {
          float lp, bp, hp;
          a.tick(c, io[c][i], g, k, lp, bp, hp);
          io[c][i] = low ? lp : hp;
        }
      return;
    }
    float hz = expmap(cut, 20.f, 20000.f);
    if (mode == 3) {
      // a feedback comb tuned to the cutoff (its pitch), resonance = feedback
      float d = clampf(kSR / clampf(hz, 30.f, 8000.f), 2.f, 4000.f);
      float fb = 0.2f + 0.78f * res;
      for (int i = 0; i < n; ++i) {
        float rp = (float)cw - d;
        while (rp < 0) rp += 4096.f;
        int i0 = (int)rp;
        float fr = rp - i0;
        for (int c = 0; c < 2; ++c) {
          std::vector<float>& bf = comb[c];
          float y = bf[i0 & 4095] + fr * (bf[(i0 + 1) & 4095] - bf[i0 & 4095]);
          cfb[c] += 0.45f * (y - cfb[c]);   // a little damping in the loop
          float w = io[c][i] + fb * cfb[c];
          bf[cw] = w > 1.5f ? 1.5f : (w < -1.5f ? -1.5f : w);
          io[c][i] = 0.5f * (io[c][i] + y);
        }
        cw = (cw + 1) & 4095;
      }
      return;
    }
    if (mode == 0 && cut >= 0.995f && res < 0.05f) return;   // wide open
    if (mode == 1 && cut <= 0.005f && res < 0.05f) return;
    float g = g_of(hz), k = 2.f - 1.95f * res;
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < n; ++i) {
        float lp, bp, hp;
        a.tick(c, io[c][i], g, k, lp, bp, hp);
        io[c][i] = mode == 0 ? lp : (mode == 1 ? hp : bp * (0.6f + k * 0.4f));
      }
  }
};

// ------------------------------------------------------------------------------------------------ the grains
// The grain buffer: the last 8 s of the source (or a frozen picture of it, or the loop sent TO GRAINS). Grains read it
// at their pitch with a window whose SHAPE goes from percussive (0) through smooth (0.5) to reversed (1).
static const int kGrainBuf = 8 * ET_SR;
static const int kMaxGrains = 48;
static const int kVoices = 8;

struct Grain {
  bool on = false;
  double pos = 0, inc = 1;
  int age = 0, len = 1;
  int wait = 0;            // frames before it starts (a synced grain lands on its beat inside a block)
  float gl = 0, gr = 0;    // pan gains x amplitude
};

// ------------------------------------------------------------------------------------------------ scales
// SCALE: the semitones a pitch may land on (within an octave), and the intervals CLOUD scatters grains to.
static const int kScaleNotes[8][8] = {
    {0, 1, 2, 3, 4, 5, 6, 7},    // CHROMATIC (every semitone: the list is not used)
    {0, 2, 4, 5, 7, 9, 11, -1},  // MAJOR
    {0, 2, 3, 5, 7, 8, 10, -1},  // MINOR
    {0, 2, 3, 5, 7, 9, 10, -1},  // DORIAN
    {0, 2, 4, 7, 9, -1},         // PENTATONIC
    {0, 3, 5, 7, 10, -1},        // MINOR PENTA
    {0, 7, -1},                  // 5THS + OCT
    {0, -1},                     // OCTAVES
};

// the nearest semitone the scale allows (scale 0: any semitone)
inline int scale_snap(int st, int scale) {
  if (scale <= 0 || scale > 7) return st;
  int best = st, bd = 99;
  for (int oct = -5; oct <= 5; ++oct)
    for (int k = 0; k < 8 && kScaleNotes[scale][k] >= 0; ++k) {
      int c = oct * 12 + kScaleNotes[scale][k];
      int d = abs(c - st);
      if (d < bd || (d == bd && c < best)) { bd = d; best = c; }
    }
  return best;
}

// PITCH (0..1) as whole semitones, -24..+24, on the scale
inline int pitch_semis(float knob, int scale) {
  int st = (int)lroundf((knob - 0.5f) * 48.f);
  return scale_snap(st, scale);
}

struct GrainParams {
  float pos, size, pitch, grains, spread, cloud, detune, shape;
  int scale = 0;
};

struct Grains {
  std::vector<float> buf[2];
  int w = 0, filled = 0;
  bool frozen = false;
  Grain g[kMaxGrains];
  Rng rng;
  double sched[kVoices + 1];   // per voice (and the drone, index kVoices): frames until the next grain
  void init() {
    buf[0].assign(kGrainBuf, 0.f);
    buf[1].assign(kGrainBuf, 0.f);
    for (auto& s : sched) s = 0;
  }
  inline void write(float l, float r) {
    if (frozen) return;
    buf[0][w] = l;
    buf[1][w] = r;
    if (++w >= kGrainBuf) w = 0;
    if (filled < kGrainBuf) filled++;
  }
  void load(const float* l, const float* r, int n) {   // TO GRAINS: the loop becomes the (frozen) grain buffer
    n = std::min(n, kGrainBuf);
    for (int i = 0; i < kGrainBuf; ++i) {
      buf[0][i] = n ? l[i % n] : 0.f;
      buf[1][i] = n ? r[i % n] : 0.f;
    }
    w = 0;
    filled = kGrainBuf;
    frozen = true;
  }
  inline float rd(int c, double p) const {
    while (p < 0) p += kGrainBuf;
    while (p >= kGrainBuf) p -= kGrainBuf;
    int i0 = (int)p;
    float fr = (float)(p - i0);
    int i1 = i0 + 1 == kGrainBuf ? 0 : i0 + 1;
    return buf[c][i0] + fr * (buf[c][i1] - buf[c][i0]);
  }
  static inline float window(float t, float shape) {
    float hann = 0.5f - 0.5f * cosf(kTwoPi * t);
    if (fabsf(shape - 0.5f) < 0.01f) return hann;
    const float a = 0.015f;
    float tt = shape < 0.5f ? t : 1.f - t;
    float perc = tt < a ? tt / a : expf(-5.f * (tt - a) / (1.f - a));
    float m = fabsf(shape - 0.5f) * 2.f;   // 0 at the middle, 1 at the ends
    return hann * (1.f - m) + perc * m;
  }
  void spawn(const GrainParams& p, float semis, float amp, int wait = 0) {
    int slot = -1, oldest = -1, oa = -1;
    for (int i = 0; i < kMaxGrains; ++i) {
      if (!g[i].on) { slot = i; break; }
      if (g[i].age > oa) { oa = g[i].age; oldest = i; }
    }
    if (slot < 0) slot = oldest;
    Grain& q = g[slot];
    float size_ms = expmap(p.size, 8.f, 1000.f);
    int len = std::max(64, (int)(size_ms * 0.001f * kSR));
    float st = semis + (float)pitch_semis(p.pitch, p.scale);               // whole semitones, on the scale
    st += p.detune * 0.5f * rng.bi();                                      // up to +-0.5 semitone per grain
    if (p.cloud > 0.f && rng.uni() < p.cloud * 0.3f) {
      // CLOUD scatters grains to musical intervals: octaves, and with a SCALE the scale's own notes above / below
      int iv = rng.uni() < 0.5f ? 12 : -12;
      if (p.scale > 0 && rng.uni() < 0.6f) {
        int cnt = 0;
        while (cnt < 8 && kScaleNotes[p.scale][cnt] >= 0) cnt++;
        int k = 1 + (int)(rng.uni() * (cnt - 1 > 0 ? cnt - 1 : 1));
        iv = cnt > 1 ? kScaleNotes[p.scale][k < cnt ? k : cnt - 1] : 12;
        if (rng.uni() < 0.35f) iv -= 12;
      }
      st += (float)iv;
    }
    double ratio = pow(2.0, st / 12.0);
    bool rev = p.cloud > 0.f && rng.uni() < p.cloud * 0.25f;
    double span = ratio * len;
    int avail = frozen ? kGrainBuf : filled;
    double back_min = span + 256.0;                                        // never overtake the write head
    double room = std::max(0.0, (double)avail - back_min - 1.0);
    double back = back_min + p.pos * room * 0.98;
    back += p.spread * rng.uni() * std::min(room, (double)kSR);           // up to 1 s of position scatter
    if (back > avail - 1) back = avail - 1;
    double start = (double)w - back;
    q.on = true;
    q.age = 0;
    q.wait = wait;
    q.len = len;
    q.inc = rev ? -ratio : ratio;
    q.pos = rev ? start + span : start;
    float pan = 0.5f + 0.5f * p.spread * rng.bi();
    q.gl = amp * sqrtf(1.f - pan);
    q.gr = amp * sqrtf(pan);
  }
  // GRAIN SYNC: a grain on each step of the beat grid that falls in this block (GRAINS: the chance a step plays,
  // CLOUD: how often a step plays two grains)
  inline void schedule_sync(const GrainParams& p, float semis, float amp, double beat0, double bpf, double step, int n) {
    double b1 = beat0 + bpf * n;
    double k = ceil(beat0 / step - 1e-9);
    int guard = 0;
    for (double t = k * step; t < b1 && guard++ < 8; t += step) {
      int at = std::max(0, std::min(n - 1, (int)((t - beat0) / bpf)));
      float chance = 0.15f + 0.85f * p.grains;
      if (rng.uni() <= chance) spawn(p, semis, amp, at);
      if (p.cloud > 0.f && rng.uni() < p.cloud * 0.5f) spawn(p, semis, amp * 0.8f, at + (int)(rng.uni() * step / bpf * 0.5));
    }
  }
  // one scheduler: grains per second from GRAINS, jittered by CLOUD
  inline void schedule(int v, const GrainParams& p, float semis, float amp, int n) {
    float rate = expmap(p.grains, 1.5f, 90.f);
    sched[v] -= n;
    int guard = 0;
    while (sched[v] <= 0 && guard++ < 16) {
      spawn(p, semis, amp);
      float jit = 1.f + p.cloud * 1.4f * rng.bi();
      sched[v] += kSR / rate * fmaxf(0.15f, jit);
    }
  }
  float wtab[1026];
  float wshape = -1.f;
  void render(float* l, float* r, int n, float shape) {
    if (shape != wshape) {   // the window, tabulated (1024 steps, interpolated)
      for (int i = 0; i <= 1024; ++i) wtab[i] = window(i / 1024.f, shape);
      wtab[1025] = wtab[1024];
      wshape = shape;
    }
    const float* b0 = buf[0].data();
    const float* b1 = buf[1].data();
    for (int k = 0; k < kMaxGrains; ++k) {
      Grain& q = g[k];
      if (!q.on) continue;
      float step = 1024.f / q.len, wp = q.age * step;
      double pos = q.pos;
      while (pos < 0) pos += kGrainBuf;
      while (pos >= kGrainBuf) pos -= kGrainBuf;
      int w0 = std::min(q.wait, n);
      q.wait -= w0;
      int m = std::min(n - w0, q.len - q.age);
      for (int i = w0; i < w0 + m; ++i) {
        int wi = (int)wp;
        float wv = wtab[wi] + (wp - wi) * (wtab[wi + 1] - wtab[wi]);
        int i0 = (int)pos;
        float fr = (float)(pos - i0);
        int i1 = i0 + 1 == kGrainBuf ? 0 : i0 + 1;
        float a = b0[i0] + fr * (b0[i1] - b0[i0]);
        float c = b1[i0] + fr * (b1[i1] - b1[i0]);
        l[i] += a * wv * q.gl;
        r[i] += c * wv * q.gr;
        pos += q.inc;
        if (pos >= kGrainBuf) pos -= kGrainBuf;
        else if (pos < 0) pos += kGrainBuf;
        wp += step;
      }
      q.pos = pos;
      q.age += m;
      if (q.age >= q.len) q.on = false;
    }
  }
  // the gain that keeps a dense cloud near the source's level
  static inline float density_gain(const GrainParams& p) {
    float overlap = expmap(p.grains, 1.5f, 90.f) * expmap(p.size, 8.f, 1000.f) * 0.001f;
    return 1.63f / sqrtf(fmaxf(1.f, overlap));   // a Hann window's power is 0.375 of the source's: 1/sqrt(0.375)
  }
};

// ------------------------------------------------------------------------------------------------ the oscillator layer
struct Osc {
  double ph = 0;
  static inline float blep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
  }
  inline float tick(int wave, float hz) {
    float dt = hz / kSR, p = (float)ph, y;
    switch (wave) {
      case 0: y = sinf(kTwoPi * p); break;
      case 1: y = 1.f - 4.f * fabsf(p - 0.5f); break;
      case 2: y = (2.f * p - 1.f) - blep(p, dt); break;
      default: {
        float q = p + 0.5f;
        if (q >= 1.f) q -= 1.f;
        y = (p < 0.5f ? 1.f : -1.f) + blep(p, dt) - blep(q, dt);
      }
    }
    ph += dt;
    if (ph >= 1.0) ph -= 1.0;
    return y;
  }
};

// ------------------------------------------------------------------------------------------------ the saturator
// TAPE / TUBE / DIODE / FUZZ: The Good Bits' drive (clickysteve, MIT): out = (1-d) x + d shape(x, 1 + 5d).
// CRUSH: bit depth 16 -> 2. DOWNSAMPLE: sample-and-hold 1..32. FLANGER, CHORUS: modulated short delays.
struct Saturator {
  std::vector<float> dl[2];
  int dw = 0;
  double lfo = 0;
  float hold[2] = {0, 0};
  int hcount = 0;
  float dc[2] = {0, 0};
  void init() {
    dl[0].assign(4096, 0.f);
    dl[1].assign(4096, 0.f);
  }
  static inline float shape(int type, float x, float g) {
    switch (type) {
      case 0: return tanhf(g * x);
      case 1: return tanhf(g * x + 0.3f) - tanhf(0.3f);
      case 2: { float y = g * x; return y > 1.f ? 1.f : (y < -1.f ? -1.f : y); }
      default: { float y = g * x; return y > 0 ? tanhf(3.f * y) : (y < -1.f ? -1.f : y); }
    }
  }
  inline float rdl(int c, float d) const {
    float p = (float)dw - d;
    while (p < 0) p += 4096.f;
    int i0 = (int)p;
    float fr = p - i0;
    return dl[c][i0 & 4095] + fr * (dl[c][(i0 + 1) & 4095] - dl[c][i0 & 4095]);
  }
  void process(float* l, float* r, int n, int type, float amt, float mix) {
    float* io[2] = {l, r};
    if (type <= 3) {
      float g = 1.f + 5.f * amt;
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) {
          float x = io[c][i];
          float y = (1.f - amt) * x + amt * shape(type, x, g);
          if (type == 1) { dc[c] += 0.0005f * (y - dc[c]); y -= dc[c]; }
          io[c][i] = x + mix * (y - x);
        }
      return;
    }
    if (type == 4) {   // CRUSH
      float bits = 16.f - 14.f * amt;
      float step = 2.f / powf(2.f, bits);
      for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i) {
          float x = io[c][i], y = roundf(x / step) * step;
          io[c][i] = x + mix * (y - x);
        }
      return;
    }
    if (type == 5) {   // DOWNSAMPLE
      int div = 1 + (int)(amt * 31.f);
      for (int i = 0; i < n; ++i) {
        if (hcount <= 0) { hold[0] = l[i]; hold[1] = r[i]; hcount = div; }
        hcount--;
        l[i] += mix * (hold[0] - l[i]);
        r[i] += mix * (hold[1] - r[i]);
      }
      return;
    }
    // FLANGER (6): 0.4..7 ms swept, feedback with AMOUNT; CHORUS (7): three taps 12..28 ms, depth with AMOUNT
    for (int i = 0; i < n; ++i) {
      lfo += (type == 6 ? 0.18 : 0.6) / kSR;
      if (lfo >= 1.0) lfo -= 1.0;
      float s = sinf(kTwoPi * (float)lfo), s2 = sinf(kTwoPi * (float)(lfo + 0.33)), s3 = sinf(kTwoPi * (float)(lfo + 0.66));
      float yl, yr;
      if (type == 6) {
        float d = (0.4f + 3.3f * (1.f + s)) * 0.001f * kSR;
        yl = rdl(0, d);
        yr = rdl(1, d * 1.07f);
        float fb = 0.2f + 0.7f * amt;
        dl[0][dw] = l[i] + fb * yl;
        dl[1][dw] = r[i] + fb * yr;
      } else {
        float dep = 2.f + 6.f * amt;
        yl = (rdl(0, (12.f + dep * s) * 0.001f * kSR) + rdl(0, (19.f + dep * s2) * 0.001f * kSR)) * 0.5f;
        yr = (rdl(1, (15.f + dep * s3) * 0.001f * kSR) + rdl(1, (23.f + dep * s) * 0.001f * kSR)) * 0.5f;
        dl[0][dw] = l[i];
        dl[1][dw] = r[i];
      }
      dw = (dw + 1) & 4095;
      l[i] = l[i] * (1.f - 0.5f * mix) + yl * 0.7f * mix;
      r[i] = r[i] * (1.f - 0.5f * mix) + yr * 0.7f * mix;
    }
  }
};

// ------------------------------------------------------------------------------------------------ the Erica delay
// LXRXXL BAZZLINE's delay (src/fx.c), itself written to behave as the Erica Synths DIY Delay's manual describes (not
// Erica's code): TIME 0..800 ms or synced (a divider / multiplier of the beat, never past 800 ms); TAPE (the time slews:
// turning it bends the pitch; darker, saturating repeats) or DIGITAL (a new time crossfades); IN LVL; FEEDBACK
// (self-oscillates at the top); DRY/WET a crossfade; HOLD loops the buffer TIME long, ADD layers the input onto it;
// REVERSE plays the repeats backwards.
static const int kDlyLen = 131072;
static const int kDlyMask = kDlyLen - 1;

struct EricaDelay {
  std::vector<float> buf[2];
  int w = 0;
  float rd = 0.4f * kSR, rdOld = 0.4f * kSR, xfade = 1.f, holdLen = 0.f;
  double wowPh = 0;
  int revPos = 0;
  float wetSm = 0, fbSm = 0, inSm = 0, lpz[2] = {0, 0}, hpz[2] = {0, 0};
  bool tape = true, sync = false, hold = false, add = false, rev = false;
  void init() {
    buf[0].assign(kDlyLen, 0.f);
    buf[1].assign(kDlyLen, 0.f);
  }
  static float seconds(float knob, bool sync, double bpm) {
    static const float F[11] = {1.f / 8, 1.f / 6, 1.f / 4, 1.f / 3, 1.f / 2, 1, 2, 3, 4, 6, 8};
    float t;
    if (!sync) t = knob * 0.8f;
    else {
      t = (float)(60.0 / bpm) * F[(int)lroundf(clampf(knob, 0, 1) * 10.f)];
      while (t > 0.8f) t *= 0.5f;
    }
    return t < 0.001f ? 0.001f : t;
  }
  static inline float rdbuf(const std::vector<float>& b, int w, float d) {
    int di = (int)d;
    float fr = d - di;
    float a = b[(w - di) & kDlyMask], c = b[(w - di - 1) & kDlyMask];
    return a + fr * (c - a);
  }
  static inline float sat(float x) {
    if (x > 3.f) return 1.f;
    if (x < -3.f) return -1.f;
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
  }
  void process(float* L, float* R, int n, float time_knob, float in_lvl, float fb, float wet, double bpm) {
    float target = seconds(time_knob, sync, bpm) * kSR;
    float fbT = fb * 1.08f, wetT = wet, inT = in_lvl * 1.6f;
    float lpA = 1.f - expf(-kTwoPi * (tape ? 4500.f : 15000.f) / kSR);
    float hpA = 1.f - expf(-kTwoPi * 40.f / kSR);
    if (!tape && fabsf(target - rd) > 1.f && xfade >= 1.f) { rdOld = rd; rd = target; xfade = 0.f; }
    if (!hold) holdLen = tape ? rd : target;
    for (int i = 0; i < n; ++i) {
      float wv[2];
      float d;
      wetSm += 0.001f * (wetT - wetSm);
      fbSm += 0.001f * (fbT - fbSm);
      inSm += 0.001f * (inT - inSm);
      if (tape) {
        rd += 0.00012f * (target - rd);
        wowPh += 0.55 / kSR;
        if (wowPh >= 1.0) wowPh -= 1.0;
        d = rd + 6.f * sinf(kTwoPi * (float)wowPh);
      } else d = rd;
      if (hold) d = holdLen;
      if (d < 2.f) d = 2.f;
      for (int c = 0; c < 2; ++c) {
        const std::vector<float>& b = buf[c];
        float y;
        if (rev) {
          float win = d, j0 = (float)revPos, j1 = fmodf(j0 + win * 0.5f, win);
          float w0 = 1.f - fabsf(2.f * j0 / win - 1.f), w1 = 1.f - fabsf(2.f * j1 / win - 1.f);
          y = rdbuf(b, w, 2.f * j0 + 1.f) * w0 + rdbuf(b, w, 2.f * j1 + 1.f) * w1;
        } else if (!tape && xfade < 1.f && !hold) {
          y = rdbuf(b, w, rdOld) * (1.f - xfade) + rdbuf(b, w, d) * xfade;
        } else y = rdbuf(b, w, d);
        wv[c] = y;
      }
      if (rev) { if (++revPos >= (int)d) revPos = 0; }
      if (!tape && xfade < 1.f) xfade += 1.f / 2048.f;
      for (int c = 0; c < 2; ++c) {
        std::vector<float>& b = buf[c];
        float in = c ? R[i] : L[i];
        float wr;
        if (hold) {
          float loop = rdbuf(b, w, holdLen);
          wr = loop + (add ? sat(in * inSm) * 0.7f : 0.f);
          wr = sat(wr * 0.98f);
        } else {
          float f = wv[c] * fbSm;
          lpz[c] += lpA * (f - lpz[c]);
          f = lpz[c];
          hpz[c] += hpA * (f - hpz[c]);
          f -= hpz[c];
          wr = sat(in * inSm + (tape ? sat(f * 1.2f) * 0.85f : f));
        }
        b[w] = wr;
        if (c) R[i] = in * (1.f - wetSm) + wv[c] * wetSm;
        else L[i] = in * (1.f - wetSm) + wv[c] * wetSm;
      }
      w = (w + 1) & kDlyMask;
    }
  }
};

// ------------------------------------------------------------------------------------------------ reverb helpers
// REVERSE: the reverb's tail cut in pieces, each played backwards with a rising envelope (a reverse-reverb swell).
struct Reverser {
  std::vector<float> rec[2], play[2];
  int len = 22050, pos = 0;
  void init() {
    for (int c = 0; c < 2; ++c) { rec[c].assign(2 * ET_SR, 0.f); play[c].assign(2 * ET_SR, 0.f); }
  }
  void process(float* l, float* r, int n, int want_len) {
    for (int i = 0; i < n; ++i) {
      rec[0][pos] = l[i];
      rec[1][pos] = r[i];
      int j = len - 1 - pos;
      float t = (float)pos / len;
      float env = t < 0.92f ? t / 0.92f : (1.f - t) / 0.08f;   // swell up, then a short fade into the next piece
      l[i] = play[0][j] * env;
      r[i] = play[1][j] * env;
      if (++pos >= len) {
        pos = 0;
        std::swap(rec[0], play[0]);
        std::swap(rec[1], play[1]);
        len = std::max(2205, std::min(want_len, 2 * ET_SR));
      }
    }
  }
};

// SHIMMER: an octave up (two crossfaded delay-line heads), fed back into the reverb.
struct Octaver {
  std::vector<float> b[2];
  int w = 0;
  double ph = 0;
  void init() { b[0].assign(8192, 0.f); b[1].assign(8192, 0.f); }
  void process(const float* l, const float* r, float* ol, float* orr, int n) {
    const float win = 2205.f;   // 50 ms
    for (int i = 0; i < n; ++i) {
      b[0][w] = l[i];
      b[1][w] = r[i];
      ph += 1.0 / win;   // the heads move at 2x: the delay shrinks by one sample per sample
      if (ph >= 1.0) ph -= 1.0;
      float p0 = (float)ph, p1 = p0 + 0.5f;
      if (p1 >= 1.f) p1 -= 1.f;
      float d0 = win * (1.f - p0), d1 = win * (1.f - p1);
      float g0 = sinf(3.14159265f * p0), g1 = sinf(3.14159265f * p1);
      for (int c = 0; c < 2; ++c) {
        auto rdx = [&](float d) {
          float p = (float)w - d;
          while (p < 0) p += 8192.f;
          int i0 = (int)p;
          float fr = p - i0;
          return b[c][i0 & 8191] + fr * (b[c][(i0 + 1) & 8191] - b[c][i0 & 8191]);
        };
        float y = rdx(d0) * g0 + rdx(d1) * g1;
        (c ? orr : ol)[i] = y * 0.7f;
      }
      w = (w + 1) & 8191;
    }
  }
};

// ------------------------------------------------------------------------------------------------ gate, stutter, duck
static const uint16_t kGatePatterns[12] = {
    0xFFFF,            // USER (not used: the steps themselves)
    0x5555,            // STRAIGHT: every other step
    0xAAAA,            // OFFBEAT
    0xB6DB,            // TRANCE 1: x.xx.xx.xx.xx.x.x (bit 0 = step 1)
    0xDDDD,            // TRANCE 2
    0x6DB6,            // GALLOP
    0x9249,            // SYNCOPATE
    0xC63B,            // BROKEN
    0x4925,            // 3-3-2
    0xF0F0,            // STUTTER
    0x00FF,            // HALF
    0x0000,            // RANDOM (a new pattern each bar)
};

struct Gate {
  uint16_t steps = 0xB6DB;
  float g = 1.f;
  int last_step = -1;
  Rng rng;
  int step_now = 0;
  // div: index into 1/1 .. 1/32; returns the step index for the LED
  void process(float* l, float* r, int n, const Tempo& t, double beat0, int div, float depth, float atk, float rel,
               float swing, bool random) {
    static const double kBeats[6] = {4.0, 2.0, 1.0, 0.5, 0.25, 0.125};
    double len = kBeats[std::min(5, std::max(0, div))];
    float ca = onepole(0.5f + 40.f * atk), cr = onepole(1.f + 400.f * rel);
    double bpf = t.beat_per_frame();
    for (int i = 0; i < n; ++i) {
      double b = beat0 + i * bpf;
      double sl = b / len;
      long si = (long)floor(sl);
      double frac = sl - si;
      int step = (int)(((si % 16) + 16) % 16);
      // swing: odd steps start later
      if (step & 1) { if (frac < swing * 0.45) step = (step + 15) % 16; }
      if (step != last_step) {
        if (step == 0 && random) steps = (uint16_t)(rng.next() | 0x0101);
        last_step = step;
      }
      step_now = step;
      float target = (steps >> step) & 1 ? 1.f : 1.f - depth;
      g += (target > g ? ca : cr) * (target - g);
      l[i] *= g;
      r[i] *= g;
    }
  }
};

struct Stutter {
  std::vector<float> hist[2], loop[2];
  int hw = 0, len = 0, pos = 0;
  bool active = false;
  float mixv = 0.f;
  void init() {
    for (int c = 0; c < 2; ++c) { hist[c].assign(2 * ET_SR, 0.f); loop[c].assign(2 * ET_SR, 0.f); }
  }
  void start(int frames) {
    len = std::max(64, std::min(frames, 2 * ET_SR - 1));
    for (int i = 0; i < len; ++i) {
      int k = hw - len + i;
      while (k < 0) k += 2 * ET_SR;
      loop[0][i] = hist[0][k];
      loop[1][i] = hist[1][k];
    }
    pos = 0;
    active = true;
  }
  void process(float* l, float* r, int n) {
    for (int i = 0; i < n; ++i) {
      hist[0][hw] = l[i];
      hist[1][hw] = r[i];
      if (++hw >= 2 * ET_SR) hw = 0;
      mixv += ((active ? 1.f : 0.f) - mixv) * 0.01f;
      if (mixv < 1e-4f && !active) continue;
      // a 2 ms crossfade over the loop's seam
      int xf = std::min(88, len / 4);
      float yl = loop[0][pos], yr = loop[1][pos];
      if (pos < xf) {
        float t = (float)pos / xf;
        yl = yl * t + loop[0][len - xf + pos] * (1.f - t);
        yr = yr * t + loop[1][len - xf + pos] * (1.f - t);
      }
      if (++pos >= len) pos = xf;
      l[i] += mixv * (yl - l[i]);
      r[i] += mixv * (yr - r[i]);
    }
  }
};

struct Duck {
  int stage = 0;
  float env = 0.f;
  void trigger() { stage = 1; }
  void process(float* l, float* r, int n, float depth, float rel_knob) {
    float atk = onepole(1.5f), rel = expf(-1.f / (0.01f * powf(200.f, rel_knob) * kSR));
    for (int i = 0; i < n; ++i) {
      if (stage == 1) { env += atk * (1.02f - env); if (env >= 1.f) { env = 1.f; stage = 2; } }
      else if (stage == 2) { env *= rel; if (env < 1e-4f) { env = 0.f; stage = 0; } }
      float gn = 1.f - depth * env;
      l[i] *= gn;
      r[i] *= gn;
    }
  }
};

// ------------------------------------------------------------------------------------------------ limiter
// RMXXXL's brickwall: stereo-linked look-ahead (64 samples) peak limiter, drive in, ceiling out.
struct Limiter {
  static const int kLook = 64;
  float delay[kLook][2];
  int pos = 0;
  float gain = 1.f;
  // the minimum of the last kLook "needed gains": a monotonic queue of (value, sample index)
  float qv[kLook + 1];
  uint32_t qi[kLook + 1];
  int qh = 0, qn = 0;
  uint32_t t = 0;
  void init() { memset(delay, 0, sizeof delay); pos = 0; gain = 1.f; qh = qn = 0; t = 0; }
  inline void push(float v) {
    const int cap = kLook + 1;
    while (qn > 0 && qv[(qh + qn - 1) % cap] >= v) qn--;
    qv[(qh + qn) % cap] = v;
    qi[(qh + qn) % cap] = t;
    qn++;
    while (qn > 0 && t - qi[qh] >= (uint32_t)kLook) { qh = (qh + 1) % cap; qn--; }
    t++;
  }
  void process(float* l, float* r, int n, float drive_db, float ceil_db) {
    float drive = db2g(drive_db), ceiling = db2g(ceil_db);
    float attack = 1.f - expf(-1.f / (kLook / 5.f));
    float release = 1.f - expf(-1.f / (0.08f * kSR));
    for (int i = 0; i < n; ++i) {
      float a = l[i] * drive, b = r[i] * drive;
      float peak = fmaxf(fabsf(a), fabsf(b));
      push(peak > ceiling ? ceiling / peak : 1.f);
      float ol = delay[pos][0], orr = delay[pos][1];
      delay[pos][0] = a;
      delay[pos][1] = b;
      pos = (pos + 1) % kLook;
      float target = qv[qh];
      gain += (target - gain) * (target < gain ? attack : release);
      l[i] = clampf(ol * gain, -ceiling, ceiling);
      r[i] = clampf(orr * gain, -ceiling, ceiling);
    }
  }
};

}  // namespace et
