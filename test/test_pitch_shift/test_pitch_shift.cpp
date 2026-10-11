/*
 * Host tests for the pitch shifter: src/pitch_shift.h.
 *
 * Same contract as the other suites - the real header the firmware compiles.
 *
 * A pitch shifter is unusual among the things in this repo in that its central
 * claim is about FREQUENCY, which no amount of staring at the code settles.
 * So these tests measure it: feed a sine, take a DFT bin at the frequency the
 * shift should have produced and one at the frequency it came in at, and
 * assert the energy moved. That is the only honest way to tell a working
 * shifter from one that is merely making a plausible noise.
 *
 * Organised by the property defended, same as the others. Two of these are
 * load-bearing beyond the DSP:
 *
 *  - test_block_size_does_not_change_the_output is what makes "written to
 *    accept audio-in later without a rewrite" a fact rather than a hope. If
 *    the host block size leaks into the result, a different-sized input
 *    callback changes the sound.
 *  - test_bypass_is_bit_exact is what makes it safe to leave the processor
 *    permanently in the audio path. Anything less than bit-exact means the
 *    instrument sounds different with the effect merely installed.
 */
#include <unity.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include "pitch_shift.h"

#define SR 44100.0f
// 441 Hz = exactly 100 frames per cycle at 44.1kHz, so the probe signal has no
// truncation artefact of its own to be confused with the shifter's.
#define PROBE_HZ 441.0f

static const float kPi = 3.14159265358979323846f;

void setUp(void) {}
void tearDown(void) {}

// ---- helpers ------------------------------------------------------------

static void fillSine(float* buf, int n, float hz, float amp) {
  for (int i = 0; i < n; i++) {
    buf[i] = amp * sinf(2.0f * kPi * hz * (float)i / SR);
  }
}

// Magnitude of one DFT bin, normalised by length. Naive on purpose: this runs
// on a desktop, and a readable 8-line DFT is worth more here than an FFT.
static float magAt(const float* buf, int n, float hz) {
  float re = 0.0f, im = 0.0f;
  for (int i = 0; i < n; i++) {
    float a = 2.0f * kPi * hz * (float)i / SR;
    re += buf[i] * cosf(a);
    im -= buf[i] * sinf(a);
  }
  return sqrtf(re * re + im * im) / (float)n;
}

static float rms(const float* buf, int n) {
  float s = 0.0f;
  for (int i = 0; i < n; i++) s += buf[i] * buf[i];
  return sqrtf(s / (float)n);
}

// Run a signal through a shifter in AUDIO_BLOCK_FRAMES chunks, same as the
// firmware does. Both channels get the same input.
static void runBlocks(PitchShift& ps, float* l, float* r, int frames) {
  for (int off = 0; off + AUDIO_BLOCK_FRAMES <= frames; off += AUDIO_BLOCK_FRAMES) {
    ps.process(l + off, r + off, AUDIO_BLOCK_FRAMES);
  }
}

// Long enough to hold the pipeline filling (one buffer) plus the ramp plus
// several crossfade cycles to average over.
#define N 16384
static float gL[N], gR[N], gRef[N];

// ---- Bypass: the effect must be free when it is off --------------------

// Not "almost unchanged" - identical bits. The processor sits permanently in
// audioUpdate()'s chain, so anything it does while off is something the
// instrument does all the time for no reason.
void test_bypass_is_bit_exact(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(2.0f);        // armed, deliberately, but never engaged

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  memcpy(gRef, gL, sizeof(float) * N);

  TEST_ASSERT_TRUE(ps.isBypassing());
  runBlocks(ps, gL, gR, N);

  for (int i = 0; i < N; i++) {
    TEST_ASSERT_EQUAL_MEMORY(&gRef[i], &gL[i], sizeof(float));
    TEST_ASSERT_EQUAL_MEMORY(&gRef[i], &gR[i], sizeof(float));
  }
}

