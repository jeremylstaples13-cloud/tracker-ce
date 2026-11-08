// Physical step-sequencer mode for the pattern editor.
//
// Pad layout (pads are numbered row * 12 + column, 4 rows x 12 columns):
//   columns 0-7  : step grid, row r / column c -> step (window + r * 8 + c), 32 steps per window
//   columns 8-11 : instrument picker, 16 instruments, Shift shows the next 16
//
// Steps are edited on the track under the cursor. Up/Down move the 32-step window,
// Left/Right change track, screen button 8 exits.
//
// Input modes are latched with one press of Note / Instr / Vol (Fx1) / Fx (Fx2), like
// choosing a column in the regular pattern editor. Pressing the active Vol/Fx button again
// (without turning the encoder) switches both effect columns to micromove / volume (m/V)
// and back, as pressing Note twice does in the regular editor.
//
//   tap a step         Note : add a note / remove it
//                      Instr: give the step the selected instrument (adds a note if empty)
//                      Fx   : add the current effect (or m/V value) / remove it
//   hold step + enc    that step's note / instrument / effect amount / m/V value
//   hold step + Shift + enc   note: octave, effect: effect type
//   hold mode button + enc    value used for the next taps (Shift: octave / effect type);
//                             never edits existing steps and never toggles the mode
//
// Removing happens when the pad is released, and only if the encoder was not turned while
// it was held, so holding an existing step to edit it never deletes it. Several steps can be
// held and edited together. Instrument pads pressed while holding steps in Instr mode assign
// that instrument to them.
//
// Notes go through Sequencer::setSelectionNote / clearSelected; other writes store an undo
// revision and set the "pattern changed" flag the same way. Pattern file format (version 6)
// is unchanged.

#include "patternEditor/patternEditor.h"

#ifdef STEP_SEQ_ENABLED

#include <stdio.h>
#include <string.h>

#include "fileManager.h"
#include "mtPadsBacklight.h"
#include "mtSequencer.h"
#include "mtStructs.h"

#include "core/interfaceDefs.h"
#include "core/interfacePopups.h"

#include "Platform.h"

extern strMtProject mtProject;

static cPatternEditor* PTE = &patternEditor;

using Step = Sequencer::strPattern::strTrack::strStep;

static constexpr uint8_t kPadColumns = 12;
static constexpr uint8_t kStepColumns = 8;
static constexpr uint8_t kInstrColumns = 4;

static constexpr uint8_t kSelectableFxCount = FX_COUNT - FX_COUNT_HIDDEN_FXes;
static constexpr uint8_t kFallbackFxIdx = 2; // "V - Volume/Velocity"

enum StepSeqMode : uint8_t
{
    kModeNote = 0,
    kModeInstr,
    kModeFx1, // Vol button: step.fx[1], or micromove in m/V view
    kModeFx2, // Fx button:  step.fx[0], or volume in m/V view
};

static uint8_t mode = kModeNote;
static bool mvView = false; // effect columns show micromove / volume

// mode button currently held (0 = none)
static uint8_t heldModeButton = 0;
static bool modeButtonWasActive = false;
static bool modeButtonTurned = false;

// step pads currently held, as window-relative bits
static uint32_t heldSteps = 0;
static uint32_t addedSteps = 0;  // written on press: never removed on release
static uint32_t turnedSteps = 0; // edited with the encoder while held
static uint16_t heldWindow = 0;
static uint8_t heldTrack = 0;

// values used for the next taps in the Fx modes
static uint8_t fxNameIdx = kFallbackFxIdx; // index into interfaceGlobals.fxIDs / fxNames
static int16_t fxValue = 0;
static int16_t microValue = 0;
static int16_t volumeValue = 100;

static bool stepSeqShiftHeld = false;

static uint16_t stepSeqLabelWindow = 0xFFFF; // window shown on the labels
static bool stepSeqLabelShift = false;

static char stepSeqWindowText[12];
static char stepSeqBankText[12];
static char stepSeqModeText[12];
static char stepSeqValueText[12];

static void functStepSeqPads(uint8_t pad, uint8_t state, int16_t velo);
static void functStepSeqShift(uint8_t state);
static void functStepSeqUp();
static void functStepSeqDown();
static void functStepSeqLeft();
static void functStepSeqRight();
static void functStepSeqModeButton(uint8_t button, uint8_t state);
static void functStepSeqEncoder(int16_t value);
static void showStepSeqModeLabel();

