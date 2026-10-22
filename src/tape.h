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
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <string>

#include "dsp.h"

namespace et {

static const int kTapeFrames = 180 * ET_SR;   // 3 minutes
static const int kLoopMax = 180 * ET_SR;      // 3 minutes

// Big buffers come from calloc: the kernel maps their pages only as they are first written, so a 3-minute buffer
// costs memory only for what has been recorded into it.
template <class T>
static T* big_alloc(size_t n) { return static_cast<T*>(calloc(n, sizeof(T))); }

struct Tape {
  int16_t* buf = nullptr;   // interleaved stereo
  std::atomic<int> w{0};
  int filled = 0;
  void init() { buf = big_alloc<int16_t>((size_t)kTapeFrames * 2); }
  void free_all() { free(buf); buf = nullptr; }
  inline void write(const float* l, const float* r, int n) {
    if (!buf) return;
    int wi = w.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
      buf[(size_t)wi * 2] = (int16_t)lrintf(clampf(l[i], -1.f, 1.f) * 32767.f);
      buf[(size_t)wi * 2 + 1] = (int16_t)lrintf(clampf(r[i], -1.f, 1.f) * 32767.f);
      if (++wi >= kTapeFrames) wi = 0;
      if (filled < kTapeFrames) filled++;
    }
    w.store(wi, std::memory_order_release);
  }
  // `frames` frames ending at write position `end` (oldest first), into l / r
  int read_back(int end, int frames, int have, float* l, float* r) const {
    frames = std::min(frames, have);
    for (int i = 0; i < frames; ++i) {
      int k = end - frames + i;
      if (k < 0) k += kTapeFrames;
      l[i] = buf[(size_t)k * 2] * (1.f / 32768.f);
      r[i] = buf[(size_t)k * 2 + 1] * (1.f / 32768.f);
    }
    return frames;
  }
};

// ------------------------------------------------------------------------------------------------ the looper
// Two loop buffers, A and B (3 minutes each). One is the loop; the other is the loop before the last change, so UNDO is
// a swap (and a second UNDO a redo). Whole-loop changes (CAPTURE, a STRETCH render) are written into the other buffer
// by the worker and swapped in; a new recording is written into the other buffer and swapped in when it closes; an
// overdub works on the loop itself while a copier, running ahead of the play head, keeps what was there in the other
// buffer. Nothing that copies a long loop ever runs on the audio thread.
struct Looper {
  float* bl[2] = {nullptr, nullptr};
  float* br[2] = {nullptr, nullptr};
  int cur = 0;                        // the loop: buffer cur; the one before: 1 - cur
  int len[2] = {0, 0};
  double pos = 0;
  bool playing = false, rec = false, od = false, rev = false;
  int speed = 0;                      // -1 HALF, 0, +1 DOUBLE
  int rec_target = 0;                 // frames (0: FREE, until REC again or 3 minutes)
  int rec_len = 0;                    // a new recording's length so far (into buffer 1 - cur)
  float fade = 0.f;                   // playback gain (clicks avoided on start / stop)
  // the overdub's copier: keeps the loop as it was before this layer, ahead of the play head
  int cp_pos = 0, cp_left = 0, cp_dir = 1;
  bool have_undo = false;
  // set by the audio thread while it writes a loop buffer (a recording, an overdub, the copier): the worker never
  // writes a buffer then (and the audio thread starts no write while the worker's `busy` is set)
  std::atomic<bool> writing{false};
  void update_writing() { writing.store(rec || od || cp_left > 0); }
  // where the play head was for each of the last frames (REC OUT writes the processed output back where it came from)
  static const int kHist = 1024;
  int hist[kHist];
  uint32_t hist_n = 0;

  bool init() {
    for (int b = 0; b < 2; ++b) { bl[b] = big_alloc<float>(kLoopMax); br[b] = big_alloc<float>(kLoopMax); }
    return bl[0] && bl[1] && br[0] && br[1];
  }
  void free_all() { for (int b = 0; b < 2; ++b) { free(bl[b]); free(br[b]); bl[b] = br[b] = nullptr; } }
  int length() const { return len[cur]; }
  float* L() { return bl[cur]; }
  float* R() { return br[cur]; }
  float* oL() { return bl[1 - cur]; }
  float* oR() { return br[1 - cur]; }

