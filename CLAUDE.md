# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a button-triggered one-shot sampler with live effects: ESP32-S3 N16R8 + PCM5102A I2S DAC + microSD (SPI), built on the [M16](https://github.com/algomusic/M16) audio library under Arduino/PlatformIO. Three sample triggers (GPIO4/5/6), one held-button pitch shifter (GPIO7), samples resident in PSRAM, no OS scheduler in the audio path beyond M16's own I2S task.

Where it is going: the board is meant to take a **live audio-in stream**, process it, and emit a live audio-out stream. The sampler is the current load, not the destination. That is why the audio engine renders **blocks of normalised float** rather than single fixed-point frames — see "Real-time design" below. Any new DSP belongs in that chain, in those units, and must not assume the host block size.

## Build / flash / debug

```bash
pio run                      # build default env (sampler firmware)
pio run -t upload            # flash
pio device monitor           # serial log, 115200 baud
pio run -e sd_test -t upload # flash the standalone SD diagnostic instead
```

PlatformIO lives in a project-local venv (gitignored), so the commands above are `.venv/bin/pio ...` unless you activate it. Create it with `python3 -m venv .venv && .venv/bin/pip install platformio` — Debian's system Python is PEP 668 externally-managed, so a bare `pip install` is refused.

```bash
pio test -e native            # host unit suite, no board
```

Trigger logic, mixer arithmetic and the DSP are verified on the host; everything else — the DAC, SD, PSRAM residency, the DMA ring depth, how it actually *sounds* — is verified only by flashing. Neither substitutes for the other: a green build and a green suite say nothing about whether PSRAM initialised.

**Every header in `src/` that ends in `.h` is compiled into the firmware AND into the tests.** That is the single most load-bearing convention in this repo. It exists because `TriggerInput` once lived in `main.cpp` with a hand-written copy in the simulator, and the two drifted: the copy grew a `confirm_us == 0` fast path the firmware never had, on the exact configuration the bench procedure in [docs/circuit-rc.md](docs/circuit-rc.md) uses. Tests were green while measuring something the board would not do. Keep new DSP header-only and Arduino-free for the same reason — divergence should be impossible by construction, not by vigilance.

- [src/trigger_input.h](src/trigger_input.h) — `TriggerInput`, the state machine every button runs. `update()` returns the strike edge; `held()` reads the **same state** as a level, for GPIO7's hold-to-engage. One machine, two readings — do not add a second debounce for the hold case.
- [src/mixer.h](src/mixer.h) — the fixed-point gain and soft-knee limiter. **No longer in the audio path**, kept as the *reference* the float limiter is tested against: it is the curve that was judged by ear on hardware. Do not delete it as dead code.
- [src/audio_block.h](src/audio_block.h) — `AudioBlock`, the float units, and the conversions at the engine's edges. The float soft knee lives here.
- [src/pitch_shift.h](src/pitch_shift.h) — `PitchShift`, the first stream processor. Two windowed taps on a delay line.
- [sim/trigger_model.h](sim/trigger_model.h) — header-only scaffolding: pin-trace generator, RC filter model, scoring runner, plus models of the two **rejected** trigger designs. Those are copies on purpose; they no longer exist in the firmware and exist only to keep failing in the documented way. **Timestamps are `uint32_t`**, matching `micros()`, so unsigned-difference arithmetic wraps in simulation exactly as on the board; `run()` takes a time base so the wrap boundary itself is testable.
- [sim/trigger_sim.cpp](sim/trigger_sim.cpp) — the sweep/report CLI (`--sweep=all`).
- [test/](test/) — four Unity suites: `test_trigger`, `test_mixer`, `test_audio_block`, `test_pitch_shift`. 71 cases.

The two windows are instance members with defaults from `TRIG_CONFIRM_US` / `TRIG_RELEASE_MS` (both `#ifndef`-guarded) rather than macros read inline, so sweeps and tests can vary them per instance. Costs 8 bytes per button.

Always pass `-e native`. A bare `pio test` also targets the ESP32 environments, which means trying to upload.

Each suite is organised by *reason*, not code shape. Some tests are load-bearing beyond what they assert, and are named so that breaking them explains itself:

