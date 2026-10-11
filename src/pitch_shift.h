#pragma once
/*
 * Pitch shifting without changing duration, as a stream processor.
 *
 * Compiled into the firmware AND into the host test suite, same contract as
 * trigger_input.h, mixer.h and audio_block.h. Nothing here touches Arduino,
 * M16, the I2S driver or any clock: it is a pure function of (input block,
 * state), which is what lets its pitch be measured on a desktop instead of
 * argued about by ear.
 *
 * WRITTEN FOR A STREAM, NOT FOR THE SAMPLER
 *
 * Shifting a stored sample's pitch is trivial and this is not that: Samp
 * already resamples, so playing a buffer faster would shift it - and shorten
 * it. This processes a signal it has never seen the end of, which is the case
 * that generalises. It reads a block, writes a block, keeps its own history,
 * and knows nothing about where the signal came from. When audio-in arrives,
 * it gets read into the top of the block in main.cpp's audioUpdate() and this
 * code does not change at all. test_block_size_does_not_change_the_output is
 * the test that holds that promise: a different host block size must produce
 * identical samples, or "no rewrite" was a guess.
 *
 * HOW IT WORKS: two taps on a delay line
 *
 * Write the input into a circular buffer at 1 frame per frame. Read it back at
 * `ratio` frames per frame. Reading faster raises the pitch, slower lowers it,
 * and the write side still consumes exactly one input frame per output frame -
 * so duration is untouched. That is the whole trick.
 *
 * The catch is that a read pointer moving at a different rate than the write
 * pointer drifts, and must eventually jump back, which is a discontinuity in
 * the signal - a click, once per drift cycle. So there are two read taps, half
 * a buffer apart, each windowed by a crossfade that is exactly zero at its own
 * jump. One tap is always jumping while the other carries the signal.
 *
 * Expressed as a phase `ph` in [0,1):
 *
 *   lag0 = LAG_MIN + ph           * span       gain0 = sin(pi * ph)
 *   lag1 = LAG_MIN + frac(ph+0.5) * span       gain1 = |cos(pi * ph)|
 *   ph advances by (1 - ratio) / span per frame
 *
 * so lag0 moves at (1 - ratio) frames per frame - which is exactly a read
 * pointer moving at `ratio` against a writer moving at 1 - and the jump lands
 * at ph = 0 for tap 0 and ph = 0.5 for tap 1, where that tap's gain is zero.
 *
 * WHAT IT COSTS, HONESTLY
 *
 * Two artefacts, both inherent to time-domain shifting rather than to this
 * implementation:
 *
 *  - Warble. The crossfade cycle repeats every span/|1-ratio| frames: at an
 *    octave that is ~23ms, a ~43Hz amplitude and phase modulation. Percussion
 *    mostly masks it under the transient. A sustained tonal note does not, and
 *    will sound rough.
 *  - Latency. The taps read a mean of span/2 frames behind the writer, so the
 *    effect is ~11.6ms late at PITCH_BUF_FRAMES = 1024. Not avoidable: pitch
 *    shifting needs a window of signal to resample, and the window IS the
 *    latency. latencyFrames() reports it rather than leaving it to be
 *    discovered.
 *
 * Both scale with PITCH_BUF_FRAMES, in opposite directions, which is the only
 * real knob here: bigger buffer = slower, less buzzy warble and more latency.
 *
 * The alternative is a phase vocoder - FFT, modify phases, inverse FFT - which
 * trades the warble for smearing on transients, needs a window at least as
 * long as this buffer anyway (so it is not faster), and costs vastly more CPU.
 * For a percussion instrument that is the wrong trade. Not ruled out forever;
 * ruled out for this.
 *
 * Equal-POWER crossfade between the taps (sin/cos, sum of squares = 1), not
 * equal-amplitude, because the two taps read the signal at different times and
 * are effectively uncorrelated, so power is what adds. The visible consequence
 * is that genuinely correlated content - DC, very low frequencies - swells by
 * up to 3dB across the crossfade instead of staying flat. The dry/wet mix
 * below uses equal-AMPLITUDE for the opposite reason: dry and wet are the same
 * signal, so they are maximally correlated and amplitude is what adds.
 */
#include <stdint.h>
#include <math.h>
#include "audio_block.h"

// History per channel, in frames. MUST be a power of two - the read taps index
// it with a mask rather than a modulo, every frame.
//
// 1024 at 44.1kHz: ~11.6ms of latency, ~43Hz of warble at an octave, and
// 2 x 1024 x 4 = 8KB of RAM. That RAM must be internal, never PSRAM: the taps
// read two moving positions every frame, which is close to the worst possible
// access pattern for a cache, and the whole point of this project is that
// nothing in the audio path stalls.
#ifndef PITCH_BUF_FRAMES
  #define PITCH_BUF_FRAMES 1024
#endif
#define PITCH_BUF_MASK (PITCH_BUF_FRAMES - 1)