//------------------------------------------------------------------------------
static inline uint8_t stepIndexToPad(uint8_t idx)
{
    return (idx / kStepColumns) * kPadColumns + (idx % kStepColumns);
}

static inline uint8_t instrIndexToPad(uint8_t idx)
{
    return (idx / kInstrColumns) * kPadColumns + kStepColumns + (idx % kInstrColumns);
}

static inline int8_t sign(int16_t value)
{
    return value > 0 ? 1 : -1;
}

static int maxInstrumentIdx()
{
#if GUI_HANDLE_SYNTH
    return kSampleInstrumentsCount + kMidiInstrumentsCount + kSynthInstrumentsCount - 1;
#else
    return kSampleInstrumentsCount + kMidiInstrumentsCount - 1;
#endif
}

// Instrument shown on picker slot `idx` (0-15) for the current track and Shift state,
// or -1 if there is none.
static int instrumentForSlot(uint8_t idx)
{
    int bankOffset = stepSeqShiftHeld ? cPatternEditor::kStepSeqInstrPads : 0;
    int instrument;

    if (isSampleTrack(PTE->trackerPattern.actualTrack))
    {
        instrument = bankOffset + idx;
        return instrument < kSampleInstrumentsCount ? instrument : -1;
    }

    instrument = MIDI_INSTRUMENTS_START_IDX + bankOffset + idx;
    return instrument <= maxInstrumentIdx() ? instrument : -1;
}

static bool isInstrumentPresent(int instrument)
{
    if (instrument < 0)
    {
        return false;
    }
    if (instrument < kSampleInstrumentsCount)
    {
        return mtProject.instrument[instrument].isActive;
    }
    return true; // MIDI / synth instruments always exist
}

static bool isMidiTypeTrack()
{
    return !isSampleTrack(PTE->trackerPattern.actualTrack);
}

// selected instrument, mapped to the current track type
static uint8_t selectedInstrument()
{
    return sequencer.changeInstrumentInSpecificOrder(
      mtProject.values.lastUsedInstrument, 0, isMidiTypeTrack());
}

// Last entered note, or middle C (MIDI 60, shown as C5 on the Tracker) if no note has been
// entered yet. New projects start with lastUsedNote = 0.
static uint8_t stepSeqNote()
{
    uint8_t note = mtProject.values.lastUsedNote;
    if (note == 0 || note > Sequencer::STEP_NOTE_MAX)
    {
        note = Sequencer::STEP_NOTE_DEFAULT;
    }
    return note;
}

static inline bool hasNote(const Step* step)
{
    return step->note >= 0;
}

static inline bool isFxMode()
{
    return mode == kModeFx1 || mode == kModeFx2;
}

static inline uint8_t fxSlot()
{
    return mode == kModeFx1 ? 1 : 0;
}

//------------------------------------------------------------------------------
// effect / m/V column helpers (Fx modes)

static void initFxBrush()
{
    // last effect used in the editor (index into the effect list, like the Fx popup);
    // fall back to Volume if none has been picked yet
    fxNameIdx = PTE->getPreviousUsedFx();
    if (fxNameIdx == 0 || fxNameIdx >= kSelectableFxCount)
    {
        fxNameIdx = kFallbackFxIdx;
    }

    uint8_t type = interfaceGlobals.fxNameToId(fxNameIdx);
    fxValue = clamp<int>(
      PTE->getPreviousUsedFxValue(), sequencer.getFxMin(type), sequencer.getFxMax(type));
}

static bool columnHasValue(const Step* step)
{
    if (mvView)
    {
        return mode == kModeFx1 ? step->micromove >= 0 : step->volume >= 0;
    }
    return step->fx[fxSlot()].type != 0;
}

static void writeColumn(Step* step)
{
    if (mvView)
    {
        if (mode == kModeFx1)
        {
            step->micromove = microValue;
        }
        else
        {
            step->volume = volumeValue;
        }
        return;
    }

    step->fx[fxSlot()].type = interfaceGlobals.fxNameToId(fxNameIdx);
    step->fx[fxSlot()].value = fxValue;
}

