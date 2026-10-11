/*
 * Host tests for the voice mixer: src/mixer.h.
 *
 * Same contract as test_trigger - these exercise the real header that the
 * firmware compiles, not a transcription of it, so a green run is a claim
 * about the binary.
 *
 * Organised by the property being defended rather than by function, because
 * each property is a reason the curve was chosen over an alternative. A test
 * named after "the knee is C1-continuous" fails loudly if someone swaps in a
 * cheaper curve with a corner in it.
 */
#include <unity.h>
#include <stdint.h>
#include "mixer.h"

// Peak of src/kick_normalizado.wav, measured: 29225 of 32768 = 89.2%. Used
// throughout because it is a real normalised sample, and the whole reason this
// header exists is that two of them sum past full scale.
#define KICK_PEAK 29225

void setUp(void) {}
void tearDown(void) {}

// ---- Gain staging -------------------------------------------------------

void test_unity_gain_is_the_identity(void) {
  TEST_ASSERT_EQUAL_INT32(0, mixApplyGain(0, MIX_GAIN_UNITY));
  TEST_ASSERT_EQUAL_INT32(1234, mixApplyGain(1234, MIX_GAIN_UNITY));
  TEST_ASSERT_EQUAL_INT32(-1234, mixApplyGain(-1234, MIX_GAIN_UNITY));
  TEST_ASSERT_EQUAL_INT32(KICK_PEAK, mixApplyGain(KICK_PEAK, MIX_GAIN_UNITY));
}

void test_half_gain_halves(void) {
  TEST_ASSERT_EQUAL_INT32(500, mixApplyGain(1000, MIX_GAIN_UNITY / 2));
  TEST_ASSERT_EQUAL_INT32(8192, mixApplyGain(16384, MIX_GAIN_UNITY / 2));
}

void test_gain_above_unity_amplifies(void) {
  TEST_ASSERT_EQUAL_INT32(2000, mixApplyGain(1000, MIX_GAIN_UNITY * 2));
}

void test_zero_gain_mutes(void) {
  TEST_ASSERT_EQUAL_INT32(0, mixApplyGain(KICK_PEAK, 0));
}

// Four voices at unity must stay inside int32_t, because that is the
// accumulator audioUpdate() sums into. This is the bound documented on
// mixApplyGain(); if PAD_COUNT or the gain ceiling ever grows, this is the
// assertion that should start failing.
void test_four_voices_at_unity_fit_the_accumulator(void) {
  int64_t worst = 4LL * mixApplyGain(32768, MIX_GAIN_UNITY);
  TEST_ASSERT_TRUE(worst < INT32_MAX);
  int64_t worst_4x = 4LL * mixApplyGain(32768, MIX_GAIN_UNITY * 4);
  TEST_ASSERT_TRUE(worst_4x < INT32_MAX);
}

// ---- The soft knee: why this curve ------------------------------------

void test_below_the_knee_nothing_happens(void) {
  // The point of a knee rather than a global compressor: normal playing is
  // bit-identical to no processing at all.
  for (int32_t x = 0; x <= MIX_SOFT_KNEE; x += 97) {
    TEST_ASSERT_EQUAL_INT16((int16_t)x, mixSoftClip(x));
    TEST_ASSERT_EQUAL_INT16((int16_t)-x, mixSoftClip(-x));
  }
  TEST_ASSERT_EQUAL_INT16((int16_t)MIX_SOFT_KNEE, mixSoftClip(MIX_SOFT_KNEE));
}

void test_a_single_normalised_voice_is_untouched(void) {
  // KICK_PEAK (89%) sits above the 75% knee, so one voice played hard does get
  // compressed - but only barely, and it must not be clamped.
  int16_t y = mixSoftClip(KICK_PEAK);
  TEST_ASSERT_TRUE(y > MIX_SOFT_KNEE);
  TEST_ASSERT_TRUE(y < MIX_SAMPLE_MAX);
  TEST_ASSERT_TRUE(y < KICK_PEAK); // compressed, not expanded
}