// Margin kept at both ends of the tap range. The taps interpolate between two
// neighbouring history samples, so they need one frame of room beyond the lag
// itself, at both the fresh end (don't read past the write pointer) and the
// stale end (don't wrap into samples the writer has already overwritten).
#define PITCH_LAG_MIN 2.0f
#define PITCH_LAG_MAX ((float)PITCH_BUF_FRAMES - 2.0f)
#define PITCH_LAG_SPAN (PITCH_LAG_MAX - PITCH_LAG_MIN)

// Shift limits, +-2 octaves. Not a musical judgement so much as a geometric
// one: the phase advance is |1 - ratio| / span per frame and has to stay well
// under 1, or a single frame skips a whole crossfade cycle and the output is
// noise rather than a shift.
#define PITCH_RATIO_MIN 0.25f
#define PITCH_RATIO_MAX 4.0f

// Dry/wet ramp on engage and release, in frames. ~5ms at 44.1kHz: long enough
// that switching the effect in is not a step discontinuity (a click), short
// enough to feel like a button rather than a fade. The wet signal is also
// ~11.6ms delayed, so the ramp is audible as a brief doubling - that is the
// latency being revealed, not the ramp misbehaving.
#ifndef PITCH_RAMP_FRAMES
  #define PITCH_RAMP_FRAMES 220
#endif

// Spelled out rather than taken from M_PI, which is a POSIX extension and not
// guaranteed by <cmath> under a strict -std=c++17. This header compiles under
// both the host toolchain and the Xtensa one; that is the whole point of it.
#define PITCH_PI 3.14159265358979323846f

/* Semitones -> playback ratio. 12 semitones = 2.0. Call this at setup time and
 * keep the result: it is a powf(), which has no business in an audio block. */
static inline float pitchRatioFromSemitones(float semitones) {
  return powf(2.0f, semitones / 12.0f);
}

class PitchShift {
 public:
  /* Clear all history and state. Must run before the first process() - the
   * history buffer is read before it is fully written otherwise, and on a
   * static instance that means reading zeroes, which is merely silent, but on
   * a stack or heap instance it means reading whatever was there. */
  void init() {
    for (int i = 0; i < PITCH_BUF_FRAMES; i++) {
      _histL[i] = 0.0f;
      _histR[i] = 0.0f;
    }
    _w = 0;
    _ph = 0.0f;
    _mix = 0.0f;
    _engaged = false;
    _request = false;
    setRatio(1.0f);
  }

  /* Set the shift. Clamped to +-2 octaves; getRatio() reports what was
   * actually taken.
   *
   * Safe to change mid-stream, and that is a deliberate property rather than
   * an accident: the ratio only sets the phase ADVANCE, so changing it bends
   * the pitch continuously instead of jumping the read pointer. A knob or an
   * envelope can drive this later with nothing else to add. */
  void setRatio(float ratio) {
    if (ratio < PITCH_RATIO_MIN) ratio = PITCH_RATIO_MIN;
    if (ratio > PITCH_RATIO_MAX) ratio = PITCH_RATIO_MAX;
    _ratio = ratio;
    _phInc = (1.0f - ratio) / PITCH_LAG_SPAN;
  }

  float getRatio() const { return _ratio; }

  /* Engage or release the effect. Both directions ramp; neither jumps.
   *
   * Called from loop(), on a different core from process(). It therefore only
   * records a REQUEST, and process() applies it at the top of a block. The
   * alternative - acting here - would have loop() writing the crossfade phase
   * while the audio task reads it, which is a genuine cross-core data race on
   * the one piece of state the whole algorithm is built on. Deferring makes
   * the audio task the only writer of everything it reads, at a cost of up to
   * one block (0.73ms) of latency on the gesture, which is below the 11.6ms
   * the effect itself carries.
   *
   * `volatile` and not std::atomic because a single aligned bool cannot tear
   * on this architecture; what was needed was that the compiler not cache it
   * across the audio task's loop. */
  void setEngaged(bool on) { _request = on; }

  bool isEngaged() const { return _request; }

  /* True when the processor is contributing nothing and process() is a
   * bit-exact passthrough. Exposed because "the effect is off" and "the effect
   * is on at zero mix" are different states and only one of them is free. */
  bool isBypassing() const { return !_engaged && _mix <= 0.0f; }

  /* Mean delay the wet signal carries, in frames. The taps sweep the whole lag
   * range, so this is an average and not a constant - but it is the number
   * that belongs in the latency budget, so it is reported rather than left to
   * be measured. */
  static float latencyFrames() { return PITCH_LAG_MIN + PITCH_LAG_SPAN * 0.5f; }

  /* Process a block in place. The convenience overload for the engine's own
   * block size; everything real happens in the one below. */
  void process(AudioBlock& io) { process(io.l, io.r, AUDIO_BLOCK_FRAMES); }

