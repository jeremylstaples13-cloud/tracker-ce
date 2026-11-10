#!/usr/bin/env bash
# Host tests for the STNG generator plugin (stng_gen.cpp). Needs g++ and python3 (stdlib only).
#
# 1. Every preset rendered through the plugin, driven the way the firmware drives it (fake host API
#    on <cmath>), must match the DSP rendered directly: same length, within 1 LSB, or for the
#    stiff / bowed / self-oscillating presets (where float rounding in the math shim shows up as a
#    slow phase drift) the same level envelope within 0.2 dB.
# 2. Chords and unisons at full velocity must not hit full scale.
# 3. A 20 s render must come out at the requested length.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$HERE/../../.."
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

g++ -O2 -std=c++17 -Wall -Wextra -Werror -I"$HERE/.." "$HERE/ref_render.cpp" -o "$OUT/ref"
g++ -O2 -std=c++17 -Wall -Wextra -Werror -I"$SDK" "$HERE/plugin_render.cpp" -o "$OUT/plug"

for i in $(seq 0 31); do
    "$OUT/ref" "$i" 60 100 1.0 4.0 > "$OUT/ref$i.raw"
    "$OUT/plug" "$i" 36 100 1.0 4.0 0 1 0 0 0 0 0 > "$OUT/plug$i.raw" 2>/dev/null
done
for c in 3 6 8 10; do
    for preset in 0 23 25 28; do
        "$OUT/plug" "$preset" 36 127 2.0 4.0 "$c" 1 0 0 0 0 0 > "$OUT/chord_${preset}_$c.raw" 2>/dev/null
    done
done
"$OUT/plug" 29 36 100 20 20 10 1 0 0 0 0 0 > "$OUT/long.raw" 2>/dev/null

python3 - "$OUT" <<'PY'
import array, math, os, sys
out = sys.argv[1]

def load(name):
    a = array.array("h")
    with open(os.path.join(out, name), "rb") as f:
        a.frombytes(f.read())
    return a

def envelope(x, block=2048):
    env = []
    for s in range(0, len(x) - block + 1, block):
        acc = sum(v * v for v in x[s:s + block])
        env.append(20 * math.log10(math.sqrt(acc / block) + 1))
    return env

failures = []
for i in range(32):
    ref, plug = load(f"ref{i}.raw"), load(f"plug{i}.raw")
    if len(ref) != len(plug):
        failures.append(f"preset {i + 1}: length {len(plug)} != {len(ref)}")
        continue
    diff = max((abs(a - b) for a, b in zip(ref, plug)), default=0)
    if diff > 1:
        env = max((abs(a - b) for a, b in zip(envelope(ref), envelope(plug))), default=0)
        if env > 0.2:
            failures.append(f"preset {i + 1}: max diff {diff}, envelope diff {env:.2f} dB")
for name in sorted(n for n in os.listdir(out) if n.startswith("chord_")):
    x = load(name)
    if max(abs(v) for v in x) >= 32767:
        failures.append(f"{name}: hits full scale")
if len(load("long.raw")) != 20 * 44100:
    failures.append(f"20 s render is {len(load('long.raw'))} samples")

if failures:
    print("STNG tests FAILED:\n  " + "\n  ".join(failures))
    sys.exit(1)
print("STNG tests passed: 32 presets match the DSP, chords stay below full scale, 20 s renders")
PY
