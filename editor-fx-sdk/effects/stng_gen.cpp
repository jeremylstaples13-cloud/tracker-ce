// PTFX_NAME: STNG Generator
// PTFX_AUTHOR: JL
// PTFX_FLAGS: GENERATOR, CONVERT_TO_MONO, IMX_ONLY
// PTFX_SCRATCH: 140000

// STNG string / pipe physical model as a Sample Editor generator: renders one note (or a chord /
// unison stack of up to 4 voices) of any of the 32 STNG presets into the instrument.
//
// Page 1: Preset, Note, Velocity, Hold (time until note off; the preset's release follows)
// Page 2: Length (total sample length), Chord, Decay (x preset), Brightness (+/- preset)
// Page 3: Exciter and Model (Preset = keep the preset's), Position and Body (+/- preset)
//
// The sample ends at Length, or earlier once the sound has died away. Long lengths need firmware
// that takes the generator length from set_extra_samples(); other firmware stops at 2 s.
//
// Freestanding C++: no standard library, no static constructors, no pointer tables (the binary is
// linked at address 0 and not relocated). All state lives in scratch RAM.

#define STNG_PTFX 1
#include "tracker_plugin.h"

#include "stng/StringSynthPresets.h"

using namespace polysynth::stng;

// clang-format off
const PluginParam PLUGIN_PARAMS[12] = {
    PTFX_PARAM_LIST("Preset", 0,
        "Nylon|Steel Gtr|Harp|Koto|Banjo|Muted Bass|Pizzicato|12 String|Sitar|Marimba|"
        "Kalimba|Glass Bell|Steel Drum|Log Drum|Gamelan|Hand Drum|Cello Bow|Violins|Glass Harm|"
        "Bowed Metl|Flute|Pan Pipe|Bottle|Clarinet|Tenor Reed|Reed Organ|Soft Horn|Trumpet|"
        "Brass Sect|Drone Str|Slide Str|Fuzz Str"),
    PTFX_PARAM_LIST("Note", 36,
        "C2|C#2|D2|D#2|E2|F2|F#2|G2|G#2|A2|A#2|B2|"
        "C3|C#3|D3|D#3|E3|F3|F#3|G3|G#3|A3|A#3|B3|"
        "C4|C#4|D4|D#4|E4|F4|F#4|G4|G#4|A4|A#4|B4|"
        "C5|C#5|D5|D#5|E5|F5|F#5|G5|G#5|A5|A#5|B5|"
        "C6|C#6|D6|D#6|E6|F6|F#6|G6|G#6|A6|A#6|B6|"
        "C7|C#7|D7|D#7|E7|F7|F#7|G7|G#7|A7|A#7|B7|C8"),
    PTFX_PARAM_INT("Velo", 1.0f, 127.0f, 100.0f, 1.0f, ""),
    PTFX_PARAM_FLOAT("Hold", 0.05f, 20.0f, 3.0f, 0.05f, " s"),

    PTFX_PARAM_FLOAT("Length", 0.1f, 20.0f, 4.0f, 0.1f, " s"),
    PTFX_PARAM_LIST("Chord", 0,
        "Single|Octave|Fifth|Major|Minor|Sus4|Maj7|Min7|Unison 2|Unison 3|Unison 4"),
    PTFX_PARAM_FLOAT("Decay", 0.25f, 4.0f, 1.0f, 0.05f, " x"),
    PTFX_PARAM_FLOAT("Bright", -1.0f, 1.0f, 0.0f, 0.05f, ""),

    PTFX_PARAM_LIST("Exciter", 0, "Preset|Pluck|Mallet|Click|Bow|Reed|Brass"),
    PTFX_PARAM_LIST("Model", 0, "Preset|String|Pipe"),
    PTFX_PARAM_FLOAT("Pos", -0.5f, 0.5f, 0.0f, 0.01f, ""),
    PTFX_PARAM_FLOAT("Body", -1.0f, 1.0f, 0.0f, 0.05f, ""),
};
// clang-format on