// Releasing must return all the way to bit-exact, not to "quiet enough". A
// residual wet path would mean the effect never really turns off, which is
// exactly what "only while the button is held" promises it does.
void test_release_returns_to_exact_bypass(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(2.0f);
  ps.setEngaged(true);

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, 4096);

  ps.setEngaged(false);
  // Give it the ramp plus a block of slack to finish coming down.
  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, PITCH_RAMP_FRAMES + 2 * AUDIO_BLOCK_FRAMES);

  TEST_ASSERT_TRUE(ps.isBypassing());

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  memcpy(gRef, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, N);
  for (int i = 0; i < N; i++) {
    TEST_ASSERT_EQUAL_MEMORY(&gRef[i], &gL[i], sizeof(float));
  }
}

// ---- The central claim: the pitch actually moves -----------------------

void test_shifting_up_an_octave_doubles_the_frequency(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(2.0f);
  ps.setEngaged(true);

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, N);

  // Measure on the second half only: the first holds the buffer filling and
  // the dry/wet ramp, which are startup and not the steady-state shift.
  const float* tail = gL + N / 2;
  int tailN = N / 2;
  float atOctave = magAt(tail, tailN, PROBE_HZ * 2.0f);
  float atInput  = magAt(tail, tailN, PROBE_HZ);

  TEST_ASSERT_TRUE_MESSAGE(atOctave > 3.0f * atInput,
                           "energy did not move to the octave above");
}

void test_shifting_down_an_octave_halves_the_frequency(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(0.5f);
  ps.setEngaged(true);

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, N);

  const float* tail = gL + N / 2;
  int tailN = N / 2;
  float atHalf  = magAt(tail, tailN, PROBE_HZ * 0.5f);
  float atInput = magAt(tail, tailN, PROBE_HZ);

  TEST_ASSERT_TRUE_MESSAGE(atHalf > 3.0f * atInput,
                           "energy did not move to the octave below");
}

// Semitones are the musical unit, and the conversion is the only place a
// factor of 12 can go wrong. A fifth up is ratio 1.4983.
void test_semitones_convert_to_the_expected_ratios(void) {
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, pitchRatioFromSemitones(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, pitchRatioFromSemitones(12.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, pitchRatioFromSemitones(-12.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.49831f, pitchRatioFromSemitones(7.0f));
}

// ---- The other half of the claim: duration is unchanged ----------------

// This is what separates pitch shifting from just playing the buffer faster.
// Resampling a 371ms input up an octave would produce 186ms of audio and then
// silence; here every frame of output has to carry signal.
//
// Checked as "no silent stretch anywhere past startup", because that is the
// failure mode: a shifter whose read pointer outruns its writer produces gaps,
// not a shorter file.
void test_duration_is_unchanged_with_no_gaps(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(2.0f);
  ps.setEngaged(true);

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, N);

  // Window of ~5ms. Smaller than a crossfade cycle, so a genuine gap shows up
  // while the ordinary warble does not.
  const int win = 220;
  for (int off = N / 2; off + win <= N; off += win) {
    TEST_ASSERT_TRUE_MESSAGE(rms(gL + off, win) > 0.05f,
                             "silent stretch in the output - the shifter left a gap");
  }
}

// ---- The audio-in promise ----------------------------------------------

// THE architectural test. The DSP must be a function of the sample stream, not
// of how it was chopped up on the way in. If this fails, the host block size
// has leaked into the sound and a different-sized audio-in callback would not
// be a drop-in - which is the whole premise this processor was written on.
void test_block_size_does_not_change_the_output(void) {
  static float aL[N], aR[N], bL[N], bR[N];

  fillSine(aL, N, PROBE_HZ, 0.8f);
  memcpy(aR, aL, sizeof(float) * N);
  memcpy(bL, aL, sizeof(float) * N);
  memcpy(bR, aL, sizeof(float) * N);

  PitchShift one;
  one.init();
  one.setRatio(1.7f);          // deliberately not a round ratio
  one.setEngaged(true);
  one.process(aL, aR, N);      // the whole signal in a single call

  PitchShift many;
  many.init();
  many.setRatio(1.7f);
  many.setEngaged(true);
  // Ragged on purpose: a run of different sizes, not a clean divisor, because
  // "works for 32 and 64" is a weaker claim than "the size is irrelevant".
  const int sizes[] = {1, 7, 32, 3, 64, 13, 128, 5};
  int off = 0, k = 0;
  while (off < N) {
    int chunk = sizes[k++ % 8];
    if (off + chunk > N) chunk = N - off;
    many.process(bL + off, bR + off, chunk);
    off += chunk;
  }

  for (int i = 0; i < N; i++) {
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&aL[i], &bL[i], sizeof(float),
        "output depends on the host block size");
  }
}

