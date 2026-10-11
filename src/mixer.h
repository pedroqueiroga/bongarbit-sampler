#pragma once
/*
 * Gain staging and output limiting for the voice mixer.
 *
 * Compiled into the firmware AND into the host test suite, for the same reason
 * trigger_input.h is: this is pure arithmetic with an exactly specified
 * contract, so it can be verified on a desktop rather than by ear on a board.
 * Nothing here touches Arduino, M16 or the I2S driver.
 *
 * WHY THIS EXISTS
 *
 * audioUpdate() sums every voice and used to clamp once at the end. With a
 * normalised kit - which is what sample packs ship - that clips by
 * construction: two samples peaking at 89% of full scale sum to 178%. Hard
 * clamping a percussive transient sounds like a click, and on the bench of
 * 13/08/2026 those clicks were mistaken for DMA underruns for some time.
 *
 * The fix is modelled on how an MPC handles it, which is not "divide by the
 * voice count": it gives every pad its own level, a master level, and leaves
 * the decision with whoever builds the kit. Dividing by PAD_COUNT would cost
 * 12dB on a single voice to protect a worst case that rarely happens, and
 * ducking voices dynamically (1/N as voices start) pumps - a new pad would
 * drop the level of one already sounding, which is worse than distortion on
 * percussion.
 *
 * So: per-voice gain, master gain, and a soft knee as the last line of defence
 * for peak collisions that get through anyway.
 *
 * FIXED POINT
 *
 * Gains are Q8: MIX_GAIN_UNITY (256) is 1.0, 128 is -6dB, 512 is +6dB. One
 * multiply and one shift per voice, no floats in the audio path.
 */
#include <stdint.h>

// Q8 gain, 256 = unity. Values above unity are allowed (and useful for quiet
// samples) but see the overflow note on mixApplyGain().
#define MIX_GAIN_UNITY 256

// Full-scale bounds of the output word. Deliberately not taken from M16's
// MAX_16/MIN_16, so this header stays independent of the library.
#define MIX_SAMPLE_MAX 32767
#define MIX_SAMPLE_MIN (-32768)

// Where the soft knee starts, as an absolute sample value. Below it the
// transfer function is exactly the identity, so normal playing is untouched;
// above it the signal is compressed towards full scale without ever reaching
// it. 75% of full scale leaves the knee clear of a single voice played hard
// while still catching two-voice collisions.
#ifndef MIX_SOFT_KNEE
  #define MIX_SOFT_KNEE 24576
#endif

/*
 * Apply a Q8 gain to one sample.
 *
 * Overflow: the result is bounded by |sample| * gain / 256. Callers accumulate
 * several of these in an int32_t, so the usable envelope is
 *   voices * 32768 * (max gain / 256) < 2^31
 * which allows 4 voices at gains up to about 4x unity. Past that the
 * accumulator, not this function, is what overflows.
 */
static inline int32_t mixApplyGain(int32_t sample, int32_t gainQ8) {
  return (sample * gainQ8) >> 8;
}

/* Clamp to the output word - the behaviour this header replaces, kept so the
 * soft knee can be switched off and compared against it. */
static inline int16_t mixHardClip(int32_t x) {
  if (x > MIX_SAMPLE_MAX) return (int16_t)MIX_SAMPLE_MAX;
  if (x < MIX_SAMPLE_MIN) return (int16_t)MIX_SAMPLE_MIN;
  return (int16_t)x;
}

/*
 * Soft knee: identity below MIX_SOFT_KNEE, then a hyperbola asymptotic to full
 * scale.
 *
 *   y = KNEE + (RANGE * over) / (RANGE + over),  over = |x| - KNEE
 *                                                RANGE = MAX - KNEE
 *
 * Three properties make this the right curve here, and all three are pinned by
 * tests in test/test_mixer:
 *
 *  - dy/d(over) = RANGE^2 / (RANGE + over)^2, which is exactly 1 at the knee,
 *    so the transfer function is C1-continuous and there is no audible corner
 *    where compression starts. (In integer arithmetic the division truncates,
 *    so individual steps just past the knee are 0 or 1 rather than exactly 1;
 *    the test asserts no discontinuity and a slope within a few percent of
 *    unity, which is the property that matters.)
 *  - (RANGE * over) / (RANGE + over) < RANGE for every finite over, so the
 *    output never reaches full scale. Clipping stops being possible by
 *    construction rather than by tuning.
 *  - It is odd, stateless and lookahead-free: no added latency, which matters
 *    because the whole project exists to keep trigger-to-sound low. A real
 *    brickwall limiter needs a delay line; one without lookahead reacts after
 *    the transient it was supposed to catch, which is useless for percussion.
 *
 * The product is computed in 64 bits. The multiply only runs on samples past
 * the knee, and keeping it wide means MIX_SOFT_KNEE can be lowered (which
 * grows RANGE) without silently overflowing.
 */
static inline int16_t mixSoftClip(int32_t x) {
  const int32_t knee  = MIX_SOFT_KNEE;
  const int32_t range = MIX_SAMPLE_MAX - knee;
  int32_t a = x < 0 ? -x : x;
  if (a <= knee) return (int16_t)x;

  int32_t over = a - knee;
  int32_t y = knee + (int32_t)(((int64_t)range * over) / ((int64_t)range + over));
  return (int16_t)(x < 0 ? -y : y);
}