static void clearColumn(Step* step)
{
    if (mvView)
    {
        if (mode == kModeFx1)
        {
            step->micromove = -1;
        }
        else
        {
            step->volume = Sequencer::STEP_VELO_DEFAULT;
        }
        return;
    }

    step->fx[fxSlot()].type = 0;
    step->fx[fxSlot()].value = 0;
}

// encoder on a held step in an Fx mode; the edited value also becomes the value for the
// next taps (like "last used" in the regular editor)
static void adjustColumn(Step* step, int16_t value)
{
    if (!columnHasValue(step))
    {
        return;
    }

    if (mvView)
    {
        if (mode == kModeFx1)
        {
            step->micromove = clamp<int>(step->micromove + value, 0, 100);
            microValue = step->micromove;
        }
        else
        {
            step->volume = clamp<int>(step->volume + value, 0, 100);
            volumeValue = step->volume;
        }
        return;
    }

    auto& fx = step->fx[fxSlot()];

    if (stepSeqShiftHeld)
    {
        // effect type, one list entry per detent (the input processor boosts encoder steps
        // while Shift is held, so only the sign is used)
        int idx = clamp<int>(interfaceGlobals.fxIdToName(fx.type) + sign(value),
                             1,
                             kSelectableFxCount - 1);
        fx.type = interfaceGlobals.fxNameToId(idx);
        fx.value = sequencer.getFxDefault(fx.type);
    }
    else
    {
        fx.value = clamp<int>(
          fx.value + value, sequencer.getFxMin(fx.type), sequencer.getFxMax(fx.type));
    }

    fxNameIdx = interfaceGlobals.fxIdToName(fx.type);
    fxValue = fx.value;
    PTE->setPreviousUsedFx(fxNameIdx);
    PTE->setPreviousUsedFxValue(fxValue);
}

// encoder with the mode button held: value for the next taps only
static void adjustBrush(int16_t value)
{
    switch (mode)
    {
        case kModeNote:
        {
            int delta = stepSeqShiftHeld ? sign(value) * 12 : value;
            // 1..127: lastUsedNote == 0 means "not set yet" (see stepSeqNote)
            mtProject.values.lastUsedNote =
              clamp<int>(stepSeqNote() + delta, 1, Sequencer::STEP_NOTE_MAX);
            break;
        }
        case kModeInstr:
            mtProject.values.lastUsedInstrument = sequencer.changeInstrumentInSpecificOrder(
              mtProject.values.lastUsedInstrument, sign(value), isMidiTypeTrack());
            PTE->showActualParamOnBar();
            break;
        default:
            if (mvView)
            {
                int16_t& v = mode == kModeFx1 ? microValue : volumeValue;
                v = clamp<int>(v + value, 0, 100);
            }
            else if (stepSeqShiftHeld)
            {
                fxNameIdx = clamp<int>(fxNameIdx + sign(value), 1, kSelectableFxCount - 1);
                fxValue = sequencer.getFxDefault(interfaceGlobals.fxNameToId(fxNameIdx));
                PTE->setPreviousUsedFx(fxNameIdx);
                PTE->setPreviousUsedFxValue(fxValue);
            }
            else
            {
                uint8_t type = interfaceGlobals.fxNameToId(fxNameIdx);
                fxValue =
                  clamp<int>(fxValue + value, sequencer.getFxMin(type), sequencer.getFxMax(type));
                PTE->setPreviousUsedFxValue(fxValue);
            }
            break;
    }
}

//------------------------------------------------------------------------------
static void resetHeldSteps()
{
    heldSteps = 0;
    addedSteps = 0;
    turnedSteps = 0;
}

// mirror the step-mode input mode on the regular pattern view (highlighted column, m/V)
static void applyModeToEditor()
{
    static constexpr uint8_t kEditParam[] = {
        cPatternEditor::EditNote,
        cPatternEditor::EditInstr,
        cPatternEditor::EditFx1,
        cPatternEditor::EditFx2,
    };

    PTE->editParam = kEditParam[mode];
    PTE->isMicVolView = mvView;
    PTE->trackerPattern.selectedParam = PTE->editParam;
    PTE->refreshPattern();
    PTE->showActualParamOnBar(true);
}

//------------------------------------------------------------------------------
void functStepSeqToggle()
{
    if (PTE->stepSeqActive)
    {
        PTE->exitStepSeq();
    }
    else
    {
        PTE->enterStepSeq();
    }
}