// History is written even while bypassed, so pressing the button reads audio
// that really happened rather than filling from silence first. Tested
// differentially: a shifter that was fed during bypass must have more energy
// immediately after engaging than an identical one that was not.
void test_history_is_fed_while_bypassed(void) {
  static float warmL[N], warmR[N], coldL[N], coldR[N];
  const int lead = PITCH_BUF_FRAMES;
  const int look = PITCH_BUF_FRAMES / 2;

  PitchShift warm;
  warm.init();
  warm.setRatio(0.5f);
  fillSine(warmL, N, PROBE_HZ, 0.8f);
  memcpy(warmR, warmL, sizeof(float) * N);
  runBlocks(warm, warmL, warmR, lead);        // bypassed, but listening

  PitchShift cold;
  cold.init();
  cold.setRatio(0.5f);

  // Both now see the same fresh signal, both engaged, from a standing start.
  fillSine(warmL, N, PROBE_HZ, 0.8f);
  memcpy(warmR, warmL, sizeof(float) * N);
  memcpy(coldL, warmL, sizeof(float) * N);
  memcpy(coldR, warmL, sizeof(float) * N);
  warm.setEngaged(true);
  cold.setEngaged(true);
  runBlocks(warm, warmL, warmR, look);
  runBlocks(cold, coldL, coldR, look);

  // Compare the shifted-down energy, which is the part that can only come from
  // history. The cold one has to read zeroes for most of this window.
  float warmMag = magAt(warmL, look, PROBE_HZ * 0.5f);
  float coldMag = magAt(coldL, look, PROBE_HZ * 0.5f);
  TEST_ASSERT_TRUE_MESSAGE(warmMag > 1.5f * coldMag,
      "bypass is not feeding the history - the effect starts from silence");
}

// ---- Behaviour at the edges -------------------------------------------

// Both channels get one phase and one pair of lags. Independent phases would
// let the channels drift apart and smear the stereo image, which sounds like
// the instrument wandering rather than like an effect.
void test_stereo_stays_locked(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(1.3f);
  ps.setEngaged(true);

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, N);

  for (int i = 0; i < N; i++) {
    TEST_ASSERT_EQUAL_MEMORY(&gL[i], &gR[i], sizeof(float));
  }
}

// The phase advance is |1 - ratio| / span per frame and has to stay well under
// 1, or one frame jumps a whole crossfade cycle and the output is noise. The
// clamp is what guarantees that, so it is the clamp that gets asserted.
void test_ratio_is_clamped_to_two_octaves(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(100.0f);
  TEST_ASSERT_EQUAL_FLOAT(PITCH_RATIO_MAX, ps.getRatio());
  ps.setRatio(0.001f);
  TEST_ASSERT_EQUAL_FLOAT(PITCH_RATIO_MIN, ps.getRatio());
  ps.setRatio(1.5f);
  TEST_ASSERT_EQUAL_FLOAT(1.5f, ps.getRatio());
}

