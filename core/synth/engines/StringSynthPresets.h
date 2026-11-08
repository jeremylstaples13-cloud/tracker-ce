#pragma once

// STNG parameter ids and factory presets. No Tracker / PolySynth dependencies, so the same
// table is used by the firmware (StringSynthVoice.cpp writes them as .patch files) and by
// host-side render tests.

#include <initializer_list>

namespace polysynth::stng
{

// Patch parameter ids. Append only: saved patches store values by id.
enum class ParamId : int
{
    // Exciter
    kExciter = 0,
    kExcTone,
    kExcLength,
    kPosition,
    kVelToTone,
    // String
    kModel,
    kDecay,
    kBrightness,
    kStiffness,
    kRelease,
    // String 2
    kStr2Tune,
    kStr2Fine,
    kStr2Level,
    // Tone
    kDrive,
    kBody,
    kBodyFreq,
    // Modulation
    kDrift,
    kDriftRate,
    // Voice
    kTune,
    kFine,
    kGlide,
    kLegato,
    kVolume,
};

struct PresetValue
{
    ParamId id;
    float value;
};

// Values not listed keep the parameter default.
struct Preset
{
    const char* name;
    std::initializer_list<PresetValue> values;
};

// exciter / model values
constexpr float kPluck = 0, kMallet = 1, kClick = 2, kBow = 3, kReed = 4, kBrass = 5;
constexpr float kString = 0, kPipe = 1;

using P = ParamId;

// clang-format off
inline const Preset kPresets[] = {
    // Plucked strings ---------------------------------------------------------------------
    { "01 Nylon Pluck", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.55f }, { P::kExcLength, 6 }, { P::kPosition, 0.18f },
        { P::kDecay, 2.5f }, { P::kBrightness, 0.55f }, { P::kRelease, 0.3f },
        { P::kBody, 0.45f }, { P::kBodyFreq, 180 }, { P::kDrift, 0.1f } } },
    { "02 Steel Guitar", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.85f }, { P::kExcLength, 4 }, { P::kPosition, 0.12f },
        { P::kDecay, 4 }, { P::kBrightness, 0.8f }, { P::kStiffness, 0.08f },
        { P::kBody, 0.3f }, { P::kBodyFreq, 220 } } },
    { "03 Harp", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.5f }, { P::kExcLength, 10 }, { P::kPosition, 0.3f },
        { P::kDecay, 6 }, { P::kBrightness, 0.6f }, { P::kRelease, 1.2f }, { P::kBody, 0.2f } } },
    { "04 Koto", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.8f }, { P::kExcLength, 3 }, { P::kPosition, 0.08f },
        { P::kDecay, 2.2f }, { P::kBrightness, 0.75f }, { P::kStiffness, 0.15f },
        { P::kBody, 0.35f }, { P::kBodyFreq, 350 }, { P::kDrift, 0.25f } } },
    { "05 Twang Banjo", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.9f }, { P::kExcLength, 2 }, { P::kPosition, 0.1f },
        { P::kDecay, 1.2f }, { P::kBrightness, 0.9f }, { P::kBody, 0.7f }, { P::kBodyFreq, 450 },
        { P::kDrive, 0.15f } } },
    { "06 Muted Bass", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.3f }, { P::kExcLength, 8 }, { P::kPosition, 0.2f },
        { P::kDecay, 0.5f }, { P::kBrightness, 0.3f }, { P::kRelease, 0.1f },
        { P::kBody, 0.4f }, { P::kBodyFreq, 120 }, { P::kVolume, 1.6f } } },
    { "07 Pizzicato", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.6f }, { P::kExcLength, 5 }, { P::kPosition, 0.2f },
        { P::kDecay, 0.35f }, { P::kBrightness, 0.5f }, { P::kRelease, 0.15f },
        { P::kBody, 0.55f }, { P::kBodyFreq, 260 } } },
    { "08 12 String", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.8f }, { P::kExcLength, 4 }, { P::kPosition, 0.15f },
        { P::kDecay, 4 }, { P::kBrightness, 0.75f },
        { P::kStr2Tune, 12 }, { P::kStr2Fine, 4 }, { P::kStr2Level, 0.6f } } },
    { "09 Sitar Drone", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.9f }, { P::kExcLength, 3 }, { P::kPosition, 0.05f },
        { P::kDecay, 8 }, { P::kBrightness, 0.9f }, { P::kStiffness, 0.1f }, { P::kDrive, 0.35f },
        { P::kStr2Fine, 9 }, { P::kStr2Level, 0.5f }, { P::kDrift, 0.2f } } },

    // Mallets and percussion -------------------------------------------------------------
    { "10 Marimba", {
        { P::kExciter, kMallet }, { P::kExcTone, 0.45f }, { P::kExcLength, 4 }, { P::kModel, kPipe },
        { P::kDecay, 0.8f }, { P::kBrightness, 0.5f }, { P::kRelease, 0.5f },
        { P::kBody, 0.6f }, { P::kBodyFreq, 300 } } },
    { "11 Kalimba", {
        { P::kExciter, kMallet }, { P::kExcTone, 0.7f }, { P::kExcLength, 2 },
        { P::kDecay, 1.5f }, { P::kBrightness, 0.7f }, { P::kStiffness, 0.35f },
        { P::kBody, 0.3f }, { P::kBodyFreq, 600 } } },
    { "12 Glass Bell", {
        { P::kExciter, kMallet }, { P::kExcTone, 0.9f }, { P::kExcLength, 1 },
        { P::kDecay, 6 }, { P::kBrightness, 0.95f }, { P::kStiffness, 0.7f }, { P::kRelease, 2 },
        { P::kStr2Tune, 7 }, { P::kStr2Fine, 0 }, { P::kStr2Level, 0.4f }, { P::kBody, 0 } } },
    { "13 Steel Drum", {
        { P::kExciter, kMallet }, { P::kExcTone, 0.6f }, { P::kExcLength, 3 }, { P::kModel, kPipe },
        { P::kDecay, 1.8f }, { P::kBrightness, 0.65f }, { P::kStiffness, 0.45f },
        { P::kStr2Tune, 12 }, { P::kStr2Fine, -6 }, { P::kStr2Level, 0.35f }, { P::kDrift, 0.1f } } },
    { "14 Log Drum", {
        { P::kExciter, kClick }, { P::kExcTone, 0.4f }, { P::kModel, kPipe },
        { P::kDecay, 0.25f }, { P::kBrightness, 0.4f }, { P::kRelease, 0.2f },
        { P::kBody, 0.8f }, { P::kBodyFreq, 500 } } },
    { "15 Gamelan", {
        { P::kExciter, kMallet }, { P::kExcTone, 0.8f }, { P::kExcLength, 2 },
        { P::kDecay, 7 }, { P::kBrightness, 0.85f }, { P::kStiffness, 0.85f }, { P::kRelease, 2.5f },
        { P::kStr2Tune, 5 }, { P::kStr2Fine, 10 }, { P::kStr2Level, 0.6f }, { P::kDrift, 0.3f } } },
    { "16 Hand Drum", {
        { P::kExciter, kClick }, { P::kExcTone, 0.3f },
        { P::kDecay, 0.2f }, { P::kBrightness, 0.25f }, { P::kStiffness, 0.2f }, { P::kRelease, 0.15f },
        { P::kBody, 1 }, { P::kBodyFreq, 140 }, { P::kVolume, 1.6f } } },

    // Bowed -----------------------------------------------------------------------------
    { "17 Cello Bow", {
        { P::kExciter, kBow }, { P::kExcTone, 0.45f }, { P::kExcLength, 120 }, { P::kPosition, 0.12f },
        { P::kDecay, 3 }, { P::kBrightness, 0.55f }, { P::kRelease, 0.5f },
        { P::kBody, 0.5f }, { P::kBodyFreq, 200 }, { P::kDrift, 0.25f }, { P::kDriftRate, 0.6f } } },
    { "18 Violin Section", {
        { P::kExciter, kBow }, { P::kExcTone, 0.65f }, { P::kExcLength, 90 },
        { P::kDecay, 3 }, { P::kBrightness, 0.7f }, { P::kRelease, 0.7f },
        { P::kStr2Fine, 12 }, { P::kStr2Level, 0.7f },
        { P::kBody, 0.4f }, { P::kBodyFreq, 400 }, { P::kDrift, 0.35f } } },
    { "19 Glass Harmonica", {
        { P::kExciter, kBow }, { P::kExcTone, 0.9f }, { P::kExcLength, 150 },
        { P::kDecay, 8 }, { P::kBrightness, 0.95f }, { P::kStiffness, 0.5f }, { P::kRelease, 1.5f },
        { P::kBody, 0 }, { P::kDrift, 0.15f } } },
    { "20 Bowed Metal", {
        { P::kExciter, kBow }, { P::kExcTone, 0.75f }, { P::kExcLength, 60 },
        { P::kDecay, 10 }, { P::kBrightness, 0.85f }, { P::kStiffness, 0.9f }, { P::kRelease, 2 },
        { P::kDrive, 0.2f }, { P::kStr2Tune, -12 }, { P::kStr2Level, 0.4f } } },

    // Pipes (bow / breath into the pipe model) ---------------------------------------------
    { "21 Breathy Flute", {
        { P::kExciter, kBow }, { P::kExcTone, 0.7f }, { P::kExcLength, 80 }, { P::kModel, kPipe },
        { P::kDecay, 2 }, { P::kBrightness, 0.65f }, { P::kRelease, 0.25f },
        { P::kBody, 0 }, { P::kDrift, 0.2f } } },
    { "22 Pan Pipe", {
        { P::kExciter, kBow }, { P::kExcTone, 0.5f }, { P::kExcLength, 40 }, { P::kModel, kPipe },
        { P::kDecay, 1.5f }, { P::kBrightness, 0.45f }, { P::kRelease, 0.15f },
        { P::kBody, 0.2f }, { P::kDrift, 0.3f } } },
    { "23 Wind Bottle", {
        { P::kExciter, kBow }, { P::kExcTone, 0.3f }, { P::kExcLength, 150 }, { P::kModel, kPipe },
        { P::kDecay, 4 }, { P::kBrightness, 0.3f }, { P::kRelease, 0.6f },
        { P::kBody, 0 }, { P::kDrift, 0.45f }, { P::kDriftRate, 1.5f }, { P::kVolume, 2.0f } } },

    // Reeds -----------------------------------------------------------------------------
    { "24 Clarinet", {
        { P::kExciter, kReed }, { P::kExcTone, 0.3f }, { P::kExcLength, 25 },
        { P::kBrightness, 0.55f }, { P::kRelease, 0.12f }, { P::kBody, 0 }, { P::kDrift, 0.1f } } },
    { "25 Tenor Reed", {
        { P::kExciter, kReed }, { P::kExcTone, 0.8f }, { P::kExcLength, 20 },
        { P::kBrightness, 0.75f }, { P::kRelease, 0.15f }, { P::kDrive, 0.25f },
        { P::kBody, 0.3f }, { P::kBodyFreq, 500 }, { P::kDrift, 0.15f }, { P::kVolume, 1.8f } } },
    { "26 Reed Organ", {
        { P::kExciter, kReed }, { P::kExcTone, 0.5f }, { P::kExcLength, 30 },
        { P::kBrightness, 0.6f }, { P::kRelease, 0.2f }, { P::kBody, 0 },
        { P::kStr2Tune, 12 }, { P::kStr2Fine, 3 }, { P::kStr2Level, 0.5f }, { P::kDrift, 0.05f } } },

    // Brass (most stable from about C3 up) -----------------------------------------------
    { "27 Soft Horn", {
        { P::kExciter, kBrass }, { P::kExcTone, 0.4f }, { P::kExcLength, 40 },
        { P::kBrightness, 0.5f }, { P::kRelease, 0.2f }, { P::kBody, 0.2f }, { P::kDrift, 0.1f } } },
    { "28 Bright Trumpet", {
        { P::kExciter, kBrass }, { P::kExcTone, 0.9f }, { P::kExcLength, 25 },
        { P::kBrightness, 0.85f }, { P::kRelease, 0.12f }, { P::kDrive, 0.2f }, { P::kBody, 0 } } },
    { "29 Brass Section", {
        { P::kExciter, kBrass }, { P::kExcTone, 0.7f }, { P::kExcLength, 35 },
        { P::kBrightness, 0.7f }, { P::kRelease, 0.25f }, { P::kBody, 0 },
        { P::kStr2Fine, 10 }, { P::kStr2Level, 0.8f }, { P::kDrift, 0.25f } } },

    // Sound design starting points ------------------------------------------------------
    { "30 Drone String", {
        { P::kExciter, kBow }, { P::kExcTone, 0.5f }, { P::kExcLength, 200 },
        { P::kDecay, 30 }, { P::kBrightness, 0.6f }, { P::kStiffness, 0.2f }, { P::kRelease, 3 },
        { P::kStr2Tune, 7 }, { P::kStr2Level, 0.6f },
        { P::kBody, 0.3f }, { P::kDrift, 0.6f }, { P::kDriftRate, 0.2f } } },
    { "31 Slide Legato", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.7f }, { P::kExcLength, 5 },
        { P::kDecay, 6 }, { P::kBrightness, 0.7f }, { P::kRelease, 0.5f },
        { P::kGlide, 0.25f }, { P::kLegato, 1 }, { P::kDrift, 0.2f } } },
    { "32 Fuzz String", {
        { P::kExciter, kPluck }, { P::kExcTone, 0.9f }, { P::kExcLength, 4 },
        { P::kDecay, 3 }, { P::kBrightness, 0.8f }, { P::kDrive, 0.9f },
        { P::kStr2Fine, 15 }, { P::kStr2Level, 0.3f } } },
};
// clang-format on

} // namespace polysynth::stng