void cPatternEditor::enterStepSeq()
{
    if (stepSeqActive || fillState > 0)
    {
        return;
    }

    cancelPopups();
    if (mtPopups.isNoteKeyboardPopupActive())
    {
        mtPopups.toggleNoteKeyboardPopup();
    }

    stepSeqActive = true;
    stepSeqShiftHeld = buttons->isButtonPressed(interfaceButtonShift);

    // note input by default, with the currently selected instrument
    mode = kModeNote;
    mvView = false;
    heldModeButton = 0;
    resetHeldSteps();
    initFxBrush();

    setStepSeqFunct();
    applyModeToEditor();
    showStepSeqLabels();

    stepSeqInvalidateLeds();
    stepSeqLightPads();
}

void cPatternEditor::exitStepSeq(bool restoreScreen)
{
    if (!stepSeqActive)
    {
        return;
    }

    stepSeqActive = false;
    stepSeqShiftHeld = false;
    heldModeButton = 0;
    resetHeldSteps();

    stepSeqInvalidateLeds();

    if (restoreScreen)
    {
        setDefaultScreenFunct();
        refreshEditState(); // restores the screen-button page and its labels
        lightUpPadBoard();
    }
}

void cPatternEditor::setStepSeqFunct()
{
    ip->clearButtonsRange(interfaceButton0, interfaceButton7);
    ip->setButtonObj(interfaceButton7, buttonPress, functStepSeqToggle);

    ip->setPadsGlobal(functStepSeqPads);

    ip->setButtonObj(interfaceButtonShift, functStepSeqShift);

    ip->setButtonObj(interfaceButtonUp, buttonPress, functStepSeqUp);
    ip->setButtonObj(interfaceButtonDown, buttonPress, functStepSeqDown);
    ip->setButtonObj(interfaceButtonLeft, buttonPress, functStepSeqLeft);
    ip->setButtonObj(interfaceButtonRight, buttonPress, functStepSeqRight);

    ip->setButtonObj(interfaceButtonNote, functStepSeqModeButton);
    ip->setButtonObj(interfaceButtonInstr, functStepSeqModeButton);
    ip->setButtonObj(interfaceButtonVol, functStepSeqModeButton);
    ip->setButtonObj(interfaceButtonFx, functStepSeqModeButton);

    ip->setPotObj(interfacePot0, functStepSeqEncoder);
}

uint16_t cPatternEditor::stepSeqWindowStart()
{
    return (trackerPattern.actualStep / kStepSeqSteps) * kStepSeqSteps;
}

void cPatternEditor::showStepSeqLabels()
{
    uint16_t windowStart = stepSeqWindowStart();
    uint8_t bankOffset = stepSeqShiftHeld ? kStepSeqInstrPads : 0;

    stepSeqLabelWindow = windowStart;
    stepSeqLabelShift = stepSeqShiftHeld;

    snprintf(stepSeqWindowText,
             sizeof(stepSeqWindowText),
             "%d-%d",
             windowStart + 1,
             windowStart + kStepSeqSteps);
    snprintf(stepSeqBankText,
             sizeof(stepSeqBankText),
             "%d-%d",
             bankOffset + 1,
             bankOffset + kStepSeqInstrPads);

    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlText(label[i], "");
        display.setControlText2(label[i], "");
        display.setControlColors(label[i], display.getTheme().inactiveLabelsColors);
    }

    display.setControlText(label[0], "Steps");
    display.setControlText2(label[0], stepSeqWindowText);
    display.setControlText(label[1], "Instr");
    display.setControlText2(label[1], stepSeqBankText);
    display.setControlText(label[2], stepSeqModeText);
    display.setControlText2(label[2], stepSeqValueText);
    display.setControlText(label[7], "Exit");

    display.setControlColors(label[0], display.getTheme().activeLabelsColors);
    display.setControlColors(label[1], display.getTheme().activeLabelsColors);
    display.setControlColors(label[2], display.getTheme().activeLabelsColors);
    display.setControlColors(label[7], display.getTheme().activeButtonLabelsColors);

    for (uint8_t i = 0; i < 8; i++)
    {
        display.setControlValue(label[i], 1);
        display.setControlStyle(label[i],
                                controlStyleCenterX | controlStyleCenterY | controlStyleFont3 |
                                  controlStyleShow);
        display.setControlStyle2(label[i],
                                 controlStyleCenterX | controlStyleCenterY | controlStyleFont3);
        display.refreshControl(label[i]);
    }

    showStepSeqModeLabel();
}

