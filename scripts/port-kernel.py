#!/usr/bin/env python3
"""Translate upstream libdispatch bands to OpenMP, preserving each band's body."""
import re
import sys
from pathlib import Path

path = Path(sys.argv[1])
src = path.read_text()
if '#include <dispatch/dispatch.h>' not in src:
    sys.exit(0)
src = src.replace('#include <dispatch/dispatch.h>\n', '')

def closing(text, start):
    # Only braces in code count; comments and strings may contain example expressions.
    tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for m in tokens.finditer(text, start):
        if m.group() == '{': depth += 1
        elif m.group() == '}':
            depth -= 1
            if depth == 0: return m.start()
    raise ValueError('unclosed body')

# Helpers only express partitioning; their call sites below get the same partition.
for name in ['in_bands', 'deep_rows', 'in_row_bands']:
    m = re.search(r'static void ' + name + r'\([^\n]+\) \{', src)
    if m:
        end = closing(src, m.end() - 1)
        src = src[:m.start()] + src[end + 1:]

pattern = re.compile(r'(in_bands|deep_rows|in_row_bands|dispatch_apply)\(([^\n]*?),\s*(?:DISPATCH_APPLY_AUTO,\s*)?\^\(([^)]*)\)\s*\{')
number = 0
while (m := pattern.search(src)):
    name, count, args = m.groups()
    end = closing(src, m.end() - 1)
    if src[end + 1:end + 3] != ');':
        raise ValueError('unexpected block ending')
    body = src[m.end():end]
    if re.search(r'\breturn\s+[^;]', body):
        raise ValueError('unexpected block return value')
    # A block's return exits the band, including from inside nested loops.
    label = f'kernel_band_end_{number}'
    body = re.sub(r'\breturn;', f'goto {label};', body)
    number += 1
    names = [arg.strip().split()[-1] for arg in args.split(',')]
    if name == 'dispatch_apply':
        prefix = f'{{\n    const size_t band_count_ = ({count});\n    _Pragma("omp parallel for schedule(static)")\n    for (long band_index_ = 0; band_index_ < (long)band_count_; ++band_index_) {{\n        size_t {names[0]} = (size_t)band_index_;'
    else:
        small, bands = (32, 16) if name == 'in_row_bands' else (64, 32)
        kind = 'int' if name == 'in_row_bands' else 'size_t'
        first, last = names[-2:]
        prefix = f'{{\n    const {kind} band_rows_ = ({count}), band_count_ = band_rows_ < {small} ? 1 : {bands}, band_size_ = (band_rows_ + band_count_ - 1) / band_count_;\n    _Pragma("omp parallel for schedule(static)")\n    for (long band_index_ = 0; band_index_ < (long)band_count_; ++band_index_) {{\n        {kind} {first} = ({kind})band_index_ * band_size_, {last} = {first} + band_size_ < band_rows_ ? {first} + band_size_ : band_rows_;\n        if ({first} >= {last}) continue;'
        if len(names) == 3: prefix += f'\n        int {names[0]} = (int)band_index_; (void){names[0]};'
    suffix = f'\n{label}: ;\n    }}\n}}' if f'goto {label}' in body else '\n    }\n}'
    src = src[:m.start()] + prefix + body + suffix + src[end + 3:]
src = src.replace('__block ', '').replace('failed = 1;', '_Pragma("omp atomic write") failed = 1;')
if '^(' in src or 'dispatch_apply' in src or 'in_bands(' in src:
    raise ValueError('untranslated block')
path.write_text("\n".join(line.rstrip() for line in src.splitlines()) + "\n")
