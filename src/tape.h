// Evil Tuner XXL -- the TAPE page: the rolling tape (the last 3 minutes of the source, always recording), the looper
// and the STRETCH renders (whole-loop transformations, done on a worker thread, swapped in when ready).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <complex>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>

#include "dsp.h"

namespace et {

static const int kTapeFrames = 180 * ET_SR;   // 3 minutes, 16-bit stereo: 31.8 MB
static const int kLoopMax = 48 * ET_SR;       // 48 s: 8 bars down to 40 BPM

struct Tape {
  std::vector<int16_t> buf;
  int w = 0, filled = 0;
  void init() { buf.assign((size_t)kTapeFrames * 2, 0); }
  inline void write(const float* l, const float* r, int n) {
    for (int i = 0; i < n; ++i) {
      buf[(size_t)w * 2] = (int16_t)lrintf(clampf(l[i], -1.f, 1.f) * 32767.f);
      buf[(size_t)w * 2 + 1] = (int16_t)lrintf(clampf(r[i], -1.f, 1.f) * 32767.f);
      if (++w >= kTapeFrames) w = 0;
      if (filled < kTapeFrames) filled++;
    }
  }
  // the last `frames` frames (oldest first)
  int last(int frames, float* l, float* r) const {
    frames = std::min(frames, filled);
    for (int i = 0; i < frames; ++i) {
      int k = w - frames + i;
      if (k < 0) k += kTapeFrames;
      l[i] = buf[(size_t)k * 2] * (1.f / 32768.f);
      r[i] = buf[(size_t)k * 2 + 1] * (1.f / 32768.f);
    }
    return frames;
  }
};

struct Looper {
  std::vector<float> L, R, uL, uR;   // the loop and the one before (UNDO)
  int len = 0, ulen = 0;
  double pos = 0;
  bool playing = false, rec = false, od = false, rev = false;
  int speed = 0;                      // -1 HALF, 0, +1 DOUBLE
  int rec_target = 0;                 // frames (0: FREE, until REC again)
  float fade = 0.f;                   // playback gain (clicks avoided on start / stop)
  void init() {
    L.assign(kLoopMax, 0.f); R.assign(kLoopMax, 0.f);
    uL.assign(kLoopMax, 0.f); uR.assign(kLoopMax, 0.f);
  }
  void keep_undo() {
    std::copy(L.begin(), L.begin() + len, uL.begin());
    std::copy(R.begin(), R.begin() + len, uR.begin());
    ulen = len;
  }
  void undo() {
    if (!ulen) return;
    std::swap(L, uL);
    std::swap(R, uR);
    std::swap(len, ulen);
    pos = 0;
  }
  void set(const float* l, const float* r, int n) {   // a new loop (capture, a stretch render)
    keep_undo();
    n = std::min(n, kLoopMax);
    std::copy(l, l + n, L.begin());
    std::copy(r, r + n, R.begin());
    len = n;
    edge_fades();
    pos = 0;
    playing = len > 0;
    rec = od = false;
  }
  void edge_fades() {
    int f = std::min(len / 4, 132);   // 3 ms
    for (int i = 0; i < f; ++i) {
      float g = (float)i / f;
      L[i] *= g; R[i] *= g;
      L[len - 1 - i] *= g; R[len - 1 - i] *= g;
    }
  }
  void clear() { keep_undo(); len = 0; pos = 0; playing = rec = od = false; }
  void start_rec(int target) {
    keep_undo();
    len = 0;
    rec = true;
    od = false;
    playing = false;
    rec_target = target;
  }
  void end_rec() {
    rec = false;
    if (len > 0) { edge_fades(); playing = true; pos = 0; }
  }
  // in: what the looper hears; out: the loop's playback (added by the caller). knob_speed 0..1 (0.5 = 1x), start 0..1
  void process(const float* il, const float* ir, float* ol, float* orr, int n, float knob_speed, float start, float fb) {
    if (rec) {
      for (int i = 0; i < n && rec; ++i) {
        if (len < kLoopMax) { L[len] = il[i]; R[len] = ir[i]; len++; }
        if ((rec_target && len >= rec_target) || len >= kLoopMax) end_rec();
      }
    }
    double rate = pow(2.0, (knob_speed - 0.5) * 2.0);   // +-1 octave
    if (fabsf(knob_speed - 0.5f) < 0.012f) rate = 1.0;
    if (speed < 0) rate *= 0.5;
    if (speed > 0) rate *= 2.0;
    if (rev) rate = -rate;
    float want = playing && len > 0 ? 1.f : 0.f;
    if (want == 0.f && fade < 1e-4f) {   // nothing playing: nothing to do
      fade = 0.f;
      for (int i = 0; i < n; ++i) ol[i] = orr[i] = 0.f;
      return;
    }
    for (int i = 0; i < n; ++i) {
      fade += (want - fade) * 0.004f;
      if (len <= 0 || fade < 1e-4f) { ol[i] = orr[i] = 0.f; continue; }
      double p = pos + start * len;
      while (p >= len) p -= len;
      while (p < 0) p += len;
      int i0 = (int)p;
      float fr = (float)(p - i0);
      int i1 = i0 + 1 >= len ? 0 : i0 + 1;
      ol[i] = (L[i0] + fr * (L[i1] - L[i0])) * fade;
      orr[i] = (R[i0] + fr * (R[i1] - R[i0])) * fade;
      if (od && playing) {   // overdub: the old sound fades by OVERDUB FB, the new one is added
        L[i0] = L[i0] * fb + il[i];
        R[i0] = R[i0] * fb + ir[i];
      }
      pos += rate;
      while (pos >= len) pos -= len;
      while (pos < 0) pos += len;
    }
  }
};

// ------------------------------------------------------------------------------------------------ STRETCH renders
// Whole-loop transformations in the spirit of The Good Bits' STRETCH FX (clickysteve, MIT); written here for the loop:
// PAULSTRETCH (big windows, random phases: the loop smeared 2x..10x longer), FREEZE (one moment's spectrum held as a
// drone the loop's length), STUTTER (chunks repeated), CLOUD (scattered grains), HALF TAPE (half speed: an octave down,
// twice as long) and REVERSE.
struct Fft {
  int n = 0;
  std::vector<std::complex<float>> tw;
  std::vector<int> rev;
  void init(int size) {
    n = size;
    tw.resize(n / 2);
    for (int i = 0; i < n / 2; ++i) tw[i] = std::polar(1.f, -kTwoPi * i / n);
    rev.resize(n);
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; ++i) {
      int r = 0;
      for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
      rev[i] = r;
    }
  }
  void run(std::vector<std::complex<float>>& a, bool inverse) const {
    for (int i = 0; i < n; ++i) if (i < rev[i]) std::swap(a[i], a[rev[i]]);
    for (int len = 2; len <= n; len <<= 1) {
      int step = n / len;
      for (int i = 0; i < n; i += len)
        for (int j = 0; j < len / 2; ++j) {
          std::complex<float> w = tw[j * step];
          if (inverse) w = std::conj(w);
          std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
          a[i + j] = u + v;
          a[i + j + len / 2] = u - v;
        }
    }
    if (inverse) for (auto& x : a) x /= (float)n;
  }
};