// Label 3: current mode and the value of the first held step, or the value used for the
// next taps when no step is held. Only this control is redrawn on edits.
static void showStepSeqModeLabel()
{
    const Step* step = nullptr;
    if (heldSteps != 0)
    {
        Sequencer::strPattern::strTrack* trackData =
          &sequencer.getPatternToUI()->track[heldTrack];
        for (uint8_t i = 0; i < cPatternEditor::kStepSeqSteps; i++)
        {
            if (heldSteps & (uint32_t(1) << i))
            {
                step = &trackData->step[heldWindow + i];
                break;
            }
        }
    }

    switch (mode)
    {
        case kModeNote:
        {
            snprintf(stepSeqModeText, sizeof(stepSeqModeText), "Note");
            int note = step ? step->note : stepSeqNote();
            const char* text = "OFF";
            if (note >= 0)
            {
                text = mtNotes[note];
            }
            else if (note == Sequencer::STEP_NOTE_EMPTY || note < Sequencer::STEP_NOTE_OFF_DEFAULT)
            {
                text = "---";
            }
            snprintf(stepSeqValueText, sizeof(stepSeqValueText), "%s", text);
            break;
        }
        case kModeInstr:
        {
            snprintf(stepSeqModeText, sizeof(stepSeqModeText), "Instr");
            int instrument = (step && hasNote(step)) ? step->instrument : selectedInstrument();
            if (step && !hasNote(step))
            {
                snprintf(stepSeqValueText, sizeof(stepSeqValueText), "---");
            }
            else if (instrument < kSampleInstrumentsCount)
            {
                snprintf(stepSeqValueText, sizeof(stepSeqValueText), "%d", instrument + 1);
            }
            else if (instrument < SYNTH_INSTRUMENTS_START_IDX)
            {
                snprintf(stepSeqValueText,
                         sizeof(stepSeqValueText),
                         "MIDI %d",
                         instrument - MIDI_INSTRUMENTS_START_IDX + 1);
            }
            else
            {
                snprintf(stepSeqValueText,
                         sizeof(stepSeqValueText),
                         "Synth %d",
                         instrument - SYNTH_INSTRUMENTS_START_IDX + 1);
            }
            break;
        }
        default:
        {
            const char* column = mode == kModeFx1 ? "Fx1" : "Fx2";

            if (mvView)
            {
                int v = mode == kModeFx1 ? (step ? step->micromove : microValue)
                                         : (step ? step->volume : volumeValue);
                snprintf(stepSeqModeText,
                         sizeof(stepSeqModeText),
                         "%s",
                         mode == kModeFx1 ? "Micro" : "Volume");
                if (v >= 0)
                {
                    snprintf(stepSeqValueText, sizeof(stepSeqValueText), "%d", v);
                }
                else
                {
                    snprintf(stepSeqValueText, sizeof(stepSeqValueText), "---");
                }
                break;
            }

            uint8_t type = interfaceGlobals.fxNameToId(fxNameIdx);
            int value = fxValue;
            if (step)
            {
                type = step->fx[fxSlot()].type;
                value = step->fx[fxSlot()].value;
            }

            if (type == 0)
            {
                snprintf(stepSeqModeText, sizeof(stepSeqModeText), "%s", column);
                snprintf(stepSeqValueText, sizeof(stepSeqValueText), "---");
                break;
            }

            // effect names look like "P - Panning"; show the part after the code
            const char* fxName = interfaceGlobals.fxNames[interfaceGlobals.fxIdToName(type)];
            const char* dash = strstr(fxName, " - ");
            snprintf(stepSeqModeText, sizeof(stepSeqModeText), "%s %d", column, value);
            snprintf(stepSeqValueText, sizeof(stepSeqValueText), "%s", dash ? dash + 3 : fxName);
            break;
        }
    }

    display.refreshControl(PTE->label[2]);
    display.synchronizeRefresh();
}

//------------------------------------------------------------------------------
// LEDs: compute the wanted brightness for each pad and only write pads that changed,
// so a playhead move costs ~2 LED writes instead of a full clear + repaint.

