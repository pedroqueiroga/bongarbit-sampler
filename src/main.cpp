/*
 * bongarbit-sampler
 *
 * One-shot sample triggers via 4 buttons (GPIO4/5/6/7), each playing a
 * sample out to a PCM5102A I2S DAC via the M16 library. Pressing a button
 * again while its sample is playing restarts it from the beginning.
 *
 * Hardware: ESP32-S3 N16R8 + PCM5102A (I2S) + microSD (SPI, optional - see
 * USE_SD_SAMPLER below). Full wiring plan: docs/circuit.md
 *
 * Two sample sources, selected at compile time. Both play through the same
 * Samp engine (M16's wavetable-style sample player) - the only difference is
 * where the raw audio buffers come from:
 *  - USE_SD_SAMPLER=1: at boot, mounts the SD card and loads whole .wav
 *    files into RAM (M16's Wav::load() tries PSRAM first, so on this N16R8
 *    board samples land in the 8MB PSRAM). SD is only touched once at
 *    startup, not during playback - the card can be removed after boot.
 *    GPIO4 plays FILE_INDEX (whichever file the card lists first); GPIO5/6/7
 *    play 1.wav/2.wav/3.wav by name.
 *  - USE_SD_SAMPLER=0: GPIO4 plays a sample embedded directly in flash
 *    (src/kick_normalizado_data.h, generated from src/kick_normalizado.wav via
 *    tools/wav_to_header.py) - no SD card needed at all. GPIO5/6/7 pads are
 *    not available in this mode.
 */

// --- Audio output latency (MUST be set before including M16.h) ---
// M16 feeds the I2S DMA ring one sample at a time and blocks when it is full,
// so the ring depth IS the trigger-to-sound latency, essentially all of it.
// The library default is 4 x 512 = 2048 frames = 46ms at 44.1kHz, sized to
// survive a flash sector erase on the host (~30-40ms).
//
// That worst case does not exist in this firmware: samples are resident in
// PSRAM, the SD card is never touched after boot, there is no WiFi, and
// nothing writes to flash at runtime. The only producer stall left is a PSRAM
// cache miss inside audioUpdate(), which is sub-microsecond. M16's own comment
// names this exact case as the reason the values are overridable
// ("percussive/Beat Machine style").
//
// 4 x 64 = 256 frames = 5.8ms. VALIDATED on hardware 13/08/2026: clean on a
// fast roll with all four voices sounding at once, which is the heaviest
// audioUpdate() path. See docs/bancada.md, "Fase E".
//
// Getting that answer required removing two confounds that both sound exactly
// like an underrun, and which made 5.8ms appear to fail:
//   - 8-bit source samples. A ~48dB quantisation floor crackles on decay tails.
//   - Clipping. audioUpdate() sums voices and clips once at the end, so two
//     normalised samples (89% of full scale) sum to 178% and clip hard.
// The ladder only became meaningful with 16-bit/44.1kHz samples attenuated to
// ~22% each, where four voices sum to 89% and nothing clips.
//
// Tuning ladder, if it ever needs walking again:
//   clicks/dropouts  -> raise DMA_BUFFER_LENGTH (128 = 11.6ms, 256 = 23ms)
//   clean, want less -> 4 x 32 = 2.9ms, then 4 x 16 = 1.5ms
// Test on a fast roll with all four voices at once, on samples that neither
// clip nor carry their own noise floor - otherwise the ladder measures those.
#define DMA_BUFFERS       4
#define DMA_BUFFER_LENGTH 64

#include "M16.h"
#include "Samp.h"

// The override above only works while it sits BEFORE the include - M16 guards
// its own values with #ifndef. Moving it below, or dropping it, silently
// restores the 2048-frame default and puts 46ms back into the trigger path
// with no other symptom. Fail the build instead.
// The bound only has to be below M16's own default of 4 x 512 = 2048 frames;
// anything smaller proves the override was seen. 512 leaves room for the two
// rungs above the validated 256 without being anywhere near the default. It was
// briefly raised to 1024 on the bench of 13/08/2026 to try 4 x 256, back when
// clipping was being mistaken for an underrun - that reason did not survive.
static_assert(DMA_BUFFERS * DMA_BUFFER_LENGTH <= 512,
              "I2S ring deeper than 512 frames (~12ms) - the DMA override has "
              "drifted below #include \"M16.h\" and M16's default came back");