struct StretchJob {
  std::vector<float> L, R;   // in, then out
  int kind = 0;
  float amt = 0.5f;
};

inline void stretch_render(StretchJob& j) {
  const int n = (int)j.L.size();
  if (n < 1024) return;
  Rng rng;
  rng.s = 0xC0FFEEu;
  std::vector<float> oL, oR;
  auto fades = [](std::vector<float>& a) {
    int m = (int)a.size(), f = std::min(m / 4, 441);
    for (int i = 0; i < f; ++i) { float g = (float)i / f; a[i] *= g; a[m - 1 - i] *= g; }
  };
  if (j.kind == 0 || j.kind == 1) {   // PAULSTRETCH / FREEZE
    const int N = j.kind == 0 ? 8192 : 4096, H = N / 4;
    float ratio = j.kind == 0 ? 2.f + 8.f * j.amt : 1.f;
    int out = std::min((int)(n * ratio), kLoopMax);
    oL.assign(out + N, 0.f);
    oR.assign(out + N, 0.f);
    std::vector<float> win(N), norm(out + N, 0.f);
    for (int i = 0; i < N; ++i) win[i] = 0.5f - 0.5f * cosf(kTwoPi * i / N);
    Fft fft;
    fft.init(N);
    std::vector<std::complex<float>> sp(N);
    std::vector<float> mag[2];
    for (int op = 0; op < out; op += H) {
      double ip = j.kind == 0 ? op / ratio : j.amt * (n - N);   // FREEZE: always the same moment
      int s0 = (int)ip;
      for (int c = 0; c < 2; ++c) {
        std::vector<float>& src = c ? j.R : j.L;
        if (j.kind == 0 || mag[c].empty()) {
          for (int i = 0; i < N; ++i) {
            int k = s0 + i;
            float x = (k >= 0 && k < n) ? src[k] : 0.f;
            sp[i] = std::complex<float>(x * win[i], 0.f);
          }
          fft.run(sp, false);
          mag[c].resize(N);
          for (int i = 0; i < N; ++i) mag[c][i] = std::abs(sp[i]);
        }
        for (int i = 1; i < N / 2; ++i) {
          float ph = rng.uni() * kTwoPi;
          sp[i] = std::polar(mag[c][i], ph);
          sp[N - i] = std::conj(sp[i]);
        }
        sp[0] = std::complex<float>(mag[c][0], 0.f);
        sp[N / 2] = std::complex<float>(mag[c][N / 2], 0.f);
        fft.run(sp, true);
        std::vector<float>& dst = c ? oR : oL;
        for (int i = 0; i < N; ++i) dst[op + i] += sp[i].real() * win[i];
      }
      for (int i = 0; i < N; ++i) norm[op + i] += win[i] * win[i];
    }
    for (int i = 0; i < out; ++i) {
      float g = norm[i] > 1e-3f ? 1.f / norm[i] : 0.f;
      oL[i] *= g;
      oR[i] *= g;
    }
    oL.resize(out);
    oR.resize(out);
    // level match: the render's peak brought to the loop's
    float pin = 1e-6f, pout = 1e-6f;
    for (int i = 0; i < n; ++i) pin = fmaxf(pin, fmaxf(fabsf(j.L[i]), fabsf(j.R[i])));
    for (int i = 0; i < out; ++i) pout = fmaxf(pout, fmaxf(fabsf(oL[i]), fabsf(oR[i])));
    float g = fminf(pin / pout, 8.f);
    for (int i = 0; i < out; ++i) { oL[i] *= g; oR[i] *= g; }
  } else if (j.kind == 2) {   // STUTTER: chunks of 1/32..1/4 of the loop, each played twice, every other one dropped
    int parts = 4 << (int)((1.f - j.amt) * 3.f + 0.5f);   // 4..32
    int ch = std::max(256, n / parts);
    oL.assign(n, 0.f);
    oR.assign(n, 0.f);
    int xf = std::min(64, ch / 4);
    for (int o = 0, k = 0; o < n; o += ch, ++k) {
      int src = (k / 2) * 2 * ch;   // chunk 0 twice, chunk 2 twice, ...
      for (int i = 0; i < ch && o + i < n; ++i) {
        int s = src + i;
        if (s >= n) s %= n;
        float g = i < xf ? (float)i / xf : (i > ch - xf ? (float)(ch - i) / xf : 1.f);
        oL[o + i] = j.L[s] * g;
        oR[o + i] = j.R[s] * g;
      }
    }
  } else if (j.kind == 3) {   // CLOUD: 60 ms grains scattered around where they are, some reversed
    oL.assign(n, 0.f);
    oR.assign(n, 0.f);
    std::vector<float> wsum(n, 0.f);
    const int G = 2646, hop = G / 4;
    float scat = j.amt * (float)ET_SR;
    for (int o = 0; o + G < n; o += hop) {
      int s = o + (int)(rng.bi() * scat);
      s = std::max(0, std::min(n - G - 1, s));
      bool rv = rng.uni() < j.amt * 0.4f;
      for (int i = 0; i < G; ++i) {
        float w = 0.5f - 0.5f * cosf(kTwoPi * i / G);
        int k = rv ? s + G - 1 - i : s + i;
        oL[o + i] += j.L[k] * w;
        oR[o + i] += j.R[k] * w;
        wsum[o + i] += w;
      }
    }
    for (int i = 0; i < n; ++i) if (wsum[i] > 0.1f) { oL[i] /= wsum[i]; oR[i] /= wsum[i]; }
  } else if (j.kind == 4) {   // HALF TAPE
    int out = std::min(n * 2, kLoopMax);
    oL.resize(out);
    oR.resize(out);
    for (int i = 0; i < out; ++i) {
      float p = i * 0.5f;
      int i0 = (int)p;
      float fr = p - i0;
      int i1 = std::min(i0 + 1, n - 1);
      oL[i] = j.L[i0] + fr * (j.L[i1] - j.L[i0]);
      oR[i] = j.R[i0] + fr * (j.R[i1] - j.R[i0]);
    }
  } else {   // REVERSE
    oL.assign(j.L.rbegin(), j.L.rend());
    oR.assign(j.R.rbegin(), j.R.rend());
  }
  fades(oL);
  fades(oR);
  j.L.swap(oL);
  j.R.swap(oR);
}

// the worker: one job at a time
struct Stretcher {
  std::thread th;
  std::mutex mu;
  StretchJob job;
  bool have = false, done = false;
  std::atomic<bool> busy{false}, quit{false};
  void start() {
    th = std::thread([this] {
      while (!quit.load()) {
        StretchJob j;
        bool go = false;
        {
          std::lock_guard<std::mutex> lk(mu);
          if (have) { j = std::move(job); have = false; go = true; }
        }
        if (!go) { usleep(20000); continue; }
        busy = true;
        stretch_render(j);
        {
          std::lock_guard<std::mutex> lk(mu);
          job = std::move(j);
          done = true;
        }
        busy = false;
      }
    });
  }
  void stop() { quit = true; if (th.joinable()) th.join(); }
  bool submit(StretchJob&& j) {
    std::unique_lock<std::mutex> lk(mu, std::try_to_lock);
    if (!lk.owns_lock() || busy.load() || have) return false;
    job = std::move(j);
    have = true;
    done = false;
    return true;
  }
  bool take(StretchJob& out) {
    std::unique_lock<std::mutex> lk(mu, std::try_to_lock);
    if (!lk.owns_lock() || !done) return false;
    out = std::move(job);
    done = false;
    return true;
  }
};

}  // namespace et