- `test_harness_reproduces_historical_machine_gun` pins the trigger model to the originally measured 112/3 fire counts — if it fails, the model drifted and no other assertion in that file means anything.
- `test_rejected_level_rearm_machine_guns` / `test_rejected_edge_rearm_latches_on_short_contact` — reintroducing either rejected debounce design fails a test named after how it failed.
- `test_float_knee_matches_the_integer_reference` sweeps the float limiter against `mixer.h` and asserts they agree within 1 LSB. This is what connects the current engine to the sound that was actually approved on hardware.
- `test_block_size_does_not_change_the_output` feeds the pitch shifter the same signal in ragged chunks (1, 7, 32, 3, 64, 13, 128, 5 frames) and demands bit-identical output to one 16384-frame call. **This is the test that makes "audio-in will drop in without a rewrite" a fact rather than a hope.** Any new processor needs its own.
- `test_bypass_is_bit_exact` — a processor sitting permanently in the chain must be free when off.

Two dependencies are pinned to exact revisions, for the same reason:

`platform` is pinned to an exact **pioarduino** release, not to `espressif32`. M16 needs Arduino core 3.x (ESP-IDF 5) for `driver/i2s_std.h`, and the official PlatformIO platform does not ship it — as of 7.0.1 it still pins core 2.0.17, and the build fails outright. pioarduino is the community fork that tracks core 3.x. Do not "modernise" this back to `platform = espressif32`.

**M16 is pinned to an exact commit**, and this is not hypothetical caution: while this project was being built on top of it, M16 rewrote its output transport. `i2s_write_samples()` went from one `i2s_channel_write()` per frame to a `[[deprecated]]` shim over `audioBlockWrite()`, which batches 32 frames per DMA write — roughly 19% of one core's time, arriving with no code change on our side, and invalidating the framing of a whole bench session. The same update added a dual-core block-split mode in which the deprecated shim **returns false and writes nothing**. A floating audio dependency makes every "validated on hardware" note in `docs/` expire silently. Call `audioBlockWrite()`, never the shim.

The three device environments share `src/` and are separated by `build_src_filter` because each has its own `setup()`/`loop()` pair. Adding a fourth sketch file means adding a filter exclusion to **every** env, not just the new one.

- `esp32-s3-devkitc-1` (default) — [src/main.cpp](src/main.cpp), the sampler. Uses SdFat via M16.
- `sd_test` — [src/test_sd.cpp](src/test_sd.cpp), the stock Arduino `SD.h` example, kept to isolate SD hardware faults from SdFat/M16-specific ones.
- `i2s_bench` — [src/test_i2s_bench.cpp](src/test_i2s_bench.cpp), the I2S transport measurement bench. No M16, no SdFat: sweeps feed models (ISR vs task, block vs per-frame) against nine DMA ring depths and reports CPU, jitter and underruns. Results in [docs/bancada.md](docs/bancada.md) section 9.

## Real-time design (the load-bearing decisions)

**Samples are fully resident in RAM before any audio plays.** `Wav::load*()` runs once in `setup()` and prefers PSRAM (8MB on this board). The SD card is never touched after boot — it can be physically removed. Any change that reads from SD, allocates, or blocks during playback breaks the latency guarantee this project exists for. Streaming-from-SD would reintroduce FAT seek jitter that PSRAM residency was chosen to eliminate.

**`audioUpdate()` is a block renderer, not a per-sample callback.** It is invoked from M16's I2S feed task, not from `loop()`, and each call produces `AUDIO_BLOCK_FRAMES` (32, = 0.73 ms) of normalised float. It must stay allocation-free, lock-free, and free of `Serial`/SD/`delay()`. It ends in a drain loop of `audioBlockWrite()`, which is the backpressure point.

Its shape is load-bearing in three ways:

1. **Voice-outer, frame-inner.** Each voice walks 32 consecutive frames of its own PSRAM buffer before the next starts. The old frame-outer loop interleaved four PSRAM streams and bounced the cache between them every frame.
2. **Voices sum; they do not replace.** Pads play concurrently — never rewrite this to output a single "last triggered" voice.
3. **The processing chain sits between the voices and the drain.** Every processor has the shape `process(AudioBlock&)`: a whole block of float, with history allowed across calls. This is the window that per-sample rendering could not offer, and the slot audio-in will be read into at the top of. M16's own 32-frame batching does **not** substitute for this — it buffers our *output*, after we have already had to decide what it is.

Float (±1.0 = full scale) is for interoperability, not precision: fixed point was fine for summing four samples, but a chain in conventional units can take DSP from anywhere unscaled. It is near-free in a task thanks to the S3's FPU, and **unavailable in an interrupt handler** — which is why the engine is a task, and the trade recorded in [docs/bancada.md](docs/bancada.md) section 9.

`setIsDualCore(false)` is called explicitly before `audioStart()`. It is already M16's ESP32 default, but it is the one default whose flipping would break this engine twice over: one static block buffer rendered by two tasks, into a finaliser expecting two per-core partitions this engine does not produce. Dual-core rendering here would mean partitioning voices with `audioPartitionOffset()`/`audioPartitionStride()` — a real option, but a design, not a flag.

