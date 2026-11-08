// Physical step-sequencer mode for the pattern editor.
//
// Pad layout (pads are numbered row * 12 + column, 4 rows x 12 columns):
//   columns 0-7  : step grid, row r / column c -> step (window + r * 8 + c), 32 steps per window
//   columns 8-11 : instrument picker, 16 instruments, Shift shows the next 16
//
// Steps are written to the track under the cursor. Tapping an empty step writes the last
// entered note (middle C if none yet) with the selected instrument, tapping a filled step
// clears it.
// Up/Down move the 32-step window, Left/Right change track, screen button 8 exits.
//
// Hold-and-tap (same column mapping as the regular pattern editor):
//   hold Vol         -> Fx1 column (step.fx[1]) with the last used effect
//   hold Fx          -> Fx2 column (step.fx[0]) with the last used effect
//   hold Note + Vol  -> micromove (m)
//   hold Note + Fx   -> volume (V)
// Tapping a step that already has that effect/value clears it. While holding, the encoder
// sets the value and updates the steps tapped during the current hold; with Vol/Fx held,
// Shift + encoder picks the effect instead.
//
// Notes go through Sequencer::setSelectionNote / clearSelected; effect writes store an undo
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

static constexpr uint8_t kPadColumns = 12;
static constexpr uint8_t kStepColumns = 8;
static constexpr uint8_t kInstrColumns = 4;

static bool stepSeqShiftHeld = false;

static uint16_t stepSeqLabelWindow = 0xFFFF; // window shown on the labels
static bool stepSeqLabelShift = false;

static char stepSeqWindowText[12];
static char stepSeqBankText[12];
static char stepSeqTargetText[12];
static char stepSeqValueText[12];

enum StepSeqTarget : uint8_t
{
    kTargetNone = 0,
    kTargetFx1,    // Vol button: step.fx[1]
    kTargetFx2,    // Fx button:  step.fx[0]
    kTargetMicro,  // Note + Vol
    kTargetVolume, // Note + Fx
};

static constexpr uint8_t kSelectableFxCount = FX_COUNT - FX_COUNT_HIDDEN_FXes;
static constexpr uint8_t kFallbackFxIdx = 2; // "V - Volume/Velocity"

static bool noteHeld = false;
static bool volHeld = false;
static bool fxHeld = false;

static uint8_t heldTarget = kTargetNone;
static uint32_t heldTapped = 0; // window-relative steps written during the current hold
static uint16_t heldWindow = 0;
static uint8_t heldTrack = 0;

static uint8_t fxNameIdx = 0; // index into interfaceGlobals.fxIDs / fxNames
static int16_t fxValue = 0;
static int16_t microValue = 0;
static int16_t volumeValue = 100;

static void functStepSeqPads(uint8_t pad, uint8_t state, int16_t velo);
static void functStepSeqShift(uint8_t state);
static void functStepSeqUp();
static void functStepSeqDown();
static void functStepSeqLeft();
static void functStepSeqRight();
static void functStepSeqParamButton(uint8_t button, uint8_t state);
static void functStepSeqEncoder(int16_t value);
static void showStepSeqTargetLabel();

//------------------------------------------------------------------------------
static inline uint8_t stepIndexToPad(uint8_t idx)
{
    return (idx / kStepColumns) * kPadColumns + (idx % kStepColumns);
}