  /*
   * Process `frames` frames of de-interleaved stereo, in place.
   *
   * Takes a frame count rather than assuming AUDIO_BLOCK_FRAMES so that the
   * host block size is not baked into the DSP. That is what
   * test_block_size_does_not_change_the_output verifies, and what makes this
   * usable from a different-sized audio-in callback later without a rewrite.
   *
   * History is written on EVERY call, including while bypassed. That costs
   * `frames` stores per channel and buys the thing that matters: when the
   * button is pressed, the taps read audio that really happened, instead of
   * the effect having to fill its buffer from silence first and arriving
   * ~11.6ms after the gesture.
   */
  void process(float* l, float* r, int frames) {
    // Apply loop()'s request here, where the audio task owns the state. This
    // must happen before isBypassing() is consulted, or an engage would be
    // deferred by a whole extra block.
    //
    // Engaging resets the crossfade phase to 0, where tap 1 carries the whole
    // signal and tap 0 is silent. Starting anywhere else means starting with
    // two taps at fixed unrelated lags, which for the first instant is a comb
    // filter rather than a pitch shift.
    const bool want = _request;
    if (want != _engaged) {
      _engaged = want;
      if (want) _ph = 0.0f;
    }

    const bool bypass = isBypassing();

    for (int n = 0; n < frames; n++) {
      const float inL = l[n];
      const float inR = r[n];

      _w = (_w + 1) & PITCH_BUF_MASK;
      _histL[_w] = inL;
      _histR[_w] = inR;

      if (bypass) continue; // history fed, output untouched, nothing computed

      // Both channels share one phase and one pair of lags. Giving them
      // independent phases would make the two channels drift against each
      // other and smear the stereo image, which reads as the instrument
      // wandering rather than as an effect.
      const float ph1  = _ph >= 0.5f ? _ph - 0.5f : _ph + 0.5f;
      const float lag0 = PITCH_LAG_MIN + _ph * PITCH_LAG_SPAN;
      const float lag1 = PITCH_LAG_MIN + ph1  * PITCH_LAG_SPAN;

      // Equal power: gain0^2 + gain1^2 = 1. Each is zero exactly where its own
      // tap jumps, which is what hides the jump. sinf() per frame is ~4% of
      // the block budget; a 256-entry table would remove most of that and is
      // the first thing to reach for if this ever needs to be cheaper.
      const float gain0 = sinf(PITCH_PI * _ph);
      const float gain1 = fabsf(cosf(PITCH_PI * _ph));

      float wetL = gain0 * tap(_histL, lag0) + gain1 * tap(_histL, lag1);
      float wetR = gain0 * tap(_histR, lag0) + gain1 * tap(_histR, lag1);

      // Equal amplitude here, unlike the taps above: dry and wet are the same
      // signal, so they are correlated and amplitude is what adds.
      const float wet = _mix;
      const float dry = 1.0f - _mix;
      l[n] = dry * inL + wet * wetL;
      r[n] = dry * inR + wet * wetR;

      _ph += _phInc;
      if (_ph >= 1.0f) _ph -= 1.0f;
      else if (_ph < 0.0f) _ph += 1.0f;

      const float step = 1.0f / (float)PITCH_RAMP_FRAMES;
      if (_engaged) {
        if (_mix < 1.0f) { _mix += step; if (_mix > 1.0f) _mix = 1.0f; }
      } else {
        if (_mix > 0.0f) { _mix -= step; if (_mix < 0.0f) _mix = 0.0f; }
      }
    }
  }

 private:
  /* One interpolated read, `lag` frames behind the write pointer.
   *
   * lag = i + frac, so the sample sits between hist[w-i] and hist[w-i-1],
   * where the second is FURTHER back in time - frac moves toward the past as
   * the lag grows. PITCH_LAG_MIN/MAX keep both indices inside the history, so
   * no bounds check is needed here; the mask only handles the wrap.
   *
   * Linear interpolation, not cubic. The error is a gentle low-pass that
   * worsens with the fractional distance, which on percussion through a 16-bit
   * DAC sits under the warble this thing already has. Cubic would be the next
   * thing to try if the shift ever needs to sound clean on tonal material -
   * though at that point the phase vocoder is the better answer.
   */
  inline float tap(const float* hist, float lag) const {
    const uint32_t i    = (uint32_t)lag;
    const float    frac = lag - (float)i;
    const float    a    = hist[(_w - i) & PITCH_BUF_MASK];
    const float    b    = hist[(_w - i - 1) & PITCH_BUF_MASK];
    return a + frac * (b - a);
  }

  float    _histL[PITCH_BUF_FRAMES];
  float    _histR[PITCH_BUF_FRAMES];
  uint32_t _w       = 0;
  float    _ph      = 0.0f;
  float    _mix     = 0.0f;
  float    _ratio   = 1.0f;
  float    _phInc   = 0.0f;
  bool     _engaged = false;         // owned by the audio task
  volatile bool _request = false;    // written by loop(), read by the audio task
};
