/*
 * test_trigger.cpp - Unit suite for the firmware's trigger conditioning.
 *
 *     pio test -e native
 *
 * Runs on the host. No board, no upload.
 *
 * The scheme under test is TriggerInput from src/trigger_input.h - the actual
 * struct the board runs, compiled into this binary, not a transcription of it.
 * sim/trigger_model.h supplies the scaffolding around it: pin traces, the RC
 * filter model, the scoring runner, and models of the two DESIGNS THAT WERE
 * REJECTED. Those last ones are copies on purpose - they no longer exist in
 * the firmware, and their whole job is to keep failing in the documented way.
 *
 * WHAT THIS SUITE IS FOR. Every bug this project has hit in the trigger path
 * was silent: the firmware kept running and the defect showed up only as odd
 * audio. So the tests are organised around the *reasons* rather than the code
 * shape - each rejected design has a test named after how it failed, so that
 * reintroducing it fails a test that explains itself instead of producing a
 * mystery buzz on a breadboard three weeks later.
 *
 * Groups:
 *   1. Harness fidelity   - the model still reproduces historically measured
 *                           numbers; if this fails, no other result is
 *                           trustworthy, so it runs first.
 *   2. Shipped behaviour  - the contract TriggerInput must satisfy.
 *   3. Rejected designs   - the two schemes that were tried and abandoned,
 *                           pinned with their failure modes.
 *   4. Timing contracts   - what the two tunable constants actually buy.
 *   5. Internal contracts - structural promises other firmware code reads.
 *   6. micros() wraparound- the firmware claims unsigned arithmetic survives
 *                           the ~71 minute wrap. This checks that claim.
 *   7. RC filter model    - what the hardware filter does, and what it does
 *                           NOT do.
 */

#include <unity.h>

#include "trigger_model.h"

// --- Fixture -------------------------------------------------------------
// Rebuilt for every test. This is not ceremony: Trace owns heap vectors that
// grow to thousands of entries once a trace is RC-filtered, and Config carries
// tunables that several tests deliberately mutate. A shared instance would let
// one test's parameter change silently alter the next one's result - exactly
// the class of coupling this suite exists to catch.

namespace {

struct Fixture {
    Config   cfg;   // documented defaults, restored per test
    Trace    trace; // scratch
    RCParams rc;    // documented defaults, restored per test
};

Fixture* fx = nullptr;

// The scheme actually shipped in src/main.cpp: TRIG_CONFIRM_US 200,
// TRIG_RELEASE_MS 3, both reading the raw pin.
TriggerInputScheme shipped() { return trig(200, 3); }

Trace pressTrace(uint64_t bounce_us = 2000, uint64_t hold_us = 150000,
                 int bounces = 6) {
    Trace t; t.name = "press"; t.duration_us = 400000;
    addPress(t, 10000, bounce_us, hold_us, bounces);
    return t;
}

} // namespace

void setUp(void)    { fx = new Fixture(); }
void tearDown(void) { delete fx; fx = nullptr; }

// =========================================================================
// 1. Harness fidelity - run these first
// =========================================================================

// The original bug was measured at 112 fires with a 50us poll and 3 with a
// 3000us poll. Those two numbers are the model's calibration against reality:
// if a refactor changes them, the model has drifted and every other assertion
// in this file is measuring something else.
void test_harness_reproduces_historical_machine_gun(void) {
    fx->trace = pressTrace();
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        112, run(fx->trace, RawFire{{10}}, 50).fires,
        "model drifted: the 50us machine-gun count no longer matches the "
        "originally measured 112");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        3, run(fx->trace, RawFire{{10}}, 3000).fires,
        "model drifted: the 3000us machine-gun count no longer matches the "
        "originally measured 3");
}

