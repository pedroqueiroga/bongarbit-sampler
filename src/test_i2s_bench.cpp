/*
 * I2S I/O measurement bench - no M16, no SdFat, no samples.
 *
 * Exists to answer the questions that decide the audio architecture, none of
 * which can be answered by reading code:
 *
 *   1. What is the real minimum dma_frame_num the ESP-IDF driver accepts?
 *      The latency ladder in main.cpp assumes it goes down to 16, or even to
 *      2 descriptors of 16. That is arithmetic, not a measured limit.
 *   2. What does M16's one-write-per-sample path actually cost? It calls
 *      i2s_channel_write() with 4 bytes, 44100 times a second. The suspicion
 *      is that this overhead is what stops shallow rings from working.
 *   3. Is an ISR-fill model (i2s_channel_register_event_callback + on_sent)
 *      cheaper, and by how much? That is the model that would replace M16's
 *      audio core if the answer is "a lot".
 *   4. Where do underruns actually start?
 *
 * It measures all four by running the same square wave through three feed
 * models at every ring depth the driver accepts, and reporting the share of
 * one core each model burns plus the underrun count.
 *
 * WHAT IT DOES NOT MEASURE
 *
 * Absolute trigger-to-sound latency. The DAC's own digital filter adds a fixed
 * ~0.50ms on this board (FLT tied low; 80us if it is ever strapped high), and
 * that offset is identical in every configuration here. It cancels out of the
 * comparison, which is why this bench is still useful without touching it.
 *
 * READING THE OUTPUT
 *
 * cpu% is the share of one core spent inside the feed path - driver calls for
 * the task models, the callback for the ISR model. Lower is what buys shallow
 * rings: the overhead per sample is what makes a 16-frame ring impossible.
 *
 * underruns counts on_send_q_ovf events: the DMA reached a descriptor with
 * nothing new in it. Non-zero means that configuration does not hold.
 *
 * SCOPE MARKER
 *
 * BENCH_MARKER_GPIO goes high for the duration of each ISR fill, written with
 * the raw GPIO registers rather than gpio_set_level() so the marker does not
 * inflate what it is measuring. Probe it to see jitter directly - the cycle
 * histogram below gives min/avg/max, but not the distribution shape.
 */
#include <Arduino.h>
#include <driver/i2s_std.h>
#include <esp_cpu.h>
#include <soc/gpio_reg.h>

// Free in this design: 4-7 are buttons, 10-13 the SD card, 16-18 I2S, and
// GPIO1 is reserved for the latency probe in main.cpp.
#define BENCH_MARKER_GPIO 8
static_assert(BENCH_MARKER_GPIO < 32, "marker uses GPIO_OUT_W1TS_REG, which only covers 0-31");

#define BENCH_SAMPLE_RATE 44100
#define BENCH_SECONDS     3      // per configuration, per feed model
#define TONE_PERIOD       100    // frames per cycle -> 441 Hz
#define TONE_AMPLITUDE    8000   // well clear of full scale; this is not a level test

// Ring depths to try, deepest first. The last three are the rungs the latency
// ladder in main.cpp wants but has never reached.
struct BenchCfg { uint32_t descriptors; uint32_t frames; };
static const BenchCfg kConfigs[] = {
    {4, 512}, // M16's default: 46.4ms
    {4, 256}, // 23.2ms
    {4, 128}, // 11.6ms
    {4, 64},  //  5.8ms - validated by ear on 13/08/2026
    {4, 32},  //  2.9ms
    {4, 16},  //  1.5ms
    {4, 8},   //  0.73ms
    {2, 16},  //  0.73ms with the minimum descriptor count for double buffering
    {2, 8},   //  0.36ms
};

enum FeedModel { FEED_ISR, FEED_TASK_BLOCK, FEED_TASK_PER_SAMPLE };

static const char* feedName(FeedModel m) {
  switch (m) {
    case FEED_ISR:             return "ISR on_sent";
    case FEED_TASK_BLOCK:      return "task, block";
    case FEED_TASK_PER_SAMPLE: return "task, 1 frame (M16)";
  }
  return "?";
}

