// Reference renderer: plays one STNG note straight through the DSP with <cmath>, using the same
// timing as stng_gen.cpp (128-sample blocks, note off at Hold, 10 ms fade at Length, stop when
// silent). Writes 16-bit mono PCM to stdout.
//
// Built against the current DSP it checks the plugin's freestanding math; built with
// -DORIGINAL_FORMAT against the live-synth sources (feature/stng-synth) it checks the port.
//
// usage: ref_render <preset 0-31> <midi note> <velocity 1-127> <hold s> <length s>

#include "StringSynthDsp.h"
#include "StringSynthPresets.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace polysynth::stng;

static void setRef(Params& p, ParamId id, float v)
{
    switch (id)
    {
        case ParamId::kExciter: p.exciter = static_cast<Exciter>(static_cast<int>(v + 0.5f)); break;
        case ParamId::kExcTone: p.excTone = v; break;
        case ParamId::kExcLength: p.excLengthMs = v; break;
        case ParamId::kPosition: p.position = v; break;
        case ParamId::kVelToTone: p.velToTone = v; break;
        case ParamId::kModel: p.model = static_cast<Model>(static_cast<int>(v + 0.5f)); break;
        case ParamId::kDecay: p.decaySec = v; break;
        case ParamId::kBrightness: p.brightness = v; break;
        case ParamId::kStiffness: p.stiffness = v; break;
        case ParamId::kRelease: p.releaseSec = v; break;
        case ParamId::kStr2Tune: p.res2Semitones = std::round(v); break;
        case ParamId::kStr2Fine: p.res2Cents = v; break;
        case ParamId::kStr2Level: p.res2Level = v; break;
        case ParamId::kDrive: p.drive = v; break;
        case ParamId::kBody: p.body = v; break;
        case ParamId::kBodyFreq: p.bodyFreqHz = v; break;
        case ParamId::kDrift: p.drift = v; break;
        case ParamId::kDriftRate: p.driftRateHz = v; break;
        case ParamId::kTune: p.tuneSemitones = std::round(v); break;
        case ParamId::kFine: p.fineCents = v; break;
        case ParamId::kGlide: p.glideSec = v; break;
        case ParamId::kVolume: p.gain = v; break;
        default: break;
    }
}

int main(int argc, char** argv)
{
    if (argc < 6)
        return 1;
    const int preset = atoi(argv[1]);
    const float note = static_cast<float>(atoi(argv[2]));
    const float velocity = atoi(argv[3]) / 127.0f;
    const uint32_t hold = static_cast<uint32_t>(atof(argv[4]) * kSampleRate + 0.5f);
    const uint32_t length = static_cast<uint32_t>(atof(argv[5]) * kSampleRate + 0.5f);

    // parameter defaults of the live STNG synth
    Params p;
    setRef(p, ParamId::kExcTone, 0.7f);
    setRef(p, ParamId::kExcLength, 8.0f);
    setRef(p, ParamId::kPosition, 0.15f);
    setRef(p, ParamId::kVelToTone, 0.5f);
    setRef(p, ParamId::kDecay, 3.0f);
    setRef(p, ParamId::kBrightness, 0.7f);
    setRef(p, ParamId::kRelease, 0.4f);
    setRef(p, ParamId::kStr2Fine, 7.0f);
    setRef(p, ParamId::kBody, 0.25f);
    setRef(p, ParamId::kBodyFreq, 220.0f);
    setRef(p, ParamId::kDrift, 0.15f);
    setRef(p, ParamId::kDriftRate, 0.7f);
    setRef(p, ParamId::kVolume, 1.0f);
#ifdef ORIGINAL_FORMAT
    for (const auto& v : kPresets[preset].values)
        setRef(p, v.id, v.value);
#else
    for (const auto& v : kPresets[preset].values)
        if (v.id != ParamId::kNone)
            setRef(p, v.id, v.value);
#endif
    p.glideSec = 0.0f;

    std::vector<float> buffer(kFloatsPerVoice);
    StringVoiceDsp voice;
    voice.attach(buffer.data());
    voice.noteOn(note, velocity, false, p);

    const uint32_t fade = static_cast<uint32_t>(0.01f * kSampleRate + 0.5f);
    float block[128];
    bool released = false;
    for (uint32_t pos = 0; pos < length;)
    {
        if (!released && pos >= hold)
        {
            voice.noteOff();
            released = true;
        }
        uint32_t n = 128;
        if (length - pos < n)
            n = length - pos;
        if (!released && hold - pos < n)
            n = hold - pos;
        if (!voice.isPlaying())
            break;
        voice.render(p, 0.0f, block, static_cast<int>(n));
        for (uint32_t i = 0; i < n; i++)
        {
            float s = block[i];
            const uint32_t toEnd = length - (pos + i);
            if (toEnd < fade)
                s *= static_cast<float>(toEnd) / static_cast<float>(fade);
            int32_t v = static_cast<int32_t>(s * 32767.0f);
            v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
            const int16_t pcm = static_cast<int16_t>(v);
            fwrite(&pcm, 2, 1, stdout);
        }
        pos += n;
    }
    return 0;
}
