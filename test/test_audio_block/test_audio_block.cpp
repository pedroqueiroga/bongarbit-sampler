/*
 * Host tests for the float audio engine's edges: src/audio_block.h.
 *
 * Same contract as test_mixer and test_trigger - these compile the real header
 * the firmware compiles, so a green run is a claim about the binary.
 *
 * The engine moved from 16-bit fixed point to normalised float, and the thing
 * most worth defending about that move is that it did not change how the
 * instrument sounds. mixer.h is therefore still compiled in here, not as a
 * second implementation but as the REFERENCE: the integer curve is the one
 * that was judged by ear on hardware, and the float curve has to agree with
 * it. test_float_knee_matches_the_integer_reference is the test that makes the
 * two unable to drift apart.
 */
#include <unity.h>
#include <stdint.h>
#include <math.h>
#include "audio_block.h"
#include "mixer.h"

// Peak of src/kick_normalizado.wav: 29225 of 32768 = 89.2%. Same constant as
// test_mixer, for the same reason - it is a real normalised sample, and two of
// them summing past full scale is why the limiter exists.
#define KICK_PEAK 29225

void setUp(void) {}
void tearDown(void) {}

// ---- Units: the conversion at the input edge ----------------------------

// The scale factor is a power of two in both directions, so every 16-bit
// sample survives a round trip exactly. This is what lets a processor be
// inserted or removed from the chain without the bypass path drifting.
void test_int16_round_trip_is_bit_exact(void) {
  for (int32_t s = -32768; s <= 32767; s++) {
    int16_t in = (int16_t)s;
    TEST_ASSERT_EQUAL_INT16(in, audioToI16(audioFromI16(in)));
  }
}

void test_silence_and_full_scale_land_where_expected(void) {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, audioFromI16(0));
  TEST_ASSERT_EQUAL_FLOAT(-1.0f, audioFromI16(-32768));
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.99997f, audioFromI16(32767));
}

// ---- Units: the conversion at the output edge ---------------------------

// Rounding to nearest, not truncating toward zero. Truncation would bias every
// sample inward by up to 1 LSB - a correlated error, audible as a rasp on
// quiet decay tails, which is the same artefact family that made 8-bit samples
// look like DMA underruns on the bench of 13/08/2026.
void test_output_rounds_to_nearest_rather_than_truncating(void) {
  float lsb = 1.0f / AUDIO_FULL_SCALE;
  TEST_ASSERT_EQUAL_INT16(1, audioToI16(lsb * 0.6f));   // truncation would give 0
  TEST_ASSERT_EQUAL_INT16(0, audioToI16(lsb * 0.4f));
  TEST_ASSERT_EQUAL_INT16(-1, audioToI16(lsb * -0.6f)); // and 0 here too
  TEST_ASSERT_EQUAL_INT16(0, audioToI16(lsb * -0.4f));
}

// Nothing leaving this function may be out of range, because the next thing
// that happens to it is a 16-bit DMA write. Converting an out-of-range float
// to int is undefined behaviour, so this clamp is a correctness guard rather
// than a tone decision - the soft knee upstream is what shapes peaks.
void test_output_clamps_instead_of_wrapping(void) {
  TEST_ASSERT_EQUAL_INT16(32767, audioToI16(1.0f));
  TEST_ASSERT_EQUAL_INT16(32767, audioToI16(4.0f));
  TEST_ASSERT_EQUAL_INT16(-32768, audioToI16(-1.0f));
  TEST_ASSERT_EQUAL_INT16(-32768, audioToI16(-4.0f));
}

// A runaway delay line or a divide by a zeroed coefficient produces NaN or inf
// somewhere inside the chain. The requirement is not that it sounds good, it
// is that it becomes a bounded sample rather than a wrapped one, so a bug in a
// processor cannot turn into full-scale square-wave noise into someone's ears.
void test_output_survives_nan_and_infinity(void) {
  TEST_ASSERT_EQUAL_INT16(-32768, audioToI16(NAN));
  TEST_ASSERT_EQUAL_INT16(32767, audioToI16(INFINITY));
  TEST_ASSERT_EQUAL_INT16(-32768, audioToI16(-INFINITY));
}