namespace
{

constexpr int kFirstNote = 24; // "Note" list index 0 = C2 (Tracker naming, C5 = note 60)
constexpr int kMaxVoices = 4;
constexpr int kBlock = 128; // same block size as the PolySynth engine the presets were made on
constexpr float kEndFadeSec = 0.01f;
constexpr float kKnee = 0.9f; // soft limit above this (single notes stay below it)

enum Param
{
    kPreset = 0,
    kNote,
    kVelo,
    kHold,
    kLength,
    kChord,
    kDecay,
    kBright,
    kExciterOverride,
    kModelOverride,
    kPos,
    kBody,
    kParamCount
};

struct ChordShape
{
    int8_t count;
    int8_t semitones[kMaxVoices];
    int8_t cents[kMaxVoices];
};

constexpr ChordShape kChords[] = {
    { 1, { 0 }, { 0 } },                                 // Single
    { 2, { 0, 12 }, { 0, 0 } },                          // Octave
    { 2, { 0, 7 }, { 0, 0 } },                           // Fifth
    { 3, { 0, 4, 7 }, { 0, 0, 0 } },                     // Major
    { 3, { 0, 3, 7 }, { 0, 0, 0 } },                     // Minor
    { 3, { 0, 5, 7 }, { 0, 0, 0 } },                     // Sus4
    { 4, { 0, 4, 7, 11 }, { 0, 0, 0, 0 } },              // Maj7
    { 4, { 0, 3, 7, 10 }, { 0, 0, 0, 0 } },              // Min7
    { 2, { 0, 0 }, { -6, 6 } },                          // Unison 2
    { 3, { 0, 0, 0 }, { -9, 0, 9 } },                    // Unison 3
    { 4, { 0, 0, 0, 0 }, { -11, -4, 4, 11 } },           // Unison 4
};
constexpr int kChordCount = static_cast<int>(sizeof(kChords) / sizeof(kChords[0]));

// 1 / sqrt(voices): keeps chords at a similar loudness to a single note
constexpr float kVoiceGain[kMaxVoices + 1] = { 1.0f, 1.0f, 0.7071f, 0.5774f, 0.5f };

struct State
{
    StringVoiceDsp voice[kMaxVoices];
    float delay[kMaxVoices][kFloatsPerVoice];
    Params params[kMaxVoices];
    float mix[kBlock];
    float tmp[kBlock];
    int16_t pcm[kBlock]; // rendered block not yet handed to the host
    uint32_t pcmLen;
    uint32_t pcmRead;
    int voices;
    uint32_t pos;
    uint32_t holdSamples;
    uint32_t lengthSamples;
    bool started;
    bool released;
    bool finished;
};
static_assert(sizeof(State) <= 140000, "raise PTFX_SCRATCH");

State* s_state = nullptr;
float s_param[kParamCount];

int toIndex(float v)
{
    return static_cast<int>(v + 0.5f);
}

uint32_t secondsToSamples(float sec)
{
    return static_cast<uint32_t>(sec * kSampleRate + 0.5f);
}

void updateLength()
{
    if (m::host == nullptr)
        return;

    // Generators: the appended sample count is the total length (firmware with long generator
    // support; other firmware ignores it and renders 2 s).
    m::host->set_extra_samples(0, secondsToSamples(s_param[kLength]));
}

void startVoices(State& st)
{
    const int presetIdx = m::clamp(toIndex(s_param[kPreset]), 0, kPresetCount - 1);
    Params p = presetParams(presetIdx);

    p.decaySec = m::clamp(p.decaySec * s_param[kDecay], 0.05f, 60.0f);
    p.brightness = m::clamp(p.brightness + s_param[kBright], 0.0f, 1.0f);
    p.position = m::clamp(p.position + s_param[kPos], 0.0f, 0.5f);
    p.body = m::clamp(p.body + s_param[kBody], 0.0f, 1.0f);
    p.glideSec = 0.0f; // single notes: nothing to glide from

    const int exciter = toIndex(s_param[kExciterOverride]);
    if (exciter > 0)
        p.exciter = static_cast<Exciter>(m::min(exciter - 1, static_cast<int>(Exciter::kCount) - 1));
    const int model = toIndex(s_param[kModelOverride]);
    if (model > 0)
        p.model = static_cast<Model>(m::min(model - 1, static_cast<int>(Model::kCount) - 1));

    const ChordShape& chord = kChords[m::clamp(toIndex(s_param[kChord]), 0, kChordCount - 1)];
    const float note = static_cast<float>(kFirstNote + m::clamp(toIndex(s_param[kNote]), 0, 72));
    const float velocity = m::clamp(s_param[kVelo] / 127.0f, 0.0f, 1.0f);

    st.voices = chord.count;
    for (int v = 0; v < st.voices; v++)
    {
        st.params[v] = p;
        st.params[v].fineCents += static_cast<float>(chord.cents[v]);
        if (v > 0)
        {
            st.voice[v].setSeed(0x2545F491u + 0x9E3779B9u * static_cast<uint32_t>(v));
        }
        st.voice[v].noteOn(note + static_cast<float>(chord.semitones[v]), velocity, false, st.params[v]);
    }

    st.holdSamples = secondsToSamples(s_param[kHold]);
    st.lengthSamples = secondsToSamples(s_param[kLength]);
    st.started = true;
}

} // namespace

