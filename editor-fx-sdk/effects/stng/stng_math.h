#pragma once

// Math and small helpers for the STNG DSP.
//
// .ptfx plugins are built freestanding (-nostdlib, no libm, no C++ runtime) and linked at address 0
// without relocation, so the DSP must not call into the standard library. Define STNG_PTFX and set
// stng::m::host before rendering to route the math through the firmware's PtfxHostApi. Without
// STNG_PTFX (host tests) the same functions use <cmath>.

#include <stdint.h>

#ifdef STNG_PTFX
#include "tracker_plugin.h"
#else
#include <cmath>
#endif

namespace polysynth::stng::m
{

#ifdef STNG_PTFX
// Set by the plugin in plugin_init(). Internal linkage: no GOT entry in the plugin binary.
namespace
{
const PtfxHostApi* host = nullptr;
}
#endif

template<typename T>
constexpr T min(T a, T b)
{
    return (b < a) ? b : a;
}

template<typename T>
constexpr T max(T a, T b)
{
    return (a < b) ? b : a;
}

template<typename T>
constexpr T clamp(T v, T lo, T hi)
{
    return (v < lo) ? lo : ((hi < v) ? hi : v);
}

inline float fabs(float x)
{
    return x < 0.0f ? -x : x;
}

inline void zero(float* p, int n)
{
    for (int i = 0; i < n; i++)
    {
        p[i] = 0.0f;
    }
}

// atan(z) for |z| <= 1: reduce to |z| <= tan(pi/12), then an odd Taylor series (error < 1e-8)
inline float atanUnit(float z)
{
    constexpr float kPi6 = 0.52359877559829887f;
    constexpr float kTanPi12 = 0.26794919243112270f;
    constexpr float kInvSqrt3 = 0.57735026918962576f;

    float offset = 0.0f;
    if (z > kTanPi12)
    {
        z = (z - kInvSqrt3) / (1.0f + z * kInvSqrt3);
        offset = kPi6;
    }
    const float z2 = z * z;
    const float p =
      z * (1.0f +
           z2 * (-1.0f / 3.0f +
                 z2 * (1.0f / 5.0f + z2 * (-1.0f / 7.0f + z2 * (1.0f / 9.0f + z2 * (-1.0f / 11.0f))))));
    return offset + p;
}

inline float atan2Approx(float y, float x)
{
    constexpr float kPi = 3.14159265358979f;
    constexpr float kPi2 = 1.57079632679490f;

    const float ax = fabs(x);
    const float ay = fabs(y);
    if (ax == 0.0f && ay == 0.0f)
    {
        return 0.0f;
    }

    float a = (ay <= ax) ? atanUnit(ay / ax) : kPi2 - atanUnit(ax / ay);
    if (x < 0.0f)
    {
        a = kPi - a;
    }
    return (y < 0.0f) ? -a : a;
}

#ifdef STNG_PTFX
inline float sin(float x) { return host->sinf(x); }
inline float cos(float x) { return host->cosf(x); }
inline float exp(float x) { return host->expf(x); }
inline float exp2(float x) { return host->expf(x * 0.69314718055994531f); }
inline float pow(float x, float y) { return host->powf(x, y); }
inline float sqrt(float x) { return host->sqrtf(x); }
inline float atan2(float y, float x) { return atan2Approx(y, x); }
#else
inline float sin(float x) { return std::sin(x); }
inline float cos(float x) { return std::cos(x); }
inline float exp(float x) { return std::exp(x); }
inline float exp2(float x) { return std::exp2(x); }
inline float pow(float x, float y) { return std::pow(x, y); }
inline float sqrt(float x) { return std::sqrt(x); }
inline float atan2(float y, float x) { return std::atan2(y, x); }
#endif

} // namespace polysynth::stng::m