// --- Sample source: 1 = load one .wav from SD into RAM/PSRAM at boot,
//                    0 = play from embedded flash array ---
#define USE_SD_SAMPLER 1

#if USE_SD_SAMPLER
  #include "Wav.h"
#else
  #include "kick_normalizado_data.h"
#endif

// --- Debug logging over Serial ---
// Split in two, because the two kinds of logging have very different costs.
//
// DEBUG_BOOT covers setup() only - SD mount, WAV load, DAC start. It runs
// before any audio and costs nothing at runtime. It is also what tells you
// PSRAM and the card actually came up, so leave it on.
//
// DEBUG_TRIGGERS prints one line per strike, from inside the polling loop.
// Serial over USB CDC blocks when the host is not draining the buffer, which
// puts an unbounded stall directly in the trigger path. Off by default: turn
// it on to diagnose an input problem, never while judging latency.
#define DEBUG_BOOT     1
#define DEBUG_TRIGGERS 0
#if DEBUG_BOOT
  #define DBG(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
#endif
#if DEBUG_TRIGGERS
  #define TRIGDBG(...) Serial.printf(__VA_ARGS__)
#else
  #define TRIGDBG(...)
#endif

// --- Trigger conditioning, shared by all four buttons ---
// The state machine and the rationale for both its windows live in
// trigger_input.h, which is compiled into this firmware AND into the host test
// suite. Keeping one definition is what makes test/test_trigger able to make
// claims about this binary rather than about a transcription of it.
//
// To change TRIG_CONFIRM_US or TRIG_RELEASE_MS, define them before this
// include - the header guards its defaults with #ifndef, same as the DMA
// override above.
#include "trigger_input.h"

// --- Mixer gain staging and output limiting (see src/mixer.h) ---
// Two independent switches, both on, both reversible, because they change how
// the instrument sounds and that is not a decision to make silently.
//
// MIXER_GAIN_STAGING gives every voice its own Q8 level plus a master level -
// the MPC model. It defaults to unity everywhere, so turning it on changes
// nothing audible until a kit is actually dialled in; it exists so that the
// control is there at all. Deliberately NOT a divide-by-voice-count, which
// would cost 12dB on one voice to protect a worst case that rarely happens.
//
// MIXER_SOFT_CLIP replaces the hard clamp with a knee that is the identity
// below 75% of full scale and asymptotic to full scale above it. This is the
// part that removes the clicks: with a normalised kit, two voices sum past
// full scale and hard-clamping a transient is what produced them.
//
// Set MIXER_SOFT_CLIP to 0 to get the previous hard clamp back for comparison.
#define MIXER_GAIN_STAGING 1
#define MIXER_SOFT_CLIP    1

#include "mixer.h"

#if MIXER_GAIN_STAGING
  #define MIXGAIN(s, g) mixApplyGain((s), (g))
#else
  #define MIXGAIN(s, g) ((int32_t)(s))
#endif

#if MIXER_SOFT_CLIP
  #define MIXCLIP(x) mixSoftClip(x)
#else
  #define MIXCLIP(x) mixHardClip(x)
#endif

#if MIXER_GAIN_STAGING
// Per-voice and master levels, Q8 (MIX_GAIN_UNITY = 256 = unity). These are
// the knobs a kit is balanced with: lower a loud sample here rather than
// re-normalising the .wav. Unity by default - the headroom convention is that
// kit material carries ~6dB of its own, same as on an MPC.
int32_t trigGain   = MIX_GAIN_UNITY;
int32_t masterGain = MIX_GAIN_UNITY;
#endif

#if USE_SD_SAMPLER
// --- Which WAV file to load (0 = first file found on the card) ---
#define FILE_INDEX 0

// --- microSD SPI pins (see docs/circuit.md) ---
#define SD_CS   10
#define SD_SCLK 12
#define SD_MISO 13
#define SD_MOSI 11

SdFs sd;
Wav  wav;

