// Evil Tuner XXL -- the plugin engine (mpc-vst-plugins wrapper/engine.h): parameters, MIDI, audio and the screen's
// state for one instance. See et.h for the chain and the threads.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include "engine.h"
}
#include "plugin_dir.h"

#include "et.h"
#include "dsp.h"
#include "tape.h"
#include "reverbs.h"
#include "param_defs.h"
#include "../third_party/loop-saboteur/Source/OutputStage.h"
#include "clouds/dsp/granular_processor.h"
#include <pthread.h>
#include <time.h>

using namespace et;

namespace {

const int kBlock = 128;
const char* const kSyncRates[11] = {"4 BAR", "2 BAR", "1 BAR", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4T", "1/8T", "1/16T"};
const double kSyncCpb[11] = {1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1, 2, 4, 8, 1.5, 3, 6};   // cycles per beat
const char* const kDlySync[11] = {"1/8 BEAT", "1/6 BEAT", "1/4 BEAT", "1/3 BEAT", "1/2 BEAT", "1 BEAT", "2 BEATS",
                                  "3 BEATS", "4 BEATS", "6 BEATS", "8 BEATS"};
const double kLoopBeats[7] = {0, 1, 2, 4, 8, 16, 32};   // FREE, 1/4 .. 8 BARS
enum { LIST_BROWSE = 0, LIST_SEARCH, LIST_FAVS };

struct Voice {
  int note = -1;
  float vel = 0.f;
  uint32_t age = 0;
  Adsr env;
  Osc osc;
};

struct Inst {
  // ---- parameters
  float v[P_COUNT];
  float eff[P_COUNT];                 // with modulation (knobs)
  float mod[P_COUNT];
  // toggles (keys)
  bool freeze = false, keys_mode = false, char_on = true, sat_on = false, lim_on = true, char_pre = false;
  bool dly_on = true, verb_on = true, gate_on = false, stut_key = false, duck_on = false, lsync[3] = {false, false, false};
  bool radio_sync = false;
  int filt_mode = 0, osc_wave = 2;
  // ---- radio / sources UI
  int list_mode = LIST_BROWSE;
  std::vector<Station> list;
  int page = 0;
  bool more = false, fetching = false, want_append = false;
  std::string list_msg;
  bool kbd = false;
  std::string query;
  Station cur;
  bool cur_valid = false;
  int want_src = SRC_RADIO;           // the source picked on the screen (RADIO even while nothing plays)
  bool radio_on = false;              // a station was asked to play
  bool wait_transport = false;        // SYNC: buffered, silent until MPC's PLAY
  int browse_countdown = 0;           // blocks until a filter change searches
  std::string title, status, ip;
  uint32_t ip_check = 0;
  // ---- KIWISDR
  KiwiDir kdir;
  std::vector<KiwiRx> kiwis;          // the receivers (the user's, then the built-in ones), with their status
  int kpage = 0;
  KiwiRx kcur;                        // the receiver picked
  bool kcur_valid = false;
  int kmode = KM_AM;
  double kfreq = 6070.0;              // the frequency (kHz): BAND jumps, TUNE steps, ENTER kHz types it
  bool kentry = false;                // the keyboard types a frequency (not a station search)
  std::string kquery;
  bool kprobe = true;                 // ask the shown receivers for their status
  std::vector<std::string> changed;   // knobs the engine moved itself (mpc_engine_changed)
  // ---- readout
  int touched = -1;
  uint32_t touched_at = 0;
  std::string flash;                  // a message instead of the readout for a moment
  uint32_t flash_until = 0;
  // ---- the engine
  Sources src;
  Directory dir;
  TapeWorker work;
  Tape tape;
  Looper looper;
  Grains grains;
  Filter filt;
  Saturator sat;
  EricaDelay dly;
  Reverser revr;
  Octaver octv;
  Gate gate;
  Stutter stut;
  Duck duck;
  Limiter lim;
  Lfo lfo[3];
  Follower fol;
  Voice voice[kVoices];
  uint32_t voice_clock = 0;
  loopsab::CrossfadingOutputStage chr;
  void* rv[2] = {nullptr, nullptr};   // Dragonfly Plate, Room
  float rv_sent[2][3];
  std::vector<float> shim[2];
  Tempo tempo;
  double own_beats = 0;
  bool host_pos = false;
  int stut_pad = -1;                  // the pad holding the stutter (-1: the key / none)
  bool pad_freeze = false;
  bool flowing = false;               // the source is playing out (buffered)
  double rd_frac = 0;
  float in_gain_sm = 0.f, vol_sm = 0.f;
  bool stretch_busy = false;
  // ---- TAPE (0.2.0)
  bool send = false, rec_out = false, auto_save = false;
  bool pending_rec = false, pending_save = false, pending_capture = false;   // waiting for the worker to finish
  bool layer_open = false;            // a recording or overdub is running (AUTO SAVE saves it when it closes)
  std::string rec_dir;                // where SAVE writes
  // ---- Clouds (0.2.0)
  clouds::GranularProcessor* cl = nullptr;
  uint8_t* cl_large = nullptr;
  uint8_t* cl_small = nullptr;
  int cl_mode_applied = 0, cl_reconf = 0;
  float cl_mix = 0.f;                 // fades Clouds in / out when the engine changes
  bool cl_trigger = false;
  pthread_t cl_thread;
  bool cl_thread_on = false;
  volatile bool cl_quit = false;
  pthread_mutex_t cl_mu;
  pthread_cond_t cl_wake;
  int last_note = -1;                 // KEYS: the note Clouds plays (the last one pressed)
  uint32_t clock = 0;                 // blocks
  uint32_t display_rev = 1;
  std::string data_dir, plugin_dir;
};

inline float knob(const Inst* s, int p) { return s->v[p]; }
inline int opt(const Inst* s, int p) { return (int)lroundf(s->v[p]); }
inline float E(const Inst* s, int p) { return s->eff[p]; }

void touch(Inst* s, int p) { s->touched = p; s->touched_at = s->clock; s->display_rev++; }
void flash(Inst* s, const std::string& m) { s->flash = m; s->flash_until = s->clock + 344 * 2; s->display_rev++; }   // ~2 s

// ------------------------------------------------------------------------------------------------ KIWISDR tuning
const KiwiBand& kband(const Inst* s) { return KIWI_BANDS[std::max(0, std::min(KIWI_NUM_BANDS - 1, opt(s, P_KIWI_BAND)))]; }
int kband_steps(const KiwiBand& b) { return b.nlist ? b.nlist - 1 : (int)lround((b.hi - b.lo) / b.step); }
int kband_default(const KiwiBand& b) { return b.nlist ? 0 : (int)lround((b.def - b.lo) / b.step); }

void moved(Inst* s, const char* key);
void flash(Inst* s, const std::string& m);
const double kKiwiTop = 30000.0;      // kHz: a KiwiSDR's range (0..30 MHz)

double kiwi_khz(const Inst* s) { return s->kfreq; }

double kiwi_step_khz(const Inst* s) { return KIWI_STEP_KHZ[std::max(0, std::min(KIWI_NUM_STEPS - 1, opt(s, P_KIWI_STEP)))]; }

// the STEP option nearest a band's own step
int kiwi_step_option(double khz) {
  int best = 3;
  for (int i = 0; i < KIWI_NUM_STEPS; ++i)
    if (fabs(KIWI_STEP_KHZ[i] - khz) < fabs(KIWI_STEP_KHZ[best] - khz)) best = i;
  return best;
}

// TUNE is an endless knob: it rests in the middle, each move is read from there (a Q-Link event or a data wheel click is
// one step whichever way it went; a bigger move, a drag or a fast spin, that many steps), and the knob is put back.
// A step is STEP on the grid of STEP (5 kHz steps land on ...5 and ...0), over the whole 0..30 MHz; in a list band
// (TIME SIGNALS ...), while the frequency is one of its entries, the next entry.
void kiwi_turn(Inst* s, float to) {
  float d = to - 0.5f;
  int steps = fabsf(d) < 0.03f ? (d > 0.f ? 1 : (d < 0.f ? -1 : 0)) : (int)lroundf(d * 128.f);
  steps = std::max(-64, std::min(64, steps));
  if (steps) {
    const KiwiBand& b = kband(s);
    int at = -1;
    for (int i = 0; i < b.nlist; ++i) if (fabs(b.list[i] - s->kfreq) < 0.001) at = i;
    if (at >= 0) s->kfreq = b.list[std::max(0, std::min(b.nlist - 1, at + steps))];
    else {
      double st = kiwi_step_khz(s), q = s->kfreq / st;
      double base = steps > 0 ? floor(q + 1e-9) : ceil(q - 1e-9);   // off the grid: the first step lands on it
      s->kfreq = std::max(0.0, std::min(kKiwiTop, (base + steps) * st));
      s->kfreq = floor(s->kfreq * 1000.0 + 0.5) / 1000.0;           // whole Hz
    }
  }
  s->v[P_KIWI_TUNE] = 0.5f;
  moved(s, "kiwi_tune");
}

// the passband for the mode, FILTER's share of the mode's range (Hz around the carrier)
KiwiTune kiwi_tune(const Inst* s) {
  KiwiTune t;
  t.khz = kiwi_khz(s);
  t.mode = s->kmode;
  float x = clampf(s->v[P_KIWI_BW], 0.f, 1.f);
  int w;
  switch (s->kmode) {
    case KM_USB: w = (int)lroundf(1500.f + 2100.f * x); t.lo = 300; t.hi = 300 + w; break;
    case KM_LSB: w = (int)lroundf(1500.f + 2100.f * x); t.lo = -300 - w; t.hi = -300; break;
    case KM_CW: w = (int)lroundf(100.f + 900.f * x); t.lo = 500 - w / 2; t.hi = 500 + w / 2; break;
    case KM_NFM: w = (int)lroundf(6000.f + 6000.f * x); t.lo = -w / 2; t.hi = w / 2; break;
    default: w = (int)lroundf(3000.f + 9000.f * x); t.lo = -w / 2; t.hi = w / 2; break;
  }
  return t;
}

void kiwi_retune(Inst* s) {
  if (s->want_src == SRC_KIWI) s->src.kiwi_tune(kiwi_tune(s));
  s->display_rev++;
}

void moved(Inst* s, const char* key) {
  if (std::find(s->changed.begin(), s->changed.end(), key) == s->changed.end()) s->changed.push_back(key);
}

// ------------------------------------------------------------------------------------------------ value texts
std::string value_text(const Inst* s, int p) {
  char b[64];
  const ParamDef& d = PARAM_DEFS[p];
  float v = s->v[p];
  if (d.nopts) return d.opts[(int)lroundf(v)];
  auto pct = [&](float x) { snprintf(b, sizeof b, "%d%%", (int)lroundf(x * 100.f)); return std::string(b); };
  auto ms = [&](float m) {
    if (m >= 1000.f) snprintf(b, sizeof b, "%.2f s", m / 1000.f);
    else snprintf(b, sizeof b, "%d ms", (int)lroundf(m));
    return std::string(b);
  };
  bool clouds = opt(s, P_ENGINE) == 1;
  if (clouds && (p == P_POS || p == P_SIZE || p == P_GRAINS || p == P_SHAPE || p == P_SPREAD || p == P_CLOUD || p == P_DETUNE)) {
    if (p == P_GRAINS && opt(s, P_GRAIN_SYNC) && opt(s, P_CL_MODE) == 0) {
      snprintf(b, sizeof b, "CHANCE %d%%", (int)lroundf((0.15f + 0.85f * v) * 100.f));
      return b;
    }
    return pct(v);
  }
  switch (p) {
    case P_IN_LVL: case P_VOLUME: {
      float g = p == P_VOLUME ? (v / 0.75f) * (v / 0.75f) : (v / 0.7f) * (v / 0.7f);
      if (g < 0.001f) return "-INF";
      snprintf(b, sizeof b, "%+.1f dB", 20.f * log10f(g));
      return b;
    }
    case P_SIZE: return ms(expmap(v, 8.f, 1000.f));
    case P_PITCH: {
      int st = pitch_semis(v, opt(s, P_SCALE));
      if (st == 0) return "0 st";
      snprintf(b, sizeof b, "%+d st", st);
      return b;
    }
    case P_GRAINS:
      if (opt(s, P_GRAIN_SYNC)) { snprintf(b, sizeof b, "CHANCE %d%%", (int)lroundf((0.15f + 0.85f * v) * 100.f)); return b; }
      snprintf(b, sizeof b, "%.1f /s", expmap(v, 1.5f, 90.f));
      return b;
    case P_SHAPE: return v < 0.47f ? "PERC " + pct(1.f - v * 2.f) : (v > 0.53f ? "REVERSE " + pct(v * 2.f - 1.f) : "SMOOTH");
    case P_CUTOFF: {
      float hz;
      if (s->filt_mode == 4) {
        if (v > 0.47f && v < 0.53f) return "OPEN";
        bool low = v <= 0.47f;
        float t = low ? v / 0.47f : (v - 0.53f) / 0.47f;
        hz = low ? expmap(t, 60.f, 20000.f) : expmap(t, 20.f, 9000.f);
        snprintf(b, sizeof b, hz >= 1000.f ? "%s %.1f kHz" : "%s %.0f Hz", low ? "LP" : "HP", hz >= 1000.f ? hz / 1000.f : hz);
        return b;
      }
      hz = expmap(v, 20.f, 20000.f);
      if (hz >= 1000.f) snprintf(b, sizeof b, "%.2f kHz", hz / 1000.f);
      else snprintf(b, sizeof b, "%.0f Hz", hz);
      return b;
    }
    case P_FLT_ENV: snprintf(b, sizeof b, "%+d%%", (int)lroundf((v - 0.5f) * 200.f)); return b;
    case P_LIM_DRIVE: snprintf(b, sizeof b, "+%.1f dB", v * 18.f); return b;
    case P_LIM_CEIL: snprintf(b, sizeof b, "%.1f dB", -12.f + 11.7f * v); return b;
    case P_DLY_TIME:
      if (s->dly.sync) return kDlySync[(int)lroundf(clampf(v, 0, 1) * 10.f)];
      return ms(fmaxf(1.f, v * 800.f));
    case P_VERB_SIZE: snprintf(b, sizeof b, "%.1f s", expmap(v, 0.3f, 8.f)); return b;
    case P_GATE_ATK: return ms(0.5f + 40.f * v);
    case P_GATE_REL: return ms(1.f + 400.f * v);
    case P_DUCK_REL: return ms(10.f * powf(200.f, v));
    case P_LOOP_START: return pct(v);
    case P_LOOP_SPEED: {
      double r = pow(2.0, (v - 0.5) * 2.0);
      if (fabsf(v - 0.5f) < 0.012f) r = 1;
      snprintf(b, sizeof b, "x%.2f", r);
      return b;
    }
    case P_STRETCH_AMT: {
      int k = opt(s, P_STRETCH);
      if (k == 0) { snprintf(b, sizeof b, "x%.1f LONGER", 2.f + 8.f * v); return b; }
      if (k == 1) return "AT " + pct(v);
      return pct(v);
    }
    case P_L1_RATE: case P_L2_RATE: case P_L3_RATE: {
      int l = p == P_L1_RATE ? 0 : (p == P_L2_RATE ? 1 : 2);
      if (s->lsync[l]) return kSyncRates[(int)lroundf(clampf(v, 0, 1) * 10.f)];
      snprintf(b, sizeof b, "%.2f Hz", expmap(v, 0.02f, 20.f));
      return b;
    }
    case P_KIWI_TUNE: snprintf(b, sizeof b, "%.2f kHz", kiwi_khz(s)); return b;   // the frequency, not the knob
    case P_KIWI_BW: {
      KiwiTune t = kiwi_tune(s);
      int w = t.hi - t.lo;
      if (w < 1000) snprintf(b, sizeof b, "%d Hz", w);
      else snprintf(b, sizeof b, "%.1f kHz", w / 1000.0);
      return b;
    }
    case P_ATK: return ms(expmap(v, 1.f, 4000.f));
    case P_DEC: return ms(expmap(v, 5.f, 4000.f));
    case P_REL: return ms(expmap(v, 5.f, 8000.f));
    default: return pct(v);
  }
}

// the PET knobs' names: XXL GRAINS' own, or what the knob does in Clouds' mode (the Clouds and Parasites manuals' names,
// as RMXXXL shows them)
const char* knob_name(const Inst* s, int p) {
  if (opt(s, P_ENGINE) != 1) return PARAM_DEFS[p].name;
  int m = std::max(0, std::min(5, opt(s, P_CL_MODE)));
  //                                   GRANULAR       STRETCH       LOOPING DELAY  SPECTRAL      OLIVERB        RESONESTOR
  static const char* kPos[6]    = {"POSITION",   "SCRUB",      "DELAY TIME", "BUFFER",     "PREDELAY",   "BURST"};
  static const char* kSize[6]   = {"SIZE",       "WINDOW",     "LOOP SIZE",  "WARP",       "ROOM SIZE",  "CHORD"};
  static const char* kShape[6]  = {"TEXTURE",    "FILTER",     "FILTER",     "QUANTIZE",   "DAMPING",    "DAMPING"};
  static const char* kDens[6]   = {"DENSITY",    "DIFFUSION",  "DIFFUSION",  "REFRESH",    "DECAY",      "DECAY"};
  static const char* kPitch[6]  = {"PITCH",      "PITCH",      "PITCH",      "PITCH",      "SHIMMER",    "PITCH"};
  static const char* kSpread[6] = {"SPREAD",     "SPREAD",     "SPREAD",     "SPREAD",     "DIFFUSION",  "VOICES L/R"};
  static const char* kFb[6]     = {"FEEDBACK",   "FEEDBACK",   "FEEDBACK",   "FEEDBACK",   "MOD SPEED",  "HARMONICS"};
  static const char* kVerb[6]   = {"CLOUD VERB", "CLOUD VERB", "CLOUD VERB", "CLOUD VERB", "MOD AMOUNT", "SCATTER"};
  switch (p) {
    case P_POS: return kPos[m];
    case P_SIZE: return kSize[m];
    case P_SHAPE: return kShape[m];
    case P_GRAINS: return kDens[m];
    case P_PITCH: return kPitch[m];
    case P_SPREAD: return kSpread[m];
    case P_CLOUD: return kFb[m];
    case P_DETUNE: return kVerb[m];
    case P_MIX: return "CLOUDS MIX";
    default: return PARAM_DEFS[p].name;
  }
}

// ------------------------------------------------------------------------------------------------ the station list
std::string row_text(const Station& st) {
  char b[96];
  std::string name = st.name;
  if (name.size() > 34) name = name.substr(0, 33) + "~";
  if (st.bitrate > 0) snprintf(b, sizeof b, "%s  %dK", name.c_str(), st.bitrate);
  else snprintf(b, sizeof b, "%s", name.c_str());
  return b;
}

void request_list(Inst* s, bool append) {
  Query q;
  int sort = opt(s, P_SORT);
  static const char* kOrder[6] = {"clickcount", "votes", "clicktrend", "name", "bitrate", "random"};
  q.order = kOrder[sort];
  q.reverse = sort != 3;
  static const int kKbps[6] = {0, 64, 96, 128, 192, 320};
  q.bitrate_max = kKbps[opt(s, P_KBPS)];
  q.countrycode = COUNTRY_CODES[opt(s, P_COUNTRY)];
  if (s->list_mode == LIST_SEARCH) {
    q.kind = 1;
    q.text = s->query;
  } else {
    q.kind = 0;
    int g = opt(s, P_GENRE);
    q.tag = g ? GENRE_TAGS[g] : "";
  }
  q.offset = append ? (int)s->list.size() : 0;
  s->want_append = append;
  if (!append) { s->page = 0; s->more = false; }
  s->dir.request(q);
  s->fetching = true;
  s->list_msg = "SEARCHING...";
  s->display_rev++;
}

void show_favs(Inst* s) {
  s->list_mode = LIST_FAVS;
  s->list = s->dir.favs;
  s->page = 0;
  s->more = false;
  s->fetching = false;
  s->list_msg = s->list.empty() ? "NO FAVOURITES YET: PRESS FAV WHILE A STATION PLAYS" : "";
  s->display_rev++;
}

int playing_index(const Inst* s) {
  if (!s->cur_valid) return -1;
  for (size_t i = 0; i < s->list.size(); ++i)
    if ((!s->cur.uuid.empty() && s->list[i].uuid == s->cur.uuid) || s->list[i].url == s->cur.url) return (int)i;
  return -1;
}

void play_station(Inst* s, const Station& st) {
  s->cur = st;
  s->cur_valid = true;
  s->want_src = SRC_RADIO;
  s->radio_on = true;
  s->title.clear();
  s->status = "CONNECTING";
  s->src.play_radio(st);
  s->dir.click(st.uuid);
  s->flowing = false;
  s->wait_transport = s->radio_sync && !s->tempo.playing;
  s->display_rev++;
}

void step_station(Inst* s, int dirn) {
  if (s->list.empty()) return;
  int i = playing_index(s);
  i = i < 0 ? (dirn > 0 ? 0 : (int)s->list.size() - 1) : (i + dirn + (int)s->list.size()) % (int)s->list.size();
  s->page = i / NUM_ROWS;
  play_station(s, s->list[i]);
}

// ------------------------------------------------------------------------------------------------ the receivers (KIWISDR)
std::string kiwi_row_text(const KiwiRx& r) {
  char b[128];
  std::string name = r.name;
  if (name.size() > 24) name = name.substr(0, 23) + "~";
  const char* tail = "";
  char users[32] = "";
  if (r.offline) tail = "  OFFLINE";
  else if (r.users >= 0 && r.users_max > 0) {
    if (r.users >= r.users_max) tail = "  FULL";
    else { snprintf(users, sizeof users, "  %d/%d", r.users, r.users_max); tail = users; }
  }
  snprintf(b, sizeof b, "%s  %s%s", name.c_str(), r.loc.c_str(), tail);
  return b;
}

int kiwi_index(const Inst* s) {
  if (!s->kcur_valid) return -1;
  for (size_t i = 0; i < s->kiwis.size(); ++i) if (s->kiwis[i].id() == s->kcur.id()) return (int)i;
  return -1;
}

void kiwi_probe_page(Inst* s) {
  std::vector<std::string> ids;
  for (int r = 0; r < NUM_ROWS; ++r) {
    int i = s->kpage * NUM_ROWS + r;
    if (i < (int)s->kiwis.size()) ids.push_back(s->kiwis[i].id());
  }
  if (s->kdir.probe(ids)) s->kprobe = false;
}

void play_kiwi(Inst* s, const KiwiRx& r) {
  s->kcur = r;
  s->kcur_valid = true;
  s->want_src = SRC_KIWI;
  s->kbd = false;
  s->title.clear();
  s->status = "CONNECTING";
  s->src.play_kiwi(r, kiwi_tune(s));
  s->flowing = false;
  s->wait_transport = s->radio_sync && !s->tempo.playing;
  s->display_rev++;
}

void step_kiwi(Inst* s, int dirn) {
  if (s->kiwis.empty()) return;
  int n = (int)s->kiwis.size();
  int i = kiwi_index(s);
  i = i < 0 ? (dirn > 0 ? 0 : n - 1) : (i + dirn + n) % n;
  if (s->kpage != i / NUM_ROWS) { s->kpage = i / NUM_ROWS; s->kprobe = true; }
  play_kiwi(s, s->kiwis[i]);
}

void set_kiwi_mode(Inst* s, int m) {
  s->kmode = m;
  touch(s, P_KIWI_BW);
  kiwi_retune(s);
}

// BAND: a jump to the band's own frequency, mode and step (TUNE goes on from there, anywhere)
void kiwi_band_changed(Inst* s) {
  const KiwiBand& b = kband(s);
  s->kfreq = b.def;
  s->kmode = b.mode;
  if (!b.nlist) {
    s->v[P_KIWI_STEP] = (float)kiwi_step_option(b.step);
    moved(s, "kiwi_step");
  }
  kiwi_retune(s);
}

// ENTER kHz: the keyboard types a frequency
void kiwi_enter(Inst* s) {
  double f = atof(s->kquery.c_str());
  s->kbd = false;
  s->kentry = false;
  if (s->kquery.empty()) return;
  if (f < 0 || f > kKiwiTop) { flash(s, "0 .. 30000 kHz"); return; }
  s->kfreq = f;
  kiwi_retune(s);
}

int view_of(const Inst* s) {
  if (s->want_src == SRC_PHONE) return 2;
  if (s->want_src == SRC_AIRPLAY) return 3;
  if (s->want_src == SRC_KIWI) return s->kbd ? 1 : 4;
  return s->kbd ? 1 : 0;
}

// ------------------------------------------------------------------------------------------------ keys
void set_filter(Inst* s, int m) {
  if (m == 4 && s->filt_mode != 4) s->v[P_CUTOFF] = 0.5f;   // DUAL starts open, in the middle
  else if (m != 4 && s->filt_mode == 4) s->v[P_CUTOFF] = m == 1 ? 0.f : 1.f;
  else if (m == 1 && s->filt_mode != 1) s->v[P_CUTOFF] = 0.f;
  else if (m != 1 && s->filt_mode == 1) s->v[P_CUTOFF] = 1.f;
  s->filt_mode = m;
  touch(s, P_CUTOFF);
}

void randomize(Inst* s) {
  static const int kP[] = {P_POS, P_SIZE, P_PITCH, P_GRAINS, P_SPREAD, P_CLOUD, P_DETUNE, P_SHAPE, P_CUTOFF, P_RESO, P_FLT_ENV};
  Rng& r = s->grains.rng;
  for (int p : kP) {
    float x = r.uni();
    if (p == P_PITCH) x = r.uni() < 0.5f ? 0.5f : 0.5f + (float)((int)(r.bi() * 12.f)) / 48.f;   // octaves / fifths often
    if (p == P_CUTOFF && s->filt_mode == 0) x = 0.45f + 0.55f * x;
    if (p == P_CUTOFF && s->filt_mode == 1) x = 0.5f * x;
    if (p == P_RESO) x *= 0.7f;
    s->v[p] = x;
  }
  flash(s, "RANDOM GRAINS");
}

int capture_frames(Inst* s) {
  int k = opt(s, P_LOOP_LEN);
  double beats = k ? kLoopBeats[k] : 8.0;   // FREE: the last two bars
  return std::min((int)(beats * 60.0 / s->tempo.bpm * kSR), kLoopMax);
}

// the worker's jobs: tried now, else kept pending and tried every block until the worker is free
bool try_capture(Inst* s) {
  Looper& L = s->looper;
  if (L.rec || L.od) return false;
  L.cp_left = 0;                        // a capture replaces the loop: the last overdub's copier stops
  L.update_writing();
  int frames = capture_frames(s);
  return s->work.submit(TapeWorker::CAPTURE, &L, &s->tape, frames);
}

bool try_save(Inst* s) {
  Looper& L = s->looper;
  if (L.rec || L.od || L.length() <= 0) return false;
  s->work.dir = s->rec_dir;
  return s->work.submit(TapeWorker::SAVE, &L, nullptr);
}

void loop_capture(Inst* s) {
  if (try_capture(s)) { s->pending_capture = false; flash(s, "CAPTURING..."); }
  else { s->pending_capture = true; flash(s, "CAPTURE: WAITING FOR THE TAPE"); }
}

void request_save(Inst* s) {
  if (s->looper.length() <= 0 && !s->looper.rec) { flash(s, "NO LOOP TO SAVE"); return; }
  if (try_save(s)) { s->pending_save = false; flash(s, "SAVING..."); }
  else s->pending_save = true;
}

// REC: a new loop, or an overdub on the playing loop, or the end of either
void rec_press(Inst* s) {
  Looper& L = s->looper;
  if (L.rec) { L.end_rec(); L.update_writing(); s->layer_open = false; if (s->auto_save) request_save(s); return; }
  if (L.od) {
    L.od = false;
    L.update_writing();
    s->layer_open = false;
    flash(s, "OVERDUB OFF (UNDO TAKES IT OUT)");
    if (s->auto_save) request_save(s);
    return;
  }
  // a new layer may not start while the worker holds the loop: announce `writing`, then check `busy`
  L.writing.store(true);
  if (s->work.busy.load() || s->work.swap_len.load() >= 0) {
    L.update_writing();
    s->pending_rec = true;
    flash(s, "REC: WAITING FOR THE TAPE");
    return;
  }
  s->pending_rec = false;
  if (L.length() > 0 && L.playing) {
    L.start_overdub();
    flash(s, s->rec_out ? "OVERDUB (OUT): WHAT YOU HEAR BECOMES THE LOOP" : "OVERDUB: THE INPUT GOES ON TOP");
  } else {
    int k = opt(s, P_LOOP_LEN);
    L.start_rec(k ? (int)(kLoopBeats[k] * 60.0 / s->tempo.bpm * kSR) : 0);
    flash(s, k ? "RECORDING THE LOOP" : "RECORDING: REC AGAIN TO CLOSE THE LOOP");
  }
  s->layer_open = true;
  L.update_writing();
}

void press(Inst* s, int p) {
  if (s->kentry && p >= P_KB_1 && p <= P_KB_M) {   // a frequency: digits only
    const char* c = PARAM_DEFS[p].name;
    if (c[0] >= '0' && c[0] <= '9' && s->kquery.size() < 9) s->kquery += c;
    s->display_rev++;
    return;
  }
  if (s->kentry && (p == P_KB_SPACE || p == P_KB_DEL || p == P_KB_CLR || p == P_KB_GO || p == P_KB_CLOSE)) {
    if (p == P_KB_SPACE && s->kquery.find('.') == std::string::npos && s->kquery.size() < 9) s->kquery += s->kquery.empty() ? "0." : ".";
    else if (p == P_KB_DEL && !s->kquery.empty()) s->kquery.pop_back();
    else if (p == P_KB_CLR) s->kquery.clear();
    else if (p == P_KB_GO) kiwi_enter(s);
    else if (p == P_KB_CLOSE) { s->kbd = false; s->kentry = false; }
    s->display_rev++;
    return;
  }
  if (p >= P_KB_1 && p <= P_KB_M) {   // the keyboard's characters
    if (s->query.size() < 40) s->query += PARAM_DEFS[p].name;
    s->display_rev++;
    return;
  }
  if (p >= P_GS1 && p <= P_GS16) {
    s->gate.steps ^= (uint16_t)(1u << (p - P_GS1));
    s->v[P_GATE_PAT] = 0;   // USER
    touch(s, P_GATE_PAT);
    return;
  }
  switch (p) {
    // ---- RADIO
    case P_SRC_RADIO:
      s->want_src = SRC_RADIO;
      if (s->cur_valid) play_station(s, s->cur);
      else { s->src.stop(); s->radio_on = false; }
      break;
    case P_SRC_PHONE: s->want_src = SRC_PHONE; s->kbd = false; s->kentry = false; s->src.play_phone(); s->title.clear(); s->flowing = false; s->ip = s->src.ip_text(); break;
    case P_SRC_AIR: s->want_src = SRC_AIRPLAY; s->kbd = false; s->kentry = false; s->src.play_airplay(); s->title.clear(); s->flowing = false; s->ip = s->src.ip_text(); break;
    case P_SRC_KIWI:
      s->kbd = false;
      s->kentry = false;
      s->kprobe = true;
      if (s->kcur_valid) play_kiwi(s, s->kcur);
      else { s->want_src = SRC_KIWI; s->src.stop(); s->title.clear(); s->flowing = false; }
      break;
    case P_KIWI_ENTER: s->kbd = true; s->kentry = true; s->kquery.clear(); break;
    case P_KM_AM: set_kiwi_mode(s, KM_AM); break;
    case P_KM_USB: set_kiwi_mode(s, KM_USB); break;
    case P_KM_LSB: set_kiwi_mode(s, KM_LSB); break;
    case P_KM_CW: set_kiwi_mode(s, KM_CW); break;
    case P_KM_NFM: set_kiwi_mode(s, KM_NFM); break;
    case P_SEARCH: s->kentry = false; s->kbd = !s->kbd; if (s->want_src != SRC_RADIO) { s->want_src = SRC_RADIO; s->src.stop(); } break;
    case P_FAVS: show_favs(s); break;
    case P_TOP: s->list_mode = LIST_BROWSE; request_list(s, false); break;
    case P_FAV:
      if (s->cur_valid) {
        bool was = s->dir.is_fav(s->cur.uuid) || std::any_of(s->dir.favs.begin(), s->dir.favs.end(), [&](const Station& f) { return f.url == s->cur.url; });
        s->dir.toggle_fav(s->cur);
        flash(s, was ? "REMOVED FROM FAVOURITES" : "ADDED TO FAVOURITES");
        if (s->list_mode == LIST_FAVS) show_favs(s);
      } else flash(s, "PLAY A STATION FIRST");
      break;
    case P_PREV: if (s->want_src == SRC_KIWI) step_kiwi(s, -1); else step_station(s, -1); break;
    case P_NEXT: if (s->want_src == SRC_KIWI) step_kiwi(s, +1); else step_station(s, +1); break;
    case P_PLAY:
      if (s->want_src == SRC_KIWI) {
        if (s->kcur_valid) play_kiwi(s, s->kcur);
        else if (!s->kiwis.empty()) play_kiwi(s, s->kiwis[std::min((int)s->kiwis.size() - 1, s->kpage * NUM_ROWS)]);
      } else if (s->want_src == SRC_RADIO) {
        if (s->cur_valid) play_station(s, s->cur);
        else if (!s->list.empty()) play_station(s, s->list[s->page * NUM_ROWS < (int)s->list.size() ? s->page * NUM_ROWS : 0]);
      } else if (s->want_src == SRC_PHONE) s->src.play_phone();
      else s->src.play_airplay();
      break;
    case P_STOP: s->src.stop(); s->radio_on = false; s->wait_transport = false; s->flowing = false; break;
    case P_SYNC: s->radio_sync = !s->radio_sync; flash(s, s->radio_sync ? "SYNC: STARTS WITH MPC'S PLAY" : "SYNC OFF"); if (!s->radio_sync) s->wait_transport = false; break;
    case P_PG_PREV:
      if (s->want_src == SRC_KIWI) { if (s->kpage > 0) { s->kpage--; s->kprobe = true; } }
      else if (s->page > 0) s->page--;
      break;
    case P_PG_NEXT: {
      if (s->want_src == SRC_KIWI) {
        if ((s->kpage + 1) * NUM_ROWS < (int)s->kiwis.size()) { s->kpage++; s->kprobe = true; }
        break;
      }
      int pages = ((int)s->list.size() + NUM_ROWS - 1) / NUM_ROWS;
      if (s->page + 1 < pages) s->page++;
      else if (s->more && !s->fetching && s->list_mode != LIST_FAVS) { s->page++; request_list(s, true); }
      break;
    }
    case P_KB_SPACE: if (!s->query.empty() && s->query.back() != ' ' && s->query.size() < 40) s->query += ' '; break;
    case P_KB_DEL: if (!s->query.empty()) s->query.pop_back(); break;
    case P_KB_CLR: s->query.clear(); break;
    case P_KB_GO:
      while (!s->query.empty() && s->query.back() == ' ') s->query.pop_back();
      s->kbd = false;
      if (s->query.empty()) { s->list_mode = LIST_BROWSE; request_list(s, false); }
      else { s->list_mode = LIST_SEARCH; request_list(s, false); }
      break;
    case P_KB_CLOSE: s->kbd = false; break;
    // ---- PET
    case P_FREEZE: s->freeze = !s->freeze; s->grains.frozen = s->freeze; flash(s, s->freeze ? "FROZEN" : "LIVE GRAINS"); break;
    case P_KEYS: s->keys_mode = !s->keys_mode; for (auto& vc : s->voice) { vc.env.gate(false); vc.note = -1; } flash(s, s->keys_mode ? "KEYS: THE PADS PLAY THE GRAINS" : "DRONE: GRAINS ALWAYS RUN"); break;
    case P_FT_LP: set_filter(s, 0); break;
    case P_FT_HP: set_filter(s, 1); break;
    case P_FT_BP: set_filter(s, 2); break;
    case P_FT_COMB: set_filter(s, 3); break;
    case P_FT_DUAL: set_filter(s, 4); break;
    case P_OSC: {
      static const char* kW[4] = {"OSC SINE", "OSC TRI", "OSC SAW", "OSC SQUARE"};
      s->osc_wave = (s->osc_wave + 1) % 4;
      flash(s, kW[s->osc_wave]);
      break;
    }
    case P_RANDOM: randomize(s); break;
    // ---- FX / SPACE
    case P_CHAR_ON: s->char_on = !s->char_on; break;
    case P_SAT_ON: s->sat_on = !s->sat_on; break;
    case P_LIM_ON: s->lim_on = !s->lim_on; break;
    case P_PREPOST: s->char_pre = !s->char_pre; flash(s, s->char_pre ? "CHARACTER BEFORE THE GRAINS" : "CHARACTER AFTER THE FILTER"); break;
    case P_DLY_ON: s->dly_on = !s->dly_on; break;
    case P_D_MODE: s->dly.tape = !s->dly.tape; flash(s, s->dly.tape ? "DELAY: TAPE" : "DELAY: DIGITAL"); break;
    case P_D_SYNC: s->dly.sync = !s->dly.sync; touch(s, P_DLY_TIME); break;
    case P_D_HOLD: s->dly.hold = !s->dly.hold; break;
    case P_D_ADD: s->dly.add = !s->dly.add; break;
    case P_D_REV: s->dly.rev = !s->dly.rev; s->dly.revPos = 0; break;
    case P_VERB_ON: s->verb_on = !s->verb_on; break;
    // ---- GATE
    case P_GATE_ON: s->gate_on = !s->gate_on; break;
    case P_STUTTER: {
      s->stut_key = !s->stut_key;
      static const double kB[5] = {1.0, 0.5, 0.25, 0.125, 0.0625};
      if (s->stut_key) s->stut.start((int)(kB[opt(s, P_STUT_LEN)] * 60.0 / s->tempo.bpm * kSR));
      else s->stut.active = false;
      break;
    }
    case P_DUCK_ON: s->duck_on = !s->duck_on; break;
    // ---- TAPE
    case P_CAPTURE: loop_capture(s); break;
    case P_L_REC: rec_press(s); break;
    case P_L_PLAY: if (s->looper.length()) { s->looper.playing = true; s->looper.pos = 0; } break;
    case P_L_STOP: {
      Looper& L = s->looper;
      bool closing = L.rec || L.od;
      if (L.rec) L.end_rec();
      L.od = false;
      L.playing = false;
      L.update_writing();
      s->pending_rec = false;
      if (closing) { s->layer_open = false; if (s->auto_save) request_save(s); }
      break;
    }
    case P_L_REV: s->looper.rev = !s->looper.rev; break;
    case P_L_HALF: s->looper.speed = s->looper.speed == -1 ? 0 : -1; break;
    case P_L_DOUBLE: s->looper.speed = s->looper.speed == 1 ? 0 : 1; break;
    case P_L_CLEAR:
      if (s->work.busy.load()) { flash(s, "WAIT: THE TAPE IS BUSY"); break; }
      s->looper.clear();
      s->looper.update_writing();
      s->layer_open = s->pending_rec = false;
      flash(s, "LOOP CLEARED (UNDO BRINGS IT BACK)");
      break;
    case P_UNDO:
      if (s->work.busy.load() || s->work.swap_len.load() >= 0) { flash(s, "WAIT: THE TAPE IS BUSY"); break; }
      flash(s, s->looper.undo() ? "UNDO (AGAIN: REDO)" : (s->looper.od || s->looper.rec ? "STOP THE RECORDING FIRST" : "NOTHING TO UNDO"));
      break;
    case P_TO_GRAINS:
      if (s->looper.length()) {
        s->grains.load(s->looper.L(), s->looper.R(), std::min(s->looper.length(), kGrainBuf));
        s->freeze = true;
        flash(s, "THE LOOP IS IN THE GRAINS (FREEZE)");
      } else flash(s, "NO LOOP: CAPTURE OR REC FIRST");
      break;
    case P_DO_STRETCH: {
      if (!s->looper.length()) { flash(s, "NO LOOP TO STRETCH"); break; }
      if (s->looper.rec || s->looper.od) { flash(s, "STOP THE RECORDING FIRST"); break; }
      s->work.skind = opt(s, P_STRETCH);
      s->work.samt = s->v[P_STRETCH_AMT];
      if (s->work.submit(TapeWorker::STRETCH, &s->looper, nullptr)) { s->stretch_busy = true; flash(s, "RENDERING..."); }
      else flash(s, "STILL BUSY: TRY AGAIN IN A MOMENT");
      break;
    }
    case P_SEND:
      s->send = !s->send;
      flash(s, s->send ? "SEND: THE LOOP GOES THROUGH PET, FX, SPACE, GATE" : "SEND OFF: THE LOOP PLAYS CLEAN");
      break;
    case P_REC_OUT:
      s->rec_out = !s->rec_out;
      flash(s, s->rec_out ? "REC OUT: RECORDS WHAT YOU HEAR" : "REC IN: RECORDS THE RADIO / PHONE / AIRPLAY");
      break;
    case P_SAVE:
      if (s->looper.rec || s->looper.od) rec_press(s);   // closes the layer (AUTO SAVE then saves it)
      if (!s->auto_save || s->looper.length() > 0) request_save(s);
      break;
    case P_AUTO_SAVE:
      s->auto_save = !s->auto_save;
      flash(s, s->auto_save ? "AUTO SAVE: EVERY RECORDING AND OVERDUB GOES TO A FILE" : "AUTO SAVE OFF");
      break;
    // ---- MOD
    case P_L1_SYNC: s->lsync[0] = !s->lsync[0]; touch(s, P_L1_RATE); break;
    case P_L2_SYNC: s->lsync[1] = !s->lsync[1]; touch(s, P_L2_RATE); break;
    case P_L3_SYNC: s->lsync[2] = !s->lsync[2]; touch(s, P_L3_RATE); break;
    default: break;
  }
  s->display_rev++;
}

bool led(const Inst* s, int p) {
  if (p >= P_GS1 && p <= P_GS16) {
    int k = p - P_GS1;
    bool on = (s->gate.steps >> k) & 1;
    if (s->gate_on && s->gate.step_now == k) return !on || ((s->clock >> 3) & 1);   // the running step blinks
    return on;
  }
  switch (p) {
    case P_SRC_RADIO: return s->want_src == SRC_RADIO;
    case P_SRC_PHONE: return s->want_src == SRC_PHONE;
    case P_SRC_AIR: return s->want_src == SRC_AIRPLAY;
    case P_SRC_KIWI: return s->want_src == SRC_KIWI;
    case P_KM_AM: return s->kmode == KM_AM;
    case P_KM_USB: return s->kmode == KM_USB;
    case P_KM_LSB: return s->kmode == KM_LSB;
    case P_KM_CW: return s->kmode == KM_CW;
    case P_KM_NFM: return s->kmode == KM_NFM;
    case P_SEARCH: return s->kbd || s->list_mode == LIST_SEARCH;
    case P_FAVS: return s->list_mode == LIST_FAVS;
    case P_TOP: return s->list_mode == LIST_BROWSE;
    case P_FAV: return s->cur_valid && (s->dir.is_fav(s->cur.uuid) || std::any_of(s->dir.favs.begin(), s->dir.favs.end(), [&](const Station& f) { return f.url == s->cur.url; }));
    case P_PLAY: {
      int st = s->src.state.load();
      if (s->wait_transport) return (s->clock >> 6) & 1;
      if (st == 0 || st == 4) return false;
      return s->flowing ? true : ((s->clock >> 4) & 1);   // connecting / buffering: blinks
    }
    case P_STOP: return s->src.state.load() == 4;
    case P_SYNC: return s->radio_sync;
    case P_FREEZE: return s->grains.frozen;
    case P_KEYS: return s->keys_mode;
    case P_FT_LP: return s->filt_mode == 0;
    case P_FT_HP: return s->filt_mode == 1;
    case P_FT_BP: return s->filt_mode == 2;
    case P_FT_COMB: return s->filt_mode == 3;
    case P_FT_DUAL: return s->filt_mode == 4;
    case P_OSC: return s->v[P_OSC_LVL] > 0.001f;
    case P_CHAR_ON: return s->char_on;
    case P_SAT_ON: return s->sat_on;
    case P_LIM_ON: return s->lim_on;
    case P_PREPOST: return s->char_pre;
    case P_DLY_ON: return s->dly_on;
    case P_D_MODE: return s->dly.tape;
    case P_D_SYNC: return s->dly.sync;
    case P_D_HOLD: return s->dly.hold;
    case P_D_ADD: return s->dly.add;
    case P_D_REV: return s->dly.rev;
    case P_VERB_ON: return s->verb_on;
    case P_GATE_ON: return s->gate_on;
    case P_STUTTER: return s->stut.active;
    case P_DUCK_ON: return s->duck_on;
    case P_L_REC: return s->looper.rec || s->looper.od || (s->pending_rec && ((s->clock >> 4) & 1));
    case P_L_PLAY: return s->looper.playing && s->looper.length() > 0;
    case P_SEND: return s->send;
    case P_REC_OUT: return s->rec_out;
    case P_AUTO_SAVE: return s->auto_save;
    case P_SAVE: return (s->pending_save || s->work.running.load() == TapeWorker::SAVE) && ((s->clock >> 4) & 1);
    case P_CAPTURE: return (s->pending_capture || s->work.running.load() == TapeWorker::CAPTURE) && ((s->clock >> 4) & 1);
    case P_L_REV: return s->looper.rev;
    case P_L_HALF: return s->looper.speed < 0;
    case P_L_DOUBLE: return s->looper.speed > 0;
    case P_DO_STRETCH: return s->stretch_busy && ((s->clock >> 4) & 1);
    case P_L1_SYNC: return s->lsync[0];
    case P_L2_SYNC: return s->lsync[1];
    case P_L3_SYNC: return s->lsync[2];
    default: return false;
  }
}

// ------------------------------------------------------------------------------------------------ texts
std::string st_name(const Inst* s) {
  if (s->want_src == SRC_KIWI) {
    char b[64];
    snprintf(b, sizeof b, "%.2f kHz  %s", kiwi_khz(s), KIWI_MODE_NAMES[s->kmode]);
    return b;
  }
  if (s->want_src == SRC_PHONE) return "PHONE  (ROC)";
  if (s->want_src == SRC_AIRPLAY) return "AIRPLAY";
  if (s->cur_valid) return s->cur.name;
  return "EVIL TUNER XXL";
}

std::string st_info(const Inst* s) {
  char b[96];
  int st = s->src.state.load();
  const char* state = st == 4 ? "ERROR" : (s->wait_transport ? "WAITING FOR PLAY" : (st == 0 ? "STOPPED" : (s->flowing ? "PLAYING" : (st == 1 ? "CONNECTING" : "BUFFERING"))));
  float buf = s->src.ring.fill() / kSR;
  if (s->want_src == SRC_KIWI) {
    if (!s->kcur_valid) return "KIWISDR: PICK A RECEIVER BELOW";
    float dbm = s->src.kiwi_rssi.load();
    KiwiTune t = kiwi_tune(s);
    char sm[24] = "S--";
    if (dbm > -190.f && st == 3) {   // S9 = -73 dBm, 6 dB an S unit
      int su = (int)floorf((dbm + 127.f) / 6.f);
      if (dbm > -73.f) snprintf(sm, sizeof sm, "S9+%d", (int)lroundf(dbm + 73.f));
      else snprintf(sm, sizeof sm, "S%d", su < 0 ? 0 : su);
    }
    snprintf(b, sizeof b, "%s %s  BW %.1fk  BUF %.1fs  %s", sm, dbm > -190.f && st == 3 ? (std::to_string((int)lroundf(dbm)) + " dBm").c_str() : "",
             (t.hi - t.lo) / 1000.f, buf, state);
    return b;
  }
  if (s->want_src == SRC_RADIO && s->cur_valid) {
    snprintf(b, sizeof b, "%s  %dK %s  BUF %.1fs  %s", s->cur.country.empty() ? "--" : s->cur.country.c_str(), s->cur.bitrate,
             s->cur.codec.c_str(), buf, state);
    return b;
  }
  if (s->want_src != SRC_RADIO) {
    snprintf(b, sizeof b, "BUF %.1fs  %s", buf, state);
    return b;
  }
  return "PICK A STATION BELOW, OR SEARCH";
}

std::string page_text(const Inst* s) {
  char b[128];
  if (!s->list_msg.empty() && (s->fetching || s->list.empty())) return s->list_msg;
  int pages = std::max(1, ((int)s->list.size() + NUM_ROWS - 1) / NUM_ROWS);
  std::string what;
  if (s->list_mode == LIST_FAVS) what = "FAVOURITES";
  else if (s->list_mode == LIST_SEARCH) what = "SEARCH \"" + s->query + "\"";
  else {
    what = PARAM_DEFS[P_GENRE].opts[opt(s, P_GENRE)];
    what += " / ";
    what += PARAM_DEFS[P_COUNTRY].opts[opt(s, P_COUNTRY)];
  }
  snprintf(b, sizeof b, "%s   PAGE %d/%d%s", what.c_str(), s->page + 1, pages, s->more ? "+" : "");
  return b;
}

std::string kiwi_text(const Inst* s) {
  char b[128];
  int pages = std::max(1, ((int)s->kiwis.size() + NUM_ROWS - 1) / NUM_ROWS);
  snprintf(b, sizeof b, "KIWISDR RECEIVERS   PAGE %d/%d   TAP ONE TO LISTEN", s->kpage + 1, pages);
  return b;
}

std::string tape_text(const Inst* s) {
  char b[128];
  const Looper& L = s->looper;
  int len = L.rec ? L.rec_len : L.length();
  const char* st = L.rec ? "RECORDING" : (L.od ? "OVERDUB" : (L.playing && len ? "PLAYING" : (len ? "STOPPED" : "EMPTY")));
  float beats = len / kSR * (float)s->tempo.bpm / 60.f;
  snprintf(b, sizeof b, "LOOP %d:%04.1f (%.1f BEATS)  %s  REC %s%s%s%s%s", len / ET_SR / 60, fmodf(len / kSR, 60.f), beats, st,
           s->rec_out ? "OUT" : "IN", s->send ? "  SEND" : "", s->auto_save ? "  AUTO SAVE" : "", L.rev ? "  REV" : "",
           L.speed < 0 ? "  HALF" : (L.speed > 0 ? "  DOUBLE" : ""));
  return b;
}

std::string readout(const Inst* s) {
  if (s->clock < s->flash_until) return s->flash;
  if (s->touched >= 0 && s->clock - s->touched_at < 344 * 4)
    return std::string(PARAM_DEFS[s->touched].kind == PK_KNOB ? knob_name(s, s->touched) : PARAM_DEFS[s->touched].name) + "   " + value_text(s, s->touched);
  return "EVIL TUNER XXL";
}

// a knob's value from the host or a saved project: an option index, a whole number, or 0..1
float knob_value(int i, float f) {
  const ParamDef& d = PARAM_DEFS[i];
  if (!(f == f)) f = d.def;   // NaN
  if (d.nopts) return clampf(roundf(f), 0, d.nopts - 1);
  if (d.hi > 1.f) return clampf(roundf(f), d.lo, d.hi);
  return clampf(f, d.lo, d.hi);
}

// ------------------------------------------------------------------------------------------------ the state
std::string serialize(const Inst* s) {
  std::string o = "evil-tuner-xxl 1\n";
  char b[256];
  for (int i = 0; i < P_COUNT; ++i)
    if (PARAM_DEFS[i].kind == PK_KNOB) { snprintf(b, sizeof b, "k %s %.6f\n", PARAM_DEFS[i].key, s->v[i]); o += b; }
  snprintf(b, sizeof b, "t %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n", s->freeze, s->keys_mode, s->char_on,
           s->sat_on, s->lim_on, s->char_pre, s->dly_on, s->verb_on, s->gate_on, s->duck_on, s->lsync[0], s->lsync[1], s->lsync[2],
           s->radio_sync, s->filt_mode, s->osc_wave, s->dly.tape, s->dly.sync, s->gate.steps, s->want_src);
  o += b;
  snprintf(b, sizeof b, "u %d %d %d\n", s->send, s->rec_out, s->auto_save);
  o += b;
  auto clean = [](std::string x) { for (auto& c : x) if (c == '\n' || c == '\t') c = ' '; return x; };
  if (s->cur_valid)
    o += "s\t" + clean(s->cur.uuid) + "\t" + clean(s->cur.name) + "\t" + clean(s->cur.url) + "\t" + clean(s->cur.codec) + "\t" +
         std::to_string(s->cur.bitrate) + "\t" + clean(s->cur.country) + "\n";
  o += "q\t" + clean(s->query) + "\n";
  snprintf(b, sizeof b, "m %d\nf %.3f\n", s->kmode, s->kfreq);
  o += b;
  if (s->kcur_valid)
    o += "w\t" + clean(s->kcur.host) + "\t" + std::to_string(s->kcur.port) + "\t" + clean(s->kcur.path) + "\t" +
         clean(s->kcur.name) + "\t" + clean(s->kcur.loc) + "\n";
  return o;
}

void deserialize(Inst* s, const char* text) {
  if (strncmp(text, "evil-tuner-xxl ", 15) != 0) return;
  const char* p = text;
  while (*p) {
    const char* e = strchr(p, '\n');
    std::string line(p, e ? e - p : strlen(p));
    p = e ? e + 1 : p + strlen(p);
    if (line.compare(0, 2, "k ") == 0) {
      char key[64];
      float val;
      if (sscanf(line.c_str() + 2, "%63s %f", key, &val) == 2)
        for (int i = 0; i < P_COUNT; ++i)
          if (PARAM_DEFS[i].kind == PK_KNOB && !strcmp(PARAM_DEFS[i].key, key)) s->v[i] = knob_value(i, val);
    } else if (line.compare(0, 2, "t ") == 0) {
      int t[20] = {0};
      int n = sscanf(line.c_str() + 2, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", &t[0], &t[1], &t[2], &t[3], &t[4],
                     &t[5], &t[6], &t[7], &t[8], &t[9], &t[10], &t[11], &t[12], &t[13], &t[14], &t[15], &t[16], &t[17], &t[18], &t[19]);
      if (n >= 20) {
        s->freeze = t[0]; s->keys_mode = t[1]; s->char_on = t[2]; s->sat_on = t[3]; s->lim_on = t[4]; s->char_pre = t[5];
        s->dly_on = t[6]; s->verb_on = t[7]; s->gate_on = t[8]; s->duck_on = t[9]; s->lsync[0] = t[10]; s->lsync[1] = t[11];
        s->lsync[2] = t[12]; s->radio_sync = t[13]; s->filt_mode = std::max(0, std::min(4, t[14])); s->osc_wave = t[15] & 3;
        s->dly.tape = t[16]; s->dly.sync = t[17]; s->gate.steps = (uint16_t)t[18];
        s->grains.frozen = s->freeze;
        int src = t[19];
        if (src == SRC_PHONE || src == SRC_AIRPLAY) s->want_src = src;   // a receiver waits again; radio doesn't autoplay
        if (src == SRC_KIWI) s->want_src = src;                          // shows the receivers; plays on PLAY
      }
    } else if (line.compare(0, 2, "u ") == 0) {
      int a = 0, b2 = 0, c = 0;
      if (sscanf(line.c_str() + 2, "%d %d %d", &a, &b2, &c) == 3) { s->send = a; s->rec_out = b2; s->auto_save = c; }
    } else if (line.compare(0, 2, "s\t") == 0) {
      std::vector<std::string> c;
      size_t a = 2;
      for (;;) { size_t b = line.find('\t', a); c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
      if (c.size() >= 6 && !c[2].empty()) {
        s->cur.uuid = c[0]; s->cur.name = c[1]; s->cur.url = c[2]; s->cur.codec = c[3]; s->cur.bitrate = atoi(c[4].c_str()); s->cur.country = c[5];
        s->cur_valid = true;
      }
    } else if (line.compare(0, 2, "q\t") == 0) {
      s->query = line.substr(2);
    } else if (line.compare(0, 2, "f ") == 0) {
      double f = atof(line.c_str() + 2);
      if (f >= 0 && f <= kKiwiTop) s->kfreq = f;
    } else if (line.compare(0, 2, "m ") == 0) {
      int m = atoi(line.c_str() + 2);
      if (m >= 0 && m < KIWI_NUM_MODES) s->kmode = m;
    } else if (line.compare(0, 2, "w\t") == 0) {
      std::vector<std::string> c;
      size_t a = 2;
      for (;;) { size_t b = line.find('\t', a); c.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; a = b + 1; }
      if (c.size() >= 5 && !c[0].empty()) {
        s->kcur = KiwiRx();
        s->kcur.host = c[0]; s->kcur.port = atoi(c[1].c_str()); s->kcur.path = c[2]; s->kcur.name = c[3]; s->kcur.loc = c[4];
        if (s->kcur.port <= 0 || s->kcur.port > 65535) s->kcur.port = 8073;
        s->kcur_valid = true;
      }
    }
  }
  if (s->want_src == SRC_PHONE) s->src.play_phone();
  else if (s->want_src == SRC_AIRPLAY) s->src.play_airplay();
  if (s->kcur_valid) {   // the page of the saved receiver
    int i = kiwi_index(s);
    if (i >= 0) s->kpage = i / NUM_ROWS;
  }
  s->kprobe = true;
  s->display_rev++;
}

// ------------------------------------------------------------------------------------------------ audio
void read_source(Inst* s, float* l, float* r, int n) {
  Ring& ring = s->src.ring;
  int kind = s->src.kind.load();
  float target = kind == SRC_PHONE ? 0.25f : (kind == SRC_AIRPLAY || kind == SRC_KIWI ? 0.6f : 2.0f);   // seconds kept in hand
  uint32_t fill = ring.fill();
  float fs = fill / kSR;
  if (s->wait_transport) {
    // SYNC: the station buffers silently; MPC's PLAY starts it (with only the target kept, so it starts on time)
    if (s->tempo.started || s->tempo.playing) {
      s->wait_transport = false;
      if (fs > target) ring.advance(fill - (uint32_t)(target * kSR));
      s->flowing = true;
      s->display_rev++;
    }
    for (int i = 0; i < n; ++i) l[i] = r[i] = 0.f;
    return;
  }
  if (!s->flowing) {
    if (fs >= target && fill > 0) {
      s->flowing = true;
      s->rd_frac = 0;
      s->display_rev++;
      if (fs > target + 4.f) { ring.advance(fill - (uint32_t)((target + 1.f) * kSR)); fill = ring.fill(); fs = fill / kSR; }   // a station's opening burst: not all of it as delay
    }
    else { for (int i = 0; i < n; ++i) l[i] = r[i] = 0.f; return; }
  }
  // a slow rate correction keeps the buffer near the target (the sender's clock is not MPC's): up to +-0.2 %
  double rate = 1.0 + 0.002 * clampf((fs - target) / target, -1.f, 1.f);
  double pos = s->rd_frac;
  for (int i = 0; i < n; ++i) {
    uint32_t i0 = (uint32_t)pos;
    if (i0 + 2 > fill) {   // ran dry: silence and buffer again
      for (; i < n; ++i) l[i] = r[i] = 0.f;
      s->flowing = false;
      s->display_rev++;
      break;
    }
    float fr = (float)(pos - i0), al, ar, bl, br;
    ring.peek(i0, al, ar);
    ring.peek(i0 + 1, bl, br);
    l[i] = al + fr * (bl - al);
    r[i] = ar + fr * (br - ar);
    pos += rate;
  }
  uint32_t adv = (uint32_t)pos;
  if (adv > fill) adv = fill;
  ring.advance(adv);
  s->rd_frac = pos - adv;
}

void update_mods(Inst* s, int n) {
  for (int i = 0; i < P_COUNT; ++i) s->mod[i] = 0.f;
  static const int kRate[3] = {P_L1_RATE, P_L2_RATE, P_L3_RATE};
  for (int l = 0; l < 3; ++l) {
    int base = kRate[l];
    float depth = s->v[base + 1];
    int dest = opt(s, base + 3);
    double inc;
    if (s->lsync[l]) {
      double cpb = kSyncCpb[(int)lroundf(clampf(s->v[base], 0, 1) * 10.f)];
      if (s->host_pos && s->tempo.playing) {   // locked to MPC's bar
        double ph = s->tempo.ppq * cpb;
        s->lfo[l].ph = ph - floor(ph);
      }
      inc = cpb * s->tempo.beat_per_frame() * n;
    } else inc = expmap(s->v[base], 0.02f, 20.f) * n / kSR;
    float y = s->lfo[l].step(opt(s, base + 2), inc);
    int p = DEST_PARAM[dest];
    if (p >= 0 && depth > 0.f) s->mod[p] += 0.5f * depth * y;
  }
  int fd = DEST_PARAM[opt(s, P_FOL_DEST)];
  if (fd >= 0 && s->v[P_FOL_AMT] > 0.f) s->mod[fd] += s->v[P_FOL_AMT] * clampf(s->fol.env * 2.f, 0.f, 1.f);
  for (int i = 0; i < P_COUNT; ++i)
    if (PARAM_DEFS[i].kind == PK_KNOB) s->eff[i] = PARAM_DEFS[i].nopts ? s->v[i] : clampf(s->v[i] + s->mod[i], 0.f, 1.f);
}

// ------------------------------------------------------------------------------------------------ CLOUDS
// Mutable Instruments Clouds with the Parasites firmware (Emilie Gillet, Matthias Puech; MIT), as RMXXXL runs it on the
// Force: 32-frame sub-blocks at 44.1 kHz, Prepare() on its own thread. Fully wet here; GRAIN MIX mixes it.
const size_t kClLarge = 118784;               // Clouds' block_mem
const size_t kClSmall = 65536 - 128;          // Clouds' block_ccm
const float kClMaxKnob = 65535.0f / 65536.0f; // Clouds' pots never reach 1.0
const int kClReconfigure = 689;               // a mode change at most every 0.5 s (in 32-frame sub-blocks)

inline short to_short(float x) {
  x = x > 1.f ? 1.f : (x < -1.f ? -1.f : x);
  return (short)lrintf(x * 32767.f);
}
void* clouds_prepare_loop(void* arg) {
  Inst* s = static_cast<Inst*>(arg);
  while (!s->cl_quit) {
    pthread_mutex_lock(&s->cl_mu);
    timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += 3000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec += 1; until.tv_nsec -= 1000000000; }
    pthread_cond_timedwait(&s->cl_wake, &s->cl_mu, &until);
    pthread_mutex_unlock(&s->cl_mu);
    for (int i = 0; i < 4 && !s->cl_quit; ++i) s->cl->Prepare();
  }
  return nullptr;
}

// the wet signal of Clouds for this block into wl / wr. sync_beats > 0: a grain on each step (GRAIN SYNC); kenv: the
// keys' envelope (1 without KEYS).
void clouds_block(Inst* s, const float* L, const float* R, float* wl, float* wr, int n, int scale, double sync_beats,
                  double beat0, double bpf, float kenv) {
  clouds::GranularProcessor& g = *s->cl;
  int mode = std::max(0, std::min(5, opt(s, P_CL_MODE)));
  clouds::Parameters* p = g.mutable_parameters();
  int semis = pitch_semis(E(s, P_PITCH), scale) + (s->keys_mode && s->last_note >= 0 ? s->last_note - 60 : 0);
  p->position = clampf(E(s, P_POS), 0.f, kClMaxKnob);
  p->size = clampf(E(s, P_SIZE), 0.f, kClMaxKnob);
  p->pitch = clampf((float)semis, -48.f, 48.f);
  bool synced = sync_beats > 0.0 && s->cl_mode_applied == 0;
  p->density = synced ? 0.5f : clampf(E(s, P_GRAINS), 0.f, kClMaxKnob);   // synced: no free grains, a trigger per step
  p->texture = clampf(E(s, P_SHAPE), 0.f, kClMaxKnob);
  p->dry_wet = kClMaxKnob;
  p->stereo_spread = clampf(E(s, P_SPREAD), 0.f, kClMaxKnob);
  p->feedback = clampf(E(s, P_CLOUD), 0.f, kClMaxKnob);
  p->reverb = clampf(E(s, P_DETUNE), 0.f, kClMaxKnob);
  p->freeze = s->grains.frozen;
  p->granular.reverse = false;
  // makeup: undo Clouds' output halving and its wet gain; per mode, a dense mode is brought down, a sparse one up
  static const float kTrim[6] = {1.6f, 1.2f, 1.3f, 0.8f, 0.6f, 0.64f};
  short in[2 * 32];
  short out[2 * 32];
  for (int o = 0; o < n; o += 32) {
    int m = std::min(32, n - o);
    if (s->cl_reconf < kClReconfigure) ++s->cl_reconf;
    if (mode != s->cl_mode_applied && s->cl_reconf >= kClReconfigure) { s->cl_mode_applied = mode; s->cl_reconf = 0; }
    g.set_playback_mode(clouds::PlaybackMode(s->cl_mode_applied));
    g.set_quality(0);
    bool trig = s->cl_trigger;
    s->cl_trigger = false;
    if (synced) {   // a step of the grid inside this sub-block
      double b0 = beat0 + bpf * o, b1 = b0 + bpf * m;
      if (floor(b1 / sync_beats - 1e-9) > floor(b0 / sync_beats - 1e-9)) trig = trig || s->grains.rng.uni() <= 0.15f + 0.85f * E(s, P_GRAINS);
    }
    p->trigger = trig;
    p->gate = trig;
    clouds::ShortFrame* fi = reinterpret_cast<clouds::ShortFrame*>(in);
    clouds::ShortFrame* fo = reinterpret_cast<clouds::ShortFrame*>(out);
    for (int i = 0; i < m; ++i) { fi[i].l = to_short(L[o + i] * 0.8f); fi[i].r = to_short(R[o + i] * 0.8f); }
    g.Process(fi, fo, m);
    if (!s->cl_thread_on) g.Prepare();
    float k = 2.357f * kTrim[s->cl_mode_applied] / 0.8f / 32768.f;
    for (int i = 0; i < m; ++i) {
      wl[o + i] = fo[i].l * k * kenv;
      wr[o + i] = fo[i].r * k * kenv;
    }
  }
  if (s->cl_thread_on) pthread_cond_signal(&s->cl_wake);
}

void push_reverb(Inst* s, int type) {
  int k = type == 2 ? 1 : 0;   // ROOM, else the plate (PLATE, and REVERSE's source)
  const ReverbApi* api = k ? &kRoomReverb : &kPlateReverb;
  void* d = s->rv[k];
  if (!d) return;
  float decay = expmap(E(s, P_VERB_SIZE), 0.3f, 8.f);
  if (fabsf(decay - s->rv_sent[k][0]) > 0.02f) { api->set(d, "decay", decay); s->rv_sent[k][0] = decay; }
}

void render_block(Inst* s, float* L, float* R, int n) {
  float in[2][kBlock], wet[2][kBlock];
  // ---- the source, at INPUT LEVEL
  read_source(s, in[0], in[1], n);
  float ig = (s->v[P_IN_LVL] / 0.7f) * (s->v[P_IN_LVL] / 0.7f);
  for (int i = 0; i < n; ++i) {
    s->in_gain_sm += 0.002f * (ig - s->in_gain_sm);
    in[0][i] *= s->in_gain_sm;
    in[1][i] *= s->in_gain_sm;
  }
  s->fol.run(in[0], in[1], n, s->v[P_FOL_SPD]);
  update_mods(s, n);
  s->tape.write(in[0], in[1], n);
  // ---- the looper. REC IN records / overdubs the source; REC OUT the plugin's output (below). SEND: the loop goes into
  // the chain (PET, filter, FX, SPACE, GATE); off, it plays clean at the end. LIVE/LOOP fades the live source out.
  float lp[2][kBlock];
  bool rec_in = !s->rec_out;
  s->looper.process(in[0], in[1], lp[0], lp[1], n, E(s, P_LOOP_SPEED), E(s, P_LOOP_START), s->v[P_OD_FB], rec_in);
  s->looper.update_writing();
  float tm = s->v[P_TAPE_MIX], ll = s->v[P_LOOP_LVL];
  float send_g = s->send ? ll : 0.f;
  for (int i = 0; i < n; ++i) {
    L[i] = in[0][i] * (1.f - tm) + lp[0][i] * send_g;
    R[i] = in[1][i] * (1.f - tm) + lp[1][i] * send_g;
  }
  // ---- the character stage (before the grains, when CHAR PRE)
  int cm = opt(s, P_CHAR_MODE);
  s->chr.setTarget(s->char_on ? cm : 0, s->v[P_CHAR_MIX], E(s, P_CHAR_INT));
  if (s->char_pre && s->char_on && cm) for (int i = 0; i < n; ++i) s->chr.process(L[i], R[i]);
  // ---- the keys' voices: their envelopes (both engines) and the oscillator layer
  int scale = opt(s, P_SCALE);
  GrainParams gp{E(s, P_POS), E(s, P_SIZE), E(s, P_PITCH), E(s, P_GRAINS), E(s, P_SPREAD), E(s, P_CLOUD), E(s, P_DETUNE), E(s, P_SHAPE)};
  gp.scale = scale;
  float mix = E(s, P_MIX);
  memset(wet, 0, sizeof wet);
  float envmax = 0.f;
  float a = onepole(expmap(s->v[P_ATK], 1.f, 4000.f)), d = onepole(expmap(s->v[P_DEC], 5.f, 4000.f));
  float su = s->v[P_SUS], rl = onepole(expmap(s->v[P_REL], 5.f, 8000.f));
  float osc_lvl = s->v[P_OSC_LVL] * 0.3f;
  float pst = (float)pitch_semis(gp.pitch, scale);
  float venv0[kVoices];
  if (s->keys_mode) {
    for (int k = 0; k < kVoices; ++k) {
      Voice& vc = s->voice[k];
      venv0[k] = vc.env.v;
      if (vc.env.st == Adsr::IDLE) continue;
      float hz = 440.f * powf(2.f, (vc.note - 69 + pst) / 12.f);
      for (int i = 0; i < n; ++i) {
        float e = vc.env.run(a, d, su, rl);
        if (osc_lvl > 0.f) {
          float y = vc.osc.tick(s->osc_wave, hz) * e * vc.vel * osc_lvl;
          wet[0][i] += y;
          wet[1][i] += y;
        }
      }
      envmax = fmaxf(envmax, vc.env.v * vc.vel);
    }
  }
  // ---- the grain engines: XXL GRAINS and CLOUDS (a short crossfade when the engine changes)
  int engine = opt(s, P_ENGINE);
  float step = 1.f / 441.f;
  float gw[2][kBlock], cw[2][kBlock];
  memset(gw, 0, sizeof gw);
  memset(cw, 0, sizeof cw);
  bool run_xxl = engine == 0 || s->cl_mix < 1.f;
  bool run_cl = (engine == 1 || s->cl_mix > 0.f) && s->cl;
  int sync = opt(s, P_GRAIN_SYNC);
  static const double kSyncBeats[7] = {0, 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0};
  double beat0 = s->host_pos && s->tempo.playing ? s->tempo.ppq : s->own_beats;
  double bpf = s->tempo.beat_per_frame();
  for (int i = 0; i < n; ++i) s->grains.write(L[i], R[i]);
  if (run_xxl) {
    float dg = Grains::density_gain(gp);
    if (s->keys_mode) {
      for (int k = 0; k < kVoices; ++k) {
        Voice& vc = s->voice[k];
        float e0 = venv0[k];
        if (vc.env.st == Adsr::IDLE || mix <= 0.001f || e0 <= 0.002f) continue;
        if (sync) s->grains.schedule_sync(gp, (float)(vc.note - 60), vc.vel * e0 * dg, beat0, bpf, kSyncBeats[sync], n);
        else s->grains.schedule(k, gp, (float)(vc.note - 60), vc.vel * e0 * dg, n);
      }
    } else if (mix > 0.001f) {
      if (sync) s->grains.schedule_sync(gp, 0.f, dg, beat0, bpf, kSyncBeats[sync], n);
      else s->grains.schedule(kVoices, gp, 0.f, dg, n);
    }
    s->grains.render(gw[0], gw[1], n, gp.shape);
  }
  if (run_cl) clouds_block(s, L, R, cw[0], cw[1], n, scale, sync ? kSyncBeats[sync] : 0.0, beat0, bpf,
                           s->keys_mode ? envmax : 1.f);
  for (int i = 0; i < n; ++i) {
    s->cl_mix += engine == 1 ? step : -step;
    s->cl_mix = clampf(s->cl_mix, 0.f, 1.f);
    float f = s->cl_mix;
    float wl = gw[0][i] * (1.f - f) + cw[0][i] * f, wr = gw[1][i] * (1.f - f) + cw[1][i] * f;
    L[i] = L[i] * (1.f - mix) + wl * mix + wet[0][i];
    R[i] = R[i] * (1.f - mix) + wr * mix + wet[1][i];
  }
  // ---- the filter (FILTER ENV: the keys' envelope, else the source's level)
  float fe = (s->v[P_FLT_ENV] - 0.5f) * 2.f, env = s->keys_mode ? envmax : clampf(s->fol.env * 2.f, 0.f, 1.f);
  float cut = clampf(E(s, P_CUTOFF) + fe * env * 0.5f, 0.f, 1.f);
  s->filt.process(L, R, n, s->filt_mode, cut, E(s, P_RESO));
  // ---- character (after the filter), saturator
  if (!s->char_pre && s->char_on && cm) for (int i = 0; i < n; ++i) s->chr.process(L[i], R[i]);
  if (s->sat_on) s->sat.process(L, R, n, opt(s, P_SAT_TYPE), E(s, P_SAT_AMT), s->v[P_SAT_MIX]);
  // ---- gate, stutter, duck
  if (s->gate_on) {
    int pat = opt(s, P_GATE_PAT);
    double beat0 = s->host_pos && s->tempo.playing ? s->tempo.ppq : s->own_beats;
    s->gate.process(L, R, n, s->tempo, beat0, opt(s, P_GATE_RATE), E(s, P_GATE_DEPTH), s->v[P_GATE_ATK], s->v[P_GATE_REL],
                    s->v[P_GATE_SWING], pat == 11);
  }
  s->stut.process(L, R, n);
  if (s->duck_on) s->duck.process(L, R, n, s->v[P_DUCK_DEPTH], s->v[P_DUCK_REL]);
  // ---- the Erica delay
  if (s->dly_on) s->dly.process(L, R, n, E(s, P_DLY_TIME), s->v[P_DLY_IN], E(s, P_DLY_FB), E(s, P_DLY_WET), s->tempo.bpm);
  // ---- the reverb (+ shimmer: the tail an octave up, fed back in)
  float vm = E(s, P_VERB_MIX);
  if (s->verb_on && vm > 0.0005f) {
    int type = opt(s, P_VERB_TYPE);
    int k = type == 2 ? 1 : 0;
    const ReverbApi* api = k ? &kRoomReverb : &kPlateReverb;
    push_reverb(s, type);
    float shim = E(s, P_VERB_SHIM);
    float vin[2][kBlock], vout[2][kBlock];
    for (int i = 0; i < n; ++i) {
      vin[0][i] = L[i] + s->shim[0][i] * shim * 0.55f;
      vin[1][i] = R[i] + s->shim[1][i] * shim * 0.55f;
    }
    const float* ip[2] = {vin[0], vin[1]};
    float* op[2] = {vout[0], vout[1]};
    if (s->rv[k]) api->run(s->rv[k], ip, op, (uint32_t)n);
    else memset(vout, 0, sizeof vout);
    if (type == 1) {   // REVERSE: the tail in pieces of a beat, backwards
      int beat = (int)(60.0 / s->tempo.bpm * kSR);
      while (beat > (int)(1.5f * kSR)) beat /= 2;
      s->revr.process(vout[0], vout[1], n, beat);
    }
    if (shim > 0.001f) s->octv.process(vout[0], vout[1], s->shim[0].data(), s->shim[1].data(), n);
    else { std::fill(s->shim[0].begin(), s->shim[0].end(), 0.f); std::fill(s->shim[1].begin(), s->shim[1].end(), 0.f); }
    for (int i = 0; i < n; ++i) {
      L[i] = L[i] * (1.f - vm) + vout[0][i] * vm * 1.6f;
      R[i] = R[i] * (1.f - vm) + vout[1][i] * vm * 1.6f;
    }
  }
  // ---- the loop, clean (SEND off), then the limiter
  if (!s->send && ll > 0.f)
    for (int i = 0; i < n; ++i) { L[i] += lp[0][i] * ll; R[i] += lp[1][i] * ll; }
  if (s->lim_on) s->lim.process(L, R, n, s->v[P_LIM_DRIVE] * 18.f, -12.f + 11.7f * s->v[P_LIM_CEIL]);
  // ---- REC OUT: what is heard (before VOLUME) goes into the loop, where the play head was when it was made
  if (s->rec_out && (s->looper.rec || s->looper.od)) s->looper.write_out(L, R, n, s->lim_on ? Limiter::kLook : 0);
  s->looper.update_writing();
  float vg = (E(s, P_VOLUME) / 0.75f) * (E(s, P_VOLUME) / 0.75f);
  for (int i = 0; i < n; ++i) {
    s->vol_sm += 0.002f * (vg - s->vol_sm);
    L[i] *= s->vol_sm;
    R[i] *= s->vol_sm;
  }
}

// ------------------------------------------------------------------------------------------------ the engine API
int key_index(const char* k, int* on) {
  // a small open hash of the parameter keys (the wrapper polls "<key>_on" and texts often)
  static int table[512];
  static bool built = false;
  auto hash = [](const char* s, size_t n) { uint32_t h = 2166136261u; for (size_t i = 0; i < n; ++i) h = (h ^ (uint8_t)s[i]) * 16777619u; return h; };
  if (!built) {
    for (int& t : table) t = -1;
    for (int i = 0; i < P_COUNT; ++i) {
      uint32_t h = hash(PARAM_DEFS[i].key, strlen(PARAM_DEFS[i].key)) & 511;
      while (table[h] >= 0) h = (h + 1) & 511;
      table[h] = i;
    }
    built = true;
  }
  size_t n = strlen(k);
  *on = 0;
  for (int pass = 0; pass < 2; ++pass) {
    size_t m = pass ? n - 3 : n;
    if (pass) {
      if (n <= 3 || strcmp(k + n - 3, "_on")) break;
      *on = 1;
    }
    uint32_t h = hash(k, m) & 511;
    while (table[h] >= 0) {
      const char* key = PARAM_DEFS[table[h]].key;
      if (strlen(key) == m && !strncmp(key, k, m)) return table[h];
      h = (h + 1) & 511;
    }
  }
  *on = 0;
  return -1;
}

int writable_dir(const char* d) {
  char probe[700], tmp[600];
  snprintf(tmp, sizeof tmp, "%s", d);
  for (char* p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
  snprintf(probe, sizeof probe, "%s/.probe", d);
  FILE* f = fopen(probe, "w");
  if (!f) return 0;
  fclose(f);
  remove(probe);
  return 1;
}

void* create(const char* dataDir) {
  Inst* s = new (std::nothrow) Inst();
  if (!s) return nullptr;
  for (int i = 0; i < P_COUNT; ++i) s->v[i] = PARAM_DEFS[i].def;
  char here[512];
  if (mpc_plugin_dir(here, sizeof here)) s->plugin_dir = here;
#ifdef ET_TEST
  if (getenv("ET_PLUGIN_DIR")) s->plugin_dir = getenv("ET_PLUGIN_DIR");
#endif
  if (dataDir && dataDir[0] && writable_dir(dataDir)) s->data_dir = dataDir;
  else if (!s->plugin_dir.empty()) {
    std::string d = s->plugin_dir + "/user_data";   // kept by the installer on upgrades (--user-data)
    if (writable_dir(d.c_str())) s->data_dir = d;
  }
  s->tape.init();
  if (!s->looper.init() || !s->tape.buf) { s->looper.free_all(); s->tape.free_all(); delete s; return nullptr; }
  {   // SAVE's folder: the device's documents folder, which MPC's own browser shows
    struct stat st;
    if (stat("/sdcard/Force Documents", &st) == 0) s->rec_dir = "/sdcard/Force Documents/Evil Tuner XXL Recordings";
    else if (stat("/sdcard/MPC Documents", &st) == 0) s->rec_dir = "/sdcard/MPC Documents/Evil Tuner XXL Recordings";
    else if (!s->data_dir.empty()) s->rec_dir = s->data_dir + "/Recordings";
    else s->rec_dir = "/tmp/Evil Tuner XXL Recordings";
#ifdef ET_TEST
    if (getenv("ET_REC_DIR")) s->rec_dir = getenv("ET_REC_DIR");
#endif
  }
  s->grains.init();
  s->filt.init();
  s->sat.init();
  s->dly.init();
  s->revr.init();
  s->octv.init();
  s->stut.init();
  s->lim.init();
  s->shim[0].assign(kBlock, 0.f);
  s->shim[1].assign(kBlock, 0.f);
  s->chr.prepare(kSR);
  s->rv[0] = kPlateReverb.create(kSR);
  s->rv[1] = kRoomReverb.create(kSR);
  for (int k = 0; k < 2; ++k) {
    const ReverbApi* api = k ? &kRoomReverb : &kPlateReverb;
    void* d = s->rv[k];
    if (!d) continue;
    api->load_default(d);
    api->set(d, "dry_level", 0.f);
    if (k == 0) { api->set(d, "early_level", 80.f); api->set(d, "algorithm", 1.f); api->set(d, "low_cut", 80.f); api->set(d, "high_cut", 9000.f); api->set(d, "predelay", 12.f); }
    else { api->set(d, "early_level", 25.f); api->set(d, "late_level", 80.f); api->set(d, "in_low_cut", 80.f); api->set(d, "in_high_cut", 9000.f); api->set(d, "size", 18.f); }
    api->set(d, "width", 120.f);
    s->rv_sent[k][0] = -1.f;
  }
  s->gate.rng.s = 0x51ED27u;
  // Clouds: zeroed memory (Init() leaves members unset), its buffers, Prepare() on a thread of its own
  s->cl_large = static_cast<uint8_t*>(calloc(1, kClLarge));
  s->cl_small = static_cast<uint8_t*>(calloc(1, kClSmall));
  void* clm = calloc(1, sizeof(clouds::GranularProcessor));
  if (clm && s->cl_large && s->cl_small) {
    s->cl = new (clm) clouds::GranularProcessor;
    s->cl->Init(s->cl_large, kClLarge, s->cl_small, kClSmall);
    memset(s->cl->mutable_parameters(), 0, sizeof(clouds::Parameters));
    s->cl->set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
    s->cl->set_quality(0);
    s->cl->Prepare();
    s->cl_reconf = kClReconfigure;
    pthread_mutex_init(&s->cl_mu, nullptr);
    pthread_cond_init(&s->cl_wake, nullptr);
    s->cl_thread_on = pthread_create(&s->cl_thread, nullptr, clouds_prepare_loop, s) == 0;
  } else free(clm);
  s->src.start(s->plugin_dir, s->data_dir);
  s->dir.start(s->data_dir);
  s->work.start();
  s->kdir.start(s->data_dir);
  s->kiwis = s->kdir.list;
  request_list(s, false);   // the most played stations, to start with
  return s;
}

void destroy(void* p) {
  Inst* s = static_cast<Inst*>(p);
  s->src.stop_all();
  s->dir.stop();
  s->work.stop();
  if (s->cl_thread_on) {
    s->cl_quit = true;
    pthread_cond_signal(&s->cl_wake);
    pthread_join(s->cl_thread, nullptr);
    pthread_cond_destroy(&s->cl_wake);
    pthread_mutex_destroy(&s->cl_mu);
  }
  if (s->cl) { s->cl->~GranularProcessor(); free(s->cl); }
  free(s->cl_large);
  free(s->cl_small);
  s->tape.free_all();
  s->looper.free_all();
  s->kdir.stop();
  if (s->rv[0]) kPlateReverb.destroy(s->rv[0]);
  if (s->rv[1]) kRoomReverb.destroy(s->rv[1]);
  delete s;
}

void midi(void* p, const uint8_t* m, int len) {
  Inst* s = static_cast<Inst*>(p);
  if (len < 3) return;
  uint8_t st = m[0] & 0xF0, ch = m[0] & 0x0F, note = m[1] & 127, vel = m[2] & 127;
  if (st == 0x90 && vel == 0) st = 0x80;
  if (st == 0xB0 && (m[1] == 120 || m[1] == 123)) {
    for (auto& vc : s->voice) { vc.env.gate(false); vc.note = -1; }
    return;
  }
  if (st != 0x90 && st != 0x80) return;
  bool on = st == 0x90;
  if (s->duck_on && ch == opt(s, P_DUCK_CH)) { if (on) s->duck.trigger(); return; }
  if (s->keys_mode) {
    if (on) {
      int k = -1;
      uint32_t oldest = UINT32_MAX;
      for (int i = 0; i < kVoices; ++i) if (s->voice[i].env.st == Adsr::IDLE) { k = i; break; }
      if (k < 0) for (int i = 0; i < kVoices; ++i) if (s->voice[i].age < oldest) { oldest = s->voice[i].age; k = i; }
      Voice& vc = s->voice[k];
      vc.note = note;
      vc.vel = vel / 127.f;
      vc.age = ++s->voice_clock;
      vc.env.gate(true);
      s->last_note = note;                // CLOUDS plays the last note pressed
      s->cl_trigger = true;
      s->grains.sched[k] = 0;
    } else {
      for (auto& vc : s->voice) if (vc.note == note && vc.env.st != Adsr::IDLE && vc.env.st != Adsr::R) vc.env.gate(false);
    }
    return;
  }
  // performance pads (KEYS off): 36-39 stutter 1/4 .. 1/32 while held, 40 freeze while held, 41 capture,
  // 42 delay HOLD while held, 43 the gate while held
  static const double kB[4] = {1.0, 0.5, 0.25, 0.125};
  if (note >= 36 && note <= 39) {
    if (on) { s->stut.start((int)(kB[note - 36] * 60.0 / s->tempo.bpm * kSR)); s->stut_pad = note; }
    else if (s->stut_pad == note) { s->stut.active = s->stut_key; s->stut_pad = -1; }
  } else if (note == 40) {
    s->grains.frozen = on ? true : s->freeze;
  } else if (note == 41) {
    if (on) loop_capture(s);
  } else if (note == 42) {
    s->dly.hold = on;
  } else if (note == 43) {
    s->gate_on = on;
  }
  s->display_rev++;
}

void set_param(void* p, const char* k, const char* val) {
  Inst* s = static_cast<Inst*>(p);
  if (!strcmp(k, "state")) { deserialize(s, val); return; }
  int on;
  int i = key_index(k, &on);
  if (i < 0 || on) return;
  const ParamDef& d = PARAM_DEFS[i];
  switch (d.kind) {
    case PK_KNOB: {
      float f = knob_value(i, (float)atof(val));
      if (i == P_KIWI_TUNE) {   // endless: a move, not a value
        kiwi_turn(s, f);
        kiwi_retune(s);
        touch(s, i);
        break;
      }
      bool changed = s->v[i] != f;
      s->v[i] = f;
      if (changed) {
        if (i == P_KIWI_BAND) kiwi_band_changed(s);
        else if (i == P_KIWI_BW) kiwi_retune(s);
        if (i == P_GENRE || i == P_COUNTRY || i == P_SORT || i == P_KBPS) {
          if (s->list_mode == LIST_FAVS) s->list_mode = LIST_BROWSE;
          s->browse_countdown = 172;   // half a second after the last turn
        }
        if (i == P_GATE_PAT) {
          int pat = (int)f;
          if (pat > 0 && pat < 11) s->gate.steps = kGatePatterns[pat];
        }
      }
      touch(s, i);
      break;
    }
    case PK_KEY: press(s, i); break;
    case PK_ROW: {
      if (s->want_src == SRC_KIWI) {
        int k = s->kpage * NUM_ROWS + (i - P_ROW1);
        if (k < (int)s->kiwis.size()) play_kiwi(s, s->kiwis[k]);
        break;
      }
      int idx = s->page * NUM_ROWS + (i - P_ROW1);
      if (idx < (int)s->list.size()) play_station(s, s->list[idx]);
      break;
    }
    default: break;
  }
}

int put(char* buf, int n, const std::string& s) {
  if (n <= 0) return 0;
  snprintf(buf, (size_t)n, "%s", s.c_str());
  return s.empty() ? 1 : (int)s.size();   // an empty text is still an answer (" " would show a space)
}

int get_param(void* p, const char* k, char* buf, int n) {
  Inst* s = static_cast<Inst*>(p);
  char t[96];
  if (!strcmp(k, "state")) {
    std::string st = serialize(s);
    if ((int)st.size() < n) memcpy(buf, st.c_str(), st.size() + 1);
    return (int)st.size() + 1;
  }
  if (!strcmp(k, "display_rev")) { snprintf(t, sizeof t, "%u", (unsigned)s->display_rev); return put(buf, n, t); }
  size_t kl = strlen(k);
  if (kl > 5 && !strcmp(k + kl - 5, "_name")) {
    std::string base(k, kl - 5);
    int on;
    int i = key_index(base.c_str(), &on);
    if (i < 0 || PARAM_DEFS[i].kind != PK_KNOB) return 0;
    return put(buf, n, knob_name(s, i));
  }
  if (kl > 8 && !strcmp(k + kl - 8, "_display")) {
    std::string base(k, kl - 8);
    int on;
    int i = key_index(base.c_str(), &on);
    if (i < 0 || PARAM_DEFS[i].kind != PK_KNOB) return 0;
    return put(buf, n, value_text(s, i));
  }
  int on;
  int i = key_index(k, &on);
  if (i < 0) return 0;
  const ParamDef& d = PARAM_DEFS[i];
  switch (d.kind) {
    case PK_KNOB:
      if (on) return 0;
      if (d.nopts || d.hi > 1.f) snprintf(t, sizeof t, "%d", (int)lroundf(s->v[i]));
      else snprintf(t, sizeof t, "%.5f", s->v[i]);
      return put(buf, n, t);
    case PK_KEY:
      if (on) return put(buf, n, led(s, i) ? "1" : "0");
      return put(buf, n, " ");
    case PK_VIEW:
      if (on) return 0;
      snprintf(t, sizeof t, "%d", view_of(s));
      return put(buf, n, t);
    case PK_ROW: {
      if (s->want_src == SRC_KIWI) {
        int k = s->kpage * NUM_ROWS + (i - P_ROW1);
        bool has = k < (int)s->kiwis.size();
        if (on) return put(buf, n, has && k == kiwi_index(s) ? "1" : "0");
        return put(buf, n, has ? kiwi_row_text(s->kiwis[k]) : " ");
      }
      int idx = s->page * NUM_ROWS + (i - P_ROW1);
      bool has = idx < (int)s->list.size();
      if (on) {
        int pi = playing_index(s);
        return put(buf, n, has && pi == idx && s->want_src == SRC_RADIO ? "1" : "0");
      }
      return put(buf, n, has ? row_text(s->list[idx]) : " ");
    }
    case PK_TEXT:
      if (on) return 0;
      switch (i) {
        case P_ST_NAME: return put(buf, n, st_name(s));
        case P_ST_TITLE: {
          if (s->want_src == SRC_KIWI) {
            int st = s->src.state.load();
            if (st == 4 || st == 1 || st == 2) return put(buf, n, s->status);
            if (!s->title.empty()) return put(buf, n, s->title);
            return put(buf, n, s->kcur_valid ? s->kcur.name : "SHORTWAVE  ·  KIWISDR.COM");
          }
          if (!s->title.empty()) return put(buf, n, s->title);
          if (s->src.state.load() == 4 || s->src.state.load() == 1) return put(buf, n, s->status);
          return put(buf, n, s->want_src == SRC_RADIO && !s->cur_valid ? "INTERNET RADIO  ·  RADIO-BROWSER.INFO" : " ");
        }
        case P_ST_INFO: return put(buf, n, st_info(s));
        case P_PAGE_TXT: return put(buf, n, page_text(s));
        case P_KIWI_TXT: return put(buf, n, kiwi_text(s));
        case P_QUERY:
          if (s->kentry) return put(buf, n, "FREQUENCY: " + s->kquery + ((s->clock >> 6) & 1 ? "_" : " ") + " kHz   (SPACE = .)");
          return put(buf, n, s->query + ((s->clock >> 6) & 1 ? "_" : " "));
        case P_PHONE_TXT:
          return put(buf, n, s->ip.empty() ? "NO NETWORK: JOIN WI-FI OR A PHONE HOTSPOT" : "ROC DROID -> SEND TO " + s->ip + "  PORT 10001");
        case P_AIR_TXT:
          return put(buf, n, s->ip.empty() ? "NO NETWORK: JOIN WI-FI OR A PHONE HOTSPOT" : "IPHONE: AIRPLAY -> \"EVIL TUNER XXL\"  (" + s->ip + ")");
        case P_TAPE_TXT: return put(buf, n, tape_text(s));
        case P_READOUT: return put(buf, n, readout(s));
        default: return 0;
      }
    default: return 0;
  }
}

void render(void* p, int16_t* out, int frames) {
  Inst* s = static_cast<Inst*>(p);
  float L[kBlock], R[kBlock];
  if (frames > kBlock) frames = kBlock;
  s->clock++;
  // the workers' news (never waits)
  std::string t;
  if (s->src.title(t)) { s->title = t; s->display_rev++; }
  if (s->src.status(t)) { s->status = t; s->display_rev++; }
  {
    std::vector<Station> got;
    std::string msg;
    int off;
    bool more;
    if (s->dir.take(got, msg, off, more)) {
      if (s->list_mode != LIST_FAVS) {
        if (s->want_append && off == (int)s->list.size()) s->list.insert(s->list.end(), got.begin(), got.end());
        else { s->list.swap(got); s->page = 0; }
        s->more = more;
        s->list_msg = msg;
        int pages = ((int)s->list.size() + NUM_ROWS - 1) / NUM_ROWS;
        if (s->page >= pages) s->page = std::max(0, pages - 1);
      }
      s->fetching = false;
      s->display_rev++;
    }
  }
  {
    std::vector<KiwiRx> st;
    if (s->kdir.take(st)) {
      for (auto& r : st)
        for (auto& k : s->kiwis)
          if (k.id() == r.id()) { k.users = r.users; k.users_max = r.users_max; k.offline = r.offline; }
      s->display_rev++;
    }
    if (s->kprobe && s->want_src == SRC_KIWI && !s->kbd) kiwi_probe_page(s);
  }
  if (s->browse_countdown > 0 && --s->browse_countdown == 0) { if (s->list_mode == LIST_FAVS) s->list_mode = LIST_BROWSE; if (s->list_mode == LIST_SEARCH) s->list_mode = LIST_BROWSE; request_list(s, false); }
  // ---- the tape worker: a new loop to swap in, a message, jobs that were waiting
  {
    Looper& L = s->looper;
    int sl = s->work.swap_len.load();
    if (sl >= 0 && !L.rec && !L.od) {
      L.cp_left = 0;
      L.swap_in(sl, true);
      L.update_writing();
      s->work.swap_len.store(-1);
      s->stretch_busy = false;
    }
    std::string m;
    if (s->work.take_msg(m)) flash(s, m);
    if (s->stretch_busy && s->work.running.load() == TapeWorker::NONE && s->work.swap_len.load() < 0 && !s->work.busy.load())
      s->stretch_busy = false;
    if (s->pending_capture && try_capture(s)) { s->pending_capture = false; flash(s, "CAPTURING..."); }
    if (s->pending_save && try_save(s)) { s->pending_save = false; flash(s, "SAVING..."); }
    if (s->pending_rec && !s->work.busy.load() && s->work.swap_len.load() < 0) rec_press(s);
    // a recording that closed by itself (its LOOP LENGTH reached, or 3 minutes): AUTO SAVE saves it
    if (s->layer_open && !L.rec && !L.od) {
      s->layer_open = false;
      if (s->auto_save) request_save(s);
      s->display_rev++;
    }
  }
  if ((s->want_src == SRC_PHONE || s->want_src == SRC_AIRPLAY) && (s->clock - s->ip_check) > 344 * 5) {   // the address may change
    s->ip_check = s->clock;
    std::string ip = s->src.ip_text();
    if (ip != s->ip) { s->ip = ip; s->display_rev++; }
  }
  render_block(s, L, R, frames);
  // our own beat count (the gate and LFOs when MPC is stopped)
  s->own_beats += frames * s->tempo.beat_per_frame();
  s->tempo.started = false;
  for (int i = 0; i < frames; ++i) {
    float l = clampf(L[i], -1.f, 1.f) * 32767.f, r = clampf(R[i], -1.f, 1.f) * 32767.f;
    out[2 * i] = (int16_t)lrintf(l);
    out[2 * i + 1] = (int16_t)lrintf(r);
  }
}

void song_pos(void* p, double ppq, double bpm, int playing, int frames) {
  Inst* s = static_cast<Inst*>(p);
  (void)frames;
  if (bpm > 20.0 && bpm < 400.0) s->tempo.bpm = bpm;
  s->host_pos = ppq >= 0.0;
  if (s->host_pos) s->tempo.ppq = ppq;
  if (playing && !s->tempo.playing) s->tempo.started = true;
  s->tempo.playing = playing != 0;
}

const mpc_engine_t kEngine = {create, destroy, midi, set_param, get_param, render, nullptr, song_pos};

}  // namespace

extern "C" const mpc_engine_t* mpc_engine(void) { return &kEngine; }

// knobs the engine moved by itself (BAND resets TUNE and FINE; TUNE stops at the band's end): the wrapper tells MPC
extern "C" int mpc_engine_changed(void* inst, char* key, int len) {
  Inst* s = static_cast<Inst*>(inst);
  if (s->changed.empty() || len <= 0) return 0;
  snprintf(key, (size_t)len, "%s", s->changed.back().c_str());
  s->changed.pop_back();
  return 1;
}

#ifdef ET_TEST
extern "C" void et_test_set_station(void* inst, const char* url) {
  Station st;
  st.name = "TEST STREAM";
  st.url = url;
  st.codec = "MP3";
  st.bitrate = 128;
  play_station(static_cast<Inst*>(inst), st);
}
extern "C" void et_test_set_kiwi(void* inst, const char* host, int port) {
  KiwiRx r;
  r.host = host;
  r.port = port;
  r.name = "TEST RECEIVER";
  r.loc = "XX";
  play_kiwi(static_cast<Inst*>(inst), r);
}
#endif