// The count scaling with poll rate is the signature that distinguishes a race
// from a debounce problem. Fewer polls, fewer spurious fires.
void test_machine_gun_count_scales_with_poll_rate(void) {
    fx->trace = pressTrace();
    int fast = run(fx->trace, RawFire{{10}}, 20).fires;
    int slow = run(fx->trace, RawFire{{10}}, 500).fires;
    TEST_ASSERT_TRUE_MESSAGE(fast > slow * 4,
        "spurious fire count should scale with poll rate - that scaling is "
        "what identifies the defect as a rearm race");
}

// =========================================================================
// 2. Shipped behaviour - the contract
// =========================================================================

void test_clean_press_fires_once(void) {
    fx->trace = pressTrace(0, 150000, 0);
    Result r = run(fx->trace, shipped(), fx->cfg.loop_us);
    TEST_ASSERT_EQUAL_INT(1, r.fires);
    TEST_ASSERT_EQUAL_INT(0, r.spurious);
    TEST_ASSERT_EQUAL_INT(0, r.missed);
}

void test_bouncy_press_fires_once(void) {
    fx->trace = pressTrace(2000, 150000, 6);
    Result r = run(fx->trace, shipped(), fx->cfg.loop_us);
    TEST_ASSERT_EQUAL_INT(1, r.fires);
    TEST_ASSERT_EQUAL_INT(0, r.spurious);
}

void test_worn_switch_fires_once(void) {
    fx->trace = pressTrace(15000, 150000, 12);
    Result r = run(fx->trace, shipped(), fx->cfg.loop_us);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.fires,
        "a switch bouncing for 15ms must still produce exactly one hit");
}

// Holding a pad down must not retrigger. This is the failure the original
// lockout existed to prevent, and the one the buggy version reintroduced.
void test_held_button_fires_once(void) {
    fx->trace = pressTrace(2000, 500000, 6);
    fx->trace.duration_us = 800000;
    Result r = run(fx->trace, shipped(), fx->cfg.loop_us);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.fires,
        "holding the pad for 500ms must produce one hit, not a roll");
}

void test_fast_roll_fires_once_per_strike(void) {
    fx->trace = makeRoll("roll", 8, 80000, 2000, 40000, 6);
    Result r = run(fx->trace, shipped(), fx->cfg.loop_us);
    TEST_ASSERT_EQUAL_INT(8, r.fires);
    TEST_ASSERT_EQUAL_INT(0, r.missed);
    TEST_ASSERT_EQUAL_INT(0, r.spurious);
}

void test_emi_glitch_is_rejected(void) {
    fx->trace = makeGlitch("glitch", 50);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, run(fx->trace, shipped(), fx->cfg.loop_us).fires,
        "a 50us glitch is shorter than TRIG_CONFIRM_US and must not fire");
}

// The property the buggy version lacked. Behaviour must not depend on how
// fast loop() happens to run - that dependence was the bug.
void test_behaviour_is_independent_of_poll_rate(void) {
    fx->trace = pressTrace();
    for (uint64_t lp : {20ULL, 50ULL, 100ULL, 200ULL, 500ULL, 1000ULL,
                        2000ULL, 3000ULL, 5000ULL}) {
        char msg[96];
        snprintf(msg, sizeof msg, "poll period %llu us changed the outcome",
                 (unsigned long long)lp);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, run(fx->trace, shipped(), lp).fires, msg);
    }
}

// =========================================================================
// 3. Rejected designs, pinned with their failure modes
// =========================================================================

// Rearming on Bounce2's debounced LEVEL. read() still returns HIGH for the
// whole debounce interval after the strike, so the rearm condition is already
// satisfied at the moment of firing.
void test_rejected_level_rearm_machine_guns(void) {
    fx->trace = pressTrace();
    TEST_ASSERT_TRUE_MESSAGE(
        run(fx->trace, RawFire{{10}}, 50).fires > 100,
        "rearming on the debounced LEVEL must be seen to machine-gun - if this "
        "ever passes as 1, the model stopped reproducing the original bug");
}