void cPatternEditor::stepSeqInvalidateLeds()
{
    padsBacklight.clearGlobalPads(1, 1, 0);
    memset(stepSeqLedCache, 0, sizeof(stepSeqLedCache));
}

void cPatternEditor::stepSeqLightPads()
{
    uint8_t wanted[interfacePadsCount];
    memset(wanted, 0, sizeof(wanted));

    Sequencer::strPattern* pattern = sequencer.getPatternToUI();
    const uint8_t track = trackerPattern.actualTrack;
    const uint16_t windowStart = stepSeqWindowStart();
    const uint16_t length = pattern->track[0].length + 1;
    const bool playing = !sequencer.isStop();

    // cursor may have been moved by the encoder or other handlers
    if (windowStart != stepSeqLabelWindow || stepSeqShiftHeld != stepSeqLabelShift)
    {
        showStepSeqLabels();
    }

    const uint8_t lightOn = mtConfig.values.padsLightBack;
    const uint8_t lightDim = mtConfig.values.padsLightBackDim;
    const uint8_t lightBright = mtConfig.values.padsLightFront;
    const uint8_t instrument = selectedInstrument();

    for (uint8_t i = 0; i < kStepSeqSteps; i++)
    {
        uint16_t step = windowStart + i;
        if (step >= length)
        {
            break;
        }

        Step* stepData = &pattern->track[track].step[step]; // isNoteEmpty() is not const
        const bool note = !stepData->isNoteEmpty();

        // Note mode: notes at normal level. Other modes: steps with the mode's value
        // (selected instrument / effect / m/V) at normal level, other notes dimmed.
        bool active;
        switch (mode)
        {
            case kModeNote: active = note; break;
            case kModeInstr:
                active = hasNote(stepData) && stepData->instrument == instrument;
                break;
            default: active = columnHasValue(stepData); break;
        }

        uint8_t level = active ? lightOn : (note ? lightDim : 0);

        if ((heldSteps & (uint32_t(1) << i)) && windowStart == heldWindow && track == heldTrack)
        {
            level = lightBright;
        }
        if (playing && step == trackerPattern.playheadPosition)
        {
            level = lightBright;
        }

        wanted[stepIndexToPad(i)] = level;
    }

    for (uint8_t i = 0; i < kStepSeqInstrPads; i++)
    {
        int slotInstrument = instrumentForSlot(i);

        uint8_t level = 0;
        if (slotInstrument >= 0 && slotInstrument == mtProject.values.lastUsedInstrument)
        {
            level = lightBright;
        }
        else if (isInstrumentPresent(slotInstrument))
        {
            level = lightDim;
        }

        wanted[instrIndexToPad(i)] = level;
    }

    for (uint8_t pad = 0; pad < interfacePadsCount; pad++)
    {
        if (wanted[pad] == stepSeqLedCache[pad])
        {
            continue;
        }

        padsBacklight.setBackLayer(wanted[pad] ? 1 : 0, wanted[pad], pad);
        stepSeqLedCache[pad] = wanted[pad];
    }
}

//------------------------------------------------------------------------------
// step pads

static Step* stepAt(uint16_t step)
{
    return &sequencer.getPatternToUI()->track[PTE->trackerPattern.actualTrack].step[step];
}

static void addNote(uint16_t step)
{
    const uint8_t track = PTE->trackerPattern.actualTrack;
    sequencer.setSelection(step, track, step, track);
    // writes the selected instrument too, stores undo, marks the pattern changed, previews
    sequencer.setSelectionNote(stepSeqNote());
}

static void removeNote(uint16_t step)
{
    const uint8_t track = PTE->trackerPattern.actualTrack;
    sequencer.setSelection(step, track, step, track);
    newFileManager.storePatternUndoRevision();
    sequencer.clearSelected(Sequencer::ELEMENTS_NOTES);
    newFileManager.setActualPatternStructChanged();
}