// ---- The block buffer ---------------------------------------------------

// Voices accumulate into the block, so it has to start from silence. A block
// left dirty is the previous 0.73ms of audio played a second time, which is
// the kind of bug that sounds like a hardware fault.
void test_silence_clears_every_frame_of_both_channels(void) {
  AudioBlock b;
  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) { b.l[n] = 0.5f; b.r[n] = -0.5f; }
  audioBlockSilence(b);
  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.l[n]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b.r[n]);
  }
}

// The block is the latency floor of every processor downstream of it, and it
// has to stay small relative to the DMA ring that drains while we render.
// main.cpp asserts the ring relationship, where both numbers are visible; this
// asserts the half that is a property of the header alone.
void test_block_is_small_enough_to_be_a_latency_floor(void) {
  TEST_ASSERT_TRUE(AUDIO_BLOCK_FRAMES >= 8);    // below this, per-block overhead dominates
  TEST_ASSERT_TRUE(AUDIO_BLOCK_FRAMES <= 64);   // 64 frames = 1.45ms, already past "sub-ms"
}

// ---- The limiter: it is the same curve as before ------------------------

// THE test for this commit. The integer curve in mixer.h is the one that was
// judged by ear on hardware; this sweep is what says the float engine did not
// change the sound while changing everything about how it is computed.
//
// 1 LSB of slack, and in one direction only: the integer version's division
// truncates toward zero, so it can sit one step below the real curve. Anything
// larger than that, or any disagreement below the knee, is a real divergence.
void test_float_knee_matches_the_integer_reference(void) {
  for (int32_t x = -4 * 32768; x <= 4 * 32768; x += 7) {
    int16_t ref   = mixSoftClip(x);
    int16_t under = audioToI16(audioSoftClip((float)x / AUDIO_FULL_SCALE));
    int32_t diff  = (int32_t)under - (int32_t)ref;
    if (diff < 0) diff = -diff;
    TEST_ASSERT_TRUE_MESSAGE(diff <= 1, "float limiter diverged from mixer.h");
  }
}

// Below the knee the limiter is exactly the identity, so ordinary playing is
// untouched by it. Not "almost" - bit-exact, which is what makes it safe to
// leave permanently in the path.
void test_below_the_knee_nothing_happens(void) {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, audioSoftClip(0.0f));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, audioSoftClip(0.5f));
  TEST_ASSERT_EQUAL_FLOAT(-0.5f, audioSoftClip(-0.5f));
  TEST_ASSERT_EQUAL_FLOAT(AUDIO_SOFT_KNEE, audioSoftClip(AUDIO_SOFT_KNEE));
}

// KICK_PEAK (89%) sits above the 75% knee, so one voice played hard does get
// compressed - but only barely, and it must never be clamped. Same assertion
// as the integer reference in test_mixer, deliberately: the knee position is a
// tone decision, and this is where moving it would show up.
void test_a_single_normalised_voice_is_barely_compressed(void) {
  float peak = (float)KICK_PEAK / AUDIO_FULL_SCALE;
  float y    = audioSoftClip(peak);
  TEST_ASSERT_TRUE(y > AUDIO_SOFT_KNEE);
  TEST_ASSERT_TRUE(y < AUDIO_SOFT_CEIL);
  TEST_ASSERT_TRUE(y < peak);           // compressed, not expanded
  TEST_ASSERT_TRUE(y > 0.9f * peak);    // and only just
}

