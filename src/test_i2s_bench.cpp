/*
 * I2S I/O measurement bench - no M16, no SdFat.
 *
 * Exists to answer the question that decides the audio architecture: stay on
 * M16's audio task, or feed the DMA from an ISR and write the audio core.
 *
 * v1 of this bench reached the right conclusion from two broken metrics, and
 * v2 exists to fix both:
 *
 *   - cpu% measured cycles spent INSIDE i2s_channel_write(), which blocks. For
 *     a blocking call that is wall time, not CPU, so every task row read ~99%
 *     by construction. Now a low-priority task counts in a tight loop and the
 *     drop against an idle baseline is the CPU that was taken away. That also
 *     captures what the old metric could not see at all: the IDF's own
 *     interrupt prologue, dispatch and queue handling, which happen outside the
 *     callback and grow as the ring gets shallower.
 *   - "underruns" counted on_send_q_ovf, which fires once per completed
 *     descriptor when nothing calls i2s_channel_write(). In the ISR model that
 *     is every descriptor, so it read as catastrophic while the tone was
 *     audibly fine. Now the ISR model times the interval between refills and
 *     flags the late ones, which is what an underrun actually is.
 *
 * Also new: the fill can read four real voices out of PSRAM through the actual
 * mixer (src/mixer.h) instead of a synthetic square wave. v1 measured 17.5
 * cycles per frame for a generator that does one compare and two stores; a
 * voice that reads out of PSRAM can miss cache, and that term is what this
 * project has been calling "sub-microsecond" without ever measuring it. The ISR
 * runs both fills, so the difference between those two rows IS the PSRAM cost.
 *
 * WHAT IT STILL DOES NOT MEASURE
 *
 * Absolute trigger-to-sound latency. The DAC's digital filter adds a fixed
 * ~0.50ms on this board (FLT tied low; 80us if ever strapped high) and cancels
 * out of every comparison here.
 *
 * SCOPE MARKER
 *
 * BENCH_MARKER_GPIO is high for the duration of each ISR fill, written through
 * the GPIO registers so the marker does not inflate what it measures. The cycle
 * counters give min/avg/max; the marker gives the distribution shape, which is
 * what jitter is.
 */
#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_cpu.h>
#include <esp_heap_caps.h>
#include <soc/gpio_reg.h>
#include "mixer.h"

// Free in this design: 4-7 buttons, 10-13 SD, 16-18 I2S, GPIO1 reserved for the
// latency probe in main.cpp.
#define BENCH_MARKER_GPIO 8
static_assert(BENCH_MARKER_GPIO < 32, "marker uses GPIO_OUT_W1TS_REG, which only covers 0-31");

#define BENCH_SAMPLE_RATE 44100
#define BENCH_MS          2000   // per row
#define BENCH_VOICES      4
#define VOICE_FRAMES      44100  // 1s each, ~88KB in PSRAM - big enough to miss cache
#define TONE_PERIOD       100    // frames per cycle -> 441 Hz
#define TONE_AMPLITUDE    8000

// The feeder runs above the counter task, so the counter only gets the core
// when the feeder blocks - that is what makes the counter a CPU measurement
// rather than a wall-clock one.
#define FEEDER_PRIORITY  23
#define COUNTER_PRIORITY 1
#define BENCH_CORE       1

struct BenchCfg { uint32_t descriptors; uint32_t frames; };
static const BenchCfg kConfigs[] = {
    {4, 512}, {4, 256}, {4, 128}, {4, 64}, {4, 32}, {4, 16}, {4, 8},
    {2, 16},  {2, 8},
};

enum FeedModel { FEED_ISR, FEED_TASK_BLOCK, FEED_TASK_PER_SAMPLE };
enum FillKind  { FILL_SYNTH, FILL_VOICES };

struct Row { FeedModel model; FillKind fill; };
static const Row kRows[] = {
    {FEED_ISR, FILL_VOICES},             // the architecture candidate, realistic load
    {FEED_ISR, FILL_SYNTH},              // same, trivial load: the delta is PSRAM
    {FEED_TASK_BLOCK, FILL_VOICES},
    {FEED_TASK_PER_SAMPLE, FILL_VOICES}, // what M16 does today
};

static const char* modelName(FeedModel m) {
  switch (m) {
    case FEED_ISR:             return "ISR on_sent";
    case FEED_TASK_BLOCK:      return "task, block";
    case FEED_TASK_PER_SAMPLE: return "task, 1 frame";
  }
  return "?";
}
static const char* fillName(FillKind f) { return f == FILL_VOICES ? "4 voices/PSRAM" : "synth"; }