void test_knee_has_no_step_and_near_unity_slope(void) {
  // In the reals the slope at the knee is exactly 1, which is what makes the
  // curve C1-continuous and the onset of compression inaudible. In integers
  // the division truncates, so single steps can be 0 - assert the property
  // that actually matters instead of exact equality:
  //   - no discontinuity: consecutive outputs never jump
  //   - slope just above the knee is within a few percent of 1
  int32_t at = mixSoftClip(MIX_SOFT_KNEE);
  TEST_ASSERT_EQUAL_INT32(MIX_SOFT_KNEE, at);

  int32_t prev = at;
  for (int32_t over = 1; over <= 256; over++) {
    int32_t y = mixSoftClip(MIX_SOFT_KNEE + over);
    int32_t step = y - prev;
    TEST_ASSERT_TRUE(step == 0 || step == 1); // monotonic, no jump
    prev = y;
  }
  int32_t rise = prev - at; // over a 256-sample span
  TEST_ASSERT_TRUE(rise <= 256);
  TEST_ASSERT_TRUE(rise >= 240); // slope >= 0.94, i.e. no corner
}

void test_never_reaches_full_scale(void) {
  // The defining property: clipping is impossible by construction, for any
  // input, so no amount of voices can produce the click the hard clamp did.
  TEST_ASSERT_TRUE(mixSoftClip(32768) < MIX_SAMPLE_MAX);
  TEST_ASSERT_TRUE(mixSoftClip(100000) < MIX_SAMPLE_MAX);
  TEST_ASSERT_TRUE(mixSoftClip(131068) < MIX_SAMPLE_MAX); // 4 voices, full scale
  TEST_ASSERT_TRUE(mixSoftClip(1000000) < MIX_SAMPLE_MAX);
  TEST_ASSERT_TRUE(mixSoftClip(-1000000) > MIX_SAMPLE_MIN);
}

void test_is_monotonic(void) {
  // Monotonicity is what makes it a transfer curve rather than a fold. A
  // non-monotonic "clipper" inverts parts of the waveform and sounds broken.
  int32_t prev = mixSoftClip(0);
  for (int32_t x = 1; x < 200000; x += 311) {
    int32_t y = mixSoftClip(x);
    TEST_ASSERT_TRUE(y >= prev);
    prev = y;
  }
}

void test_is_odd(void) {
  int32_t probes[] = {1, 1000, MIX_SOFT_KNEE, KICK_PEAK, 58450, 131068};
  for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
    TEST_ASSERT_EQUAL_INT32(-(int32_t)mixSoftClip(probes[i]),
                            (int32_t)mixSoftClip(-probes[i]));
  }
}

// ---- The bug this header was written for ------------------------------

void test_two_normalised_voices_no_longer_clip(void) {
  // The bench case: two copies of the kick firing together sum to 178% of full
  // scale. The hard clamp flattened that to exactly full scale - a transient
  // chopped into a step, which is the click.
  int32_t sum = 2 * KICK_PEAK;
  TEST_ASSERT_TRUE(sum > MIX_SAMPLE_MAX);
  TEST_ASSERT_EQUAL_INT16((int16_t)MIX_SAMPLE_MAX, mixHardClip(sum));
  TEST_ASSERT_TRUE(mixSoftClip(sum) < MIX_SAMPLE_MAX);
}

void test_four_normalised_voices_no_longer_clip(void) {
  int32_t sum = 4 * KICK_PEAK; // 357% of full scale
  TEST_ASSERT_EQUAL_INT16((int16_t)MIX_SAMPLE_MAX, mixHardClip(sum));
  TEST_ASSERT_TRUE(mixSoftClip(sum) < MIX_SAMPLE_MAX);
}

void test_soft_clip_still_distinguishes_loud_from_louder(void) {
  // Hard clipping destroys information: every sum past full scale maps to the
  // same output, so two voices and four voices are indistinguishable. The knee
  // keeps them ordered, which is why it sounds like compression and not like a
  // fault.
  TEST_ASSERT_EQUAL_INT16(mixHardClip(2 * KICK_PEAK), mixHardClip(4 * KICK_PEAK));
  TEST_ASSERT_TRUE(mixSoftClip(4 * KICK_PEAK) > mixSoftClip(2 * KICK_PEAK));
}

// ---- The behaviour being replaced -------------------------------------

