#!/usr/bin/env python3
"""Wrap a plain-C mac header in extern "C" so LX can include it from C++ (idempotent)."""
import sys
path = sys.argv[1]
s = open(path).read()
if "__cplusplus" in s:
    sys.exit(0)
OPEN, CLOSE = '#ifdef __cplusplus\nextern "C" {\n#endif\n', '#ifdef __cplusplus\n}\n#endif\n'
if "#define" in s and "#endif" in s:          # include-guarded: wrap inside the guard
    i = s.index("#define"); j = s.index("\n", i) + 1; k = s.rindex("#endif")
    s = s[:j] + OPEN + s[j:k] + CLOSE + s[k:]
else:                                          # unguarded: wrap everything after the leading #includes
    lines = s.split("\n"); n = 0
    while n < len(lines) and (lines[n].startswith("#include") or not lines[n].strip()):
        n += 1
    s = "\n".join(lines[:n]) + "\n" + OPEN + "\n".join(lines[n:]).rstrip("\n") + "\n" + CLOSE
open(path, "w").write(s)