**Two deliberately different debounce strategies**, and this asymmetry is intentional, not an oversight:

All four buttons run one shared `TriggerInput` state machine. Fires on the **raw** pin read after `TRIG_CONFIRM_US` (200 µs, the only defense against phantom hits), then disarms until the raw pin reads HIGH continuously for `TRIG_RELEASE_MS` (3 ms). No debounce library is involved.

GPIO7 holds an effect on instead of striking a sample, and reads `held()` — the same `armed` state as a level rather than as an edge. Rearming and releasing are the same question, so a hold needs exactly the conditioning a strike needs. `held()` only moves when `update()` is called, so the pin must still be polled every pass; a loop that stops polling leaves the effect latched with no error anywhere.

The sharing is the point. GPIO4 used to have its own wait-for-stable debounce at 25 ms; having two divergent implementations in one trigger path is what let the pad retrigger bug survive review. Do not reintroduce a second one.

  Both guards read the pin directly, and that is load-bearing. They were originally derived from Bounce2's debounced state, which lags the pin by its interval — rearming on the debounced *level* re-fired ~112 times per strike, and rearming on the debounced *edge* (`rose()`) latched the pad off permanently whenever contact time was under `bounce + interval`. Any rearm condition read from a filtered state couples correctness to the filter's constant. Do not reintroduce one. [sim/trigger_sim.cpp](sim/trigger_sim.cpp) measures all of this on the host.

  `PAD_CONFIRM_US` must stay well under one contact-bounce segment (~300 µs). Above that it cannot be satisfied until the whole bounce settles and the trigger latency becomes the bounce duration: 200 µs costs 0.2 ms, 1 ms costs 3 ms.

The pad scheme is the reference pattern for any new trigger input. GPIO4 is the legacy path.

**The I2S DMA ring is the latency budget.** Ring depth ≈ trigger-to-sound time. M16's default of 4 × 512 frames is 46 ms, sized to survive a host flash-sector erase — a stall that cannot happen here, since samples are PSRAM-resident, SD is untouched after boot, and nothing writes flash at runtime. [src/main.cpp](src/main.cpp) overrides it to 4 × 64 = 5.8 ms **before** `#include "M16.h"`, guarded by a `static_assert`: M16 uses `#ifndef`, so moving the defines below the include silently restores 46 ms with no other symptom.

A second `static_assert` ties `AUDIO_BLOCK_FRAMES` to the ring: rendering one block may not consume more than a quarter of it, because the ring drains while we render. Both constants are only visible together in `main.cpp`, which is why that assert lives there rather than in the header.

5.8 ms was validated by ear on 13/08/2026 against a four-voice roll, the heaviest render path. **That validation is provisional**: it was measured against an M16 checkout that no longer exists, from before the transport changed, and has not been re-confirmed against the current pin. Bursts drain a ring faster rather than slower, so it is very likely still fine — but "likely" is the honest word. If the ladder ever needs walking again, test on a four-voice roll with 16-bit samples that neither clip nor carry their own noise floor; both of those confounds were mistaken for underruns once already.

Serial logging is split: `DEBUG_BOOT` (setup only, free, leave on) and `DEBUG_TRIGGERS` (one line per strike, from inside the polling loop, **off by default**). Errors are unconditional regardless — a missing PSRAM or a failed SD mount prints and names its own likely cause, because M16's `psramAllocSafe()` has no internal-RAM fallback and a PSRAM misconfiguration would otherwise surface only as `WAV: failed to load`. USB CDC blocks when the host is not draining, which puts an unbounded stall in the trigger path — never leave it on while judging latency, and don't add unconditional Serial output to `loop()`.

## The pitch shifter (and what not to "fix" about it)

[src/pitch_shift.h](src/pitch_shift.h). Hold GPIO7, the whole mix drops an octave (`PITCH_SHIFT_SEMITONES`, default −12); release and it returns. Two windowed read taps on a delay line, read at `ratio` while written at 1, crossfaded **equal-power** so each tap's gain is exactly zero where it has to jump.

Two artefacts are inherent to time-domain shifting, not implementation slips, and both scale with `PITCH_BUF_FRAMES` in opposite directions:

- **Warble**, ~43 Hz at an octave. The crossfade cycle repeats every `span/|1−ratio|` frames. Percussion largely masks it; a sustained tonal note will not.
- **Latency**, ~11.6 ms. The taps read a mean of half the buffer behind the writer. Pitch shifting needs a window of signal to resample and the window *is* the latency. `latencyFrames()` reports it rather than leaving it to be discovered.