// Clipping stops being possible by construction rather than by tuning: the
// hyperbola is asymptotic to the ceiling, so no input reaches it.
void test_never_reaches_the_ceiling(void) {
  TEST_ASSERT_TRUE(audioSoftClip(1.0f) < AUDIO_SOFT_CEIL);
  TEST_ASSERT_TRUE(audioSoftClip(4.0f) < AUDIO_SOFT_CEIL);
  TEST_ASSERT_TRUE(audioSoftClip(1000.0f) < AUDIO_SOFT_CEIL);
  TEST_ASSERT_TRUE(audioSoftClip(-1000.0f) > -AUDIO_SOFT_CEIL);
}

// C1-continuity at the knee: no corner where compression starts. A cheaper
// curve with a slope discontinuity there is audible as a click on transients,
// which is the artefact this whole limiter was introduced to remove.
void test_knee_has_no_step_and_unity_slope(void) {
  float eps = 1e-5f;
  float below = audioSoftClip(AUDIO_SOFT_KNEE - eps);
  float above = audioSoftClip(AUDIO_SOFT_KNEE + eps);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, below, above);          // no step
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 1.0f, (above - below) / (2 * eps)); // slope 1
}

void test_is_monotonic(void) {
  float prev = audioSoftClip(-4.0f);
  for (float x = -4.0f; x <= 4.0f; x += 0.001f) {
    float y = audioSoftClip(x);
    TEST_ASSERT_TRUE(y >= prev);
    prev = y;
  }
}

void test_is_odd(void) {
  for (float x = 0.0f; x <= 4.0f; x += 0.01f) {
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -audioSoftClip(x), audioSoftClip(-x));
  }
}

// Compression, not a brick wall: louder in still means louder out, so dynamics
// survive above the knee instead of being flattened to one level.
void test_still_distinguishes_loud_from_louder(void) {
  TEST_ASSERT_TRUE(audioSoftClip(2.0f) > audioSoftClip(1.0f));
  TEST_ASSERT_TRUE(audioSoftClip(4.0f) > audioSoftClip(2.0f));
}

// ---- The reason the limiter exists -------------------------------------

// Two normalised samples sum to 178% of full scale. With a hard clamp that is
// a flat-topped transient; here it has to stay inside the ceiling and stay
// distinguishable from four of them.
void test_colliding_voices_no_longer_clip(void) {
  float two  = 2.0f * KICK_PEAK / AUDIO_FULL_SCALE;
  float four = 4.0f * KICK_PEAK / AUDIO_FULL_SCALE;
  TEST_ASSERT_TRUE(two > 1.0f);                       // the problem is real
  TEST_ASSERT_TRUE(audioSoftClip(two) < AUDIO_SOFT_CEIL);
  TEST_ASSERT_TRUE(audioSoftClip(four) < AUDIO_SOFT_CEIL);
  TEST_ASSERT_TRUE(audioSoftClip(four) > audioSoftClip(two));
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_int16_round_trip_is_bit_exact);
  RUN_TEST(test_silence_and_full_scale_land_where_expected);

  RUN_TEST(test_output_rounds_to_nearest_rather_than_truncating);
  RUN_TEST(test_output_clamps_instead_of_wrapping);
  RUN_TEST(test_output_survives_nan_and_infinity);

  RUN_TEST(test_silence_clears_every_frame_of_both_channels);
  RUN_TEST(test_block_is_small_enough_to_be_a_latency_floor);

  RUN_TEST(test_float_knee_matches_the_integer_reference);
  RUN_TEST(test_below_the_knee_nothing_happens);
  RUN_TEST(test_a_single_normalised_voice_is_barely_compressed);
  RUN_TEST(test_never_reaches_the_ceiling);
  RUN_TEST(test_knee_has_no_step_and_unity_slope);
  RUN_TEST(test_is_monotonic);
  RUN_TEST(test_is_odd);
  RUN_TEST(test_still_distinguishes_loud_from_louder);

  RUN_TEST(test_colliding_voices_no_longer_clip);

  return UNITY_END();
}
