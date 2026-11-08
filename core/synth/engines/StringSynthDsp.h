#pragma once

// STNG: Karplus-Strong style physical model (exciter -> 1 or 2 string/pipe resonators).
//
// Plain C++ with no Tracker or PolySynth dependencies, so it can be rendered and tested on a
// host. Delay-line memory is owned by the caller (see StringSynthVoice.cpp).
//
// Per resonator and sample: 4-point Hermite fractional delay read, one-pole damping lowpass,
// two first-order allpasses for stiffness (dispersion), optional soft saturation, loop gain
// from the T60 decay time. The "pipe" model inverts the reflection, which leaves only odd
// harmonics and halves the loop length. The tuning compensates for the phase delay of the
// loop filters at the fundamental, so notes stay in tune across brightness/stiffness.
//
// Reed and Brass are self-oscillating exciters: instead of adding energy to the loop, a
// nonlinear junction at the mouthpiece reacts to the wave coming back down the bore.
//   Reed : pressure-controlled reed reflection (clarinet-style), half-period bore
//   Brass: resonant lip valve tuned near the note, full-period bore
// Breath pressure follows the note (attack = Exciter Length, level from velocity).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace polysynth::stng
{

constexpr float kSampleRate = 44100.0f;
constexpr float kPi = 3.14159265358979f;

constexpr int kMaxDelay = 3072;   // per resonator: lowest string ~14.4 Hz
constexpr int kExcHistory = 2048; // pluck-position comb history (power of two)
constexpr int kFloatsPerVoice = 2 * kMaxDelay + kExcHistory;

enum class Exciter : int
{
    kPluck = 0, // decaying noise burst
    kMallet,    // smooth raised-cosine strike
    kClick,     // impulse
    kBow,       // continuous noise while the note is held (bow / breath)
    kReed,      // self-oscillating reed valve (clarinet / sax-like)
    kBrass,     // self-oscillating lip valve (trumpet / horn-like)
    kCount
};

enum class Model : int
{
    kString = 0, // all harmonics
    kPipe,       // inverted reflection: odd harmonics, hollow
    kCount
};

struct Params
{
    // exciter
    Exciter exciter = Exciter::kPluck;
    float excTone = 0.7f;     // 0..1 exciter brightness
    float excLengthMs = 8.0f; // burst length / bow attack
    float position = 0.15f;   // 0..0.5 pluck position, 0 = off
    float velToTone = 0.5f;   // 0..1 velocity -> exciter brightness

    // resonator
    Model model = Model::kString;
    float decaySec = 3.0f;   // T60 while held
    float releaseSec = 0.4f; // T60 after note off
    float brightness = 0.7f; // 0..1 loop damping filter
    float stiffness = 0.0f;  // 0..1 dispersion (metallic)

    // second resonator
    float res2Semitones = 0.0f;
    float res2Cents = 7.0f;
    float res2Level = 0.0f; // 0..1

    // character
    float drive = 0.0f;       // 0..1 in-loop saturation
    float body = 0.0f;        // 0..1 body resonance amount
    float bodyFreqHz = 220.0f;
    float drift = 0.15f;      // 0..1 slow random pitch wander
    float driftRateHz = 0.7f;

    // voice
    float tuneSemitones = 0.0f;
    float fineCents = 0.0f;
    float glideSec = 0.0f;
    float gain = 1.0f;
};

inline bool isDriven(Exciter e)
{
    return e == Exciter::kReed || e == Exciter::kBrass;
}

// reed stiffness (magnitude of the reed-table slope) for Exciter Tone 0..1
inline float reedStiffness(float tone)
{
    return 0.18f + 0.22f * tone;
}

// Brass pitch correction (cents) for Exciter Tone, measured on the model: the lip valve pulls
// the pitch slightly sharp, most at the extremes of lip tension.
inline float brassPitchCorrectionCents(float tone)
{
    constexpr float kTone[] = { 0.0f, 0.3f, 0.5f, 0.7f, 1.0f };
    constexpr float kCents[] = { 22.0f, 13.0f, 3.0f, 11.0f, 42.0f };
    tone = std::clamp(tone, 0.0f, 1.0f);
    for (int i = 1; i < 5; i++)
    {
        if (tone <= kTone[i])
        {
            const float t = (tone - kTone[i - 1]) / (kTone[i] - kTone[i - 1]);
            return kCents[i - 1] + t * (kCents[i] - kCents[i - 1]);
        }
    }
    return kCents[4];
}

// small helpers ------------------------------------------------------------------------
inline float fastTanh(float x)
{
    x = std::clamp(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// phase delay (samples) of y = (1-a) x + a y[-1] at angular frequency w
inline float onePoleDelay(float a, float w)
{
    return std::atan2(a * std::sin(w), 1.0f - a * std::cos(w)) / w;
}

// phase delay (samples) of H(z) = (c + z^-1) / (1 + c z^-1) at angular frequency w
inline float allpassDelay(float c, float w)
{
    const float s = std::sin(w);
    const float co = std::cos(w);
    const float numPhase = std::atan2(-s, c + co);
    const float denPhase = std::atan2(-c * s, 1.0f + c * co);
    float phase = numPhase - denPhase; // negative
    while (phase > 0.0f)
    {
        phase -= 2.0f * kPi;
    }
    return -phase / w;
}

class Noise
{
  public:
    float next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<int32_t>(state)) * (1.0f / 2147483648.0f);
    }
    void seed(uint32_t s) { state = s ? s : 0x9E3779B9u; }

  private:
    uint32_t state = 0x12345678u;
};

//--------------------------------------------------------------------------------------
class Resonator
{
  public:
    void attach(float* buffer)
    {
        line = buffer;
        clear();
    }

    void clear()
    {
        if (line)
        {
            std::memset(line, 0, sizeof(float) * kMaxDelay);
        }
        write = 0;
        lp = 0.0f;
        ap1x = ap1y = ap2x = ap2y = 0.0f;
        lipY1 = lipY2 = 0.0f;
        jDcX = jDcY = 0.0f;
        avgPrev = 0.0f;
    }

    // Called once per block. delay is ramped to the new target over the block.
    void prepare(float freqHz, const Params& p, float t60, int blockSize)
    {
        const bool pipe = p.model == Model::kPipe;
        const bool reed = p.exciter == Exciter::kReed;
        const bool brass = p.exciter == Exciter::kBrass;
        if (brass)
        {
            freqHz *= std::exp2(-brassPitchCorrectionCents(p.excTone) / 1200.0f);
        }
        // reed: closed at the reed, open at the bell -> half-period loop, the inversion is in
        // the junction. brass: bore modes at multiples of f/2 (two-period loop); the lip
        // resonance picks the mode at the note.
        const float loopsPerPeriod = reed ? 2.0f : (brass ? 0.5f : (pipe ? 2.0f : 1.0f));
        const float tone = std::clamp(p.excTone, 0.0f, 1.0f);

        freqHz = std::clamp(freqHz, 8.0f, 8000.0f);
        const float w = 2.0f * kPi * freqHz / kSampleRate;

        // Damping (Brightness 0..1). The classic Karplus-Strong two-point averager is always on,
        // so the noise of the attack dies away quickly and only the tone keeps ringing (with too
        // little damping the burst keeps circulating and is heard repeating, like a short delay).
        // Brightness adds a one-pole lowpass that tracks the note: 3x the fundamental (muffled)
        // up to 43x (bright); at full brightness the averager is also eased off a little.
        const float b = std::clamp(p.brightness, 0.0f, 1.0f);
        avgS = 0.5f - 0.15f * b * b;
        {
            const float fc = std::min(freqHz * (3.0f + 40.0f * b * b), 16000.0f);
            lpA = std::exp(-2.0f * kPi * fc / kSampleRate);
        }

        apC = -0.65f * std::clamp(p.stiffness, 0.0f, 1.0f);

        const float co = std::cos(w);
        const float sn = std::sin(w);

        // averager (1-S) + S z^-1: phase delay and magnitude at the fundamental
        float filterDelay = -std::atan2(-avgS * sn, (1.0f - avgS) + avgS * co) / w;
        float filterMag =
          std::sqrt((1.0f - avgS) * (1.0f - avgS) + avgS * avgS + 2.0f * avgS * (1.0f - avgS) * co);
        if (lpA > 0.0f)
        {
            filterDelay += onePoleDelay(lpA, w);
            filterMag *= (1.0f - lpA) / std::sqrt(1.0f - 2.0f * lpA * co + lpA * lpA);
        }
        if (apC != 0.0f)
        {
            filterDelay += 2.0f * allpassDelay(apC, w);
        }
        const bool driven = reed || brass;

        float target = kSampleRate / (freqHz * loopsPerPeriod) - filterDelay;
        target = std::clamp(target, 3.0f, static_cast<float>(kMaxDelay - 4));

        if (delay <= 0.0f)
        {
            delay = target; // first block: no ramp
        }
        delayStep = (target - delay) / static_cast<float>(blockSize);

        // loop gain for the T60 decay: 10^(-3 / (T60 * f)) per period. For the passive
        // exciters the damping filter's loss at the fundamental is made up as far as possible
        // without the loop gain exceeding 1 (always stable): bright and mid settings decay as
        // set at every pitch, very dark settings decay faster on high notes.
        const float exponent = -9.9657843f / (std::max(t60, 0.01f) * freqHz * loopsPerPeriod);
        gain = std::exp2(exponent);
        if (!driven)
        {
            gain /= std::max(filterMag, 0.05f);
        }
        gain = std::min(gain, 0.9999f);
        if (brass)
        {
            gain = std::min(gain, kBrassMaxBoreGain); // lip valve needs some bore loss to speak
        }
        if (pipe && !reed && !brass)
        {
            gain = -gain;
        }

        // reed: stiffer reed (higher tone) -> brighter, buzzier. The breath pressure is scaled
        // in the exciter so stiffness x pressure stays where the reed speaks.
        reedSlope = -reedStiffness(tone);
        // brass: lip resonance relative to the note. Kept in the range where the instrument
        // speaks close to pitch: lower tone = slacker lips (softer), higher = tighter (brassier).
        // Constant-Q resonator with normalised peak gain, so it behaves the same at all pitches.
        const float lipFreq =
          std::clamp(freqHz * (0.88f + 0.12f * tone), 20.0f, 0.45f * kSampleRate);
        const float wl = 2.0f * kPi * lipFreq / kSampleRate;
        const float r = 1.0f - kPi * (lipFreq / kLipQ) / kSampleRate;
        lipA1 = 2.0f * r * std::cos(wl);
        lipA2 = -r * r;
        lipNorm = kLipPeakGain * (1.0f - r) * 2.0f * std::sin(wl);

        driveK = 1.0f + 7.0f * std::clamp(p.drive, 0.0f, 1.0f);
        useDrive = p.drive > 0.001f;
    }

    float absGain() const { return std::fabs(gain); }

    inline float process(float excitation)
    {
        float read;
        const float s = readAndFilter(read);
        push(s * gain + excitation);
        return read;
    }

    // Reed / Brass: the excitation is a nonlinear junction driven by breath pressure
    inline float processDriven(Exciter type, float breath)
    {
        float read;
        const float bore = readAndFilter(read) * std::fabs(gain);
        float in;

        if (type == Exciter::kReed)
        {
            // wave returning from the open bell is inverted
            const float pd = -bore - breath;
            const float reflection = std::clamp(0.7f + reedSlope * pd, -1.0f, 1.0f);
            in = breath + pd * reflection;
        }
        else
        {
            const float mouth = 0.3f * breath;
            float lip = lipNorm * (mouth - bore) + lipA1 * lipY1 + lipA2 * lipY2;
            lipY2 = lipY1;
            lipY1 = lip;
            lip = std::min(lip * lip, 1.0f); // lip opening
            in = lip * mouth + (1.0f - lip) * bore;
            // DC blocker in the loop (the lip valve rectifies)
            const float y = in - jDcX + 0.995f * jDcY;
            jDcX = in;
            jDcY = y;
            in = y;
        }

        push(in);
        return read;
    }

  private:
    inline float readAndFilter(float& readOut)
    {
        delay += delayStep;

        // Hermite read, delay samples behind the write position
        float pos = static_cast<float>(write) - delay;
        if (pos < 0.0f)
        {
            pos += static_cast<float>(kMaxDelay);
        }
        int i0 = static_cast<int>(pos);
        const float t = pos - static_cast<float>(i0);
        int im1 = i0 - 1;
        if (im1 < 0)
        {
            im1 += kMaxDelay;
        }
        int i1 = i0 + 1;
        if (i1 >= kMaxDelay)
        {
            i1 -= kMaxDelay;
        }
        int i2 = i1 + 1;
        if (i2 >= kMaxDelay)
        {
            i2 -= kMaxDelay;
        }

        const float xm1 = line[im1];
        const float x0 = line[i0];
        const float x1 = line[i1];
        const float x2 = line[i2];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        const float read = ((c3 * t + c2) * t + c1) * t + x0;

        // damping
        float s = (1.0f - avgS) * read + avgS * avgPrev;
        avgPrev = read;
        if (lpA > 0.0f)
        {
            lp = (1.0f - lpA) * s + lpA * lp;
            s = lp;
        }

        // stiffness
        if (apC != 0.0f)
        {
            const float y1 = apC * s + ap1x - apC * ap1y;
            ap1x = s;
            ap1y = y1;
            const float y2 = apC * y1 + ap2x - apC * ap2y;
            ap2x = y1;
            ap2y = y2;
            s = y2;
        }

        if (useDrive)
        {
            s = fastTanh(s * driveK) / driveK;
        }

        readOut = read;
        return s;
    }

    inline void push(float value)
    {
        line[write] = std::clamp(value, -4.0f, 4.0f);
        if (++write >= kMaxDelay)
        {
            write = 0;
        }
    }

  public:
    float currentDelay() const { return delay; }

  private:
    float* line = nullptr;
    int write = 0;
    float delay = 0.0f;
    float delayStep = 0.0f;
    float lp = 0.0f;
    float lpA = 0.5f;
    float apC = 0.0f;
    float ap1x = 0.0f, ap1y = 0.0f, ap2x = 0.0f, ap2y = 0.0f;
    float gain = 0.99f;
    float driveK = 1.0f;
    bool useDrive = false;
    // junctions
    float reedSlope = -0.3f;
    float lipA1 = 0.0f, lipA2 = 0.0f, lipY1 = 0.0f, lipY2 = 0.0f, lipNorm = 0.0f;
    static constexpr float kLipQ = 15.0f;
    static constexpr float kLipPeakGain = 15.0f;
    static constexpr float kBrassMaxBoreGain = 0.9f;
    float jDcX = 0.0f, jDcY = 0.0f;
    float avgS = 0.25f;
    float avgPrev = 0.0f;
};

//--------------------------------------------------------------------------------------
class ExciterGen
{
  public:
    void attach(float* historyBuffer)
    {
        history = historyBuffer;
        std::memset(history, 0, sizeof(float) * kExcHistory);
        histIdx = 0;
    }

    void trigger(const Params& p, float velocity, uint32_t seed)
    {
        type = p.exciter;
        noise.seed(seed);
        t = 0;
        len = std::max(1, static_cast<int>(p.excLengthMs * 0.001f * kSampleRate));
        amp = 0.25f + 0.75f * velocity;
        if (type == Exciter::kReed)
        {
            // breath pressure: stiffness x pressure 0.17 .. 0.28 speaks reliably
            amp = (0.17f + 0.11f * velocity) / reedStiffness(p.excTone);
        }
        else if (type == Exciter::kBrass)
        {
            amp = 1.6f + 0.9f * velocity; // breath pressure into the lip valve
        }
        held = true;
        bowEnv = 0.0f;
        active = true;

        // exciter brightness: 300 Hz .. 18 kHz, velocity can darken soft notes
        const float tone = std::clamp(p.excTone - p.velToTone * (1.0f - velocity) * 0.6f, 0.0f, 1.0f);
        const float fc = 300.0f * std::pow(60.0f, tone);
        toneK = 1.0f - std::exp(-2.0f * kPi * std::min(fc, 18000.0f) / kSampleRate);
    }

    void release() { held = false; }

    // bow gain normalisation for the current loop gain
    void setBowScale(float loopGain)
    {
        bowScale = 1.5f * std::sqrt(std::max(1.0f - loopGain * loopGain, 1e-5f));
    }

    void setPositionDelay(int samples) { posDelay = std::clamp(samples, 0, kExcHistory - 1); }

    bool isActive() const { return active; }

    inline float next()
    {
        if (!active)
        {
            return 0.0f;
        }

        float e = 0.0f;
        switch (type)
        {
            case Exciter::kPluck:
                if (t < len)
                {
                    e = noise.next() * (1.0f - static_cast<float>(t) / static_cast<float>(len));
                }
                break;
            case Exciter::kMallet:
                if (t < len)
                {
                    e = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(t) /
                                                static_cast<float>(len)));
                    e *= 1.0f + 0.15f * noise.next();
                }
                break;
            case Exciter::kClick:
                e = (t == 0) ? 3.0f : 0.0f;
                break;
            case Exciter::kReed:
            case Exciter::kBrass:
            {
                // breath pressure envelope plus a little breath noise; no tone filter or
                // position comb: this drives the junction directly
                if (held)
                {
                    bowEnv = std::min(1.0f, bowEnv + 1.0f / static_cast<float>(len));
                }
                else
                {
                    bowEnv = std::max(0.0f, bowEnv - 1.0f / (0.04f * kSampleRate));
                }
                ++t;
                if (!held && bowEnv <= 0.0f)
                {
                    active = false;
                }
                return amp * bowEnv * (1.0f + 0.03f * noise.next());
            }
            case Exciter::kBow:
            default:
            {
                const float attack = 1.0f / static_cast<float>(len);
                if (held)
                {
                    bowEnv = std::min(1.0f, bowEnv + attack);
                }
                else
                {
                    bowEnv = std::max(0.0f, bowEnv - 1.0f / (0.03f * kSampleRate));
                }
                e = noise.next() * bowEnv * bowScale;
                break;
            }
        }

        tone += toneK * (e - tone);
        e = tone * amp;

        // pluck position: comb on the excitation
        float out = e;
        if (posDelay > 0)
        {
            out = e - history[(histIdx - posDelay) & (kExcHistory - 1)];
        }
        history[histIdx] = e;
        histIdx = (histIdx + 1) & (kExcHistory - 1);

        ++t;
        const bool burstDone = (type == Exciter::kBow) ? (!held && bowEnv <= 0.0f) : (t > len + 64);
        if (burstDone && t > posDelay + len + 64)
        {
            active = false;
            tone = 0.0f;
        }
        return out;
    }

  private:
    Exciter type = Exciter::kPluck;
    Noise noise;
    float* history = nullptr;
    int histIdx = 0;
    int posDelay = 0;
    int t = 0;
    int len = 1;
    float amp = 1.0f;
    float toneK = 0.5f;
    float tone = 0.0f;
    float bowEnv = 0.0f;
    float bowScale = 0.1f;
    bool held = false;
    bool active = false;
};

