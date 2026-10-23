# Changelog

## 0.2.3 (this fork: upstream 0.2.0 + KIWISDR)
- Upstream's 0.2.0 (below: CLOUDS, the 3-minute looper, SAVE, the AirPlay fix) with this fork's **KIWISDR** source.
- **KIWISDR**, a fourth source: live shortwave (10 kHz to 30 MHz) from public KiwiSDR receivers, tuned from the MPC.
  A list of public receivers (plus your own in `user_data/kiwisdr.txt`) with their listeners, FULL or OFFLINE; **BAND**
  (a jump to a band's frequency, mode and step: broadcast and ham bands, LF / LW / MW, CB, NDB beacons, time signals,
  VOLMET, weather fax), **TUNE** (an endless knob over 0 .. 30 MHz, one **STEP** a detent: 10 Hz .. 1 MHz),
  **FILTER**, the AM / USB / LSB / CW / NFM keys, **ENTER kHz** to type a frequency; the S-meter in the window; Q-Link
  page RADIO 2. A session the receiver ends is not reopened; a dropped network is retried three times.
- The KIWISDR parameters come after upstream's: projects saved with upstream 0.1.x / 0.2.0 open with their values. (The
  KIWISDR parameters moved from this fork's 0.2.0 .. 0.2.2 places; the plugin's own saved state is by name, so a
  project keeps them, but MPC automation drawn on them would need redrawing.)
- The RADIO page shows a control of several views once per view (MPC shows a part only while all its view conditions
  hold); `tools/skin_views_check.py` (run by build.sh) checks it.
- `build.sh` can build with a GCC cross toolchain instead of Zig (`ARM_GCC=...`).

## 0.2.0

Tested on an Akai Force (MPC OS 3.9.1).

- PET: a second grain engine, CLOUDS (Mutable Instruments Clouds with the Parasites firmware: granular, stretch,
  looping delay, spectral, Oliverb, Resonestor) beside XXL GRAINS (ENGINE). The knobs show what they do in each mode.
  SCALE snaps PITCH (and CLOUD's scattered grains) to a scale; GRAIN SYNC puts grains on the beat grid.
- TAPE: loops up to 3 minutes; SEND (the loop goes through PET / FX / SPACE / GATE, or plays clean); REC OUT (record
  what you hear instead of the input); overdub the radio on top of a playing loop; UNDO / REDO per layer.
- SAVE / AUTO SAVE: the loop as eviltunerrec01.wav, 02 ... (24-bit) in "Force Documents/Evil Tuner XXL Recordings";
  AUTO SAVE writes every recording and overdub to a new file.
- AIRPLAY: Apple devices listed the receiver but could not connect (the Apple-Response was wrong with mbedTLS 3);
  fixed, and the receiver advertises its own host name and only the address of the network it is on.
- Larger text everywhere; knob names are live.
- The mpc-vst-plugins wrapper's Q-Link / automation UpdateDisplay fix (#486-#489).

## 0.1.1
- Packed under **L'Cronx**: the plugin folder is now `L'Cronx - VST - Evil Tuner XXL` and MPC lists the maker as L'Cronx. Nothing else
  changed from 0.1.0. If you installed 0.1.0, run its `uninstall.sh` first (copy out anything you saved inside the plugin
  folder), then install 0.1.1. The installer handles the ' in the folder name.

## 0.1.0

First release (offline-tested).

- Sources: RADIO (Radio Browser: browse by genre, country, sort and bitrate, search with an on-screen keyboard,
  favourites, SYNC to start with MPC's Play, ICY titles, auto-reconnect), PHONE (Roc / Roc Droid receiver on udp
  10001-10003) and AIRPLAY (classic AirPlay, shows the title), each a static helper program.
- PET: 8-voice granular engine on the last 8 s (position, size, pitch, grains, spread, cloud, detune, shape, freeze,
  KEYS), LP / HP / BP / COMB / DUAL filter with an envelope, oscillator layer, RANDOM.
- FX: Loop Saboteur's eleven output stages, an eight-mode saturator, a look-ahead limiter.
- SPACE: the Erica-style delay from LXRXXL BAZZLINE; Dragonfly Plate, reverse and Room reverb with shimmer.
- GATE: 16-step trance gate with patterns and swing, stutter, MIDI duck.
- TAPE: a rolling 3-minute tape, a tempo-synced looper (capture, overdub, reverse, half / double, undo, to grains) and
  background STRETCH renders (Paulstretch, freeze, stutter, cloud, half tape, reverse).
- MOD: three LFOs (free or synced), an envelope follower, the keys' ADSR.
- Seven pages: a classic tuner RADIO page and six modern effect pages.