// Rearming on the debounced EDGE. Fixes the machine-gun, but rose() can only
// fire if the debounced state went LOW first, which needs the contact held for
// bounce + interval. A shorter strike never registers, so nothing rises and
// the pad latches off permanently - at ANY retrigger interval.
void test_rejected_edge_rearm_latches_on_short_contact(void) {
    // 12 strikes, 300ms apart: spacing is far above every retrigger cliff, so
    // anything missed here is the contact-time floor, not a rate limit.
    fx->trace = makeRoll("short taps", 12, 300000, 2000, 8000 /*8ms hold*/, 6);
    Result rose = run(fx->trace, RawFireRose{{10}}, 50, 300000);
    TEST_ASSERT_TRUE_MESSAGE(rose.missed >= 10,
        "rose() must be seen to latch off on 8ms contacts with a 10ms debounce");

    Result ship = run(fx->trace, shipped(), 50, 300000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ship.missed,
        "the shipped scheme must have no contact-time floor");
}

// =========================================================================
// 4. Timing contracts
// =========================================================================

// Below one contact-bounce segment, the window IS the trigger latency.
void test_confirm_window_sets_trigger_latency(void) {
    fx->trace = pressTrace(2000, 150000, 6); // 2ms / 6 = 333us segments
    for (uint32_t confirm : {0u, 100u, 200u}) {
        Result r = run(fx->trace, trig(confirm, 3), 10);
        double expected_ms = confirm / 1000.0;
        char msg[96];
        snprintf(msg, sizeof msg, "confirm %u us should cost %.3f ms",
                 confirm, expected_ms);
        TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.05, expected_ms, r.meanLat_ms(), msg);
    }
}

// Above one segment it is a cliff, not a gradient. A window wider than a
// contact-open segment can never complete inside one, so it only starts
// counting once the bounce settles - and the latency becomes bounce + confirm.
// This is why the firmware comment says to keep TRIG_CONFIRM_US under ~300us,
// and it is the constraint that makes 1ms cost 3ms instead of 1ms.
void test_confirm_window_wider_than_a_bounce_segment_costs_the_whole_bounce(void) {
    fx->trace = pressTrace(2000, 150000, 6);
    Result r = run(fx->trace, trig(500, 3), 10);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.1, 2.5, r.meanLat_ms(),
        "a 500us window against 333us segments costs bounce+confirm = 2.5ms, "
        "not 0.5ms - keep TRIG_CONFIRM_US under one segment");
}

// Glitches shorter than the window are rejected; longer ones are not. The
// window is a threshold, not a general noise filter.
void test_confirm_window_is_a_threshold(void) {
    auto s = trig(200, 3);
    fx->trace = makeGlitch("short", 100);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run(fx->trace, s, 10).fires,
        "100us glitch is under the 200us window and must be rejected");
    fx->trace = makeGlitch("long", 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, run(fx->trace, s, 10).fires,
        "1ms glitch exceeds the window and WILL fire - the window is a "
        "threshold, and the hardware RC does not cover this either");
}

// Rearming needs a clean-HIGH window, so retriggering faster than
// hold + bounce + release drops hits. This is a real limit, not a bug, and it
// is documented so nobody 'fixes' it by reintroducing a filtered-state rearm.
void test_release_window_sets_the_retrigger_floor(void) {
    const uint64_t hold = 40000, bounce = 2000;
    const uint32_t release = 3;
    uint64_t floor_us = hold + bounce + release * 1000;

    Trace tooFast = makeRoll("", 12, floor_us - 5000, bounce, hold, 6);
    TEST_ASSERT_TRUE_MESSAGE(
        run(tooFast, trig(200, release), 50, floor_us).missed > 0,
        "below hold+bounce+release, hits must be dropped");

    Trace ok = makeRoll("", 12, floor_us + 5000, bounce, hold, 6);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, run(ok, trig(200, release), 50, floor_us + 5000).missed,
        "above the floor, every hit must play");
}

