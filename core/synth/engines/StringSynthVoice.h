#pragma once

#include "PolySynth/sdk/InstrumentType.h"
#include <memory>

namespace polysynth
{
// STNG: mono Karplus-Strong style string / pipe physical model (see StringSynthDsp.h)
std::unique_ptr<InstrumentType> createStringSynth();
} // namespace polysynth