A bigger buffer means slower, less buzzy warble and more latency. That is the only real knob.

The alternative is a phase vocoder (FFT → modify phases → inverse FFT), which trades the warble for transient smearing, needs a window at least as long as this buffer anyway so it is **not** lower latency, and costs vastly more CPU. For a percussion instrument that is the wrong trade. Not ruled out forever; ruled out for this.

Three details that look like candidates for simplification and are not:

- **The processor runs unconditionally**, feeding its delay line even while disengaged. Otherwise pressing the button starts the effect from silence. Bypass is bit-exact, which is what makes leaving it in the chain free.
- **`setEngaged()` only records a request**; the audio task applies it at a block boundary. Acting directly would have `loop()` writing the crossfade phase while the audio task reads it — a real cross-core race on the state the whole algorithm is built on.
- **Equal-power between the taps, equal-amplitude for dry/wet.** The taps read the signal at different times and are effectively uncorrelated, so power adds. Dry and wet are the *same* signal, maximally correlated, so amplitude adds. Making both the same is wrong in one of the two places.

## Compile-time sample source

`USE_SD_SAMPLER` ([src/main.cpp:98](src/main.cpp:98)) selects where audio buffers come from, and it gates more than it looks like:

- `1` (default): GPIO4 plays `FILE_INDEX` (first `.wav` the card enumerates); GPIO5/6 play `/1.wav` and `/2.wav`. All the pad code — pins, `Wav`/`Samp` arrays, the trigger state arrays, and the pad loop in `audioUpdate()` — lives inside `#if USE_SD_SAMPLER`.
- `0`: single voice from an array in flash (`.rodata`), no SD card, **no pads at all**. Changing the embedded sample means editing both the `#include` and the `samp.setTable()` call.

GPIO7's pitch shifter is outside that switch and works in both modes, because it processes the mix rather than a sample of its own. `/3.wav` is no longer loaded in either mode — that pad became the effect pin.

`Samp::setTable()` takes the source sample rate and resamples internally, so a file's native rate does not require reconfiguring the I2S clock.

## Embedding a sample in flash

```bash
python3 tools/wav_to_header.py <input.wav> src/<name>_data.h <ARRAY_NAME>
```

[tools/wav_to_header.py](tools/wav_to_header.py) downconverts 8/24/32-bit PCM to 16-bit using the same arithmetic as M16's `Wav::readFramesFast()`, so a sample is bit-identical whether it plays from SD or from flash. If M16's conversion changes, this script has to follow it.

Only `src/kick_normalizado_data.h` is included by the firmware. `src/kick_data.h` (from the un-normalized `kick.wav`) is a leftover and is referenced nowhere.

## Pin constraints — N16R8 specific

Full wiring rationale, including the PCM5102A control pins (`SCK`→GND for internal clock, `XSMT`→3V3 or output stays muted) and the common-ground requirement, is in [docs/circuit.md](docs/circuit.md). Read it before changing any pin assignment.

The trap: **GPIO 33–37 are consumed by the Octal PSRAM** on R8 modules. Generic ESP32-S3 pinout guides list them as free and suggest them for VSPI; using them here bricks PSRAM. SD therefore sits on HSPI (10–13). Also unusable: 0/3/45/46 (strapping), 26–32 (flash), 43/44 (UART0), 19/20 (native USB).

`board = esp32-s3-devkitc1-n16r8`, **not** the generic `esp32-s3-devkitc-1` — that one is the N8 (8MB flash, no PSRAM). The N16R8 definition carries `default_16MB.csv`, `memory_type = qio_opi`, `psram_type = opi`, `-DBOARD_HAS_PSRAM` and the correct 16MB size limits together. This used to be a stack of `board_build`/`board_upload` overrides on top of the generic board; they mostly worked, but left `maximum_size` at the N8's 8MB while writing a partition table running to 0xff0000, so firmware size was checked against the wrong ceiling.

Two `build_flags` are load-bearing, both about serial. `-DARDUINO_USB_CDC_ON_BOOT=1` because the board has no UART bridge chip — without it Serial goes to UART0 (GPIO 43/44), wired to nothing, and the monitor stays empty while the firmware runs fine. `-DARDUINO_USB_MODE=1` because the generic board definition provided it via `extra_flags` and the n16r8 one does not; drop it and serial goes quiet the same way.

## Conventions

Prose docs (`README.md`, `docs/circuit.md`) are written in Brazilian Portuguese; source comments are in English. Match whichever file you are editing. Comments in `main.cpp` explain *why* a timing or memory choice was made rather than what a line does — preserve that when refactoring.