// =========================================================================
// 5. Internal contracts other firmware code depends on
// =========================================================================
// These drive TriggerInput directly rather than through the sim's runner.
// They are not about timing or about what a player observes - they pin
// structural promises that other parts of the firmware read, and that until
// now only a comment was guarding.

// LATENCY_PROBE subtracts trigInput.pendingSince after a strike to report true
// button-to-sound rather than start()-to-sound. That only works because
// update() deliberately does NOT clear it on fire.
void test_pendingSince_survives_the_strike_for_the_latency_probe(void) {
    TriggerInput t;
    t.confirm_us = 200; t.release_ms = 3;
    const uint32_t edge = 1000000;
    TEST_ASSERT_FALSE(t.update(edge, true));        // first LOW: starts the window
    TEST_ASSERT_TRUE(t.update(edge + 200, true));   // window satisfied: fires
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(edge, t.pendingSince,
        "pendingSince must still hold the first-LOW instant after firing - "
        "LATENCY_PROBE subtracts it and would otherwise report nonsense");
}

// Step 3 of the bench procedure in docs/circuit-rc.md sets the confirm window
// to 0. Even then the firmware needs the pin LOW on two consecutive polls, so
// it keeps one poll period of glitch rejection for free.
//
// This is the exact behaviour the old hand-written model got wrong - it fired
// on the very first LOW - and it was wrong on the one configuration the bench
// procedure uses. Pinned here now that both sides share one definition.
void test_zero_confirm_window_still_requires_two_polls(void) {
    TriggerInput t;
    t.confirm_us = 0; t.release_ms = 3;
    TEST_ASSERT_FALSE_MESSAGE(t.update(1000, true),
        "a zero window must not fire on the very first LOW poll");
    TEST_ASSERT_TRUE_MESSAGE(t.update(1010, true),
        "it must fire on the next poll that still reads LOW");
}

// =========================================================================
// 5b. held(): the same state read as a level
// =========================================================================

// GPIO7 holds the pitch shifter on rather than striking a sample, and reads
// held() instead of update()'s edge. It must be the SAME machine - a separate
// hold debounce is the second divergent implementation in the trigger path
// that this header exists to prevent - so what gets pinned here is that
// held() tracks exactly the interval between a confirmed strike and a believed
// release, with both of the shipped windows intact.
void test_held_is_true_from_the_confirmed_strike_to_the_believed_release(void) {
    TriggerInput t;
    t.confirm_us = 200; t.release_ms = 3;
    const uint32_t edge = 1000000;

    TEST_ASSERT_FALSE_MESSAGE(t.held(), "idle must not read as held");

    // A LOW shorter than the confirm window is a glitch and must not engage.
    t.update(edge, true);
    TEST_ASSERT_FALSE_MESSAGE(t.held(),
        "held() went true before the confirm window - the effect would engage "
        "on the same glitches the confirm window exists to reject");

    TEST_ASSERT_TRUE(t.update(edge + 200, true));
    TEST_ASSERT_TRUE_MESSAGE(t.held(), "held() must be true from the strike");

    // Contact bounce during the hold: a momentary HIGH must not release.
    t.update(edge + 1000, false);
    t.update(edge + 1200, true);
    TEST_ASSERT_TRUE_MESSAGE(t.held(),
        "a bounce back LOW restarted the release window, so held() must stay "
        "true - otherwise the effect stutters on every press");

    // Continuous HIGH, but not yet long enough.
    t.update(edge + 2000, false);
    t.update(edge + 2000 + 2500, false);
    TEST_ASSERT_TRUE_MESSAGE(t.held(), "released before release_ms elapsed");

    t.update(edge + 2000 + 3000, false);
    TEST_ASSERT_FALSE_MESSAGE(t.held(), "held() must clear once the release is believed");
}