  static void edge_fades(float* l, float* r, int n) {
    int f = std::min(n / 4, 132);   // 3 ms
    for (int i = 0; i < f; ++i) {
      float g = (float)i / f;
      l[i] *= g; r[i] *= g;
      l[n - 1 - i] *= g; r[n - 1 - i] *= g;
    }
  }
  // the other buffer becomes the loop (a capture, a render, a closed recording); the old loop is kept for UNDO
  void swap_in(int new_len, bool play) {
    cur = 1 - cur;
    len[cur] = new_len;
    pos = 0;
    playing = play && new_len > 0;
    od = rec = false;
    cp_left = 0;
    have_undo = true;
  }
  bool undo() {
    if (!have_undo || rec || od || cp_left > 0) return false;
    cur = 1 - cur;
    if (pos >= len[cur]) pos = 0;
    playing = playing && len[cur] > 0;
    return true;
  }
  void clear() {
    if (rec || od) { rec = od = false; }
    cur = 1 - cur;
    len[cur] = 0;
    pos = 0;
    playing = false;
    have_undo = true;
  }
  void start_rec(int target) {
    rec = true;
    od = false;
    playing = false;
    rec_target = target;
    rec_len = 0;
  }
  // a new recording closes: it becomes the loop
  void end_rec() {
    rec = false;
    if (rec_len > 0) {
      edge_fades(oL(), oR(), rec_len);
      swap_in(rec_len, true);
    }
  }
  void start_overdub() {
    od = true;
    cp_pos = (int)pos;
    cp_left = len[cur];
    cp_dir = rev ? -1 : 1;
    len[1 - cur] = len[cur];
    have_undo = true;
  }
  // the copier: up to `n` frames of the loop into the other buffer, from cp_pos in the play direction
  void copy_ahead(int n) {
    int L_ = len[cur];
    if (cp_left <= 0 || L_ <= 0) return;
    float *sl = L(), *sr = R(), *dl = oL(), *dr = oR();
    int m = std::min(n, cp_left);
    for (int i = 0; i < m; ++i) {
      int k = cp_pos;
      dl[k] = sl[k];
      dr[k] = sr[k];
      cp_pos += cp_dir;
      if (cp_pos >= L_) cp_pos = 0;
      else if (cp_pos < 0) cp_pos = L_ - 1;
    }
    cp_left -= m;
  }
  double rate_of(float knob_speed) const {
    double rate = pow(2.0, (knob_speed - 0.5) * 2.0);   // +-1 octave
    if (fabsf(knob_speed - 0.5f) < 0.012f) rate = 1.0;
    if (speed < 0) rate *= 0.5;
    if (speed > 0) rate *= 2.0;
    return rev ? -rate : rate;
  }
  // il / ir: what a recording or an overdub (REC IN) takes; ol / orr: the loop's playback. knob_speed 0..1 (0.5 = 1x),
  // start 0..1. rec_in: REC takes il / ir now (REC OUT takes the processed output later: write_out()).
  void process(const float* il, const float* ir, float* ol, float* orr, int n, float knob_speed, float start, float fb,
               bool rec_in) {
    if (od) copy_ahead(8192);
    else if (cp_left > 0) copy_ahead(8192);
    if (rec && rec_in) {
      float *dl = oL(), *dr = oR();
      for (int i = 0; i < n && rec; ++i) {
        if (rec_len < kLoopMax) { dl[rec_len] = il[i]; dr[rec_len] = ir[i]; rec_len++; }
        if ((rec_target && rec_len >= rec_target) || rec_len >= kLoopMax) end_rec();
      }
    }
    int L_ = len[cur];
    double rate = rate_of(knob_speed);
    float want = playing && L_ > 0 ? 1.f : 0.f;
    float *sl = L(), *sr = R();
    for (int i = 0; i < n; ++i) {
      fade += (want - fade) * 0.004f;
      if (L_ <= 0 || fade < 1e-4f) {
        ol[i] = orr[i] = 0.f;
        hist[hist_n++ & (kHist - 1)] = -1;
        continue;
      }
      double p = pos + start * L_;
      while (p >= L_) p -= L_;
      while (p < 0) p += L_;
      int i0 = (int)p;
      float fr = (float)(p - i0);
      int i1 = i0 + 1 >= L_ ? 0 : i0 + 1;
      ol[i] = (sl[i0] + fr * (sl[i1] - sl[i0])) * fade;
      orr[i] = (sr[i0] + fr * (sr[i1] - sr[i0])) * fade;
      hist[hist_n++ & (kHist - 1)] = playing ? i0 : -1;
      if (od && playing && rec_in) {   // overdub (REC IN): the old sound fades by OVERDUB FB, the new one is added
        sl[i0] = sl[i0] * fb + il[i];
        sr[i0] = sr[i0] * fb + ir[i];
      }
      if (playing) {
        pos += rate;
        while (pos >= L_) pos -= L_;
        while (pos < 0) pos += L_;
      }
    }
  }
  // REC OUT: the plugin's output (`latency` frames behind the loop's play head) goes where it came from: an overdub
  // replaces those frames with what is heard (the loop is in it already), a new recording takes it in order.
  void write_out(const float* l, const float* r, int n, int latency) {
    if (rec) {
      float *dl = oL(), *dr = oR();
      for (int i = 0; i < n && rec; ++i) {
        if (rec_len < kLoopMax) { dl[rec_len] = l[i]; dr[rec_len] = r[i]; rec_len++; }
        if ((rec_target && rec_len >= rec_target) || rec_len >= kLoopMax) end_rec();
      }
      return;
    }
    if (!od) return;
    float *sl = L(), *sr = R();
    for (int i = 0; i < n; ++i) {
      uint32_t at = hist_n - (uint32_t)n + (uint32_t)i - (uint32_t)latency;
      int k = hist[at & (kHist - 1)];
      if (k < 0 || k >= len[cur]) continue;
      sl[k] = l[i];
      sr[k] = r[i];
    }
  }
};

