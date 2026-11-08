#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
class PatchStore;

// STNG: mono Karplus-Strong style string / pipe physical model (see StringSynthDsp.h)
std::unique_ptr<InstrumentType> createStringSynth();

// Writes the STNG factory presets (StringSynthPresets.h) as .patch files into the store.
void createStringSynthPresets(PatchStore& store);
} // namespace polysynth
