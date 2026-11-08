// Physical step-sequencer mode for the pattern editor.
//
// Pad layout (pads are numbered row * 12 + column, 4 rows x 12 columns):
//   columns 0-7  : step grid, row r / column c -> step (window + r * 8 + c), 32 steps per window
//   columns 8-11 : instrument picker, 16 instruments, Shift shows the next 16
//
// Steps are written to the track under the cursor. Tapping an empty step writes the last
// entered note with the selected instrument, tapping a filled step clears it.
// Up/Down move the 32-step window, Left/Right change track, screen button 8 exits.
//
// Edits go through the regular Sequencer::setSelection* / clearSelected paths so undo,
// "pattern changed" flags and the pattern file format (version 6) are unchanged.

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

static void functStepSeqPads(uint8_t pad, uint8_t state, int16_t velo);
static void functStepSeqShift(uint8_t state);
static void functStepSeqUp();
static void functStepSeqDown();
static void functStepSeqLeft();
static void functStepSeqRight();
static void functStepSeqParamButton(uint8_t state);

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

    // Note/Instr/Vol/Fx: no column switching or popups in step mode.
    // Holding Fx buttons to apply effects to tapped steps comes next.
    ip->setButtonObj(interfaceButtonNote, functStepSeqParamButton);
    ip->setButtonObj(interfaceButtonInstr, functStepSeqParamButton);
    ip->setButtonObj(interfaceButtonVol, functStepSeqParamButton);
    ip->setButtonObj(interfaceButtonFx, functStepSeqParamButton);
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

        uint8_t level = 0;
        if (!pattern->track[track].step[step].isNoteEmpty())
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
static void stepSeqToggleStep(uint16_t step)
{
    const uint8_t track = PTE->trackerPattern.actualTrack;
    Sequencer::strPattern* pattern = sequencer.getPatternToUI();
    Sequencer::strPattern::strTrack::strStep* stepData = &pattern->track[track].step[step];

    sequencer.setSelection(step, track, step, track);

    if (stepData->isNoteEmpty())
    {
        // writes lastUsedInstrument too, stores undo and marks the pattern changed
        sequencer.setSelectionNote(mtProject.values.lastUsedNote);
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

        stepSeqToggleStep(step);
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

static void functStepSeqParamButton(uint8_t state)
{
    (void)state;
}

#endif // STEP_SEQ_ENABLED