#ifndef STNG_HOST_TEST
// Compiler-generated struct copies may call these; there is no C library to provide them.
extern "C" __attribute__((visibility("hidden"))) void* memcpy(void* dst, const void* src, size_t n)
{
    uint8_t* d = static_cast<uint8_t*>(dst);
    const uint8_t* s = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

extern "C" __attribute__((visibility("hidden"))) void* memset(void* dst, int c, size_t n)
{
    uint8_t* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i++)
        d[i] = static_cast<uint8_t>(c);
    return dst;
}
#endif

extern "C" void plugin_init(void* scratch_mem, uint32_t scratch_size, const PtfxHostApi* host)
{
    m::host = host;
    s_state = (scratch_mem != nullptr && scratch_size >= sizeof(State)) ? static_cast<State*>(scratch_mem)
                                                                        : nullptr;
    if (s_state == nullptr)
        return;

    State& st = *s_state;
    for (int v = 0; v < kMaxVoices; v++)
    {
        st.voice[v] = StringVoiceDsp();
        st.voice[v].attach(st.delay[v]);
    }
    st.voices = 0;
    st.pcmLen = 0;
    st.pcmRead = 0;
    st.pos = 0;
    st.started = false;
    st.released = false;
    st.finished = false;

    updateLength();
}

extern "C" void plugin_set_param(uint8_t index, float value)
{
    if (index >= kParamCount)
        return;

    s_param[index] = value;
    if (index == kLength)
        updateLength();
}

// Renders the next DSP block (note off and the sample end split blocks, the host's buffer size
// never does, so the result doesn't depend on how the firmware chunks the calls).
// Returns false once the sample is complete.
static bool renderBlock(State& st)
{
    if (st.finished || st.pos >= st.lengthSamples)
        return false;

    if (!st.released && st.pos >= st.holdSamples)
    {
        for (int v = 0; v < st.voices; v++)
            st.voice[v].noteOff();
        st.released = true;
    }

    uint32_t n = kBlock;
    n = m::min(n, st.lengthSamples - st.pos);
    if (!st.released)
        n = m::min(n, st.holdSamples - st.pos);

    bool anyPlaying = false;
    m::zero(st.mix, static_cast<int>(n));
    for (int v = 0; v < st.voices; v++)
    {
        if (!st.voice[v].isPlaying())
            continue;
        anyPlaying = true;
        st.voice[v].render(st.params[v], 0.0f, st.tmp, static_cast<int>(n));
        for (uint32_t i = 0; i < n; i++)
            st.mix[i] += st.tmp[i];
    }

    if (!anyPlaying)
    {
        // everything has died away: the sample ends here
        st.finished = true;
        return false;
    }

    const uint32_t fadeSamples = secondsToSamples(kEndFadeSec);
    const float gain = kVoiceGain[st.voices];
    for (uint32_t i = 0; i < n; i++)
    {
        float s = st.mix[i] * gain;
        const float a = m::fabs(s);
        if (a > kKnee)
        {
            // slope 1 at the knee, approaches full scale: big chords don't clip
            const float limited = kKnee + (1.0f - kKnee) * fastTanh((a - kKnee) / (1.0f - kKnee));
            s = (s < 0.0f) ? -limited : limited;
        }
        const uint32_t toEnd = st.lengthSamples - (st.pos + i);
        if (toEnd < fadeSamples)
            s *= static_cast<float>(toEnd) / static_cast<float>(fadeSamples);
        st.pcm[i] = static_cast<int16_t>(SATURATE_16BIT(static_cast<int32_t>(s * 32767.0f)));
    }

    st.pos += n;
    st.pcmLen = n;
    st.pcmRead = 0;
    return true;
}

extern "C" int32_t plugin_process_block(const int16_t** in, int16_t** out, uint32_t samples, uint8_t channels)
{
    (void)in;
    if (s_state == nullptr || m::host == nullptr)
        return 0;

    State& st = *s_state;
    if (!st.started)
        startVoices(st);

    uint32_t written = 0;
    while (written < samples)
    {
        if (st.pcmRead >= st.pcmLen && !renderBlock(st))
            break;

        const uint32_t n = m::min(samples - written, st.pcmLen - st.pcmRead);
        for (uint8_t ch = 0; ch < channels; ch++)
        {
            for (uint32_t i = 0; i < n; i++)
                out[ch][written + i] = st.pcm[st.pcmRead + i];
        }
        st.pcmRead += n;
        written += n;
    }

    return static_cast<int32_t>(written);
}