// ------------------------------------------------------------------------------------------------ WAV files
// 24-bit PCM, 44.1 kHz stereo: <dir>/eviltunerrecNN.wav, NN the next number after the highest already there (01, 02 ..
// 99, 100 ..), never an existing file. Written to a temporary name, then renamed.
inline int next_rec_number(const std::string& dir) {
  int best = 0;
  DIR* d = opendir(dir.c_str());
  if (!d) return 1;
  while (struct dirent* e = readdir(d)) {
    int n = 0;
    char tail[8];
    if (sscanf(e->d_name, "eviltunerrec%d.%7s", &n, tail) == 2 && !strcasecmp(tail, "wav") && n > best) best = n;
  }
  closedir(d);
  return best + 1;
}

inline bool make_dirs(const std::string& path) {
  std::string acc;
  for (size_t i = 0; i < path.size(); ++i) {
    acc += path[i];
    if ((path[i] == '/' && i > 0) || i + 1 == path.size()) mkdir(acc.c_str(), 0755);
  }
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool write_wav24(const std::string& path, const float* l, const float* r, int frames) {
  std::string tmp = path + ".part";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  auto u32 = [&](uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; fwrite(b, 1, 4, f); };
  auto u16 = [&](uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; fwrite(b, 1, 2, f); };
  uint32_t data = (uint32_t)frames * 6;
  fwrite("RIFF", 1, 4, f); u32(36 + data); fwrite("WAVEfmt ", 1, 8, f); u32(16);
  u16(1); u16(2); u32(ET_SR); u32(ET_SR * 6); u16(6); u16(24);
  fwrite("data", 1, 4, f); u32(data);
  std::vector<uint8_t> chunk;
  chunk.reserve(4096 * 6);
  bool ok = true;
  for (int i = 0; i < frames && ok; ) {
    chunk.clear();
    int m = std::min(4096, frames - i);
    for (int k = 0; k < m; ++k)
      for (int c = 0; c < 2; ++c) {
        float x = clampf(c ? r[i + k] : l[i + k], -1.f, 1.f);
        int32_t v = (int32_t)lrintf(x * 8388607.f);
        chunk.push_back((uint8_t)v); chunk.push_back((uint8_t)(v >> 8)); chunk.push_back((uint8_t)(v >> 16));
      }
    ok = fwrite(chunk.data(), 1, chunk.size(), f) == chunk.size();
    i += m;
  }
  if (fclose(f) != 0) ok = false;
  if (!ok) { unlink(tmp.c_str()); return false; }
  return rename(tmp.c_str(), path.c_str()) == 0;
}

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

// ------------------------------------------------------------------------------------------------ the tape worker
// One thread for everything long on the TAPE page: CAPTURE (the tape into the other loop buffer), a STRETCH render
// (copy the loop, render, write the result into the other buffer) and SAVE (the loop to a WAV file). While a job reads
// or writes a loop buffer, `busy` is set: the audio thread starts no recording, overdub, swap or undo (it keeps
// playing). When a job has filled the other buffer, `swap_len` >= 0 tells the audio thread to swap it in.
struct TapeWorker {
  enum Kind { NONE, CAPTURE, STRETCH, SAVE };
  std::thread th;
  std::mutex mu;
  std::atomic<bool> quit{false};
  std::atomic<bool> busy{false};        // the loop buffers are in use by a job
  std::atomic<int> swap_len{-1};        // >= 0: the other buffer holds a new loop of this length
  std::atomic<int> running{NONE};
  // the job (set by the audio thread when not busy)
  int kind = NONE;
  Looper* lp = nullptr;
  Tape* tp = nullptr;
  int src = 0, n = 0;                   // the loop buffer to read, its length
  int tape_end = 0, tape_have = 0;      // CAPTURE: where the tape ended, how much it had
  int skind = 0;
  float samt = 0.5f;
  std::string dir;                      // SAVE: the folder
  // the result message, for the screen
  std::string msg;
  bool msg_new = false;

  void start() {
    th = std::thread([this] {
      while (!quit.load()) {
        int k;
        {
          std::lock_guard<std::mutex> lk(mu);
          k = kind;
          kind = NONE;
        }
        if (k == NONE) { usleep(10000); continue; }
        running = k;
        run(k);
        running = NONE;
      }
    });
  }
  void stop() { quit = true; if (th.joinable()) th.join(); }
  void post(const std::string& m) { std::lock_guard<std::mutex> lk(mu); msg = m; msg_new = true; }
  bool take_msg(std::string& out) {
    std::unique_lock<std::mutex> lk(mu, std::try_to_lock);
    if (!lk.owns_lock() || !msg_new) return false;
    out = msg;
    msg_new = false;
    return true;
  }
  // the audio thread: a job, when idle (returns false while one runs)
  bool submit(int k, Looper* l, Tape* t, int frames = -1) {
    if (busy.load() || running.load() != NONE || swap_len.load() >= 0) return false;
    std::unique_lock<std::mutex> lk(mu, std::try_to_lock);
    if (!lk.owns_lock() || kind != NONE) return false;
    lp = l;
    tp = t;
    src = l->cur;
    n = frames >= 0 ? frames : l->length();
    if (t) { tape_end = t->w.load(); tape_have = t->filled; }
    busy = true;
    kind = k;
    return true;
  }
  void run(int k) {
    if (k == CAPTURE) {
      int got = tp->read_back(tape_end, n, std::min(tape_have, kTapeFrames - 10 * ET_SR), lp->bl[1 - src], lp->br[1 - src]);
      if (got > 0) Looper::edge_fades(lp->bl[1 - src], lp->br[1 - src], got);
      char b[64];
      snprintf(b, sizeof b, got ? "CAPTURED %.1f S" : "THE TAPE IS EMPTY", got / (float)ET_SR);
      post(b);
      if (got > 0) swap_len = got;
      busy = false;
    } else if (k == STRETCH) {
      StretchJob j;
      j.kind = skind;
      j.amt = samt;
      j.L.assign(lp->bl[src], lp->bl[src] + n);
      j.R.assign(lp->br[src], lp->br[src] + n);
      busy = false;                     // the render works on its own copy: the loop is free meanwhile
      stretch_render(j);
      // write the result into the other buffer once the audio thread writes no loop buffer (a recording, an
      // overdub or its copier): take `busy` first, then check `writing` (the audio thread sets `writing`, then checks
      // `busy`), so one of the two always sees the other
      while (!quit.load()) {
        busy.store(true);
        if (!lp->writing.load()) break;
        busy.store(false);
        usleep(20000);
      }
      int m = std::min((int)j.L.size(), kLoopMax);
      int dst = 1 - lp->cur;
      std::copy(j.L.begin(), j.L.begin() + m, lp->bl[dst]);
      std::copy(j.R.begin(), j.R.begin() + m, lp->br[dst]);
      post("STRETCH DONE (UNDO TO GO BACK)");
      swap_len = m;
      busy = false;
    } else if (k == SAVE) {
      std::string text;
      if (n <= 0) text = "NO LOOP TO SAVE";
      else if (!make_dirs(dir)) text = "CAN'T WRITE THE RECORDINGS FOLDER";
      else {
        int num = next_rec_number(dir);
        char name[64];
        snprintf(name, sizeof name, "eviltunerrec%02d.wav", num);
        bool ok = write_wav24(dir + "/" + name, lp->bl[src], lp->br[src], n);
        char b[96];
        snprintf(b, sizeof b, ok ? "SAVED %s" : "SAVE FAILED (DISK FULL?)", name);
        text = b;
        for (auto& c : text) c = (char)toupper((unsigned char)c);
      }
      busy = false;
      post(text);
    }
  }
};

}  // namespace et
