/*
 * trigger_input.h - Trigger conditioning for every button on the instrument.
 *
 * THIS FILE IS COMPILED INTO THE FIRMWARE AND INTO THE TESTS. That is the
 * whole reason it exists as a header instead of living in main.cpp. It used to
 * live there, with a hand-written copy in sim/trigger_model.h for the tests to
 * exercise, and the two had already drifted: the copy grew a confirm_us == 0
 * fast path that the firmware never had, on the exact code path the bench
 * procedure in docs/circuit-rc.md tells you to use. The tests were green and
 * measuring something the board would not do.
 *
 * Divergence is now impossible by construction rather than by vigilance, which
 * matters here more than usual: this project's trigger bugs have all been
 * silent, showing up as odd audio rather than as anything a build or a log
 * would flag.
 *
 * --- The design ---
 *
 * All four buttons run this one state machine. The sharing is deliberate: the
 * retrigger bug that this replaced survived review because the trigger path
 * had drifted into two divergent debounce implementations and only one of them
 * was ever scrutinised.
 *
 * Both guards read the RAW pin. Nothing here derives a decision from a
 * filtered or debounced state - the lag of such a state couples correctness to
 * the filter's constant, which is precisely how the old bug worked. Every
 * comparison is an unsigned difference, so the ~71 minute micros() wraparound
 * is a non-event; test_trigger.cpp verifies that across the boundary.
 *
 *   confirm_us: the LOW must persist this long before firing. Rejects glitches
 *     shorter than the window, and is the ONLY defense against phantom hits -
 *     the RC filter in docs/circuit-rc.md does not cover them, and in fact
 *     makes contact-type glitches longer. Keep it under one contact-bounce
 *     segment (~300us): above that it cannot be satisfied until the whole
 *     bounce settles and the latency becomes the bounce duration instead.
 *     200us costs 0.2ms; 1ms costs 3ms.
 *   release_ms: rearm only after the raw pin reads HIGH continuously. Guards
 *     re-fire during contact bounce. With the RC filter fitted, 1 is enough,
 *     and lowering it raises the maximum sustainable roll rate.
 *
 * The two windows are instance members rather than baked-in macros so the
 * sweeps and tests can vary them per instance. The firmware never sets them
 * and pays 8 bytes per button for the privilege - 32 bytes total, against
 * 320KB of internal SRAM.
 */

#pragma once

#include <stdint.h>

#ifndef TRIG_CONFIRM_US
  #define TRIG_CONFIRM_US 200
#endif
#ifndef TRIG_RELEASE_MS
  #define TRIG_RELEASE_MS 3
#endif

struct TriggerInput {
  uint32_t confirm_us   = TRIG_CONFIRM_US;
  uint32_t release_ms   = TRIG_RELEASE_MS;

  bool     armed        = true;
  bool     pending      = false;
  // pendingSince is deliberately NOT cleared when a strike fires: it then holds
  // the instant the raw pin first went LOW, which is what LATENCY_PROBE needs
  // to measure true button-to-sound rather than just start()-to-sound.
  uint32_t pendingSince = 0;
  bool     highValid    = false;
  uint32_t highSince    = 0;

  // True on the single poll where a strike is confirmed. `low` is the raw pin,
  // active-low: true means the contact is closed.
  bool update(uint32_t now_us, bool low) {
    if (armed) {
      if (!low) {
        pending = false;            // bounced back HIGH: a glitch, not a strike
      } else if (!pending) {
        pending = true; pendingSince = now_us;
      } else if ((uint32_t)(now_us - pendingSince) >= confirm_us) {
        armed = false; pending = false; highValid = false;
        return true;
      }
    } else if (low) {
      highValid = false;            // still held or bouncing: restart the window
    } else if (!highValid) {
      highValid = true; highSince = now_us;
    } else if ((uint32_t)(now_us - highSince) >= release_ms * 1000UL) {
      armed = true;
    }
    return false;
  }

  // The same state read as a LEVEL instead of an edge: true from the poll a
  // strike is confirmed until the raw pin has read HIGH continuously for
  // release_ms. For a hold-to-engage control (GPIO7 runs the pitch shifter
  // while pressed) rather than a one-shot strike.
  //
  // Deliberately an accessor on this machine and not a second machine. A hold
  // needs exactly the conditioning a strike needs - confirm before acting,
  // require continuous HIGH before believing the release - and `armed` already
  // tracks precisely that interval, because rearming and releasing are the
  // same question. A separate hold debounce would be the second divergent
  // implementation in the trigger path that this header exists to prevent; it
  // is also where a shorter release window would quietly make the effect
  // stutter on contact bounce while the pads kept working.
  bool held() const { return !armed; }
};