static inline uint8_t instrIndexToPad(uint8_t idx)
{
    return (idx / kInstrColumns) * kPadColumns + kStepColumns + (idx % kInstrColumns);
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

//------------------------------------------------------------------------------
// hold-and-tap targets

static inline uint8_t fxSlotForTarget(uint8_t target)
{
    return target == kTargetFx1 ? 1 : 0;
}

static bool isFxTarget(uint8_t target)
{
    return target == kTargetFx1 || target == kTargetFx2;
}

static uint8_t computeTarget()
{
    if (volHeld)
    {
        return noteHeld ? kTargetMicro : kTargetFx1;
    }
    if (fxHeld)
    {
        return noteHeld ? kTargetVolume : kTargetFx2;
    }
    return kTargetNone;
}

static void initTargetValue(uint8_t target)
{
    if (!isFxTarget(target))
    {
        return;
    }

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

static bool stepHasTarget(Sequencer::strPattern::strTrack::strStep* step, uint8_t target)
{
    switch (target)
    {
        case kTargetFx1:
        case kTargetFx2:
            return step->fx[fxSlotForTarget(target)].type ==
                   interfaceGlobals.fxNameToId(fxNameIdx);
        case kTargetMicro: return step->micromove >= 0;
        case kTargetVolume: return step->volume >= 0;
        default: return false;
    }
}

static void writeTarget(Sequencer::strPattern::strTrack::strStep* step, uint8_t target)
{
    switch (target)
    {
        case kTargetFx1:
        case kTargetFx2:
        {
            auto& fx = step->fx[fxSlotForTarget(target)];
            fx.type = interfaceGlobals.fxNameToId(fxNameIdx);
            fx.value = fxValue;
            break;
        }
        case kTargetMicro: step->micromove = microValue; break;
        case kTargetVolume: step->volume = volumeValue; break;
        default: break;
    }
}

static void clearTarget(Sequencer::strPattern::strTrack::strStep* step, uint8_t target)
{
    switch (target)
    {
        case kTargetFx1:
        case kTargetFx2:
        {
            auto& fx = step->fx[fxSlotForTarget(target)];
            fx.type = 0;
            fx.value = 0;
            break;
        }
        case kTargetMicro: step->micromove = -1; break;
        case kTargetVolume: step->volume = Sequencer::STEP_VELO_DEFAULT; break;
        default: break;
    }
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
    noteHeld = volHeld = fxHeld = false;
    heldTarget = kTargetNone;
    heldTapped = 0;

    // note input by default, with the currently selected instrument
    editParam = EditNote;
    isMicVolView = false;
    trackerPattern.selectedParam = EditNote;

    setStepSeqFunct();
    showStepSeqLabels();
    showActualParamOnBar(true);

    stepSeqInvalidateLeds();
    stepSeqLightPads();

    refreshPattern();
}

void cPatternEditor::exitStepSeq(bool restoreScreen)
{
    if (!stepSeqActive)
    {
        return;
    }

    stepSeqActive = false;
    stepSeqShiftHeld = false;
    noteHeld = volHeld = fxHeld = false;
    heldTarget = kTargetNone;
    heldTapped = 0;

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

    // Note/Vol/Fx are hold modifiers in step mode (no column switching or popups)
    ip->setButtonObj(interfaceButtonNote, functStepSeqParamButton);
    ip->setButtonObj(interfaceButtonInstr, functStepSeqParamButton);
    ip->setButtonObj(interfaceButtonVol, functStepSeqParamButton);
    ip->setButtonObj(interfaceButtonFx, functStepSeqParamButton);

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
    display.setControlText(label[7], "Exit");

    display.setControlColors(label[0], display.getTheme().activeLabelsColors);
    display.setControlColors(label[1], display.getTheme().activeLabelsColors);
    display.setControlColors(label[7], display.getTheme().activeButtonLabelsColors);

    display.setControlText(label[2], stepSeqTargetText);
    display.setControlText2(label[2], stepSeqValueText);

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

    showStepSeqTargetLabel();
}

// Label 3 shows what holding Vol/Fx (and Note) will write. Only this one control is
// redrawn on button holds and encoder turns.
static void showStepSeqTargetLabel()
{
    switch (heldTarget)
    {
        case kTargetFx1:
        case kTargetFx2:
        {
            // effect names look like "P - Panning"; show the part after the code
            const char* fxName = interfaceGlobals.fxNames[fxNameIdx];
            const char* dash = strstr(fxName, " - ");
            snprintf(stepSeqTargetText,
                     sizeof(stepSeqTargetText),
                     "%s %d",
                     heldTarget == kTargetFx1 ? "Fx1" : "Fx2",
                     fxValue);
            snprintf(stepSeqValueText, sizeof(stepSeqValueText), "%s", dash ? dash + 3 : fxName);
            break;
        }
        case kTargetMicro:
            snprintf(stepSeqTargetText, sizeof(stepSeqTargetText), "Micro");
            snprintf(stepSeqValueText, sizeof(stepSeqValueText), "%d", microValue);
            break;
        case kTargetVolume:
            snprintf(stepSeqTargetText, sizeof(stepSeqTargetText), "Volume");
            snprintf(stepSeqValueText, sizeof(stepSeqValueText), "%d", volumeValue);
            break;
        default:
            stepSeqTargetText[0] = 0;
            stepSeqValueText[0] = 0;
            break;
    }

    display.setControlColors(PTE->label[2],
                             heldTarget != kTargetNone ? display.getTheme().activeLabelsColors
                                                       : display.getTheme().inactiveLabelsColors);
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

    for (uint8_t i = 0; i < kStepSeqSteps; i++)
    {
        uint16_t step = windowStart + i;
        if (step >= length)
        {
            break;
        }

        // while Vol/Fx is held: steps with that effect/value at normal level, other notes dimmed
        Sequencer::strPattern::strTrack::strStep* stepData = &pattern->track[track].step[step];
        const bool hasNote = !stepData->isNoteEmpty();

        uint8_t level = 0;
        if (heldTarget != kTargetNone)
        {
            if (stepHasTarget(stepData, heldTarget))
            {
                level = lightOn;
            }
            else if (hasNote)
            {
                level = lightDim;
            }
        }
        else if (hasNote)
        {
            level = lightOn;
        }
        if (playing && step == trackerPattern.playheadPosition)
        {
            level = lightBright;
        }

        wanted[stepIndexToPad(i)] = level;
    }

    for (uint8_t i = 0; i < kStepSeqInstrPads; i++)
    {
        int instrument = instrumentForSlot(i);

        uint8_t level = 0;
        if (instrument >= 0 && instrument == mtProject.values.lastUsedInstrument)
        {
            level = lightBright;
        }
        else if (isInstrumentPresent(instrument))
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

static void stepSeqToggleStep(uint16_t step)
{
    const uint8_t track = PTE->trackerPattern.actualTrack;
    Sequencer::strPattern* pattern = sequencer.getPatternToUI();
    Sequencer::strPattern::strTrack::strStep* stepData = &pattern->track[track].step[step];

    sequencer.setSelection(step, track, step, track);

    if (stepData->isNoteEmpty())
    {
        // writes lastUsedInstrument too, stores undo and marks the pattern changed
        sequencer.setSelectionNote(stepSeqNote());
    }
    else
    {
        newFileManager.storePatternUndoRevision();
        sequencer.clearSelected(Sequencer::ELEMENTS_NOTES);
        newFileManager.setActualPatternStructChanged();
    }

    PTE->trackerPattern.actualStep = step;
    PTE->focusOnActual();
    PTE->refreshPattern();
}

static void stepSeqApplyTarget(uint16_t step)
{
    const uint8_t track = PTE->trackerPattern.actualTrack;
    const uint16_t window = PTE->stepSeqWindowStart();
    Sequencer::strPattern::strTrack::strStep* stepData =
      &sequencer.getPatternToUI()->track[track].step[step];

    if (window != heldWindow || track != heldTrack)
    {
        heldTapped = 0;
        heldWindow = window;
        heldTrack = track;
    }

    const uint32_t bit = uint32_t(1) << (step - window);

    newFileManager.storePatternUndoRevision();

    if ((heldTapped & bit) || stepHasTarget(stepData, heldTarget))
    {
        clearTarget(stepData, heldTarget);
        heldTapped &= ~bit;
    }
    else
    {
        writeTarget(stepData, heldTarget);
        heldTapped |= bit;
    }

    newFileManager.setActualPatternStructChanged();

    PTE->trackerPattern.actualStep = step;
    PTE->focusOnActual();
    PTE->refreshPattern();
}

static void functStepSeqPads(uint8_t pad, uint8_t state, int16_t velo)
{
    (void)velo;

    if (state != buttonPress)
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

        if (heldTarget != kTargetNone)
        {
            stepSeqApplyTarget(step);
        }
        else
        {
            stepSeqToggleStep(step);
        }
    }
    else
    {
        int instrument = instrumentForSlot(row * kInstrColumns + (column - kStepColumns));
        if (instrument < 0)
        {
            return;
        }

        mtProject.values.lastUsedInstrument = instrument;
        PTE->showActualParamOnBar();
    }

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
    PTE->previousTrack(); // repaints pads through lightUpPadBoard()
}

static void functStepSeqRight()
{
    PTE->nextTrack();
}

static void functStepSeqParamButton(uint8_t button, uint8_t state)
{
    if (state != buttonPress && state != buttonRelease)
    {
        return;
    }

    const bool pressed = state == buttonPress;

    switch (button)
    {
        case interfaceButtonNote: noteHeld = pressed; break;
        case interfaceButtonVol: volHeld = pressed; break;
        case interfaceButtonFx: fxHeld = pressed; break;
        default: return; // Instr: no function in step mode
    }

    uint8_t target = computeTarget();
    if (target == heldTarget)
    {
        return;
    }

    heldTarget = target;
    heldTapped = 0;
    initTargetValue(target);

    showStepSeqTargetLabel();
    PTE->stepSeqLightPads();
}

static void functStepSeqEncoder(int16_t value)
{
    if (heldTarget == kTargetNone)
    {
        stepSeqStockEncoder(value);
        return;
    }

    switch (heldTarget)
    {
        case kTargetFx1:
        case kTargetFx2:
        {
            if (stepSeqShiftHeld)
            {
                // Shift + encoder: pick the effect, one list entry per detent (the input
                // processor boosts encoder steps while Shift is held, so use the sign only)
                int idx = clamp<int>(fxNameIdx + (value > 0 ? 1 : -1), 1, kSelectableFxCount - 1);
                if (idx == fxNameIdx)
                {
                    return;
                }
                fxNameIdx = idx;
                fxValue = sequencer.getFxDefault(interfaceGlobals.fxNameToId(fxNameIdx));
                PTE->setPreviousUsedFx(fxNameIdx);
            }
            else
            {
                uint8_t type = interfaceGlobals.fxNameToId(fxNameIdx);
                fxValue =
                  clamp<int>(fxValue + value, sequencer.getFxMin(type), sequencer.getFxMax(type));
            }
            PTE->setPreviousUsedFxValue(fxValue);
            break;
        }
        case kTargetMicro: microValue = clamp<int>(microValue + value, 0, 100); break;
        case kTargetVolume: volumeValue = clamp<int>(volumeValue + value, 0, 100); break;
        default: break;
    }

    // update the steps tapped during this hold, if still on the same window and track
    if (heldTapped != 0 && heldWindow == PTE->stepSeqWindowStart() &&
        heldTrack == PTE->trackerPattern.actualTrack)
    {
        Sequencer::strPattern::strTrack* trackData = &sequencer.getPatternToUI()->track[heldTrack];

        newFileManager.storePatternUndoRevision();
        for (uint8_t i = 0; i < cPatternEditor::kStepSeqSteps; i++)
        {
            if (heldTapped & (uint32_t(1) << i))
            {
                writeTarget(&trackData->step[heldWindow + i], heldTarget);
            }
        }
        newFileManager.setActualPatternStructChanged();
        PTE->refreshPattern();
    }

    showStepSeqTargetLabel();
}

#endif // STEP_SEQ_ENABLED
