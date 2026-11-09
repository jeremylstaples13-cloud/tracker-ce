#include "mtSliceManager.h"
#include "SliceFades.h"
#include "VoiceStreamManager.h"
#include "mtEffect.h"
#include "mtSliceDetector.h"
#include "string.h"
#include <algorithm>

mtSliceManager sliceManager;

static uint8_t instrumentIndex(const strInstrument* instr)
{
    int idx = (int)(instr - mtProject.instrument);
    return (idx >= 0 && idx < kSampleInstrumentsCount) ? (uint8_t)idx : UINT8_MAX;
}

static SliceFades::Fade selectedSliceFade(const strInstrument* instr)
{
    uint8_t idx = instrumentIndex(instr);
    if (idx == UINT8_MAX || instr->sliceNumber == 0)
        return { 0, 0 };
    return sliceFades.get(idx, instr->selectedSlice);
}

void mtSliceManager::addSlice(strInstrument* instr)
{
    if (instr->sliceNumber == MAX_SLICE_NUMBER)
        return;

    uint8_t idx = instrumentIndex(instr);
    sliceFades.prepareEdit(idx);
    addSliceInternal(instr);
    sliceFades.onSliceInserted(idx, instr->selectedSlice);
}

void mtSliceManager::removeSlice(strInstrument* instr)
{
    if ((instr->sliceNumber == 0) || (instr->sliceNumber <= instr->selectedSlice))
        return;

    uint8_t idx = instrumentIndex(instr);
    uint8_t removed = instr->selectedSlice;
    sliceFades.prepareEdit(idx);
    removeSliceInternal(instr);
    sliceFades.onSliceRemoved(idx, removed);
}

void mtSliceManager::adjustSlice(strInstrument* instr, int32_t val)
{
    uint8_t idx = instrumentIndex(instr);
    sliceFades.prepareEdit(idx);
    adjustSliceInternal(instr, val);
    sliceFades.onSlicesMoved(idx);
}