//--------------------------------------------------------------------------------------
// One mono STNG voice: exciter, two resonators, body resonance, drift, glide, output stage.
class StringVoiceDsp
{
  public:
    // buffer: kFloatsPerVoice floats owned by the caller
    void attach(float* buffer)
    {
        res1.attach(buffer);
        res2.attach(buffer + kMaxDelay);
        exciter.attach(buffer + 2 * kMaxDelay);
        attached = true;
        reset();
    }

    bool isAttached() const { return attached; }

    void reset()
    {
        res1.clear();
        res2.clear();
        playing = false;
        quietBlocks = 0;
        bodyLow = bodyBand = 0.0f;
        dcX = dcY = 0.0f;
    }

    void noteOn(float note, float velocity, bool legato, const Params& p)
    {
        targetNote = note;
        if (!playing || p.glideSec <= 0.0f)
        {
            currentNote = note;
        }
        held = true;
        playing = true;
        quietBlocks = 0;

        // new note: re-excite. Legato step: glide only, the string keeps ringing.
        if (!legato || !exciter.isActive())
        {
            seed = seed * 1664525u + 1013904223u;
            exciter.trigger(p, velocity, seed);
            noteDetuneCents = p.drift * 4.0f * random.next(); // small per-note variation
        }
    }

    void noteOff()
    {
        held = false;
        exciter.release();
    }

