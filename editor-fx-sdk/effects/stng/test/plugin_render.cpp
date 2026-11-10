// Runs stng_gen.cpp natively the way the firmware does (plugin_init, every parameter through
// plugin_set_param, plugin_process_block in 8192-sample chunks on 2 channels) with a host API backed
// by <cmath>. Writes the left channel as 16-bit mono PCM to stdout and checks L == R.
//
// usage: plugin_render <p0> <p1> ... <p11>   (parameter values, as the firmware passes them)

#define STNG_HOST_TEST 1
#include "../../stng_gen.cpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static uint32_t g_append = 0;

static float hSin(float x) { return std::sin(x); }
static float hCos(float x) { return std::cos(x); }
static float hTan(float x) { return std::tan(x); }
static float hTanh(float x) { return std::tanh(x); }
static float hPow(float x, float y) { return std::pow(x, y); }
static float hExp(float x) { return std::exp(x); }
static float hSqrt(float x) { return std::sqrt(x); }
static float hLog(float x) { return std::log(x); }
static float hLog10(float x) { return std::log10(x); }
static float hFmin(float a, float b) { return std::fmin(a, b); }
static float hFmax(float a, float b) { return std::fmax(a, b); }
static float hFabs(float x) { return std::fabs(x); }
static void* hMemcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
static void* hMemset(void* d, int c, size_t n) { return std::memset(d, c, n); }
static int hRand() { return std::rand(); }
static void hExtra(uint32_t, uint32_t append) { g_append = append; }

int main(int argc, char** argv)
{
    if (argc < 13)
        return 1;

    static const PtfxHostApi host = { 44100, hSin, hCos, hTan, hTanh, hPow, hExp, hSqrt, hLog,
                                      hLog10, hFmin, hFmax, hFabs, hMemcpy, hMemset, hRand, hExtra };

    std::vector<uint8_t> scratch(140000, 0);
    plugin_init(scratch.data(), static_cast<uint32_t>(scratch.size()), &host);
    for (int i = 0; i < 12; i++)
        plugin_set_param(static_cast<uint8_t>(i), static_cast<float>(atof(argv[1 + i])));

    const uint32_t target = g_append > 0 ? g_append : 88200;
    std::vector<int16_t> left(target), right(target);
    uint32_t done = 0;
    while (done < target)
    {
        int16_t* out[2] = { left.data() + done, right.data() + done };
        const uint32_t chunk = (target - done) > 8192 ? 8192 : (target - done);
        const int32_t n = plugin_process_block(nullptr, out, chunk, 2);
        if (n <= 0)
            break;
        done += static_cast<uint32_t>(n);
    }

    if (std::memcmp(left.data(), right.data(), done * sizeof(int16_t)) != 0)
    {
        fprintf(stderr, "left and right differ\n");
        return 2;
    }
    fwrite(left.data(), 2, done, stdout);
    fprintf(stderr, "target %u rendered %u\n", target, done);
    return 0;
}