// --- Shared state. Plain .bss, which is internal DRAM, so the ISR can touch
// it without risking a cache miss on PSRAM. ---
static i2s_chan_handle_t s_tx = NULL;
static volatile uint32_t s_phase      = 0;
static volatile uint32_t s_fills      = 0;
static volatile uint32_t s_underruns  = 0;
static volatile uint32_t s_cycleMin   = 0xFFFFFFFF;
static volatile uint32_t s_cycleMax   = 0;
static volatile uint64_t s_cycleTotal = 0;

static inline void statsReset() {
  s_phase = 0; s_fills = 0; s_underruns = 0;
  s_cycleMin = 0xFFFFFFFF; s_cycleMax = 0; s_cycleTotal = 0;
}

static inline void statsRecord(uint32_t cycles) {
  // Explicit read-modify-write: ++ on a volatile is deprecated in C++20.
  s_fills = s_fills + 1;
  s_cycleTotal = s_cycleTotal + cycles;
  if (cycles < s_cycleMin) s_cycleMin = cycles;
  if (cycles > s_cycleMax) s_cycleMax = cycles;
}

/* Generate the test signal into a frame buffer. Deliberately trivial - this
 * bench measures the cost of moving samples, not of producing them. */
static inline void IRAM_ATTR fillFrames(int16_t* dst, size_t frames) {
  uint32_t ph = s_phase;
  for (size_t i = 0; i < frames; i++) {
    int16_t v = (ph < TONE_PERIOD / 2) ? TONE_AMPLITUDE : -TONE_AMPLITUDE;
    dst[2 * i]     = v; // left
    dst[2 * i + 1] = v; // right
    if (++ph >= TONE_PERIOD) ph = 0;
  }
  s_phase = ph;
}

/* on_sent: the descriptor just drained, refill it in place. This is the model
 * that would replace M16's audio task - no task wakeup, no semaphore wait, the
 * hardware sets the pace and we answer.
 *
 * IRAM_ATTR because an ISR that lives in flash freezes whenever the cache is
 * disabled. Note that the driver's own ISR is only IRAM-safe if the IDF was
 * built with CONFIG_I2S_ISR_IRAM_SAFE, which the Arduino prebuilt libraries
 * most likely do not set - fine here, since nothing in this sketch writes
 * flash, but it is a real constraint for a production ISR feed. */
static bool IRAM_ATTR onSent(i2s_chan_handle_t, i2s_event_data_t* evt, void*) {
  uint32_t t0 = esp_cpu_get_cycle_count();
  REG_WRITE(GPIO_OUT_W1TS_REG, 1u << BENCH_MARKER_GPIO);

  // dma_buf, NOT the deprecated evt->data: the header documents data as "the
  // secondary pointer" and dma_buf as "the first level pointer of DMA buffer
  // that just finished sending". They are different levels of indirection, so
  // using the deprecated one writes to the wrong address.
  fillFrames((int16_t*)evt->dma_buf, evt->size / 4); // 4 bytes per stereo frame

  REG_WRITE(GPIO_OUT_W1TC_REG, 1u << BENCH_MARKER_GPIO);
  statsRecord(esp_cpu_get_cycle_count() - t0);
  return false; // nothing to wake
}

static bool IRAM_ATTR onSendOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*) {
  s_underruns = s_underruns + 1;
  return false;
}

/* Bring up a channel at the requested ring depth. Returns the driver's verdict
 * instead of asserting, because "which depths are rejected" is one of the
 * things being measured. */
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

  // Callbacks must be registered while the channel is READY, before enable.
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

/* Feed from this task, one block per write - the efficient shape. */
static void feedTaskBlock(const BenchCfg& cfg, uint32_t ms) {
  static int16_t block[2 * 1024];
  size_t frames = cfg.frames;
  if (frames > 1024) frames = 1024;
  uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    fillFrames(block, frames);
    size_t wrote = 0;
    uint32_t t0 = esp_cpu_get_cycle_count();
    i2s_channel_write(s_tx, block, frames * 4, &wrote, portMAX_DELAY);
    statsRecord(esp_cpu_get_cycle_count() - t0);
  }
}