// held() is derived from the state update() maintains, so it only moves when
// update() is called. Worth pinning because the failure is silent: a loop()
// that stops polling this pin leaves the effect latched in whatever state it
// was in, with no error anywhere.
void test_held_does_not_change_without_polling(void) {
    TriggerInput t;
    t.confirm_us = 200; t.release_ms = 3;
    t.update(1000, true);
    TEST_ASSERT_TRUE(t.update(1200, true));
    TEST_ASSERT_TRUE(t.held());
    // Time passes with the button released, but nobody asks.
    TEST_ASSERT_TRUE_MESSAGE(t.held(), "held() changed with no call to update()");
}

// =========================================================================
// 6. micros() wraparound
// =========================================================================

// TriggerInput stamps micros() and compares with unsigned differences, which
// the firmware comments claim survives the ~71 minute wrap. Nothing had ever
// verified that. The model now uses uint32_t timestamps for exactly this
// reason, so placing a trace across the boundary is a real test of the
// firmware's arithmetic rather than of 64-bit maths.
void test_behaviour_is_identical_across_micros_wraparound(void) {
    fx->trace = makeRoll("roll", 8, 80000, 2000, 40000, 6);
    Result base = run(fx->trace, shipped(), 50);

    // Bases chosen so the wrap lands at different points of the roll: during a
    // press, during a release window, and between strikes.
    for (uint32_t offset : {20000u, 45000u, 100000u, 250000u}) {
        uint32_t timeBase = (uint32_t)(0x100000000ULL - offset);
        Result r = run(fx->trace, shipped(), 50, 60000, timeBase);
        char msg[128];
        snprintf(msg, sizeof msg,
                 "wrap %u us into the trace changed the outcome - unsigned "
                 "difference arithmetic is not wrap-safe", offset);
        TEST_ASSERT_EQUAL_INT_MESSAGE(base.fires,    r.fires,    msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(base.missed,   r.missed,   msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(base.spurious, r.spurious, msg);
    }
}

// =========================================================================
// 7. RC filter model
// =========================================================================

void test_rc_derived_time_constants(void) {
    RCDerived d = rcDerive(fx->rc);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1.0, 21.4, d.tau_closed_us,
        "strike constant should be (R1||Rint||R2)*C ~ 21us");
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(20.0, 818.0, d.tau_open_us,
        "release constant should be (R1||Rint)*C ~ 818us");
    TEST_ASSERT_TRUE_MESSAGE(d.tau_open_us > d.tau_closed_us * 20,
        "the asymmetry between the two constants is the whole point of the "
        "circuit - fast on the strike, filtered on the release");
}

void test_rc_collapses_bounce_to_a_single_edge(void) {
    fx->trace = pressTrace(2000, 150000, 6);
    TEST_ASSERT_EQUAL_INT_MESSAGE(14, (int)fx->trace.edges.size(),
        "raw trace should present 14 transitions");
    Trace filtered = applyRC(fx->trace, fx->rc);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)filtered.edges.size(),
        "through the RC the pin should see one fall and one rise");
}

// The filter stops absorbing bounce once a contact-open segment outlasts the
// charge constant. Documented ceiling, not a surprise.
void test_rc_stops_filtering_beyond_its_time_constant(void) {
    fx->trace = pressTrace(3000 * 6, 150000, 6); // 3ms segments
    Trace filtered = applyRC(fx->trace, fx->rc);
    TEST_ASSERT_TRUE_MESSAGE((int)filtered.edges.size() > 2,
        "segments longer than the charge constant must reappear as edges");
}

// The RC does not merely fail to reject a contact-type glitch - it makes one
// WORSE. Discharge is fast by design (that is what keeps the strike instant)
// but recharge runs through the 10k, so every glitch comes out stretched by a
// fixed ~1.08ms: the time to climb back across VIH. A 50us contact blip
// becomes over a millisecond of LOW at the pin, 21x longer.
//
// Measured, and pinned here because it inverts the obvious intuition and was
// stated the other way round in the docs before it was simulated.
void test_rc_stretches_contact_glitches(void) {
    for (uint64_t w : {50ULL, 200ULL, 1000ULL}) {
        fx->trace = makeGlitch("glitch", w);
        Trace filtered = applyRC(fx->trace, fx->rc);
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, (int)filtered.edges.size(),
            "a contact glitch still crosses the threshold through the RC");
        uint64_t lowDur = filtered.edges[1].t_us - filtered.edges[0].t_us;
        char msg[128];
        snprintf(msg, sizeof msg,
                 "%llu us glitch should emerge ~1.08ms longer, measured %llu us",
                 (unsigned long long)w, (unsigned long long)lowDur);
        TEST_ASSERT_TRUE_MESSAGE(lowDur > w + 900 && lowDur < w + 1300, msg);
    }
}