// Nothing leaving here may be unbounded: the next stop is the limiter and then
// a 16-bit DMA write, and a NaN loose in a delay line poisons its own history
// permanently. Full-scale input at both rails of the ratio range.
void test_output_stays_bounded_and_finite(void) {
  const float ratios[] = {PITCH_RATIO_MIN, 0.5f, 1.0f, 2.0f, PITCH_RATIO_MAX};
  for (int k = 0; k < 5; k++) {
    PitchShift ps;
    ps.init();
    ps.setRatio(ratios[k]);
    ps.setEngaged(true);
    fillSine(gL, N, PROBE_HZ, 1.0f);
    memcpy(gR, gL, sizeof(float) * N);
    runBlocks(ps, gL, gR, N);
    for (int i = 0; i < N; i++) {
      TEST_ASSERT_TRUE_MESSAGE(isfinite(gL[i]), "non-finite sample in the output");
      // Two equal-power taps can sum above the input peak on correlated
      // content - up to 3dB (sqrt 2). Allow that and no more; the limiter
      // downstream is sized on the assumption that this stays near unity.
      TEST_ASSERT_TRUE_MESSAGE(fabsf(gL[i]) < 1.5f, "output grew past the taps' 3dB bound");
    }
  }
}

// Engaging must not step. A discontinuity here is a click, and a click on
// every button press is worse than not having the effect.
void test_engaging_ramps_instead_of_stepping(void) {
  PitchShift ps;
  ps.init();
  ps.setRatio(0.5f);

  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gR, gL, sizeof(float) * N);
  runBlocks(ps, gL, gR, PITCH_BUF_FRAMES);   // bypassed, filling history

  // Largest frame-to-frame step the input itself takes, as the reference: the
  // effect may not introduce a jump bigger than the signal already has.
  fillSine(gL, N, PROBE_HZ, 0.8f);
  memcpy(gRef, gL, sizeof(float) * N);
  float inputSlew = 0.0f;
  for (int i = 1; i < 2048; i++) {
    float d = fabsf(gRef[i] - gRef[i - 1]);
    if (d > inputSlew) inputSlew = d;
  }

  memcpy(gR, gL, sizeof(float) * N);
  ps.setEngaged(true);
  runBlocks(ps, gL, gR, 2048);

  for (int i = 1; i < 2048; i++) {
    float d = fabsf(gL[i] - gL[i - 1]);
    TEST_ASSERT_TRUE_MESSAGE(d < 3.0f * inputSlew,
        "step at engage - the dry/wet ramp is not covering the transition");
  }
}

// The latency is a number that goes into the project's latency budget and into
// docs/bancada.md, so it is reported by the processor rather than inferred.
// ~11.6ms at 44.1kHz with a 1024-frame buffer.
void test_latency_is_reported_and_is_half_the_buffer(void) {
  float frames = PitchShift::latencyFrames();
  TEST_ASSERT_FLOAT_WITHIN(4.0f, (float)PITCH_BUF_FRAMES / 2.0f, frames);
  float ms = 1000.0f * frames / SR;
  TEST_ASSERT_TRUE(ms > 5.0f);
  TEST_ASSERT_TRUE(ms < 25.0f);
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_bypass_is_bit_exact);
  RUN_TEST(test_release_returns_to_exact_bypass);

  RUN_TEST(test_shifting_up_an_octave_doubles_the_frequency);
  RUN_TEST(test_shifting_down_an_octave_halves_the_frequency);
  RUN_TEST(test_semitones_convert_to_the_expected_ratios);

  RUN_TEST(test_duration_is_unchanged_with_no_gaps);

  RUN_TEST(test_block_size_does_not_change_the_output);
  RUN_TEST(test_history_is_fed_while_bypassed);

  RUN_TEST(test_stereo_stays_locked);
  RUN_TEST(test_ratio_is_clamped_to_two_octaves);
  RUN_TEST(test_output_stays_bounded_and_finite);
  RUN_TEST(test_engaging_ramps_instead_of_stepping);
  RUN_TEST(test_latency_is_reported_and_is_half_the_buffer);

  return UNITY_END();
}
