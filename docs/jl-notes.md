# JL Firmware Notes

Working notes for the JL build of the Polyend Tracker CE firmware: what has been added, how it is built,
and what is still open. Paste or point a new session at this file to pick up where the last one left off.

Repository: `jeremylstaples13-cloud/tracker-ce` (fork of `polyend/tracker-ce`). Only **Tracker+** is built.

## Branches

| Branch | Contents |
| :--- | :--- |
| `feature/stng-generator` | **Latest — work here.** STNG as a Sample Editor generator (live synth removed) + slice fades. Based on `feature/slice-fades`. |
| `feature/slice-fades` | Per-slice fades for the sampler. Based on `feature/stng-synth`. |
| `feature/stng-synth` | Step-sequencer mode + STNG synth. Based on `feature/step-seq-mode`. |
| `feature/step-seq-mode` | Step-sequencer mode only. |

## Building

- **GitHub Actions**: [`.github/workflows/build-step-seq-plus.yml`](../.github/workflows/build-step-seq-plus.yml) builds
  Tracker+ on every push to any of these branches with `make plus CUSTOM_DEFINES="-DSTEP_SEQ_MODE"` and uploads
  `TrackerPlus_JL_1.0.0.ptf` plus the Sample Editor plugins (`*.ptfx`, including `stng_gen.ptfx`) as a run
  artifact (Actions tab → latest run → Artifacts). It also runs the STNG host tests.
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

## STNG generator (Sample Editor)

Code: [`editor-fx-sdk/effects/stng_gen.cpp`](../editor-fx-sdk/effects/stng_gen.cpp) (plugin),
[`editor-fx-sdk/effects/stng/`](../editor-fx-sdk/effects/stng/) — `StringSynthDsp.h` (DSP core), `StringSynthPresets.h`
(32 presets, pointer-free), `stng_math.h` (freestanding math shim), `test/` (host tests).

STNG used to be a live mono synth engine (`feature/stng-synth`). On `feature/stng-generator` it is a `.ptfx`
Sample Editor generator instead, for rendering sounds into samples; the live engine is removed (its 128 KB of delay
lines went back to sample memory, minus 16 KB now reserved for slice fades and plugin lists).

- **Install**: copy `stng_gen.ptfx` (CI artifact, or `make ptfx` → `editor-fx-sdk/build/Effects/`) to `/Effects/` on the
  SD card. Sample Editor → effect list → "STNG Generator" → Preview / Apply. Works on an empty instrument slot.
- **Parameters**: page 1 Preset, Note (C2–C8, Tracker naming: C5 = note 60, plays at the sample's original pitch),
  Velo, Hold (time to note off; the preset's release follows). Page 2 Length (up to 20 s), Chord (Single, Octave, Fifth,
  Major, Minor, Sus4, Maj7, Min7, Unison 2/3/4), Decay (× preset), Bright (± preset). Page 3 Exciter and Model
  ("Preset" keeps the preset's), Pos and Body (± preset).
- **Rendering**: offline, up to 4 voices (each with its own noise / drift seed), 128-sample DSP blocks independent of the
  host's chunking, soft knee above 90 % so chords don't clip, 10 ms fade at Length, the sample ends early once the
  sound has died away. Always mono (`CONVERT_TO_MONO`).
- **Firmware support** (JL): generators set their length with `set_extra_samples(0, length)`; plugin lists can have
  128 items (Note needs 73); defaults reach the plugin when it is selected. Other firmware: 2 s max, 16 notes.
- **Tests**: `editor-fx-sdk/effects/stng/test/run_tests.sh` (also run in CI) renders every preset through the plugin
  with a fake host and compares it with the DSP: 18 presets within 1 LSB, the stiff / bowed / reed / brass ones differ
  only by float rounding (same pitch to 0.00 cents, level within 0.04 dB). The DSP itself is bit-identical to the live
  engine's. Brass is unstable at low notes (pedal tones).
- **Builder**: `ptfx_builder.py` now builds `.cpp` plugins, joins split string literals in `PLUGIN_PARAMS`, and refuses
  objects that would need relocation (GOT, absolute addresses in code, static constructors).

## Per-slice fades (Slice / Beat Slice)

Code: [`core/FileManager/SliceFades.{h,cpp}`](../core/FileManager/SliceFades.cpp) (data + file),
`AudioPlayMemory::setupSliceFade` / `applySliceFade` in `core/libs/Audio/` (playback), hooks in `mtSliceManager.cpp`
and the Sample Playback screen (`fSamplePlay.cpp` / `gSamplePlay.cpp`).

- **UI**: Sample Playback → Slice / Beat Slice → More (screen 2) → 5th control "Fade In n" / "Fade Out n" (n = selected
  slice). Press it again to switch In/Out. Knob or Up/Down edits (0–1000 ms; 1 ms steps below 20, then 5 / 10 / 25).
  Shift + knob applies the value to every slice. Delete resets it to 0. Pick the slice on screen 1 (or with the pads in
  Beat Slice).
- **Playback**: a linear gain ramp by playhead position inside the slice: fade-in after the playback start, fade-out
  before the playback end (swapped in reverse). Times are real time at the note's pitch. Only slice-end cuts are faded,
  not a slice cut short by a new note on the same track.
- **Slice edits**: adding a slice copies the fades of the slice it splits; removing shifts them; moving keeps them;
  Auto / Evenly re-slicing gives every new slice the fades of the slice that was selected.
- **Storage**: one sidecar file per project, `Workspace/sliceFades` → `Projects/<name>/sliceFades` (copied on project
  open / save, written with the instrument autosave). `.pti` files are unchanged, so stock firmware ignores the fades.
  Each instrument record stores a fingerprint of its slice layout; if the slices change outside the slice editor
  (sample import, instrument load, undo...) the stale fades are ignored and dropped on the next edit.

## Open ideas

- Step-seq: in Fx modes, the knob with nothing held sets the amount for the next taps, Shift + knob the effect.
- Slice fades: a short automatic fade when a slice is cut off by a new note on the same track.
- STNG generator: stereo output (spread chord / unison voices across L/R).
- STNG: replace the noise Bow with a bowed-string friction model and add a flute-style jet for pipes, for steady
  sustains without the breathy flutter.