// --- State. Plain .bss is internal DRAM, so the ISR can touch it freely. ---
static i2s_chan_handle_t s_tx = NULL;
static int16_t* s_voice[BENCH_VOICES]    = {NULL, NULL, NULL, NULL};
static uint32_t s_voicePos[BENCH_VOICES] = {0, 0, 0, 0};
static FillKind s_fill = FILL_SYNTH;

static volatile uint32_t s_phase       = 0;
static volatile uint32_t s_fills       = 0;
static volatile uint32_t s_cycleMin    = 0xFFFFFFFF;
static volatile uint32_t s_cycleMax    = 0;
static volatile uint64_t s_cycleTotal  = 0;
static volatile uint32_t s_lastCall    = 0;  // cycle stamp of the previous refill
static volatile uint32_t s_gapMax      = 0;
static volatile uint32_t s_late        = 0;  // refills arriving past 2x the period
static volatile uint32_t s_starved     = 0;  // past the whole ring: a real underrun
static volatile uint32_t s_expectedGap = 0;
static volatile uint32_t s_ringGap     = 0;
static volatile uint32_t s_qOverflow   = 0;

// CPU load probe
static volatile uint32_t s_counter = 0;
static TaskHandle_t s_counterTask  = NULL;
static uint32_t s_baselineRate     = 0;  // counts per ms with nothing else running

static void counterTaskFn(void*) {
  for (;;) s_counter = s_counter + 1;
}

/* Samples the counter over ms milliseconds, in counts per ms. The caller must
 * be blocked for the whole window, or it measures itself. */
static uint32_t counterRate(uint32_t ms) {
  uint32_t c0 = s_counter;
  uint32_t t0 = millis();
  delay(ms);
  uint32_t dt = millis() - t0;
  uint32_t dc = s_counter - c0;
  return dt ? dc / dt : 0;
}

static inline void statsReset(const BenchCfg& cfg) {
  s_phase = 0; s_fills = 0; s_cycleMin = 0xFFFFFFFF; s_cycleMax = 0;
  s_cycleTotal = 0; s_lastCall = 0; s_gapMax = 0; s_late = 0; s_starved = 0;
  s_qOverflow = 0;
  for (int v = 0; v < BENCH_VOICES; v++) s_voicePos[v] = v * 1000; // spread the reads
  uint64_t cyclesPerFrame = (uint64_t)getCpuFrequencyMhz() * 1000000ULL / BENCH_SAMPLE_RATE;
  s_expectedGap = (uint32_t)(cyclesPerFrame * cfg.frames);
  s_ringGap     = (uint32_t)(cyclesPerFrame * cfg.frames * cfg.descriptors);
}

/* Trivial generator: one compare and two stores per frame. */
static inline void IRAM_ATTR fillSynth(int16_t* dst, size_t frames) {
  uint32_t ph = s_phase;
  for (size_t i = 0; i < frames; i++) {
    int16_t v = (ph < TONE_PERIOD / 2) ? TONE_AMPLITUDE : -TONE_AMPLITUDE;
    dst[2 * i]     = v;
    dst[2 * i + 1] = v;
    if (++ph >= TONE_PERIOD) ph = 0;
  }
  s_phase = ph;
}

/* Four voices read out of PSRAM, summed through the real mixer: per-voice Q8
 * gain, sum in int32, one limit at the end - the shape audioUpdate() has. The
 * reads are what can miss cache. */
static inline void IRAM_ATTR fillVoices(int16_t* dst, size_t frames) {
  uint32_t pos[BENCH_VOICES];
  for (int v = 0; v < BENCH_VOICES; v++) pos[v] = s_voicePos[v];

  for (size_t i = 0; i < frames; i++) {
    int32_t mix = 0;
    for (int v = 0; v < BENCH_VOICES; v++) {
      mix += mixApplyGain(s_voice[v][pos[v]], MIX_GAIN_UNITY / 2);
      if (++pos[v] >= VOICE_FRAMES) pos[v] = 0;
    }
    int16_t out = mixSoftClip(mix);
    dst[2 * i]     = out;
    dst[2 * i + 1] = out;
  }

  for (int v = 0; v < BENCH_VOICES; v++) s_voicePos[v] = pos[v];
}

static inline void IRAM_ATTR fillFrames(int16_t* dst, size_t frames) {
  if (s_fill == FILL_VOICES) fillVoices(dst, frames);
  else                       fillSynth(dst, frames);
}

/* on_sent: the descriptor just drained, refill it in place. No task wakeup, no
 * semaphore wait - the hardware sets the pace and we answer. IRAM_ATTR because
 * an ISR living in flash freezes whenever the cache is disabled. */
