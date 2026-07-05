# EX Synth

A compact **dual-core RP2040 (Raspberry Pi Pico)** monophonic synthesizer and
16-step sequencer with an SSD1306 OLED UI, capacitive/touch controls, an 8-key
keyboard, per-step storage in flash-emulated EEPROM, and a real-time I²S audio
engine.

> Firmware: [`final_ex_ver.ino`](final_ex_ver.ino) · Target core: [arduino-pico](https://github.com/earlephilhower/arduino-pico) (earlephilhower)

---

## Features

- **Two cores, clean split** — core 0 runs the UI, input scan and sequencer; core 1 runs the audio engine (I²S, 44.1 kHz, 16-bit stereo).
- **Synth voice** — two detunable oscillators + a sub-oscillator, a morphing waveform (SINE → TRIANGLE → SAWTOOTH → SQUARE), a one-pole tone/filter, AD envelope, glide/portamento, and an LFO.
- **11 editable parameters** — `WAVE, TONE, GLIDE, DETUNE, SWING, SUB-OSC, ATTACK, DECAY, LFO RATE, LFO DEPTH, CLOCK DIV`.
- **16-step sequencer** — record from the keyboard, play back at 120 BPM base × `CLOCK DIV`, with **swing**, **mute modes** (Off / 2nd / 3rd / Random) and a **roller/stutter** effect.
- **7 pattern slots** in EEPROM — save, load, and auto-load slot 1 on boot.
- **External sync input** to advance the sequencer from an outside clock.

## Hardware

- An **RP2040** board (Raspberry Pi Pico or compatible).
- **SSD1306 128×64 OLED**, I²C, on the second I²C bus (`Wire1`).
- **I²S DAC** (e.g. PCM5102 / MAX98357A).
- **8 momentary keys**, **8 LEDs**, **4 touch/capacitive buttons**, **4 sensor inputs**, and a sync input.

### Pinout (as coded)

| Function | GPIO |
|---|---|
| I²S BCLK / LRCLK / DATA | `0` / `1` (auto) / `2` |
| Sensors (Oct−, Oct+, Play/Pause, Record) | `3`, `4`, `5`, `22` |
| Keyboard keys 1–8 | `6, 7, 8, 9, 10, 11, 12, 13` |
| OLED I²C (`Wire1` SDA / SCL) | `14` / `15` |
| LEDs 1–8 | `16, 17, 18, 19, 20, 21, 22, 23` |
| Touch (SELECT, +, −, FX) | `26, 27, 28, 29` |
| Sync in | `20` |

> ⚠️ **Pin conflicts — read before wiring.** As written, **GP20** is assigned to both
> `LED 5` and the sync input, and **GP22** to both `LED 7` and the record sensor. On a
> stock Pico, **GP23** (LED 8) and **GP29** (touch FX) are reserved for the SMPS and VSYS
> sensing. See **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)** (#1, #2, #6) before building the hardware.

## Controls

| Control | Action |
|---|---|
| **SELECT** (touch) | Open the parameter pop-up / cycle through the 11 parameters |
| **+ / −** (touch) | Change the current parameter (hold to auto-repeat) |
| **FX** (touch) | Tap: cycle mute mode (Off → 2nd → 3rd → Rnd). Hold: roller/stutter while held |
| **Keys 1–7** | Play notes DO–SI |
| **Key 8** | REST ("NO") |
| **Sensor 1 / 2** | Octave down / up (1–7) |
| **Sensor 3** | Play / pause the sequencer |
| **Sensor 4** | Toggle record mode |
| **Hold REST + key** | Save current pattern to slot 1–7 |

## Build & flash

There is no build system committed (the sketch targets the Arduino IDE). Two options:

### Arduino IDE
1. Install the **arduino-pico** core (Boards Manager → "Raspberry Pi Pico/RP2040").
2. Install libraries: **Adafruit GFX**, **Adafruit SSD1306**.
3. Arduino needs the sketch folder name to match the `.ino` — put `final_ex_ver.ino`
   inside a folder named `final_ex_ver/`, or let the IDE offer to create it.
4. Select your RP2040 board and **Upload**.

### arduino-cli
```bash
arduino-cli core install rp2040:rp2040
arduino-cli lib install "Adafruit GFX Library" "Adafruit SSD1306"

# arduino-cli also expects a folder named like the sketch:
mkdir -p final_ex_ver && cp final_ex_ver.ino final_ex_ver/
arduino-cli compile --fqbn rp2040:rp2040:rpipico final_ex_ver
arduino-cli upload  --fqbn rp2040:rp2040:rpipico -p /dev/ttyACM0 final_ex_ver
```

## Known issues

This repository ships with a full code review. Several **functional bugs** (pin
conflicts, a shared-pin control collision, sync runaway, and a swing/roller
underflow) are documented — see **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)**.

## Project layout

```
.
├── final_ex_ver.ino   # firmware (single sketch, two cores)
├── README.md
├── KNOWN_ISSUES.md    # code review / bug list
├── LICENSE
└── .gitignore
```

## License

Released under the [MIT License](LICENSE). Update the copyright holder in `LICENSE`
to your name before publishing.
