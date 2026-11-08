#include "config.h"
#ifdef FEATURE_SYNTH

#include "StringSynthVoice.h"
#include "PolySynth/sdk/SynthVoiceSDK.h"
#include "StringSynthDsp.h"

#include <cr_section_macros.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace polysynth
{

namespace
{

enum class StngParamId : int
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

constexpr std::array<const char*, 4> kExciterNames = { "Pluck", "Mallet", "Click", "Bow" };
constexpr std::array<const char*, 2> kModelNames = { "String", "Pipe" };
constexpr std::array<const char*, 2> kOffOnNames = { "Off", "On" };

// Delay lines live in EXTERNAL_RAM: one block per voice. STNG is mono, so at most one voice
// per synth slot (3), plus one spare for a voice being replaced while a new one is created.
// Each block is stng::kFloatsPerVoice floats (32 KB), 128 KB in total.
constexpr int kPoolVoices = 4;
__NOINIT(EXTERNAL_RAM) float stngDelayPool[kPoolVoices][stng::kFloatsPerVoice];
std::array<std::atomic<uint8_t>, kPoolVoices> stngDelayPoolUsed{};

int acquirePoolSlot()
{
    for (int i = 0; i < kPoolVoices; i++)
    {
        uint8_t expected = 0;
        if (stngDelayPoolUsed[i].compare_exchange_strong(expected, 1))
        {
            return i;
        }
    }
    return -1;
}

auto msFormatter()
{
    return [](const Patch& /*patch*/, Patch::paramvalue_t value)
    { return value < 10.0f ? fmt::format("{:.1f} ms", value) : fmt::format("{:.0f} ms", value); };
}

template<size_t N>
auto namesFormatter(const std::array<const char*, N>& names)
{
    return [&names](const Patch& /*patch*/, Patch::paramvalue_t value)
    { return std::string(names[std::clamp<int>(std::lround(value), 0, N - 1)]); };
}

} // namespace

class StringSynthVoice : public sdk::CustomSynthVoice<StringSynthVoice, StngParamId>
{
  public:
    StringSynthVoice() { tryAttach(); }

    ~StringSynthVoice() override
    {
        if (poolSlot >= 0)
        {
            stngDelayPoolUsed[poolSlot].store(0);
        }
    }

    bool isActive() final { return dsp.isPlaying(); }

    void onNoteOn(const InstrumentVoiceContext& /*ctx*/,
                  note_t note,
                  float velocity,
                  bool isLegatoStep) final
    {
        if (!dsp.isAttached())
        {
            tryAttach(); // pool was full when this voice was created
        }
        if (dsp.isAttached())
        {
            dsp.noteOn(static_cast<float>(note), velocity, isLegatoStep, params);
        }
    }

    void onNoteOff(const InstrumentVoiceContext& /*ctx*/, note_t /*note*/) final { dsp.noteOff(); }

    void renderVoiceAudio(const InstrumentVoiceContext& /*ctx*/,
                          std::optional<AudioBufferStereoRef> /*inputBuffer*/,
                          AudioBufferStereoRef outputBuffer,
                          std::span<AudioBufferRef> /*auxOutBuffer*/) final
    {
        auto out = outputBuffer[0];
        dsp.render(params, pitchBendSemitones, out.data(), static_cast<int>(out.size()));
    }

    static void createMetadata(const Patch& /*patch*/, Patch::Metadata& metadata)
    {
        using namespace polysynth::sdk;
        using Curve = Patch::Metadata::Param::Curve;

        metadata.params.resize(enumutil::EnumMax<StngParamId>::value + 1);
        metadata.params.shrink_to_fit();

        if (paramHandlers.empty())
        {
            paramHandlers.resize(enumutil::EnumMax<StngParamId>::value + 1);
        }

        ParamHelper<StringSynthVoice> helper(metadata, paramHandlers);

        // Exciter -----------------------------------------------------------------------
        helper.addParam(
          *StngParamId::kExciter,
          {
            .displayName = "Exciter",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kExciterNames.size() - 1),
            .isEnum = true,
            .midiCC = *MidiCCs::kOscParam0,
            .valueFormatter = namesFormatter(kExciterNames),
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          {
              voice.params.exciter = static_cast<stng::Exciter>(
                std::clamp<int>(std::lround(*val), 0, kExciterNames.size() - 1));
          });

        helper.addParam(
          *StngParamId::kExcTone,
          {
            .displayName = "Exciter Tone",
            .defaultValue = 0.7f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 1,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.excTone = *val; });

        helper.addParam(
          *StngParamId::kExcLength,
          {
            .displayName = "Exciter Length",
            .defaultValue = 8.0f,
            .minValue = 0.5f,
            .maxValue = 200.0f,
            .editCurve = Curve::kExponential,
            .midiCC = *MidiCCs::kOscParam0 + 2,
            .valueFormatter = msFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.excLengthMs = *val; });

        helper.addParam(
          *StngParamId::kPosition,
          {
            .displayName = "Position",
            .defaultValue = 0.15f,
            .minValue = 0,
            .maxValue = 0.5f,
            .midiCC = *MidiCCs::kOscParam0 + 3,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.position = *val; });

        helper.addParam(
          *StngParamId::kVelToTone,
          {
            .displayName = "Velocity > Tone",
            .defaultValue = 0.5f,
            .minValue = 0,
            .maxValue = 1,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.velToTone = *val; });

        // String ------------------------------------------------------------------------
        helper.addParam(
          *StngParamId::kModel,
          {
            .displayName = "Model",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = static_cast<Patch::paramvalue_short_t>(kModelNames.size() - 1),
            .isEnum = true,
            .midiCC = *MidiCCs::kOscParam0 + 4,
            .valueFormatter = namesFormatter(kModelNames),
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          {
              voice.params.model = static_cast<stng::Model>(
                std::clamp<int>(std::lround(*val), 0, kModelNames.size() - 1));
          });

        helper.addParam(
          *StngParamId::kDecay,
          {
            .displayName = "Decay",
            .defaultValue = 3.0f,
            .minValue = 0.05f,
            .maxValue = 30.0f,
            .editCurve = Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvDecay,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.decaySec = *val; });

        helper.addParam(
          *StngParamId::kBrightness,
          {
            .displayName = "Brightness",
            .defaultValue = 0.7f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kFilterCutoff,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.brightness = *val; });

        helper.addParam(
          *StngParamId::kStiffness,
          {
            .displayName = "Stiffness",
            .defaultValue = 0.0f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 5,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.stiffness = *val; });

        helper.addParam(
          *StngParamId::kRelease,
          {
            .displayName = "Release",
            .defaultValue = 0.4f,
            .minValue = 0.02f,
            .maxValue = 10.0f,
            .editCurve = Curve::kExponential,
            .midiCC = *MidiCCs::kAmpEnvRelease,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.releaseSec = *val; });

        // String 2 ----------------------------------------------------------------------
        helper.addParam(
          *StngParamId::kStr2Tune,
          {
            .displayName = "String 2 Tune",
            .defaultValue = 0.0f,
            .minValue = -24,
            .maxValue = 24,
            .midiCC = *MidiCCs::kOscParam0 + 6,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          { voice.params.res2Semitones = std::round(*val); });

        helper.addParam(
          *StngParamId::kStr2Fine,
          {
            .displayName = "String 2 Fine",
            .defaultValue = 7.0f,
            .minValue = -50,
            .maxValue = 50,
            .midiCC = *MidiCCs::kOscParam0 + 7,
            .valueFormatter = createCentsFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.res2Cents = *val; });

        helper.addParam(
          *StngParamId::kStr2Level,
          {
            .displayName = "String 2 Level",
            .defaultValue = 0.0f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 8,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.res2Level = *val; });

        // Tone --------------------------------------------------------------------------
        helper.addParam(
          *StngParamId::kDrive,
          {
            .displayName = "Drive",
            .defaultValue = 0.0f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 9,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.drive = *val; });

        helper.addParam(
          *StngParamId::kBody,
          {
            .displayName = "Body",
            .defaultValue = 0.25f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kOscParam0 + 10,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.body = *val; });

        helper.addParam(
          *StngParamId::kBodyFreq,
          {
            .displayName = "Body Freq",
            .defaultValue = 220.0f,
            .minValue = 60.0f,
            .maxValue = 2000.0f,
            .editCurve = Curve::kExponential,
            .macroOperation = Patch::Metadata::Param::MacroOperation::kMult,
            .midiCC = *MidiCCs::kOscParam0 + 11,
            .valueFormatter = createFrequencyFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.bodyFreqHz = *val; });

        // Modulation --------------------------------------------------------------------
        helper.addParam(
          *StngParamId::kDrift,
          {
            .displayName = "Drift",
            .defaultValue = 0.15f,
            .minValue = 0,
            .maxValue = 1,
            .midiCC = *MidiCCs::kLfo1Frequency,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.drift = *val; });

        helper.addParam(
          *StngParamId::kDriftRate,
          {
            .displayName = "Drift Rate",
            .defaultValue = 0.7f,
            .minValue = 0.05f,
            .maxValue = 8.0f,
            .editCurve = Curve::kExponential,
            .midiCC = *MidiCCs::kLfo2Frequency,
            .valueFormatter = createFrequencyFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.driftRateHz = *val; });

        // Voice -------------------------------------------------------------------------
        helper.addParam(
          *StngParamId::kTune,
          {
            .displayName = "Tune",
            .defaultValue = 0.0f,
            .minValue = -24,
            .maxValue = 24,
            .valueFormatter = createSemitoneFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          { voice.params.tuneSemitones = std::round(*val); });

        helper.addParam(
          *StngParamId::kFine,
          {
            .displayName = "Fine",
            .defaultValue = 0.0f,
            .minValue = -100,
            .maxValue = 100,
            .valueFormatter = createCentsFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.fineCents = *val; });

        helper.addParam(
          *StngParamId::kGlide,
          {
            .displayName = "Glide Time",
            .defaultValue = 0.0f,
            .minValue = 0.0f,
            .maxValue = 2.0f,
            .editCurve = Curve::kExponential,
            .valueFormatter = createTimeFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.glideSec = *val; });

        helper.addParam(
          *StngParamId::kLegato,
          {
            .displayName = "Legato",
            .defaultValue = 0,
            .minValue = 0,
            .maxValue = 1,
            .isEnum = true,
            .valueFormatter = namesFormatter(kOffOnNames),
          },
          [](auto& voice, Patch::optional_paramvalue_t val)
          { voice.playMode = std::lround(*val) ? PlayMode::kLegato : PlayMode::kAlways; });

        helper.addParam(
          *StngParamId::kVolume,
          {
            .displayName = "Volume",
            .defaultValue = 1.0f,
            .minValue = 0.0f,
            .maxValue = 2.0f,
            .macroOperation = Patch::Metadata::Param::MacroOperation::kMult,
            .valueFormatter = createPercentFormatter(),
          },
          [](auto& voice, Patch::optional_paramvalue_t val) { voice.params.gain = *val; });

        auto param = [](StngParamId id)
        { return EditorElement{ .type = EditorElement::Type::kParam, .id = *id }; };

        // Section names contain the keyword the patch editor uses to pick each tile's
        // icon and label (Osc, Filter, Mixer, Shp, Mod, Voice).
        metadata.layoutFlat = Patch::Metadata::EditorLayout{
            .sections = {
              {
                .name = "Exciter Oscillator",
                .shortName = "Exc",
                .elements = { param(StngParamId::kExciter),
                              param(StngParamId::kExcTone),
                              param(StngParamId::kExcLength),
                              param(StngParamId::kPosition),
                              param(StngParamId::kVelToTone) },
              },
              {
                .name = "String Filter",
                .shortName = "String",
                .elements = { param(StngParamId::kModel),
                              param(StngParamId::kDecay),
                              param(StngParamId::kBrightness),
                              param(StngParamId::kStiffness),
                              param(StngParamId::kRelease) },
              },
              {
                .name = "String 2 Mixer",
                .shortName = "Str 2",
                .elements = { param(StngParamId::kStr2Tune),
                              param(StngParamId::kStr2Fine),
                              param(StngParamId::kStr2Level) },
              },
              {
                .name = "Tone Shaper",
                .shortName = "Tone",
                .elements = { param(StngParamId::kDrive),
                              param(StngParamId::kBody),
                              param(StngParamId::kBodyFreq) },
              },
              {
                .name = "Drift Modulation",
                .shortName = "Drift",
                .elements = { param(StngParamId::kDrift), param(StngParamId::kDriftRate) },
              },
              {
                .name = "Voice",
                .shortName = "Voice",
                .elements = { param(StngParamId::kTune),
                              param(StngParamId::kFine),
                              param(StngParamId::kGlide),
                              param(StngParamId::kLegato),
                              param(StngParamId::kVolume) },
              },
            },
        };

        metadata.layout23 = metadata.layoutFlat;

        metadata.initDefaultMacros = [](auto& macros)
        {
            macros[0] = Patch::Metadata::Macro{
                .displayName = "Brightness",
                .modifiers = { { *StngParamId::kBrightness, 0.6f },
                               { *StngParamId::kExcTone, 0.4f } },
                .bipolar = true,
            };
            macros[1] = Patch::Metadata::Macro{
                .displayName = "Decay",
                .modifiers = { { *StngParamId::kDecay, 0.8f }, { *StngParamId::kRelease, 0.5f } },
                .bipolar = true,
            };
            macros[2] = Patch::Metadata::Macro{
                .displayName = "Position",
                .modifiers = { { *StngParamId::kPosition, 1.0f } },
                .bipolar = true,
            };
            macros[3] = Patch::Metadata::Macro{
                .displayName = "Stiffness / Metal",
                .modifiers = { { *StngParamId::kStiffness, 0.8f } },
                .bipolar = false,
            };
            macros[4] = Patch::Metadata::Macro{
                .displayName = "Body / Wood",
                .modifiers = { { *StngParamId::kBody, 0.8f } },
                .bipolar = false,
            };
            macros[5] = Patch::Metadata::Macro{
                .displayName = "Drive",
                .modifiers = { { *StngParamId::kDrive, 0.8f } },
                .bipolar = false,
            };
            // Velocity already drives the exciter level and tone inside the model
        };

        metadata.instrumentTypeId = static_cast<int>(InstrumentTypeId::kStringSynth);
        metadata.outputChannels = Patch::Metadata::OutputChannels::kMono;
        metadata.maxVoices = 1; // mono
    }

  private:
    void tryAttach()
    {
        int slot = acquirePoolSlot();
        if (slot >= 0)
        {
            poolSlot = slot;
            dsp.attach(stngDelayPool[slot]);
        }
    }

    stng::Params params;
    stng::StringVoiceDsp dsp;
    int poolSlot = -1;
};

std::unique_ptr<InstrumentType> createStringSynth()
{
    return std::make_unique<InstrumentType>(
      InstrumentType{ .name = "STNG",
                      .createMetadata = &StringSynthVoice::createMetadata,
                      .createVoice = [](int /*voiceIdx*/) -> std::unique_ptr<InstrumentVoice>
                      { return std::make_unique<StringSynthVoice>(); } });
}

} // namespace polysynth

#endif