static bool IRAM_ATTR onSent(i2s_chan_handle_t, i2s_event_data_t* evt, void*) {
  uint32_t t0 = esp_cpu_get_cycle_count();
  REG_WRITE(GPIO_OUT_W1TS_REG, 1u << BENCH_MARKER_GPIO);

  // Interval since the previous refill. Late means the fill is falling behind
  // the DMA; past the whole ring means the DAC has already replayed stale data,
  // which is what an underrun actually is here.
  if (s_lastCall) {
    uint32_t gap = t0 - s_lastCall;
    if (gap > s_gapMax)          s_gapMax = gap;
    if (gap > 2 * s_expectedGap) s_late = s_late + 1;
    if (gap > s_ringGap)         s_starved = s_starved + 1;
  }
  s_lastCall = t0;

  // dma_buf, NOT the deprecated evt->data: the header calls data "the secondary
  // pointer" and dma_buf "the first level pointer of DMA buffer that just
  // finished sending". Different levels of indirection.
  fillFrames((int16_t*)evt->dma_buf, evt->size / 4); // 4 bytes per stereo frame

  REG_WRITE(GPIO_OUT_W1TC_REG, 1u << BENCH_MARKER_GPIO);

  uint32_t dt = esp_cpu_get_cycle_count() - t0;
  s_fills      = s_fills + 1;
  s_cycleTotal = s_cycleTotal + dt;
  if (dt < s_cycleMin) s_cycleMin = dt;
  if (dt > s_cycleMax) s_cycleMax = dt;
  return false;
}

/* Reported, not trusted. In the ISR model this fires once per completed
 * descriptor because nothing consumes the driver's send queue - it is NOT an
 * underrun, which is exactly the mistake v1 made. */
static bool IRAM_ATTR onSendOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  s_qOverflow = s_qOverflow + 1;
  return false;
}

static esp_err_t channelOpen(const BenchCfg& cfg, FeedModel model) {
  i2s_chan_config_t chanCfg = {
      .id            = I2S_NUM_0,
      .role          = I2S_ROLE_MASTER,
      .dma_desc_num  = cfg.descriptors,
      .dma_frame_num = cfg.frames,
      .auto_clear    = true,
  };
  esp_err_t err = i2s_new_channel(&chanCfg, &s_tx, NULL);
  if (err != ESP_OK) return err;

  i2s_std_config_t stdCfg = {
      .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(BENCH_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = GPIO_NUM_16,
          .ws   = GPIO_NUM_17,
          .dout = GPIO_NUM_18,
          .din  = I2S_GPIO_UNUSED,
          .invert_flags = {false, false, false},
      },
  };
  err = i2s_channel_init_std_mode(s_tx, &stdCfg);
  if (err != ESP_OK) { i2s_del_channel(s_tx); s_tx = NULL; return err; }

  i2s_event_callbacks_t cbs = {};
  cbs.on_send_q_ovf = onSendOverflow;
  if (model == FEED_ISR) cbs.on_sent = onSent;
  err = i2s_channel_register_event_callback(s_tx, &cbs, NULL);
  if (err != ESP_OK) { i2s_del_channel(s_tx); s_tx = NULL; return err; }

  err = i2s_channel_enable(s_tx);
  if (err != ESP_OK) { i2s_del_channel(s_tx); s_tx = NULL; return err; }
  return ESP_OK;
}

static void channelClose() {
  if (!s_tx) return;
  i2s_channel_disable(s_tx);
  i2s_del_channel(s_tx);
  s_tx = NULL;
}

// --- Feeder task, above the counter so the counter measures CPU ---
static volatile bool s_feederRun = false;
static TaskHandle_t  s_feeder    = NULL;
static BenchCfg      s_feederCfg = {0, 0};
static FeedModel     s_feederModel = FEED_TASK_BLOCK;

static inline void recordWrite(uint32_t dt) {
  s_fills      = s_fills + 1;
  s_cycleTotal = s_cycleTotal + dt;
  if (dt < s_cycleMin) s_cycleMin = dt;
  if (dt > s_cycleMax) s_cycleMax = dt;
}

static void feederTaskFn(void*) {
  static int16_t block[2 * 512];
  size_t frames = s_feederCfg.frames;
  if (frames > 512) frames = 512;

  while (s_feederRun) {
    size_t wrote = 0;
    if (s_feederModel == FEED_TASK_PER_SAMPLE) {
      int16_t frame[2];
      fillFrames(frame, 1);
      uint32_t t0 = esp_cpu_get_cycle_count();
      i2s_channel_write(s_tx, frame, 4, &wrote, portMAX_DELAY);
      recordWrite(esp_cpu_get_cycle_count() - t0);
    } else {
      fillFrames(block, frames);
      uint32_t t0 = esp_cpu_get_cycle_count();
      i2s_channel_write(s_tx, block, frames * 4, &wrote, portMAX_DELAY);
      recordWrite(esp_cpu_get_cycle_count() - t0);
    }
  }
  s_feeder = NULL;
  vTaskDelete(NULL);
}

