#!/usr/bin/env python3
"""Regenerate the GPU contract and the embedded Vulkan 1.0 shader.

The tables below are the single source for operation IDs, blend-mode IDs, storage
formats and per-operation parameter slots. They generate both the C++ header
(src/rendering/GpuContract.h) and the GLSL include (shaders/contract.glsl), so C++
and the shader cannot drift. Normal builds need no shader compiler: --check only
verifies that the generated files match these tables and the shader source.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

# Order is the wire format: append new operations, never reorder.
OPERATIONS = [
    ('Source', []),
    ('Copy', []),
    ('Invert', []),
    ('Exposure', ['multiplier', 'offset', 'inverseGamma', 'linear']),
    ('Lookup', []),
    ('Cube', ['grid', 'clipping']),
    ('BoxBlur', ['radius', 'axis', 'span']),
    ('Gaussian', ['reach', 'axis', 'clear']),
    ('Mask', ['useAlpha', 'factor']),
    ('Bloom', ['amount', 'deep']),
    # `origin`: the document position of the input's first pixel along the streak, as the CPU shears by absolute steps.
    ('MotionBlur', ['horizontal', 'slope', 'reach', 'origin']),
    ('ProgressiveMix', ['startX', 'startY', 'deltaX', 'deltaY', 'radial', 'low', 'high', 'strongest', 'level']),
    ('Composite', ['mode', 'opacity']),
    ('Transform', ['m11', 'm21', 'dx', 'm12', 'm22', 'dy', 'sourceWidth', 'sourceHeight', 'smooth', 'opacity']),
    ('GradientMap', []),
    ('Morphology', ['radius', 'axis', 'erode']),
    ('RgbSplit', ['distance', 'blur', 'directionX', 'directionY', 'centerX', 'centerY', 'reach', 'radial',
                  'spectralSamples', 'smearSamples', 'edges']),
    ('Mix', ['amount']),
    ('HueSaturation', ['colorize', 'hue', 'saturation', 'lightness']),
    ('BlackWhite', ['red', 'yellow', 'green', 'cyan', 'blue', 'magenta', 'tint', 'tintHue', 'tintSaturation']),
    ('ColorBalance', ['shadowRed', 'shadowGreen', 'shadowBlue', 'midRed', 'midGreen', 'midBlue',
                      'highlightRed', 'highlightGreen', 'highlightBlue', 'preserveLuminosity']),
    ('Clear', []),
    # O(log r) morphology: each step multiplies the run by `count` (a sparse table), then the combine covers the window.
    ('MorphologyStep', ['step', 'axis', 'erode', 'count']),
    ('MorphologyCombine', ['radius', 'axis', 'erode', 'run']),
    # Lens Effects' Bokeh (macOS MetalBokeh): every pixel gathers the light within the aperture's shape.
    ('Bokeh', ['radius', 'shape', 'ring']),
    # The rest of Lens Effects (macOS LensEffects.swift), on one color channel at a time (Red32 planes).
    # A point operation: to linear light, to gamma, a CIToneCurve spline (up to five points), the flare's power.
    ('LensPoint', ['mode', 'inScale', 'outScale', 'count', 'x0', 'x1', 'x2', 'x3', 'x4', 'y0', 'y1', 'y2', 'y3', 'y4',
                   'm0', 'm1', 'm2', 'm3', 'm4']),
    # Two planes into one (sum, brighter, difference), the auxiliary read at an offset into a wider plane.
    ('LensCombine', ['mode', 'a', 'b', 'offsetX', 'offsetY', 'auxWidth']),
    # A streak's squeeze along the rows (each pixel averages the source pixels it covers) and stretch back. The
    # squeeze first draws each light out along the row (the maximum within `radius`), read from a MorphologyStep
    # table of runs of `run` pixels.
    ('LensSqueeze', ['squeeze', 'pad', 'sourceWidth', 'radius', 'run']),
    ('LensStretch', ['squeeze', 'pad', 'smallWidth', 'gain', 'accumulate']),
    # Starburst's rays turned back onto the frame in one pass: four squeezed ray planes side by side (at `offset0`
    # … `offset3`, the last ending at packedWidth), each stretched by its squeeze and added times its gain.
    ('LensStreakBack', ['ux', 'uy', 'vx', 'vy', 'cx', 'cy', 'umin', 'vmin', 'turnedWidth', 'turnedHeight', 'pad',
                        'packedWidth', 'squeeze0', 'squeeze1', 'squeeze2', 'squeeze3', 'gain0', 'gain1', 'gain2',
                        'gain3', 'offset0', 'offset1', 'offset2', 'offset3']),
    # Bilinear resampling through an affine map, clear outside the source or (clamp) its edges carried on; `channel`
    # picks which of an RGBA source's channels.
    ('LensResample', ['m11', 'm12', 'dx', 'm21', 'm22', 'dy', 'sourceWidth', 'sourceHeight', 'clamp', 'channel']),
    # Prism's copy of the picture, shifted one way or both.
    ('LensPrism', ['dx', 'dy', 'along0', 'along1', 'ways', 'margin', 'left', 'top', 'right', 'bottom', 'sourceWidth',
                   'sourceHeight']),
    # A wide blur's smaller copy: each pixel the average of a factor × factor block (clear outside, or only the
    # pixels inside counted).
    ('LensReduce', ['factor', 'sourceWidth', 'sourceHeight', 'clear']),
    # Add Noise and Grain (macOS GPUNoise.swift: NoisePixels.c's noise and adjust_grain with the same hashes). Seeds
    # come in 16-bit halves, as parameters are floats; the origin places the pixels in the document.
    ('Noise', ['amount', 'gaussian', 'monochromatic', 'seedLow', 'seedHigh', 'originX', 'originY']),
    ('Grain', ['amount', 'size', 'roughness', 'seedLow', 'seedHigh', 'originX', 'originY', 'unitsPerPixel']),
]
# Lens operations can update their input in place over a region (GpuGraph::update); the region (x, y, width, height)
# is passed in the last four parameter slots, which those operations leave free.
REGION_SLOTS = (24, 25, 26, 27)
# Must match compositor::BlendMode (core/Document.h); GpuContract.cpp static_asserts each value.
BLEND_MODES = ['Normal', 'Darken', 'Multiply', 'ColorBurn', 'LinearBurn', 'Lighten', 'Screen', 'ColorDodge',
               'LinearDodge', 'Overlay', 'SoftLight', 'HardLight', 'VividLight', 'LinearLight', 'PinLight', 'HardMix',
               'Difference', 'Exclusion', 'Subtract', 'Divide', 'Hue', 'Saturation', 'Color', 'Luminosity']
# Storage per node. Bytes per pixel: 16, 4, 8, 8, 1, 4.
FORMATS = ['Float32', 'Unorm8', 'Unorm16', 'Half', 'Gray8', 'Red32']
QUANTIZATION = ['Float', 'Unorm8', 'Unorm16', 'Half']
PARAMETER_SLOTS = 28


def snake(name):
    out = ''
    for i, ch in enumerate(name):
        if ch.isupper() and i and (not name[i - 1].isupper() or (i + 1 < len(name) and name[i + 1].islower())):
            out += '_'
        out += ch.upper()
    return out


def contract_cpp():
    lines = ['// Generated by scripts/compile-rendering-shader.py; do not edit.', '#pragma once', '',
             'namespace compositor {',
             '// Source nodes own implicitly shared immutable snapshots. Passes refer only to earlier',
             '// nodes; execution retains intermediate surfaces on the device and reads the result once.',
             'enum class GpuOp : unsigned {']
    lines += [f'    {name} = {i},' for i, (name, _) in enumerate(OPERATIONS)]
    lines += ['};', f'inline constexpr unsigned gpuOperationCount = {len(OPERATIONS)};', '',
              'enum class GpuFormat : unsigned {']
    lines += [f'    {name} = {i},' for i, name in enumerate(FORMATS)]
    lines += ['};', '', '// Intermediate rounding that emulates the CPU pipeline (and selects compact storage).',
              'enum class GpuQuantization : unsigned {']
    lines += [f'    {name} = {i},' for i, name in enumerate(QUANTIZATION)]
    lines += ['};', f'inline constexpr int gpuParameterSlots = {PARAMETER_SLOTS};',
              '// Where a region pass (GpuGraph::update) passes its rectangle.',
              f'inline constexpr int gpuRegionX = {REGION_SLOTS[0]}, gpuRegionY = {REGION_SLOTS[1]}, '
              f'gpuRegionWidth = {REGION_SLOTS[2]}, gpuRegionHeight = {REGION_SLOTS[3]};', '',
              '// Blend IDs used by GpuOp::Composite (parameter "mode").', 'namespace GpuBlend {']
    lines += [f'inline constexpr int {name} = {i};' for i, name in enumerate(BLEND_MODES)]
    lines += ['} // namespace GpuBlend', '', '// Named parameter slots per operation.', 'namespace GpuParam {']
    for name, params in OPERATIONS:
        if params:
            assert len(params) <= (REGION_SLOTS[0] if name.startswith('Lens') else PARAMETER_SLOTS)
            body = ' '.join(f'{p} = {i},' for i, p in enumerate(params))
            lines.append(f'struct {name} {{ enum : int {{ {body} }}; }};')
    lines += ['} // namespace GpuParam', '} // namespace compositor', '']
    return '\n'.join(lines)


def contract_glsl():
    lines = ['// Generated by scripts/compile-rendering-shader.py; do not edit.']
    lines += [f'const uint OP_{snake(name)} = {i}u;' for i, (name, _) in enumerate(OPERATIONS)]
    lines += [f'const uint FORMAT_{snake(name)} = {i}u;' for i, name in enumerate(FORMATS)]
    lines += [f'const uint QUANTIZE_{snake(name)} = {i}u;' for i, name in enumerate(QUANTIZATION)]
    lines += [f'const int BLEND_{snake(name)} = {i};' for i, name in enumerate(BLEND_MODES)]
    lines.append(f'#define PARAMETER_SLOTS {PARAMETER_SLOTS}')
    for name, slot in zip(['X', 'Y', 'WIDTH', 'HEIGHT'], REGION_SLOTS):
        lines.append(f'const int REGION_{name} = {slot};')
    for name, params in OPERATIONS:
        lines += [f'const int {snake(name)}_{snake(p)} = {i};' for i, p in enumerate(params)]
    return '\n'.join(lines) + '\n'


parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--compiler', default='glslangValidator')
parser.add_argument('--check', action='store_true', help='Check generated files without regenerating')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
shaders = root / 'src/rendering/shaders'
source = shaders / 'rendering.comp'
glsl = shaders / 'contract.glsl'
header = root / 'src/rendering/GpuContract.h'
output = shaders / 'RenderingSpirv.h'
cpp_text, glsl_text = contract_cpp(), contract_glsl()
sha = hashlib.sha256(source.read_bytes() + glsl_text.encode()).hexdigest()
if args.check:
    problems = []
    if header.read_text() != cpp_text:
        problems.append('GpuContract.h does not match the contract tables')
    if glsl.read_text() != glsl_text:
        problems.append('contract.glsl does not match the contract tables')
    if f'// Source SHA256: {sha}\n' not in output.read_text():
        problems.append('rendering shader source changed; regenerate RenderingSpirv.h')
    if problems:
        raise SystemExit('; '.join(problems))
    print('GPU contract and rendering shader match the generated files')
else:
    header.write_text(cpp_text)
    glsl.write_text(glsl_text)
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'rendering.spv'
        subprocess.run([args.compiler, '-V', '--target-env', 'vulkan1.0', '-I' + str(shaders), '-o', str(binary),
                        str(source)], check=True)
        if validator := shutil.which('spirv-val'):
            subprocess.run([validator, '--target-env', 'vulkan1.0', str(binary)], check=True)
        data = binary.read_bytes()
    words = struct.unpack(f'<{len(data)//4}I', data)
    rows = ['    ' + ', '.join(f'0x{v:08x}' for v in words[i:i+8]) + ',' for i in range(0, len(words), 8)]
    output.write_text('// Generated by scripts/compile-rendering-shader.py; do not edit.\n'
                      f'// Source SHA256: {sha}\n#pragma once\n#include <cstdint>\n'
                      'namespace compositor {\ninline constexpr std::uint32_t renderingSpirv[] = {\n'
                      + '\n'.join(rows) + '\n};\n}\n')