// --- Extra pads: 1.wav / 2.wav / 3.wav on the SD card root ---
#define PAD_COUNT 3
const uint8_t   padPins[PAD_COUNT]  = {5, 6, 7};
const char*     padFiles[PAD_COUNT] = {"/1.wav", "/2.wav", "/3.wav"};
Wav             padWav[PAD_COUNT];
Samp            padSamp[PAD_COUNT];
uint8_t         padChannels[PAD_COUNT] = {1, 1, 1};
TriggerInput    padInput[PAD_COUNT];
#if MIXER_GAIN_STAGING
// Per-pad level, Q8. Balance the kit here.
int32_t         padGain[PAD_COUNT] = {MIX_GAIN_UNITY, MIX_GAIN_UNITY, MIX_GAIN_UNITY};
#endif
#endif

// --- Latency probe (see docs/circuit-rc.md, "Medir de verdade") ---
// Measures real button-to-sound by watching the DAC's own analog output, which
// is the only way to capture the part that happens after the CPU writes: the
// DMA ring draining and the DAC's digital filter. A GPIO marker cannot see it.
//
// Needs a biased divider feeding an ADC pin - two 10k from 3V3/GND to make a
// 1.65V midpoint, DAC output AC-coupled in through 100nF. Silence then sits
// mid-scale and the signal swings around it without ever going negative, which
// the ADC could not survive.
//
// Off by default: it puts a blocking analogRead() in loop() and is a bench
// instrument, not a runtime feature.
#define LATENCY_PROBE 0