// The practical consequence: with the RC fitted, the shipped 200us confirm
// window no longer rejects a contact glitch, because the RC handed it a
// millisecond of LOW to satisfy the window with. Rejecting it post-RC costs a
// confirm window longer than the stretch.
void test_rc_defeats_the_shipped_confirm_window(void) {
    fx->trace = makeGlitch("glitch", 50);
    Trace filtered = applyRC(fx->trace, fx->rc);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, run(fx->trace, shipped(), 50).fires,
        "without the RC, 200us rejects a 50us glitch");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, run(filtered, shipped(), 50).fires,
        "with the RC, the same glitch gets through - stretched past the window");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, run(filtered, trig(1500, 3), 50).fires,
        "a window longer than the stretch rejects it again, at 1.5ms of latency");
}

// =========================================================================

int main(int, char**) {
    UNITY_BEGIN();

    // 1. fidelity first - everything below is meaningless if these fail
    RUN_TEST(test_harness_reproduces_historical_machine_gun);
    RUN_TEST(test_machine_gun_count_scales_with_poll_rate);

    // 2. shipped behaviour
    RUN_TEST(test_clean_press_fires_once);
    RUN_TEST(test_bouncy_press_fires_once);
    RUN_TEST(test_worn_switch_fires_once);
    RUN_TEST(test_held_button_fires_once);
    RUN_TEST(test_fast_roll_fires_once_per_strike);
    RUN_TEST(test_emi_glitch_is_rejected);
    RUN_TEST(test_behaviour_is_independent_of_poll_rate);

    // 3. rejected designs
    RUN_TEST(test_rejected_level_rearm_machine_guns);
    RUN_TEST(test_rejected_edge_rearm_latches_on_short_contact);

    // 4. timing contracts
    RUN_TEST(test_confirm_window_sets_trigger_latency);
    RUN_TEST(test_confirm_window_wider_than_a_bounce_segment_costs_the_whole_bounce);
    RUN_TEST(test_confirm_window_is_a_threshold);
    RUN_TEST(test_release_window_sets_the_retrigger_floor);

    // 5. internal contracts
    RUN_TEST(test_pendingSince_survives_the_strike_for_the_latency_probe);
    RUN_TEST(test_zero_confirm_window_still_requires_two_polls);

    RUN_TEST(test_held_is_true_from_the_confirmed_strike_to_the_believed_release);
    RUN_TEST(test_held_does_not_change_without_polling);

    // 6. wraparound
    RUN_TEST(test_behaviour_is_identical_across_micros_wraparound);

    // 7. RC model
    RUN_TEST(test_rc_derived_time_constants);
    RUN_TEST(test_rc_collapses_bounce_to_a_single_edge);
    RUN_TEST(test_rc_stops_filtering_beyond_its_time_constant);
    RUN_TEST(test_rc_stretches_contact_glitches);
    RUN_TEST(test_rc_defeats_the_shipped_confirm_window);

    int failures = UNITY_END();

    // Belt and braces: tearDown() frees the fixture after every test, but if a
    // test aborted early Unity still calls tearDown, so fx must be null here.
    // Assert rather than free, so a leak is reported instead of hidden.
    if (fx != nullptr) {
        printf("LEAK: fixture still allocated after the suite\n");
        delete fx;
        fx = nullptr;
        return failures + 1;
    }
    return failures;
}
