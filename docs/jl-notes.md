# JL Firmware Notes

Working notes for the JL build of the Polyend Tracker CE firmware: what has been added, how it is built,
and what is still open. Paste or point a new session at this file to pick up where the last one left off.

Repository: `jeremylstaples13-cloud/tracker-ce` (fork of `polyend/tracker-ce`). Only **Tracker+** is built.

## Branches

| Branch | Contents |
| :--- | :--- |
| `feature/stng-synth` | **Latest — work here.** Step-sequencer mode + STNG synth. Based on `feature/step-seq-mode`. |
| `feature/step-seq-mode` | Step-sequencer mode only. |

## Building

- **GitHub Actions**: [`.github/workflows/build-step-seq-plus.yml`](../.github/workflows/build-step-seq-plus.yml) builds
  Tracker+ on every push to either branch with `make plus CUSTOM_DEFINES="-DSTEP_SEQ_MODE"` and uploads
  `TrackerPlus_JL_1.0.0.ptf` as a run artifact (Actions tab → latest run → Artifacts).
- **Edition name** is `JL` (`EDITION_NAME` in [`version.mk`](../version.mk)): Config → Firmware shows "1.0.0 JL" and the
  file is `TrackerPlus_JL_<version>.ptf`.
- **Firmware file names** on the SD card must start with `TrackerPlus`, end in `.ptf`, be at most 31 characters, and sit
  in `/Firmware/`. A stuck "preparing for update" screen is fixed by deleting `/Firmware/_fwinfo` and `/Firmware/_turnon`.
- **Local builds** need GNU Arm Embedded **10.3** (GCC 13 fails on `-ffreestanding` with hosted headers).
  `developer.arm.com` is blocked from cloud sessions; use the xPack `v10.3.1-2.3` `linux-x64` release from GitHub
  (`xpack-dev-tools/arm-none-eabi-gcc-xpack`), extracted to `agt/10.3/` (gitignored).
- **Line endings**: some files are CRLF or mixed (e.g. `core/interface/patternEditor/fPatternEdit.cpp`). Preserve
  them when editing.
- **Upstream PRs**: `polyend/tracker-ce` CI rejects AI `Co-authored-by:` commit trailers. Remove them before opening a
  PR there.

## Step-sequencer mode

Code: [`core/interface/patternEditor/fStepSeq.cpp`](../core/interface/patternEditor/fStepSeq.cpp), hooks in
`fPatternEdit.cpp` / `gPatternEdit.cpp` / `patternEditor.h`. Compiled only with `STEP_SEQ_MODE` on Tracker+
(`PLATFORM_IMX && !TRACKER_MINI`). `Interface::modules[]` is unchanged; pattern format stays version 6.

- **Enter / exit**: pattern editor, screen-button page 3 → "Step Seq Mode"; screen button 8 ("Exit") leaves.
- **Pads**: left 8×4 = 32 steps of the cursor track (one row = half a bar); right 4×4 = instruments, Shift = next 16.
- **Navigation**: Up/Down move the 32-step window, Left/Right change track.
- **Modes** (latched with one press): Note, Instr, Vol (= Fx1 column, `fx[1]`), Fx (= Fx2 column, `fx[0]`).
  Pressing the active Vol/Fx again toggles both effect columns to micromove / volume (m/V), unless the knob was turned
  while it was held.
- **Editing**:
  - Tap a step: add / remove (Instr mode assigns the selected instrument, adding a note if empty).
  - Hold a step + knob: edit only that step (note, instrument, effect amount, m/V); with Shift: octave / effect type.
    Several held steps are edited together. Removal happens on release and only if the step was not edited.
  - Hold the mode button + knob: value for the next taps (Shift: octave / effect type); never edits steps or toggles.
  - In Instr mode, tapping an instrument pad while holding steps assigns that instrument to them.
- **Default note**: last entered note, or middle C (MIDI 60, shown as C5) if none.
- **Pad LEDs** are diff-based (only changed pads are written). The current mode's values light at normal level,
  other notes dim, held steps and the playhead bright.

## STNG synth engine

Code: [`core/synth/engines/`](../core/synth/engines/) — `StringSynthDsp.h` (DSP core, no Tracker dependencies, host
testable), `StringSynthVoice.{h,cpp}` (PolySynth wrapper), `StringSynthPresets.h` (parameter ids + presets).
Registered as `InstrumentTypeId::kStringSynth` ("STNG") with badges in FT812 `RAM_G` after the CHIP badges.

- **Mono** Karplus-Strong style physical model (`maxVoices = 1`), selectable on SYN 1–3.
- **Exciters**: Pluck, Mallet, Click, Bow (filtered noise), Reed and Brass (self-oscillating junctions:
  clarinet-style reed table; constant-Q lip valve). Exciter Tone = reed stiffness / lip tension for Reed / Brass.
- **Models**: String (all harmonics) and Pipe (inverted reflection, odd harmonics). Second resonator ("String 2").
- **Damping**: the KS two-point averager is always on; Brightness sets a note-tracking one-pole (3×–43× the
  fundamental). Loss at the fundamental is compensated without loop gain above 1, so Decay is accurate at all pitches.
  Too little damping makes the attack noise audibly repeat — keep the averager.
- **Tuning** (host-tested): plucked within ~5 cents, Reed within ~2 cents C1–C7, Brass within ~10 cents from C4
  (unstable below C3). Stiffness is intentionally inharmonic.
- **Memory**: delay lines live in a 128 KB pool in `EXTERNAL_RAM` (4 voices × 32 KB); `SAMPLE_MEMORY_SIZE` on IMX was
  reduced by 128 KB to make room.
- **Presets**: 32 in `StringSynthPresets.h`, written as normal `.patch` files the first time the STNG patch list is
  opened and no STNG patches exist (`Synth::getPatchList`). Changing preset values in code only reaches a card after
  all STNG presets are deleted from it.
- **Parameter ids** (`stng::ParamId`) are append-only: saved patches store values by id.

## Open ideas

- Step-seq: in Fx modes, the knob with nothing held sets the amount for the next taps, Shift + knob the effect.
- STNG: replace the noise Bow with a bowed-string friction model and add a flute-style jet for pipes, for steady
  sustains without the breathy flutter.