#if LATENCY_PROBE
  #define PROBE_ADC_PIN     1       // GPIO1 = ADC1_CH0, free in this design
  #define PROBE_TIMEOUT_US  50000
  #define PROBE_BASELINE_N  256

  uint16_t    probeBaseline  = 0;
  uint16_t    probeThreshold = 0;
  bool        probeArmed     = false;
  uint32_t    probeEdgeUs    = 0;   // raw pin went LOW
  uint32_t    probeFireUs    = 0;   // samp.start() was called
  const char* probeSource    = "";

  // Sample the idle DAC output to learn where silence sits and how noisy it is,
  // so the detection threshold is derived from the actual rig rather than
  // guessed. Must run after audioStart(), with nothing playing.
  void probeCalibrate() {
    uint32_t sum = 0, lo = 4095, hi = 0;
    for (int i = 0; i < PROBE_BASELINE_N; i++) {
      uint16_t v = analogRead(PROBE_ADC_PIN);
      sum += v;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    probeBaseline  = sum / PROBE_BASELINE_N;
    uint16_t noise = hi - lo;
    probeThreshold = noise * 4 + 40;  // 4x the observed idle spread, plus floor
    Serial.printf("PROBE: baseline=%u noise=%u threshold=%u (GPIO%d)\n",
                  probeBaseline, noise, probeThreshold, PROBE_ADC_PIN);
    if (noise > 200)
      Serial.println("PROBE: idle noise is high - check the divider and grounding");
  }

  void probeArm(const char* source, uint32_t edgeUs, uint32_t fireUs) {
    if (probeArmed) return;         // a measurement is already in flight
    probeArmed  = true;
    probeSource = source;
    probeEdgeUs = edgeUs;
    probeFireUs = fireUs;
  }

  // Poll the DAC output until it leaves the silence band. Reports both halves
  // of the chain: what the firmware spent deciding, and what the audio path
  // added after start() - the second is the number the DMA ring controls.
  void probePoll() {
    if (!probeArmed) return;
    uint32_t now = micros();
    int      v   = analogRead(PROBE_ADC_PIN);
    if (abs(v - (int)probeBaseline) > (int)probeThreshold) {
      Serial.printf("PROBE %s: total %lu us  (trigger %lu + audio path %lu)\n",
                    probeSource,
                    (unsigned long)(now - probeEdgeUs),
                    (unsigned long)(probeFireUs - probeEdgeUs),
                    (unsigned long)(now - probeFireUs));
      probeArmed = false;
    } else if ((uint32_t)(now - probeFireUs) > PROBE_TIMEOUT_US) {
      Serial.printf("PROBE %s: no audio within %d us - sample silent at its "
                    "start, or the divider is not connected\n",
                    probeSource, PROBE_TIMEOUT_US);
      probeArmed = false;
    }
  }
#endif

// --- Trigger button (see docs/circuit.md) ---
// One leg on TRIGGER_PIN (internal pull-up, pressed = LOW), the other leg
// on a real GND pin on the dev kit. Runs the same TriggerInput as the pads -
// it used to have its own wait-for-stable debounce costing 25ms, which was
// most of this button's trigger-to-sound time.
#define TRIGGER_PIN 4
TriggerInput trigInput;

Samp    samp;
uint8_t sampleChannels = 1; // set at boot from whichever source is used below

void setup() {
  Serial.begin(115200);
  delay(500);

  // PSRAM is the single most important thing this log can confirm, and its
  // absence used to be silent. M16's psramAllocSafe() has no internal-RAM
  // fallback, so without PSRAM every Wav::load() below fails and the only
  // symptom is "WAV: failed to load" with no stated cause - which sends you
  // debugging the SD wiring for a build configuration problem. Report it
  // before anything can depend on it, and shout when it is missing (same
  // convention as the SD failure below: errors are unconditional, successes
  // go through DBG).
  size_t psramTotal = ESP.getPsramSize();
  if (psramTotal == 0) {
    Serial.println("PSRAM: NOT AVAILABLE - samples cannot load. This is a "
                   "board/build configuration problem, not wiring: check that "
                   "platformio.ini still has board = esp32-s3-devkitc1-n16r8");
  } else {
    DBG("PSRAM: %u bytes free of %u\n",
        (unsigned)ESP.getFreePsram(), (unsigned)psramTotal);
  }

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

#if USE_SD_SAMPLER
  // initSD() already lists the .wav files it finds on the card (Serial).
  if (!wav.initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, 16)) {
    Serial.print("SD: init FAILED - ");
    sd.printSdError(&Serial); // prints the specific SdFat error (e.g. CMD0 = card not responding at all)
    Serial.println("Check wiring (docs/circuit.md)");
    while (1) delay(1000);
  }
  DBG("SD: init OK\n");

  // Loads the whole file into RAM (PSRAM preferred, tried first by the
  // library) - a one-time blocking read at boot, not during playback.
  if (!wav.loadNumber(FILE_INDEX)) {
    Serial.println("WAV: failed to load file at index " + String(FILE_INDEX));
    while (1) delay(1000);
  }

  sampleChannels = wav.getChannels();
  DBG("WAV: loaded '%s' - %u ch, %lu Hz, %lu frames (~%lu ms)\n",
      wav.getFilename(), sampleChannels, (unsigned long)wav.getSampleRate(),
      wav.getFrameCount(),
      (unsigned long)(1000ULL * wav.getFrameCount() / wav.getSampleRate()));

  // Samp::setTable() computes the buffer-rate -> output-rate ratio itself,
  // so the sample plays at its original speed regardless of the file's
  // native sample rate - no need to reconfigure the I2S clock here.
  samp.setTable(wav.getBuffer(), wav.getFrameCount(), wav.getSampleRate(), sampleChannels);

  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    pinMode(padPins[i], INPUT_PULLUP);

    if (!padWav[i].initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, 16) ||
        !padWav[i].load(padFiles[i])) {
      Serial.printf("Pad %u: failed to load %s\n", i + 1, padFiles[i]);
      continue;
    }
    padChannels[i] = padWav[i].getChannels();
    padSamp[i].setTable(padWav[i].getBuffer(), padWav[i].getFrameCount(),
                         padWav[i].getSampleRate(), padChannels[i]);
    DBG("Pad %u: loaded '%s' - %u ch, %lu Hz, %lu frames (~%lu ms)\n",
        i + 1, padWav[i].getFilename(), padChannels[i],
        (unsigned long)padWav[i].getSampleRate(), padWav[i].getFrameCount(),
        (unsigned long)(1000ULL * padWav[i].getFrameCount() / padWav[i].getSampleRate()));
  }