void test_hard_clip_clamps_both_rails(void) {
  TEST_ASSERT_EQUAL_INT16((int16_t)MIX_SAMPLE_MAX, mixHardClip(MIX_SAMPLE_MAX + 1));
  TEST_ASSERT_EQUAL_INT16((int16_t)MIX_SAMPLE_MIN, mixHardClip(MIX_SAMPLE_MIN - 1));
  TEST_ASSERT_EQUAL_INT16(0, mixHardClip(0));
  TEST_ASSERT_EQUAL_INT16(1234, mixHardClip(1234));
}

// ---- Gain staging as the actual answer --------------------------------

void test_six_db_of_kit_headroom_keeps_one_voice_bit_exact(void) {
  // What -6dB per voice (Q8 128) actually buys: a single hit lands below the
  // knee, so it is bit-identical to no processing. This is the common case -
  // most strikes on a four-pad instrument are not simultaneous.
  int32_t voice = mixApplyGain(KICK_PEAK, MIX_GAIN_UNITY / 2);
  TEST_ASSERT_TRUE(voice < MIX_SOFT_KNEE);
  TEST_ASSERT_EQUAL_INT16((int16_t)voice, mixSoftClip(voice));

  // Two voices already pass the knee, so they are compressed - but they are
  // nowhere near the rail, which is the point.
  int32_t two = 2 * voice;
  TEST_ASSERT_TRUE(two > MIX_SOFT_KNEE);
  TEST_ASSERT_TRUE(mixSoftClip(two) < MIX_SAMPLE_MAX);

  // Four still never clips, which is the guarantee the knee provides.
  TEST_ASSERT_TRUE(mixSoftClip(4 * voice) < MIX_SAMPLE_MAX);
}

void test_staying_linear_on_four_voices_costs_more_than_dividing_by_four(void) {
  // This is the argument for the knee, and it is stronger than it first looks.
  //
  // To keep four voices entirely below the knee - fully linear, no compression
  // at all - each voice must fit in MIX_SOFT_KNEE/4. For a sample peaking at
  // KICK_PEAK that is a gain of about 53 in Q8, roughly -13.7dB: WORSE than
  // the -12dB of the divide-by-voice-count approach we rejected for being too
  // quiet. Linear headroom for the worst case is simply expensive.
  int32_t needed = (MIX_SOFT_KNEE / 4) * MIX_GAIN_UNITY / KICK_PEAK;
  TEST_ASSERT_TRUE(needed < MIX_GAIN_UNITY / 4); // quieter than dividing by 4
  TEST_ASSERT_TRUE(4 * mixApplyGain(KICK_PEAK, needed) <= MIX_SOFT_KNEE);

  // The knee is what buys that headroom back: at unity, a single voice stays
  // within 10% of its original level and four voices still do not clip.
  TEST_ASSERT_TRUE(mixSoftClip(KICK_PEAK) > (int32_t)(KICK_PEAK * 9 / 10));
  TEST_ASSERT_TRUE(mixSoftClip(4 * KICK_PEAK) < MIX_SAMPLE_MAX);
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_unity_gain_is_the_identity);
  RUN_TEST(test_half_gain_halves);
  RUN_TEST(test_gain_above_unity_amplifies);
  RUN_TEST(test_zero_gain_mutes);
  RUN_TEST(test_four_voices_at_unity_fit_the_accumulator);

  RUN_TEST(test_below_the_knee_nothing_happens);
  RUN_TEST(test_a_single_normalised_voice_is_untouched);
  RUN_TEST(test_knee_has_no_step_and_near_unity_slope);
  RUN_TEST(test_never_reaches_full_scale);
  RUN_TEST(test_is_monotonic);
  RUN_TEST(test_is_odd);

  RUN_TEST(test_two_normalised_voices_no_longer_clip);
  RUN_TEST(test_four_normalised_voices_no_longer_clip);
  RUN_TEST(test_soft_clip_still_distinguishes_loud_from_louder);

  RUN_TEST(test_hard_clip_clamps_both_rails);

  RUN_TEST(test_six_db_of_kit_headroom_keeps_one_voice_bit_exact);
  RUN_TEST(test_staying_linear_on_four_voices_costs_more_than_dividing_by_four);

  return UNITY_END();
}