/* Feed from this task, one stereo frame per write - exactly what M16 does in
 * _i2s_write_samples_direct_external(). The point of the bench. */
static void feedTaskPerSample(uint32_t ms) {
  int16_t frame[2];
  uint32_t until = millis() + ms;
  while ((int32_t)(millis() - until) < 0) {
    fillFrames(frame, 1);
    size_t wrote = 0;
    uint32_t t0 = esp_cpu_get_cycle_count();
    i2s_channel_write(s_tx, frame, 4, &wrote, portMAX_DELAY);
    statsRecord(esp_cpu_get_cycle_count() - t0);
  }
}

static void runOne(const BenchCfg& cfg, FeedModel model) {
  float ringMs = 1000.0f * cfg.descriptors * cfg.frames / BENCH_SAMPLE_RATE;
  Serial.printf("%u x %-4u  %6.2f ms  %-20s ", (unsigned)cfg.descriptors,
                (unsigned)cfg.frames, ringMs, feedName(model));

  statsReset();
  esp_err_t err = channelOpen(cfg, model);
  if (err != ESP_OK) {
    Serial.printf("REJECTED by driver: %s\n", esp_err_to_name(err));
    return;
  }

  uint32_t cpuHz   = getCpuFrequencyMhz() * 1000000UL;
  uint32_t startMs = millis();
  uint32_t t0      = esp_cpu_get_cycle_count();

  switch (model) {
    case FEED_ISR:             delay(BENCH_SECONDS * 1000); break;
    case FEED_TASK_BLOCK:      feedTaskBlock(cfg, BENCH_SECONDS * 1000); break;
    case FEED_TASK_PER_SAMPLE: feedTaskPerSample(BENCH_SECONDS * 1000); break;
  }

  uint32_t elapsedCycles = esp_cpu_get_cycle_count() - t0;
  uint32_t elapsedMs     = millis() - startMs;
  channelClose();

  uint32_t fills = s_fills;
  uint64_t busy  = s_cycleTotal;
  float cpuPct   = elapsedCycles ? (100.0f * (float)busy / (float)elapsedCycles) : 0.0f;
  float perSec   = elapsedMs ? (1000.0f * fills / elapsedMs) : 0.0f;
  float avgCyc   = fills ? (float)busy / fills : 0.0f;
  float avgUs    = avgCyc * 1000000.0f / cpuHz;

  Serial.printf("cpu %5.1f%%  fills/s %8.0f  cycles min/avg/max %5u/%7.0f/%6u  (%5.2f us)  underruns %u\n",
                cpuPct, perSec, (unsigned)(fills ? s_cycleMin : 0), avgCyc,
                (unsigned)s_cycleMax, avgUs, (unsigned)s_underruns);
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println();
  Serial.println("=== I2S I/O bench (no M16) ===");
  Serial.printf("CPU %u MHz, %d Hz stereo 16-bit, BCK=16 WS=17 DOUT=18\n",
                getCpuFrequencyMhz(), BENCH_SAMPLE_RATE);
  Serial.printf("ISR marker on GPIO%d, %d s per run\n\n", BENCH_MARKER_GPIO, BENCH_SECONDS);

  pinMode(BENCH_MARKER_GPIO, OUTPUT);
  digitalWrite(BENCH_MARKER_GPIO, LOW);

  const FeedModel models[] = {FEED_ISR, FEED_TASK_BLOCK, FEED_TASK_PER_SAMPLE};

  for (unsigned c = 0; c < sizeof(kConfigs) / sizeof(kConfigs[0]); c++) {
    for (unsigned m = 0; m < sizeof(models) / sizeof(models[0]); m++) {
      runOne(kConfigs[c], models[m]);
      delay(200); // let the driver settle between runs
    }
    Serial.println();
  }

  Serial.println("=== done ===");
  Serial.println("Lowest ring with 0 underruns and acceptable cpu% is the floor");
  Serial.println("for that feed model. Compare ISR against task/1-frame: the gap");
  Serial.println("is what leaving M16's audio core would buy.");
}

void loop() {
  delay(1000);
}