#else
  sampleChannels = KICK_NORMALIZADO_CHANNELS;
  // KICK_NORMALIZADO_DATA is const (lives in flash/.rodata) - setTable() only
  // reads from it during playback, so the cast away from const is safe.
  samp.setTable(const_cast<int16_t*>(KICK_NORMALIZADO_DATA), KICK_NORMALIZADO_FRAME_COUNT,
                KICK_NORMALIZADO_SAMPLE_RATE, sampleChannels);
  DBG("Flash sample: %lu frames, %u ch, %u Hz (~%lu ms)\n",
      KICK_NORMALIZADO_FRAME_COUNT, sampleChannels, KICK_NORMALIZADO_SAMPLE_RATE,
      (unsigned long)(1000ULL * KICK_NORMALIZADO_FRAME_COUNT / KICK_NORMALIZADO_SAMPLE_RATE));
#endif

  // DOUT is GPIO8, not M16's default GPIO18. On this board GPIO18 does not
  // drive: with the data wire on 18 the DAC's DIN sits at 0V through an entire
  // sample, while BCK/WS on 16/17 read the expected ~1.6V average. Moving the
  // same wire to 16 or 17 puts 1.6V on DIN and makes the DAC audibly convert
  // the clock as garbage - so the wire, the DAC and the whole analog path are
  // fine and the pin is not. Not diagnosed further; GPIO8 is free here.
  //
  // din = -1 also disables the I2S RX channel. M16's default set is
  // {16, 17, 18, 21}, and any din other than -1 makes audioStart() allocate an
  // RX channel and re-run i2s_channel_init_std_mode() with the same std_cfg.
  // This project has no I2S input (docs/circuit.md lists GPIO21 as unused).
  seti2sPins(16, 17, 18, -1);

  audioStart();
  DBG("DAC: I2S started - BCK=GPIO16 WS=GPIO17 DOUT=GPIO18, RX disabled\n");
  DBG("DAC: PCM5102A has no status feedback pin - the 'M16 is running' line "
      "above is the only software confirmation; audible output still "
      "depends on correct wiring (docs/circuit.md)\n");
#if LATENCY_PROBE
  probeCalibrate(); // after audioStart(), while nothing is playing
#endif
  Serial.println("Ready. Press the trigger button to play the sample.");
}

void loop() {
  // One timestamp for the whole sweep, so all four inputs are conditioned
  // against the same instant. Nothing blocking belongs in this function: with
  // DEBUG_TRIGGERS off it is four digitalRead()s and some arithmetic, which
  // keeps the poll period in the low tens of microseconds.
  uint32_t now_us = micros();

  if (trigInput.update(now_us, digitalRead(TRIGGER_PIN) == LOW)) {
    TRIGDBG("Trigger: GPIO%d\n", TRIGGER_PIN);
    samp.start();
#if LATENCY_PROBE
    probeArm("trigger", trigInput.pendingSince, micros());
#endif
  }

#if USE_SD_SAMPLER
  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    if (padInput[i].update(now_us, digitalRead(padPins[i]) == LOW)) {
      TRIGDBG("Pad %u: triggered\n", i + 1);
      padSamp[i].start();
#if LATENCY_PROBE
      probeArm(padFiles[i], padInput[i].pendingSince, micros());
#endif
    }
  }
#endif

#if LATENCY_PROBE
  probePoll();
#endif
}

void audioUpdate() {
  int16_t L, R;
  if (sampleChannels == 2) {
    samp.nextStereo(L, R);
  } else {
    L = R = samp.next(); // nextStereo() only works for stereo buffers (num_channels==2)
  }
  int32_t mixL = MIXGAIN(L, trigGain), mixR = MIXGAIN(R, trigGain);

#if USE_SD_SAMPLER
  // Pads can play concurrently with the main button and with each other -
  // sum all voices and clip, instead of just outputting the last one.
  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    int16_t pl, pr;
    if (padChannels[i] == 2) {
      padSamp[i].nextStereo(pl, pr);
    } else {
      pl = pr = padSamp[i].next();
    }
    mixL += MIXGAIN(pl, padGain[i]);
    mixR += MIXGAIN(pr, padGain[i]);
  }
#endif

#if MIXER_GAIN_STAGING
  mixL = mixApplyGain(mixL, masterGain);
  mixR = mixApplyGain(mixR, masterGain);
#endif

  i2s_write_samples(MIXCLIP(mixL), MIXCLIP(mixR));
}
