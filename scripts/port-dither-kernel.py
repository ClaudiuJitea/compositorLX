#!/usr/bin/env python3
"""Make the mac DitherPixels.c build with GCC on Linux.

The mac file parallelises with libdispatch and clang blocks (`in_bands(n, ^(size_t a, size_t b) { ... });`).
This rewrites that into a macro pair over an OpenMP loop (serial when OpenMP is absent). Anything unexpected
aborts, so a mac-side change to the file's shape is noticed at sync time rather than miscompiled.
"""
import re, sys

path = sys.argv[1]
src = open(path).read()
if "BANDS_BEGIN" in src:
    sys.exit(0)

src = src.replace("#include <dispatch/dispatch.h>\n", "")
src, n = re.subn(
    r"// Runs `body`.*?\n\}\n",
    '''// Runs the braces between BANDS_BEGIN and BANDS_END over `count` items split into bands, each a (first, last)
// range, in parallel when OpenMP is available. `continue` leaves one band. (Linux port of the mac in_bands block.)
#define BANDS_BEGIN(count, first, last) { \\
    size_t bands_n_ = (count), bands_k_ = bands_n_ < 64 ? 1 : 32, bands_size_ = (bands_n_ + bands_k_ - 1) / bands_k_; \\
    _Pragma("omp parallel for schedule(dynamic)") \\
    for (long band_ = 0; band_ < (long)bands_k_; ++band_) { \\
        size_t first = (size_t)band_ * bands_size_, last = first + bands_size_ < bands_n_ ? first + bands_size_ : bands_n_; \\
        if (first >= last) continue;
#define BANDS_END } }
''', src, count=1, flags=re.S)
if n != 1:
    sys.exit("in_bands helper not found; update scripts/port-dither-kernel.py")

lines = src.split("\n")
out, i, calls = [], 0, 0
while i < len(lines):
    m = re.match(r"^(\s*)in_bands\((.+?), \^\(size_t (\w+), size_t (\w+)\) \{$", lines[i])
    if not m:
        out.append(lines[i]); i += 1; continue
    indent, count, a, b = m.groups()
    out.append(f"{indent}BANDS_BEGIN({count}, {a}, {b})")
    i += 1
    while lines[i] != f"{indent}}});":
        out.append(re.sub(r"\breturn;", "continue;", lines[i]).replace("__block ", ""))
        i += 1
    out.append(f"{indent}BANDS_END")
    i += 1; calls += 1
src = "\n".join(out).replace("__block ", "")
# Several bands may report failure at once: make that write atomic (all writers store 1).
src = src.replace("failed = 1;", '_Pragma("omp atomic write") failed = 1;')
if calls != 3 or "^(" in src or "dispatch" in src:
    sys.exit(f"unexpected DitherPixels.c shape ({calls} in_bands calls); update scripts/port-dither-kernel.py")
open(path, "w").write(src)