    bool isPlaying() const { return playing; }

    void render(const Params& p, float pitchBendSemitones, float* out, int n)
    {
        if (!playing || !attached)
        {
            std::fill(out, out + n, 0.0f);
            return;
        }

        const float blockSec = static_cast<float>(n) / kSampleRate;

        // glide
        if (p.glideSec > 0.0f)
        {
            const float k = 1.0f - std::exp(-blockSec / p.glideSec);
            currentNote += (targetNote - currentNote) * k;
        }
        else
        {
            currentNote = targetNote;
        }

        // drift: smoothed random walk
        driftTimer -= blockSec * std::max(p.driftRateHz, 0.01f);
        if (driftTimer <= 0.0f)
        {
            driftTimer += 1.0f;
            driftTarget = random.next();
        }
        driftValue += (driftTarget - driftValue) * std::min(1.0f, blockSec * p.driftRateHz * 3.0f);
        const float driftCents = p.drift * 18.0f * driftValue + noteDetuneCents;

        const float semis = currentNote - 69.0f + p.tuneSemitones +
                            (p.fineCents + driftCents) * 0.01f + pitchBendSemitones;
        const float freq1 = 440.0f * std::exp2(semis / 12.0f);
        const float freq2 =
          freq1 * std::exp2((p.res2Semitones + p.res2Cents * 0.01f) / 12.0f);

        const float t60 = held ? p.decaySec : std::min(p.decaySec, p.releaseSec);
        res1.prepare(freq1, p, t60, n);
        const bool useRes2 = p.res2Level > 0.001f;
        if (useRes2)
        {
            res2.prepare(freq2, p, t60, n);
        }

        const bool driven = isDriven(p.exciter);
        exciter.setBowScale(res1.absGain());
        exciter.setPositionDelay(!driven && p.position > 0.01f
                                   ? static_cast<int>(p.position * res1.currentDelay())
                                   : 0);

        // body resonance: Chamberlin SVF bandpass
        const float bodyF = 2.0f * std::sin(kPi * std::clamp(p.bodyFreqHz, 40.0f, 4000.0f) / kSampleRate);
        const float bodyQ = 0.35f;
        const float bodyMix = p.body * 2.5f;
        const float level2 = p.res2Level;
        // sustained reed / brass tones are trimmed to sit closer to the plucked exciters
        const float exciterTrim =
          p.exciter == Exciter::kReed ? 0.6f : (p.exciter == Exciter::kBrass ? 0.8f : 1.0f);
        const float outGain = p.gain * 0.8f * exciterTrim;

        float peak = 0.0f;
        for (int i = 0; i < n; i++)
        {
            const float e = exciter.next();
            float s;
            if (driven)
            {
                s = res1.processDriven(p.exciter, e);
                if (useRes2)
                {
                    s = (s + level2 * res2.processDriven(p.exciter, e)) / (1.0f + 0.5f * level2);
                }
            }
            else
            {
                s = res1.process(e);
                if (useRes2)
                {
                    s = (s + level2 * res2.process(e)) / (1.0f + 0.5f * level2);
                }
            }

            if (bodyMix > 0.0f)
            {
                bodyLow += bodyF * bodyBand;
                const float high = s - bodyLow - bodyQ * bodyBand;
                bodyBand += bodyF * high;
                bodyBand = std::clamp(bodyBand, -8.0f, 8.0f);
                bodyLow = std::clamp(bodyLow, -8.0f, 8.0f);
                s += bodyMix * bodyBand * bodyQ;
            }

            // DC blocker
            const float y = s - dcX + 0.995f * dcY;
            dcX = s;
            dcY = y;

            const float o = fastTanh(y * outGain);
            out[i] = o;
            peak = std::max(peak, std::fabs(o));
        }

        // stop rendering once the string has died away
        if (peak < 1e-4f && !exciter.isActive())
        {
            if (++quietBlocks > 8)
            {
                playing = false;
            }
        }
        else
        {
            quietBlocks = 0;
        }
    }

  private:
    Resonator res1;
    Resonator res2;
    ExciterGen exciter;
    Noise random;
    bool attached = false;
    bool playing = false;
    bool held = false;
    int quietBlocks = 0;
    uint32_t seed = 0x2545F491u;
    float currentNote = 60.0f;
    float targetNote = 60.0f;
    float noteDetuneCents = 0.0f;
    float driftTimer = 0.0f;
    float driftTarget = 0.0f;
    float driftValue = 0.0f;
    float bodyLow = 0.0f;
    float bodyBand = 0.0f;
    float dcX = 0.0f;
    float dcY = 0.0f;
};

} // namespace polysynth::stng