static void stepPress(uint16_t step)
{
    const uint8_t track = PTE->trackerPattern.actualTrack;
    const uint16_t window = PTE->stepSeqWindowStart();

    if (heldSteps == 0 || window != heldWindow || track != heldTrack)
    {
        resetHeldSteps();
        heldWindow = window;
        heldTrack = track;
    }

    const uint32_t bit = uint32_t(1) << (step - window);
    Step* stepData = stepAt(step);

    heldSteps |= bit;

    switch (mode)
    {
        case kModeNote:
            if (stepData->isNoteEmpty())
            {
                addNote(step);
                addedSteps |= bit;
            }
            break; // a filled step is removed on release if not edited

        case kModeInstr:
            if (stepData->isNoteEmpty())
            {
                addNote(step);
            }
            else if (hasNote(stepData) && stepData->instrument != selectedInstrument())
            {
                newFileManager.storePatternUndoRevision();
                stepData->instrument = selectedInstrument();
                newFileManager.setActualPatternStructChanged();
            }
            addedSteps |= bit; // Instr mode never removes
            break;

        default:
            if (!columnHasValue(stepData))
            {
                newFileManager.storePatternUndoRevision();
                writeColumn(stepData);
                newFileManager.setActualPatternStructChanged();
                addedSteps |= bit;
            }
            break;
    }

    PTE->trackerPattern.actualStep = step;
    PTE->focusOnActual();
    PTE->refreshPattern();
    showStepSeqModeLabel();
}

static void stepRelease(uint16_t step)
{
    const uint16_t window = PTE->stepSeqWindowStart();
    if (window != heldWindow || PTE->trackerPattern.actualTrack != heldTrack)
    {
        return; // window or track changed while held
    }

    const uint32_t bit = uint32_t(1) << (step - window);
    if (!(heldSteps & bit))
    {
        return;
    }

    // a plain tap on a step that already had a value removes it
    if (!(addedSteps & bit) && !(turnedSteps & bit))
    {
        if (mode == kModeNote)
        {
            removeNote(step);
        }
        else if (isFxMode())
        {
            newFileManager.storePatternUndoRevision();
            clearColumn(stepAt(step));
            newFileManager.setActualPatternStructChanged();
        }
    }

    heldSteps &= ~bit;
    addedSteps &= ~bit;
    turnedSteps &= ~bit;

    PTE->refreshPattern();
    showStepSeqModeLabel();
}

// encoder with step pads held: edit only those steps
static void adjustHeldSteps(int16_t value)
{
    Sequencer::strPattern::strTrack* trackData = &sequencer.getPatternToUI()->track[heldTrack];

    if (mode != kModeNote)
    {
        newFileManager.storePatternUndoRevision();
    }

    for (uint8_t i = 0; i < cPatternEditor::kStepSeqSteps; i++)
    {
        if (!(heldSteps & (uint32_t(1) << i)))
        {
            continue;
        }

        const uint16_t step = heldWindow + i;
        Step* stepData = &trackData->step[step];

        switch (mode)
        {
            case kModeNote:
                if (hasNote(stepData))
                {
                    int delta = stepSeqShiftHeld ? sign(value) * 12 : value;
                    int note = clamp<int>(stepData->note + delta, 0, Sequencer::STEP_NOTE_MAX);
                    sequencer.setSelection(step, heldTrack, step, heldTrack);
                    sequencer.setSelectionNote(note); // stores undo, previews the step
                    if (note > 0)
                    {
                        mtProject.values.lastUsedNote = note;
                    }
                }
                break;

            case kModeInstr:
                if (hasNote(stepData))
                {
                    stepData->instrument = sequencer.changeInstrumentInSpecificOrder(
                      stepData->instrument, sign(value), !isSampleTrack(heldTrack));
                    mtProject.values.lastUsedInstrument = stepData->instrument;
                }
                break;

            default: adjustColumn(stepData, value); break;
        }
    }

    if (mode != kModeNote)
    {
        newFileManager.setActualPatternStructChanged();
    }
    if (mode == kModeInstr)
    {
        PTE->showActualParamOnBar();
    }

    turnedSteps |= heldSteps;
    PTE->refreshPattern();
}

