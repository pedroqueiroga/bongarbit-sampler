#pragma once
/*
 * The block-based float audio engine: its units, its buffer, and the two
 * conversions at its edges.
 *
 * Compiled into the firmware AND into the host test suite, for the same reason
 * trigger_input.h and mixer.h are. Nothing here touches Arduino, M16 or the
 * I2S driver, so the arithmetic can be verified on a desktop.
 *
 * WHY BLOCKS
 *
 * Three reasons, in increasing order of how much they actually mattered:
 *
 *  1. Driver calls. M16 used to issue one i2s_channel_write() per frame, which
 *     docs/bancada.md section 9 measured at ~1,126 cycles of pure overhead per
 *     frame - about 19% of one core at 44.1kHz. The library now batches 32
 *     frames itself (see the pin note in platformio.ini), so this reason is
 *     already banked and is NOT why this header exists.
 *
 *  2. Memory locality. Rendering voice-by-voice across a whole block means
 *     each voice walks 32 consecutive frames of its own PSRAM buffer before
 *     the next voice starts. Frame-by-frame rendering interleaves four PSRAM
 *     streams and thrashes the cache between them. This is a real gain that
 *     the library batching does nothing for.
 *
 *  3. The one that forced the change: a block is the only place DSP with
 *     memory can live. A pitch shifter needs a window of signal, not one
 *     sample - see pitch_shift.h. Per-sample audioUpdate() has no window to
 *     offer, and no amount of library batching creates one, because M16
 *     buffers OUR OUTPUT, after we have already had to decide what it is.
 *
 * WHY FLOAT
 *
 * Normalised float (+-1.0 = full scale) is what every audio framework speaks -
 * Bela, JUCE, plugin APIs, every DSP paper. The point is not precision: 16-bit
 * fixed point was fine for summing four samples. The point is that the next
 * thing going in here is a live audio-in stream and a processing chain, and a
 * chain in conventional units can take DSP from anywhere without rescaling it,
 * and hand its own out the same way.
 *
 * The S3's single-precision FPU makes this close to free in a task - one
 * convert and one multiply per voice per frame. It is NOT free in an interrupt
 * handler, where the FPU is unavailable; that is the trade recorded in
 * docs/bancada.md section 9, and it is the reason the engine is a task.
 */
#include <stdint.h>

// Frames per block.
//
// 32 at 44.1kHz = 0.73ms, deliberately the same as M16's own M16_BLOCK_SIZE so
// one render fills exactly one of its DMA bursts. That alignment is a
// convenience, not a requirement: the engine drains each block immediately
// instead of holding it, so a mismatch would only mean the pacing granularity
// is the larger of the two. What IS a requirement is staying well under the
// DMA ring depth, because the ring drains while we render - main.cpp asserts
// that relationship, where both numbers are visible.
//
// It is also the latency floor of any block-based processor downstream, which
// is the other reason not to grow it casually.
#ifndef AUDIO_BLOCK_FRAMES
  #define AUDIO_BLOCK_FRAMES 32
#endif

// 2^15, the scale factor between normalised float and the 16-bit output word.
// Deliberately 32768 and not 32767: it makes the conversion an exact power of
// two in both directions, so a round trip of any int16 is bit-exact and no
// rounding error accumulates in the chain. The cost is that +1.0 maps to 32768
// and has to be clamped, which audioToI16() does anyway for the general case.
#define AUDIO_FULL_SCALE 32768.0f

// Where the output limiter starts bending, in normalised units, and the
// ceiling it approaches. Both are the integer values from mixer.h expressed in
// this scale (24576/32768 and 32767/32768) rather than independently chosen
// round numbers, so the curve is the same curve - see audioSoftClip().
#ifndef AUDIO_SOFT_KNEE
  #define AUDIO_SOFT_KNEE 0.75f
#endif
#define AUDIO_SOFT_CEIL (32767.0f / AUDIO_FULL_SCALE)

/*
 * One block of stereo audio, de-interleaved.
 *
 * Separate L and R arrays rather than interleaved frames: every processor in
 * the chain walks one channel at a time, and the pitch shifter keeps a
 * per-channel history buffer, so de-interleaved is the layout that makes those
 * loops contiguous. Interleaving is a property of the wire, and it happens
 * once, at the edge, in the drain loop.
 *
 * 32 frames x 2 channels x 4 bytes = 256 bytes. Small enough to live in
 * internal RAM without thinking about it, which matters: a block buffer in
 * PSRAM would reintroduce exactly the cache traffic that reason 2 above is
 * about.
 */
struct AudioBlock {
  float l[AUDIO_BLOCK_FRAMES];
  float r[AUDIO_BLOCK_FRAMES];
};

/* Start a block from silence. Voices accumulate into it, so this has to run
 * first - a stale block is the previous 0.73ms of audio played again. */
static inline void audioBlockSilence(AudioBlock& b) {
  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
    b.l[n] = 0.0f;
    b.r[n] = 0.0f;
  }
}

/* 16-bit sample word -> normalised float. Exact for every int16. */
static inline float audioFromI16(int16_t s) {
  return (float)s * (1.0f / AUDIO_FULL_SCALE);
}

/*
 * Normalised float -> 16-bit sample word.
 *
 * Rounds to nearest rather than truncating. Truncation toward zero biases
 * every sample inward by up to 1 LSB, which on a signal crossing zero is a
 * DC-free but correlated error - audible as a faint rasp on quiet tails, the
 * same family of artefact as the 8-bit sample noise floor that confounded the
 * bench of 13/08/2026.
 *
 * Clamps as the last act before the wire. The soft knee upstream already keeps
 * the signal inside the ceiling, so this clamp should never bind in normal
 * operation; it is here because a NaN or a runaway delay line must become a
 * bounded sample rather than a wrapped one, and int conversion of an
 * out-of-range float is undefined behaviour.
 */
static inline int16_t audioToI16(float x) {
  float scaled = x * AUDIO_FULL_SCALE;
  if (!(scaled > -32768.0f)) return (int16_t)-32768; // also catches NaN
  if (scaled >= 32767.0f) return (int16_t)32767;
  return (int16_t)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

/*
 * The output limiter: identity below the knee, hyperbola asymptotic to the
 * ceiling above it.
 *
 *   y = KNEE + (RANGE * over) / (RANGE + over),  over = |x| - KNEE
 *                                                RANGE = CEIL - KNEE
 *
 * This is mixSoftClip() from mixer.h, in normalised units. The full rationale
 * for the curve - unity slope at the knee, never reaches full scale, odd,
 * stateless, no lookahead - is documented there and pinned by tests there.
 *
 * mixer.h is no longer in the audio path, and is kept as the REFERENCE this
 * function is tested against: test/test_audio_block sweeps both and asserts
 * they agree within 1 LSB. That is what connects this implementation to the
 * behaviour that was actually judged by ear on hardware, and what makes the
 * two unable to drift apart silently. Do not delete mixer.h as dead code.
 *
 * The 1 LSB of slack is the integer version's truncating division, which pulls
 * it toward zero by less than one step. Nothing else differs.
 */
static inline float audioSoftClip(float x) {
  const float knee  = AUDIO_SOFT_KNEE;
  const float range = AUDIO_SOFT_CEIL - knee;
  float a = x < 0.0f ? -x : x;
  if (a <= knee) return x;

  float over = a - knee;
  float y = knee + (range * over) / (range + over);
  return x < 0.0f ? -y : y;
}