static void runRow(const BenchCfg& cfg, const Row& row) {
  float ringMs = 1000.0f * cfg.descriptors * cfg.frames / BENCH_SAMPLE_RATE;
  Serial.printf("%u x %-3u %6.2fms  %-14s %-15s ", (unsigned)cfg.descriptors,
                (unsigned)cfg.frames, ringMs, modelName(row.model), fillName(row.fill));

  s_fill = row.fill;
  statsReset(cfg);

  esp_err_t err = channelOpen(cfg, row.model);
  if (err != ESP_OK) {
    Serial.printf("REJECTED: %s\n", esp_err_to_name(err));
    return;
  }

  if (row.model != FEED_ISR) {
    s_feederCfg   = cfg;
    s_feederModel = row.model;
    s_feederRun   = true;
    xTaskCreatePinnedToCore(feederTaskFn, "feeder", 4096, NULL, FEEDER_PRIORITY,
                            &s_feeder, BENCH_CORE);
  }

  uint32_t rate = counterRate(BENCH_MS); // blocks here; the work happens around us

  if (row.model != FEED_ISR) {
    s_feederRun = false;
    while (s_feeder) delay(1);
  }
  channelClose();

  float cpuPct = s_baselineRate ? 100.0f * (1.0f - (float)rate / (float)s_baselineRate) : 0.0f;
  if (cpuPct < 0) cpuPct = 0;

  uint32_t fills = s_fills;
  float perSec = 1000.0f * fills / BENCH_MS;
  float avgCyc = fills ? (float)s_cycleTotal / fills : 0.0f;
  float gapPct = s_expectedGap ? 100.0f * s_gapMax / s_expectedGap : 0.0f;

  Serial.printf("cpu %5.1f%%  fills/s %7.0f  cyc %6u/%8.0f/%7u  ",
                cpuPct, perSec, (unsigned)(fills ? s_cycleMin : 0), avgCyc,
                (unsigned)s_cycleMax);
  if (row.model == FEED_ISR) {
    Serial.printf("gap_max %4.0f%%  late %u  STARVED %u  qovf %u\n",
                  gapPct, (unsigned)s_late, (unsigned)s_starved, (unsigned)s_qOverflow);
  } else {
    Serial.printf("qovf %u\n", (unsigned)s_qOverflow);
  }
}

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println();
  Serial.println("=== I2S I/O bench v2 (no M16) ===");
  Serial.printf("CPU %u MHz, %d Hz stereo 16-bit, BCK=16 WS=17 DOUT=18, marker GPIO%d\n",
                getCpuFrequencyMhz(), BENCH_SAMPLE_RATE, BENCH_MARKER_GPIO);

  pinMode(BENCH_MARKER_GPIO, OUTPUT);
  digitalWrite(BENCH_MARKER_GPIO, LOW);

  // A priority-1 task in a tight loop starves IDLE on this core, which trips
  // the task watchdog. This is a bench, so drop the watchdog rather than
  // distort the measurement with a periodic yield.
  disableCore1WDT();

  for (int v = 0; v < BENCH_VOICES; v++) {
    s_voice[v] = (int16_t*)heap_caps_malloc(VOICE_FRAMES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_voice[v]) {
      Serial.println("PSRAM alloc FAILED - the 4-voice rows cannot run");
      while (1) delay(1000);
    }
    for (uint32_t i = 0; i < VOICE_FRAMES; i++) // content is irrelevant, only the reads matter
      s_voice[v][i] = (int16_t)((i * (7 + v)) & 0x3FFF) - 8192;
  }
  Serial.printf("4 voice buffers of %u frames in PSRAM (%u KB)\n",
                VOICE_FRAMES, (unsigned)(BENCH_VOICES * VOICE_FRAMES * 2 / 1024));

  xTaskCreatePinnedToCore(counterTaskFn, "cpucount", 2048, NULL, COUNTER_PRIORITY,
                          &s_counterTask, BENCH_CORE);
  delay(200);
  s_baselineRate = counterRate(1000);
  Serial.printf("CPU probe baseline: %u counts/ms on core %d\n\n",
                (unsigned)s_baselineRate, BENCH_CORE);

  for (unsigned c = 0; c < sizeof(kConfigs) / sizeof(kConfigs[0]); c++) {
    for (unsigned r = 0; r < sizeof(kRows) / sizeof(kRows[0]); r++) {
      runRow(kConfigs[c], kRows[r]);
      delay(150);
    }
    Serial.println();
  }

  Serial.println("=== done ===");
  Serial.println("cpu% is real CPU taken from a low-priority counter, so it includes");
  Serial.println("the IDF interrupt dispatch. STARVED is the only true underrun column;");
  Serial.println("qovf is just the unused send queue overflowing. The two ISR rows differ");
  Serial.println("only in the fill, so their gap is the cost of reading PSRAM.");
}

void loop() { delay(1000); }