static void functStepSeqPads(uint8_t pad, uint8_t state, int16_t velo)
{
    (void)velo;

    if (state != buttonPress && state != buttonRelease)
    {
        return;
    }

    const uint8_t column = pad % kPadColumns;
    const uint8_t row = pad / kPadColumns;

    if (column < kStepColumns)
    {
        uint16_t step = PTE->stepSeqWindowStart() + row * kStepColumns + column;
        if (step >= sequencer.getPatternLength())
        {
            return;
        }

        if (state == buttonPress)
        {
            stepPress(step);
        }
        else
        {
            stepRelease(step);
        }
    }
    else
    {
        if (state != buttonPress)
        {
            return;
        }

        int instrument = instrumentForSlot(row * kInstrColumns + (column - kStepColumns));
        if (instrument < 0)
        {
            return;
        }

        mtProject.values.lastUsedInstrument = instrument;

        // Instr mode with steps held: give them this instrument
        if (mode == kModeInstr && heldSteps != 0)
        {
            Sequencer::strPattern::strTrack* trackData =
              &sequencer.getPatternToUI()->track[heldTrack];

            newFileManager.storePatternUndoRevision();
            for (uint8_t i = 0; i < cPatternEditor::kStepSeqSteps; i++)
            {
                Step* stepData = &trackData->step[heldWindow + i];
                if ((heldSteps & (uint32_t(1) << i)) && hasNote(stepData))
                {
                    stepData->instrument = selectedInstrument();
                }
            }
            newFileManager.setActualPatternStructChanged();
            turnedSteps |= heldSteps;
            PTE->refreshPattern();
        }

        PTE->showActualParamOnBar();
        showStepSeqModeLabel();
    }

    PTE->stepSeqLightPads();
}

//------------------------------------------------------------------------------
// buttons and encoder

static void functStepSeqModeButton(uint8_t button, uint8_t state)
{
    uint8_t buttonMode;
    switch (button)
    {
        case interfaceButtonNote: buttonMode = kModeNote; break;
        case interfaceButtonInstr: buttonMode = kModeInstr; break;
        case interfaceButtonVol: buttonMode = kModeFx1; break;
        case interfaceButtonFx: buttonMode = kModeFx2; break;
        default: return;
    }

    if (state == buttonPress)
    {
        heldModeButton = button;
        modeButtonTurned = false;
        modeButtonWasActive = (buttonMode == mode);

        if (buttonMode != mode)
        {
            mode = buttonMode;
            resetHeldSteps();
            applyModeToEditor();
        }
    }
    else if (state == buttonRelease)
    {
        if (button != heldModeButton)
        {
            return;
        }

        // pressing the active Vol/Fx again switches the effect columns <-> m/V, unless the
        // button was held to change its value
        if (modeButtonWasActive && !modeButtonTurned && isFxMode())
        {
            mvView = !mvView;
            resetHeldSteps();
            applyModeToEditor();
        }

        heldModeButton = 0;
    }
    else
    {
        return;
    }

    showStepSeqModeLabel();
    PTE->stepSeqLightPads();
}

static void functStepSeqEncoder(int16_t value)
{
    if (value == 0)
    {
        return;
    }

    if (heldSteps != 0 && heldWindow == PTE->stepSeqWindowStart() &&
        heldTrack == PTE->trackerPattern.actualTrack)
    {
        adjustHeldSteps(value);
    }
    else if (heldModeButton != 0)
    {
        modeButtonTurned = true;
        adjustBrush(value);
    }
    else
    {
        stepSeqStockEncoder(value);
        return;
    }

    showStepSeqModeLabel();
    PTE->stepSeqLightPads();
}

static void functStepSeqShift(uint8_t state)
{
    if (state == buttonPress)
    {
        stepSeqShiftHeld = true;
    }
    else if (state == buttonRelease)
    {
        stepSeqShiftHeld = false;
    }
    else
    {
        return;
    }

    PTE->showStepSeqLabels();
    PTE->stepSeqLightPads();
}

static void stepSeqMoveWindow(int8_t dir)
{
    int length = sequencer.getPatternLength();
    int step = PTE->trackerPattern.actualStep + dir * cPatternEditor::kStepSeqSteps;

    if (step < 0 || step >= length)
    {
        return;
    }

    resetHeldSteps();
    PTE->trackerPattern.actualStep = step;
    PTE->focusOnActual();
    PTE->refreshPattern();
    PTE->showStepSeqLabels();
    PTE->stepSeqLightPads();
}

static void functStepSeqUp()
{
    stepSeqMoveWindow(-1);
}

static void functStepSeqDown()
{
    stepSeqMoveWindow(1);
}

static void functStepSeqLeft()
{
    resetHeldSteps();
    PTE->previousTrack(); // repaints pads through lightUpPadBoard()
    showStepSeqModeLabel();
}

static void functStepSeqRight()
{
    resetHeldSteps();
    PTE->nextTrack();
    showStepSeqModeLabel();
}

#endif // STEP_SEQ_ENABLED