void mtSliceManager::addSliceInternal(strInstrument* instr)
{
    if (instr->sliceNumber == 0)
    {
        instr->sliceNumber++;
        instr->selectedSlice = 0;
        instr->slices[0] = 0;
        return;
    }
    if (instr->selectedSlice == (instr->sliceNumber - 1))
    {
        instr->selectedSlice++;
        instr->sliceNumber++;
        instr->slices[instr->selectedSlice] =
          (MAX_16BIT + instr->slices[instr->selectedSlice - 1]) / 2;
        return;
    }
    for (int8_t i = instr->sliceNumber; i > (instr->selectedSlice + 1); i--)
    {
        instr->slices[i] = instr->slices[i - 1];
    }
    instr->selectedSlice++;
    instr->slices[instr->selectedSlice] = ((uint32_t)(instr->slices[instr->selectedSlice - 1] +
                                                      instr->slices[instr->selectedSlice + 1])) /
                                          2;

    instr->sliceNumber++;
}
void mtSliceManager::removeSliceInternal(strInstrument* instr)
{
    if ((instr->sliceNumber - 1) == instr->selectedSlice)
    {
        instr->selectedSlice = instr->sliceNumber == 1 ? 0 : instr->selectedSlice - 1;
        instr->sliceNumber--;
        return;
    }

    for (uint8_t i = instr->selectedSlice; i < instr->sliceNumber - 1; i++)
    {
        instr->slices[i] = instr->slices[i + 1];
    }
    instr->sliceNumber--;
    if (instr->selectedSlice != 0)
        instr->selectedSlice--;
    else
        instr->selectedSlice = 0;
}
void mtSliceManager::adjustSliceInternal(strInstrument* instr, int32_t val)
{
    if ((instr->sliceNumber == 0) || (instr->selectedSlice >= instr->sliceNumber))
        return;

    if ((instr->selectedSlice != (instr->sliceNumber - 1)) && (instr->selectedSlice != 0))
    {
        if (instr->slices[instr->selectedSlice] + val >= instr->slices[instr->selectedSlice + 1])
            instr->slices[instr->selectedSlice] = instr->slices[instr->selectedSlice + 1] - 1;
        else if (instr->slices[instr->selectedSlice] + val <=
                 instr->slices[instr->selectedSlice - 1])
            instr->slices[instr->selectedSlice] = instr->slices[instr->selectedSlice - 1] + 1;
        else
            instr->slices[instr->selectedSlice] += val;
    }
    else if ((instr->selectedSlice == 0) && (instr->selectedSlice == (instr->sliceNumber - 1)))
    {
        if (instr->slices[instr->selectedSlice] + val > MAX_16BIT)
            instr->slices[instr->selectedSlice] = MAX_16BIT;
        else if (instr->slices[instr->selectedSlice] + val < 0)
            instr->slices[instr->selectedSlice] = 0;
        else
            instr->slices[instr->selectedSlice] += val;
    }
    else if (instr->selectedSlice == instr->sliceNumber - 1)
    {
        if (instr->slices[instr->selectedSlice] + val > MAX_16BIT)
            instr->slices[instr->selectedSlice] = MAX_16BIT;
        else if (instr->slices[instr->selectedSlice] + val <=
                 instr->slices[instr->selectedSlice - 1])
            instr->slices[instr->selectedSlice] = instr->slices[instr->selectedSlice - 1] + 1;
        else
            instr->slices[instr->selectedSlice] += val;
    }
    else if (instr->selectedSlice == 0)
    {
        if (instr->slices[instr->selectedSlice] + val >= instr->slices[instr->selectedSlice + 1])
            instr->slices[instr->selectedSlice] = instr->slices[instr->selectedSlice + 1] - 1;
        else if (instr->slices[instr->selectedSlice] + val < 0)
            instr->slices[instr->selectedSlice] = 0;
        else
            instr->slices[instr->selectedSlice] += val;
    }
}
void mtSliceManager::autoSlice(strInstrument* instr)
{
    // Detection runs asynchronously; the fades are re-applied in onAutoSliceFinished().
    pendingFadeInstr = instrumentIndex(instr);
    pendingFade = selectedSliceFade(instr);

    memset(instr->slices, 0, MAX_SLICE_NUMBER * 2);
    instr->selectedSlice = 0;

    int instrIdx = (int)(instr - mtProject.instrument);
    if (instrIdx >= 0 && instrIdx < kSampleInstrumentsCount &&
        mtProject.sampleMemory[instrIdx].isStreamed)
    {
        uint32_t maxFrames = std::min<uint32_t>(instr->sample.length, (uint32_t)EDIT_BUFF_SIZE);
        voiceStreamManager.readInstrumentFramesBlocking(
          instrIdx, 0, 0, maxFrames, externalRamEffectsBuff);
        sliceDetector.start(
          externalRamEffectsBuff, maxFrames, instr->slices, &instr->sliceNumber);
        return;
    }

    sliceDetector.start(
      instr->sample.address, instr->sample.length, instr->slices, &instr->sliceNumber);
}

void mtSliceManager::onAutoSliceFinished()
{
    if (pendingFadeInstr == UINT8_MAX)
        return;

    sliceFades.onSlicesRebuilt(pendingFadeInstr, pendingFade);
    pendingFadeInstr = UINT8_MAX;
}

void mtSliceManager::equalSlice(strInstrument* instr, uint8_t n)
{
    SliceFades::Fade fade = selectedSliceFade(instr);

    instr->sliceNumber = n;

    for (uint8_t i = 0; i < n; i++)
    {
        instr->slices[i] = (MAX_16BIT * i) / n;
    }

    sliceFades.onSlicesRebuilt(instrumentIndex(instr), fade);
}

uint8_t mtSliceManager::getAutoSliceProgress()
{
    return sliceDetector.getProgress();
}
uint8_t mtSliceManager::getAutoSliceState()
{
    return sliceDetector.getState();
}
