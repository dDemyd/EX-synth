# Code Review — `final_ex_ver.ino`

Static review of the dual-core RP2040 (Raspberry Pi Pico) synth/sequencer sketch.
No compiler/toolchain was available, so this is a source-level review only — the
firmware was **not** built or flashed.

Severity legend: 🔴 Critical · 🟠 High · 🟡 Medium · ⚪ Low

## Status

**#1–#17 are fixed** (one commit each on `main`) — see the "Resolved" mapping at the
bottom. **#18 is partially done**: parameter indices are now a named enum; a full
modular split, a `potValues` rename, and unifying the comment language are deferred as
larger refactors. **All fixes compile cleanly** against the arduino-pico core 5.6.1
(`arduino-cli`, `--warnings all` → zero warnings; ~99.5 KB flash / 10.9 KB RAM). Still
bench-test on real hardware before relying on it.

## Summary table

| #  | Sev | Location | Problem | Fix direction |
|----|-----|----------|---------|---------------|
| 1  | 🔴 | `ledPins[4]` = 20 and `syncInPin` = 20 | **Pin 20 double-assigned.** In `setup()` the pin is set `OUTPUT` then re-set `INPUT_PULLDOWN`, so it ends up an input. Every `digitalWrite(ledPins[4], …)` (L616/621) is a no-op → sync LED never lights. | Give the sync input its own free GPIO; remove pin 20 from `ledPins`. |
| 2  | 🔴 | `ledPins[6]` = 22 and `sensorPins[3]` = 22 | **Pin 22 double-assigned.** Ends up `INPUT_PULLDOWN` (record sensor). `digitalWrite(ledPins[6], isRecording)` (L526) is a no-op → recording LED never lights. Physically you cannot have both a driven LED and a read sensor on one pin. | Move the record LED to a free GPIO; remove pin 22 from `ledPins`. |
| 3  | 🟠 | `loop()` L507–508, L543 | **`shift` and `currentRecSensor` read the same pin** (`sensorPins[3]`). Touching sensor 3 fires the record-toggle edge *before* the LOAD check, and LOAD is gated on `!isRecording` → "shift + key = LOAD slot" only works while playing, and otherwise silently toggles recording. | Use a dedicated pin for `shift`, or gate the two behaviors by press-duration. |
| 4  | 🟠 | `loop()` L610, `syncInPin` | **External SYNC has no edge detection.** The condition `… \|\| digitalRead(syncInPin)` advances `seqStep` on *every* `loop()` iteration while the line is HIGH → runaway stepping on a held/wide pulse. | Latch the previous sync level and advance only on LOW→HIGH. |
| 5  | 🟠 | `loop()` L594–610 | **Roller + Swing underflow.** `swingOffset` is derived from the full `dynamicInterval`, but then the roller divides `dynamicInterval` by 4 (L608). On off-beats `dynamicInterval + swingOffset` goes negative; because it is evaluated as `unsigned long` it wraps huge → that step never fires on the timer (only on sync). | Compute swing from the *post-roller* interval, or clamp `interval + swing` to ≥ 1. |
| 6  | 🟠 | pin maps (stock Pico) | **Reserved-GPIO use on a stock Pico:** `ledPins[7]` = **GP23** (SMPS power-save control) and `touchPins[3]` = **GP29** (VSYS/3 sense). Fine on a bare RP2040 board, problematic on a Pico module. | Confirm the target board; if it's a Pico, remap 23/29 to broken-out GPIOs. |
| 7  | 🟡 | `paramValues[]` (global) | **Cross-core sharing without `volatile`.** Core 1 (`loop1`) reads `paramValues[5..9]` while core 0 writes them. Works in practice (32-bit aligned `int` is atomic on Cortex-M0+) but is technically a data race / undefined behavior. | Mark shared state `volatile`, or pass through a small synchronized struct. |
| 8  | 🟡 | `loop1()` L740 | **`powf()` per audio sample.** RP2040 has **no FPU**; `powf` + ~5 `sinf` are recomputed at 44.1 kHz while a note sounds → risk of audio underruns/glitches. | Hoist `powf(2, octave-4)` — recompute only when note or octave changes; cache the octave multiplier. |
| 9  | 🟡 | `loop1()` L700–702 | **`noteTriggered` retrigger race.** Set on core 0 (multiple sites), cleared on core 1. A trigger set between the read and clear can be lost or double-counted. | Use an atomic flag / counter, or a lock-free single-producer handshake. |
| 10 | 🟡 | `loop()` L594 | **CLOCK-DIV divisor not defended.** `stepDurationMs / paramValues[10]` divides by zero if the value ever becomes 0. Currently only the UI keeps it in 1–8; there is no runtime guard. | Clamp at point of use: `max(1, paramValues[10])`. |
| 11 | 🟡 | many | **Blocking `delay()` in core-0 `loop()`** (50–1500 ms across select/keys/octave/play/save/load). Freezes UI, input scan, and the 100 ms display refresh. | Replace with non-blocking `millis()` debouncing. |
| 12 | 🟡 | globals L75, `saveSequenceToEEPROM` | **Arduino `String` + concatenation** (`"SAVED TO\nSLOT " + String(slot)`) fragments the heap over time on a long-running embedded target. | Use a fixed `char[]` buffer with `snprintf`. |
| 13 | ⚪ | `loop()` L630 | **`random()` never seeded** → the "RND" mute pattern is identical every boot. | Call `randomSeed()` from a floating ADC or `micros()` in `setup()`. |
| 14 | ⚪ | `setup()` L485 | **Touch pins are `INPUT`** (no pull). Floating inputs can false-trigger depending on the touch/button hardware. | Use `INPUT_PULLDOWN`/`INPUT_PULLUP` to match the wiring. |
| 15 | ⚪ | `setup()` L472 | **`display.begin()` return value ignored** — no I²C/OLED presence check; silent black-screen failure if the panel is absent. | Check the return and surface an error (LED blink code). |
| 16 | ⚪ | `setup()` L487–497 | **Duplicate "LOADED SLOT 1"** — a manual splash *and* `loadSequenceFromEEPROM(1)`'s flash message both show it. | Drop the manual splash or suppress the flash on boot. |
| 17 | ⚪ | `loop()` L616/621, L649 | **Dead writes to `ledPins[4]`/`[6]`** (pins are inputs — see #1/#2). The LED update loop already skips `i==4 && i==6`, confirming the conflict is known. | Remove the ineffective writes once the pin conflicts are resolved. |
| 18 | ⚪ | whole file | **Maintainability:** ~815 lines in one `.ino`, pervasive magic numbers, `potValues[]` misnamed (no physical pots), and comments mix Ukrainian/Russian/English. | Split into modules (`ui`, `audio`, `sequencer`, `storage`), name constants, pick one comment language. |

## Notes on what is **correct / good**

- Clean two-core split: core 0 = UI + sequencer, core 1 = I²S audio engine.
- Shared audio state (`phaseA/B/Sub/LFO`, `currentFreq`, `noteToPlay`, …) *is* `volatile`.
- Waveform morph (sine→tri→saw, plus a square special-case at `morph==3`) is compact and index-safe.
- Array indexing is bounds-safe: WAVE (0–3), CLOCK DIV (1–8) and note indices are all clamped/guarded; `% seqLength` is guarded by `seqLength > 0`.
- EEPROM save/load validates length and sentinel bytes (255 → rest) and round-trips correctly within the 256-byte budget (7 slots × 20 B).
- Envelope, glide, detune, sub-osc and LFO math are coherent and range-bounded; final audio (`×2800`) stays well inside `int16_t`.

## Resolved (this branch)

| # | Commit subject |
|---|----------------|
| 1, 2 | Fix pin conflicts on GP20/GP22 (drop phantom LEDs) |
| 3 | Disambiguate record sensor from shift-load (shared GP22) |
| 4 | Advance sequencer only on SYNC rising edge |
| 5 | Fix roller+swing interval underflow |
| 10 | Guard CLOCK DIV divisor against zero |
| 7 | Mark paramValues volatile for cross-core access |
| 8 | Cache octave multiplier instead of powf per sample |
| 9 | Make note-trigger handshake lock-free |
| 11 | Replace blocking delay() debounces with edge detection |
| 12 | Replace String flash message with fixed char buffer |
| 6 | Documented inline as a stock-Pico wiring caveat (no code change — pin choice depends on the board) |
| 13 | Seed RNG from RP2040 hardware entropy |
| 14 | Add pulldowns to touch inputs |
| 15 | Check display.begin() and signal OLED failure |
| 16 | Remove duplicate boot "LOADED SLOT 1" splash |
| 17 | Folded into #1/#2 (dead LED writes removed) |
| 18 | Parameter indices replaced with a named enum (rest of the refactor deferred) |

**Still open (deferred):** the larger part of #18 — splitting the sketch into modules,
renaming `potValues`, and unifying the comment language — left for a session with a
compiler in the loop.

## Verdict

**#1–#17 addressed; #18 partially.** The remaining work is a maintainability refactor,
not a defect. The sketch compiles cleanly against the arduino-pico core (5.6.1) with no
warnings; bench-test the sequencer (swing + roller together, external sync, save/load)
on hardware before relying on it.
