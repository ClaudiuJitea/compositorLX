#!/usr/bin/env python3
"""
Generates simulated reference .comp packages (v1-v9) and negative test packages.
Note: These packages are programmatically synthesized according to the macOS Compositor
schema contract (Compositor/docs/project-format.md) for automated schema validation,
negative security boundaries, and headless round-trip testing.
They are simulated reference fixtures, NOT exported packages from a live macOS Compositor app run.
JSON format matches Swift's JSONEncoder with [.prettyPrinted, .sortedKeys].
PNG assets are valid 8-bit RGBA or Grayscale PNGs.
"""

import json
import os
import shutil
import struct
import zlib
from pathlib import Path

def make_rgba_png(width, height, r=255, g=0, b=0, a=255):
    raw = bytearray()
    row_bytes = bytes([r, g, b, a] * width)
    for _ in range(height):
        raw.append(0) # filter None
        raw.extend(row_bytes)
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b'')

def make_gray_png(width, height, value=255):
    raw = bytearray()
    row_bytes = bytes([value] * width)
    for _ in range(height):
        raw.append(0) # filter None
        raw.extend(row_bytes)
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(bytes(raw))) + chunk(b'IEND', b'')

def write_package(path, manifest, images=None, masks=None):
    os.makedirs(path, exist_ok=True)
    images_dir = os.path.join(path, "images")
    os.makedirs(images_dir, exist_ok=True)
    with open(os.path.join(path, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
        f.write("\n")
    if images:
        for fname, data in images.items():
            with open(os.path.join(images_dir, fname), "wb") as f:
                f.write(data)
    if masks:
        for fname, data in masks.items():
            with open(os.path.join(images_dir, fname), "wb") as f:
                f.write(data)

def default_transform(w=64, h=64, x=0, y=0):
    return {
        "flipX": False,
        "flipY": False,
        "origin": [x, y],
        "rotation": 0.0,
        "sampling": "High quality",
        "size": [w, h]
    }

def default_adjustment_base():
    return {
        "colorize": False,
        "curves": {
            "channel": "RGB",
            "channels": [
                [{"x": 0.0, "y": 0.0}, {"x": 255.0, "y": 255.0}],
                [{"x": 0.0, "y": 0.0}, {"x": 255.0, "y": 255.0}],
                [{"x": 0.0, "y": 0.0}, {"x": 255.0, "y": 255.0}],
                [{"x": 0.0, "y": 0.0}, {"x": 255.0, "y": 255.0}]
            ]
        },
        "hue": 0.0,
        "levels": {
            "channel": "RGB",
            "ranges": [
                {"black": 0.0, "gamma": 1.0, "outputBlack": 0.0, "outputWhite": 255.0, "white": 255.0},
                {"black": 0.0, "gamma": 1.0, "outputBlack": 0.0, "outputWhite": 255.0, "white": 255.0},
                {"black": 0.0, "gamma": 1.0, "outputBlack": 0.0, "outputWhite": 255.0, "white": 255.0},
                {"black": 0.0, "gamma": 1.0, "outputBlack": 0.0, "outputWhite": 255.0, "white": 255.0}
            ]
        },
        "lightness": 0.0,
        "saturation": 0.0
    }

def generate_all(base_dir):
    valid_dir = os.path.join(base_dir, "valid")
    malformed_dir = os.path.join(base_dir, "malformed")
    os.makedirs(valid_dir, exist_ok=True)
    os.makedirs(malformed_dir, exist_ok=True)

    img64 = make_rgba_png(64, 64, 50, 100, 200, 255)
    img64_red = make_rgba_png(64, 64, 220, 40, 40, 255)
    mask64 = make_gray_png(64, 64, 180)

    # 1. v1_basic.comp
    l1_id = "11111111-1111-1111-1111-111111111111"
    write_package(
        os.path.join(valid_dir, "v1_basic.comp"),
        {
            "activeLayerID": l1_id,
            "colorSpace": "sRGB",
            "documentID": "00000001-0000-0000-0000-000000000001",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": l1_id,
                    "imageFile": f"{l1_id}.png",
                    "isVisible": True,
                    "name": "Background",
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 1,
            "width": 64
        },
        images={f"{l1_id}.png": img64}
    )

    # 2. v2_groups.comp
    grp_id = "22222222-2222-2222-2222-222222222221"
    leaf_id = "22222222-2222-2222-2222-222222222222"
    write_package(
        os.path.join(valid_dir, "v2_groups.comp"),
        {
            "activeLayerID": leaf_id,
            "colorSpace": "sRGB",
            "documentID": "00000002-0000-0000-0000-000000000002",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": grp_id,
                    "isGroup": True,
                    "isVisible": True,
                    "name": "Group",
                    "transform": default_transform(64, 64)
                },
                {
                    "id": leaf_id,
                    "imageFile": f"{leaf_id}.png",
                    "isGroup": False,
                    "isVisible": True,
                    "name": "Child",
                    "parentID": grp_id,
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 2,
            "width": 64
        },
        images={f"{leaf_id}.png": img64}
    )

    # 3. v3_appearance.comp
    l3_id = "33333333-3333-3333-3333-333333333333"
    write_package(
        os.path.join(valid_dir, "v3_appearance.comp"),
        {
            "activeLayerID": l3_id,
            "colorSpace": "sRGB",
            "documentID": "00000003-0000-0000-0000-000000000003",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "blendMode": "Multiply",
                    "id": l3_id,
                    "imageFile": f"{l3_id}.png",
                    "isVisible": True,
                    "name": "BlendLayer",
                    "opacity": 0.75,
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 3,
            "width": 64
        },
        images={f"{l3_id}.png": img64}
    )

    # 4. v4_layer_mask.comp
    l4_id = "44444444-4444-4444-4444-444444444444"
    write_package(
        os.path.join(valid_dir, "v4_layer_mask.comp"),
        {
            "activeLayerID": l4_id,
            "colorSpace": "sRGB",
            "documentID": "00000004-0000-0000-0000-000000000004",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "blendMode": "Normal",
                    "id": l4_id,
                    "imageFile": f"{l4_id}.png",
                    "isVisible": True,
                    "maskEnabled": True,
                    "maskFile": f"{l4_id}.mask.png",
                    "name": "MaskedLayer",
                    "opacity": 1.0,
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 4,
            "width": 64
        },
        images={f"{l4_id}.png": img64},
        masks={f"{l4_id}.mask.png": mask64}
    )

    # 5. v5_clipping_mask.comp
    base5_id = "55555555-5555-5555-5555-555555555551"
    clip5_id = "55555555-5555-5555-5555-555555555552"
    write_package(
        os.path.join(valid_dir, "v5_clipping_mask.comp"),
        {
            "activeLayerID": clip5_id,
            "colorSpace": "sRGB",
            "documentID": "00000005-0000-0000-0000-000000000005",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": base5_id,
                    "imageFile": f"{base5_id}.png",
                    "isVisible": True,
                    "name": "Base",
                    "transform": default_transform(64, 64)
                },
                {
                    "id": clip5_id,
                    "imageFile": f"{clip5_id}.png",
                    "isVisible": True,
                    "maskSourceID": base5_id,
                    "name": "Clipped",
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 5,
            "width": 64
        },
        images={f"{base5_id}.png": img64, f"{clip5_id}.png": img64_red}
    )

    # 6. v6_folder_mask.comp
    grp6_id = "66666666-6666-6666-6666-666666666661"
    child6_id = "66666666-6666-6666-6666-666666666662"
    write_package(
        os.path.join(valid_dir, "v6_folder_mask.comp"),
        {
            "activeLayerID": child6_id,
            "colorSpace": "sRGB",
            "documentID": "00000006-0000-0000-0000-000000000006",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": grp6_id,
                    "isGroup": True,
                    "isVisible": True,
                    "maskEnabled": True,
                    "maskFile": f"{grp6_id}.mask.png",
                    "name": "MaskedFolder",
                    "transform": default_transform(64, 64)
                },
                {
                    "id": child6_id,
                    "imageFile": f"{child6_id}.png",
                    "isVisible": True,
                    "name": "Child",
                    "parentID": grp6_id,
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 6,
            "width": 64
        },
        images={f"{child6_id}.png": img64},
        masks={f"{grp6_id}.mask.png": mask64}
    )

    # 7. v7_adjustments.comp
    bg7_id = "77777777-7777-7777-7777-777777777770"
    adj_hsv_id = "77777777-7777-7777-7777-777777777771"
    adj_levels_id = "77777777-7777-7777-7777-777777777772"
    adj_curves_id = "77777777-7777-7777-7777-777777777773"
    adj_exp_id = "77777777-7777-7777-7777-777777777774"
    adj_gmap_id = "77777777-7777-7777-7777-777777777775"
    adj_grain_id = "77777777-7777-7777-7777-777777777776"
    adj_inv_id = "77777777-7777-7777-7777-777777777777"
    adj_bw_id = "77777777-7777-7777-7777-777777777778"
    adj_cb_id = "77777777-7777-7777-7777-777777777779"

    hsv_adj = default_adjustment_base()
    hsv_adj.update({"kind": "Hue/Saturation", "hue": 30.0, "saturation": 20.0, "lightness": -10.0})

    levels_adj = default_adjustment_base()
    levels_adj.update({"kind": "Levels"})

    curves_adj = default_adjustment_base()
    curves_adj.update({"kind": "Curves"})

    exp_adj = default_adjustment_base()
    exp_adj.update({"kind": "Exposure", "exposureSettings": {"exposure": 1.2, "gamma": 1.1, "offset": 0.05}})

    gmap_adj = default_adjustment_base()
    gmap_adj.update({
        "kind": "Gradient Map",
        "gradientMapSettings": {
            "highlights": {"blue": 0.8, "green": 0.9, "red": 0.95},
            "reversed": False,
            "shadows": {"blue": 0.2, "green": 0.05, "red": 0.1}
        }
    })

    grain_adj = default_adjustment_base()
    grain_adj.update({"kind": "Grain", "grainSettings": {"amount": 25.0, "roughness": 50.0, "seed": 100, "size": 1.5}})

    inv_adj = default_adjustment_base()
    inv_adj.update({"kind": "Invert"})

    bw_adj = default_adjustment_base()
    bw_adj.update({
        "kind": "Black & White",
        "blackWhiteSettings": {
            "blues": 20.0, "cyans": 60.0, "greens": 40.0, "magentas": 80.0, "reds": 40.0, "yellows": 60.0,
            "tint": False, "tintHue": 40.0, "tintSaturation": 20.0
        }
    })

    cb_adj = default_adjustment_base()
    cb_adj.update({
        "kind": "Color Balance",
        "colorBalanceSettings": {
            "highlightCyanRed": 0.0, "highlightMagentaGreen": 0.0, "highlightYellowBlue": 0.0,
            "midCyanRed": 10.0, "midMagentaGreen": -5.0, "midYellowBlue": 15.0,
            "preserveLuminosity": True,
            "shadowCyanRed": 0.0, "shadowMagentaGreen": 0.0, "shadowYellowBlue": 0.0
        }
    })

    write_package(
        os.path.join(valid_dir, "v7_adjustments.comp"),
        {
            "activeLayerID": bg7_id,
            "colorSpace": "sRGB",
            "documentID": "00000007-0000-0000-0000-000000000007",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {"id": bg7_id, "imageFile": f"{bg7_id}.png", "isVisible": True, "name": "Background", "transform": default_transform(64, 64)},
                {"adjustment": hsv_adj, "id": adj_hsv_id, "isVisible": True, "name": "HSV", "transform": default_transform(64, 64)},
                {"adjustment": levels_adj, "id": adj_levels_id, "isVisible": True, "name": "Levels", "transform": default_transform(64, 64)},
                {"adjustment": curves_adj, "id": adj_curves_id, "isVisible": True, "name": "Curves", "transform": default_transform(64, 64)},
                {"adjustment": exp_adj, "id": adj_exp_id, "isVisible": True, "name": "Exposure", "transform": default_transform(64, 64)},
                {"adjustment": gmap_adj, "id": adj_gmap_id, "isVisible": True, "name": "GradientMap", "transform": default_transform(64, 64)},
                {"adjustment": grain_adj, "id": adj_grain_id, "isVisible": True, "name": "Grain", "transform": default_transform(64, 64)},
                {"adjustment": inv_adj, "id": adj_inv_id, "isVisible": True, "name": "Invert", "transform": default_transform(64, 64)},
                {"adjustment": bw_adj, "id": adj_bw_id, "isVisible": True, "name": "BW", "transform": default_transform(64, 64)},
                {"adjustment": cb_adj, "id": adj_cb_id, "isVisible": True, "name": "ColorBalance", "transform": default_transform(64, 64)}
            ],
            "resolution": 72.0,
            "version": 7,
            "width": 64
        },
        images={f"{bg7_id}.png": img64}
    )

    # 8. v8_folder_opacity_guides.comp
    grp8_id = "88888888-8888-8888-8888-888888888881"
    child8_id = "88888888-8888-8888-8888-888888888882"
    guide1_id = "88888888-AAAA-1111-2222-333333333333"
    guide2_id = "88888888-BBBB-1111-2222-333333333333"
    write_package(
        os.path.join(valid_dir, "v8_folder_opacity_guides.comp"),
        {
            "activeLayerID": child8_id,
            "colorSpace": "sRGB",
            "documentID": "00000008-0000-0000-0000-000000000008",
            "format": "com.compositor.project",
            "guides": [
                {"axis": "horizontal", "id": guide1_id, "position": 32.0},
                {"axis": "vertical", "id": guide2_id, "position": 16.0}
            ],
            "height": 64,
            "layers": [
                {
                    "blendMode": "Normal",
                    "id": grp8_id,
                    "isGroup": True,
                    "isVisible": True,
                    "name": "DimmedFolder",
                    "opacity": 0.4,
                    "transform": default_transform(64, 64)
                },
                {
                    "id": child8_id,
                    "imageFile": f"{child8_id}.png",
                    "isVisible": True,
                    "name": "Child",
                    "parentID": grp8_id,
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 8,
            "width": 64
        },
        images={f"{child8_id}.png": img64}
    )

    # 9. v9_blurs_noise.comp
    bg9_id = "99999999-9999-9999-9999-999999999990"
    adj_gblur_id = "99999999-9999-9999-9999-999999999991"
    adj_mblur_id = "99999999-9999-9999-9999-999999999992"
    adj_noise_id = "99999999-9999-9999-9999-999999999993"

    gblur_adj = default_adjustment_base()
    gblur_adj.update({"blurRadius": 12.5, "kind": "Gaussian Blur"})

    mblur_adj = default_adjustment_base()
    mblur_adj.update({"kind": "Motion Blur", "motionAngle": 30.0, "motionDistance": 20.0})

    noise_adj = default_adjustment_base()
    noise_adj.update({"kind": "Add Noise", "noiseAmount": 45.0, "noiseGaussian": True, "noiseMonochromatic": True, "noiseSeed": 98765})

    write_package(
        os.path.join(valid_dir, "v9_blurs_noise.comp"),
        {
            "activeLayerID": bg9_id,
            "colorSpace": "sRGB",
            "documentID": "00000009-0000-0000-0000-000000000009",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {"id": bg9_id, "imageFile": f"{bg9_id}.png", "isVisible": True, "name": "Background", "transform": default_transform(64, 64)},
                {"adjustment": gblur_adj, "id": adj_gblur_id, "isVisible": True, "name": "GaussianBlur", "transform": default_transform(64, 64)},
                {"adjustment": mblur_adj, "id": adj_mblur_id, "isVisible": True, "name": "MotionBlur", "transform": default_transform(64, 64)},
                {"adjustment": noise_adj, "id": adj_noise_id, "isVisible": True, "name": "AddNoise", "transform": default_transform(64, 64)}
            ],
            "resolution": 72.0,
            "version": 9,
            "width": 64
        },
        images={f"{bg9_id}.png": img64}
    )

    # 10. additive_fields.comp (effects, text, shape, maskPlacement)
    add_effects_id = "AAAAAAAA-1111-2222-3333-000000000001"
    add_text_id = "AAAAAAAA-1111-2222-3333-000000000002"
    add_shape_id = "AAAAAAAA-1111-2222-3333-000000000003"
    add_unlinked_id = "AAAAAAAA-1111-2222-3333-000000000004"

    effects_record = {
        "colorOverlay": {
            "enabled": True,
            "opacity": 0.65,
            "red": 0.9,
            "green": 0.1,
            "blue": 0.4
        },
        "innerGlow": {
            "enabled": True,
            "opacity": 0.7,
            "red": 0.5,
            "green": 1.0,
            "blue": 0.5,
            "size": 8.0
        },
        "innerShadow": {
            "angle": 135.0,
            "blur": 4.0,
            "distance": 6.0,
            "enabled": True,
            "opacity": 0.5,
            "red": 0.05,
            "green": 0.05,
            "blue": 0.05
        },
        "outerGlow": {
            "enabled": True,
            "opacity": 0.8,
            "red": 1.0,
            "green": 1.0,
            "blue": 0.5,
            "size": 15.0
        },
        "shadow": {
            "angle": 45.0,
            "blur": 10.0,
            "distance": 15.0,
            "enabled": True,
            "opacity": 0.75,
            "red": 0.2,
            "green": 0.2,
            "blue": 0.3
        },
        "stroke": {
            "enabled": True,
            "inside": False,
            "opacity": 0.9,
            "red": 0.1,
            "green": 0.8,
            "blue": 0.2,
            "size": 4.0
        }
    }

    text_record = {
        "alignment": "Center",
        "boxSize": [200.0, 50.0],
        "content": "Hello World",
        "fontName": "Helvetica",
        "fontSize": 24.0,
        "leading": 28.0,
        "red": 0.1,
        "green": 0.2,
        "blue": 0.3,
        "tracking": 5.0
    }

    shape_record = {
        "cornerRadius": 8.0,
        "kind": "Rectangle",
        "red": 0.2,
        "green": 0.8,
        "blue": 0.2
    }

    write_package(
        os.path.join(valid_dir, "additive_fields.comp"),
        {
            "activeLayerID": add_effects_id,
            "colorSpace": "sRGB",
            "documentID": "0000000A-0000-0000-0000-00000000000A",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "effects": effects_record,
                    "id": add_effects_id,
                    "imageFile": f"{add_effects_id}.png",
                    "isVisible": True,
                    "name": "EffectsLayer",
                    "transform": default_transform(64, 64)
                },
                {
                    "id": add_text_id,
                    "imageFile": f"{add_text_id}.png",
                    "isVisible": True,
                    "name": "TextLayer",
                    "text": text_record,
                    "transform": default_transform(64, 64)
                },
                {
                    "id": add_shape_id,
                    "imageFile": f"{add_shape_id}.png",
                    "isVisible": True,
                    "name": "ShapeLayer",
                    "shape": shape_record,
                    "transform": default_transform(64, 64)
                },
                {
                    "id": add_unlinked_id,
                    "imageFile": f"{add_unlinked_id}.png",
                    "isVisible": True,
                    "maskEnabled": True,
                    "maskFile": f"{add_unlinked_id}.mask.png",
                    "maskLinked": False,
                    "maskPlacement": default_transform(40, 40, 10, 10),
                    "name": "UnlinkedMaskLayer",
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 9,
            "width": 64
        },
        images={
            f"{add_effects_id}.png": img64,
            f"{add_text_id}.png": img64,
            f"{add_shape_id}.png": img64,
            f"{add_unlinked_id}.png": img64
        },
        masks={
            f"{add_unlinked_id}.mask.png": mask64
        }
    )

    # 11. lx_legacy_text.comp (shape.kind == "Text")
    lx_text_id = "BBBBBBBB-1111-2222-3333-000000000001"
    write_package(
        os.path.join(valid_dir, "lx_legacy_text.comp"),
        {
            "activeLayerID": lx_text_id,
            "colorSpace": "sRGB",
            "documentID": "0000000B-0000-0000-0000-00000000000B",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": lx_text_id,
                    "imageFile": f"{lx_text_id}.png",
                    "isVisible": True,
                    "name": "LegacyText",
                    "shape": {
                        "alignment": 1,
                        "areaText": True,
                        "baseHeight": 40,
                        "baseWidth": 150,
                        "bold": True,
                        "fill": "#ffff0000",
                        "fontFamily": "DejaVu Sans",
                        "italic": False,
                        "kind": "Text",
                        "pixelSize": 32,
                        "text": "Legacy LX Text",
                        "underline": False
                    },
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 7,
            "width": 64
        },
        images={f"{lx_text_id}.png": img64}
    )

    # 12. v7_text.comp (v7 document with macOS editable text metadata)
    v7_text_layer_id = "CCCCCCCC-1111-2222-3333-000000000001"
    write_package(
        os.path.join(valid_dir, "v7_text.comp"),
        {
            "activeLayerID": v7_text_layer_id,
            "colorSpace": "sRGB",
            "documentID": "0000000C-0000-0000-0000-00000000000C",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": v7_text_layer_id,
                    "imageFile": f"{v7_text_layer_id}.png",
                    "isVisible": True,
                    "name": "TextLayer",
                    "text": {
                        "alignment": "Center",
                        "boxSize": [200.0, 50.0],
                        "content": "Hello World",
                        "fontName": "Helvetica",
                        "fontSize": 24.0,
                        "leading": 28.0,
                        "red": 0.1,
                        "green": 0.2,
                        "blue": 0.3,
                        "tracking": 5.0
                    },
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 7,
            "width": 64
        },
        images={f"{v7_text_layer_id}.png": img64}
    )

    # 12b. v10/v11 text runs (per-letter color and face), plus negative variants
    def text_runs_manifest(version, text_extra, doc_suffix):
        lid = "CCCCCCCC-4444-5555-6666-0000000000" + doc_suffix
        text = {
            "alignment": "Left", "content": "Hello World", "fontName": "Helvetica", "fontSize": 24.0,
            "leading": 0.0, "red": 0.1, "green": 0.2, "blue": 0.3, "tracking": 0.0,
        }
        text.update(text_extra)
        return {
            "activeLayerID": lid, "colorSpace": "sRGB",
            "documentID": "0000000D-0000-0000-0000-0000000000" + doc_suffix,
            "format": "com.compositor.project", "height": 64,
            "layers": [{"id": lid, "imageFile": f"{lid}.png", "isVisible": True, "name": "TextLayer",
                        "text": text, "transform": default_transform(64, 64)}],
            "resolution": 72.0, "version": version, "width": 64,
        }, {f"{lid}.png": img64}
    color_runs = [{"location": 0, "length": 5, "red": 1.0, "green": 0.0, "blue": 0.0},
                  {"location": 6, "length": 5, "red": 0.0, "green": 0.0, "blue": 1.0}]
    font_runs = [{"location": 6, "length": 5, "fontName": "Courier"}]
    manifest, imgs = text_runs_manifest(10, {"colorRuns": color_runs}, "10")
    write_package(os.path.join(valid_dir, "v10_text_color_runs.comp"), manifest, images=imgs)
    manifest, imgs = text_runs_manifest(11, {"colorRuns": color_runs, "fontRuns": font_runs}, "11")
    write_package(os.path.join(valid_dir, "v11_text_font_runs.comp"), manifest, images=imgs)
    negatives = {
        "malformed_v9_with_color_runs": (9, {"colorRuns": color_runs}),
        "malformed_v10_with_font_runs": (10, {"fontRuns": font_runs}),
        "malformed_text_runs_overlap": (11, {"colorRuns": [
            {"location": 0, "length": 6, "red": 1.0, "green": 0.0, "blue": 0.0},
            {"location": 5, "length": 3, "red": 0.0, "green": 0.0, "blue": 1.0}]}),
        "malformed_text_runs_past_end": (11, {"fontRuns": [{"location": 8, "length": 9, "fontName": "Courier"}]}),
        "malformed_text_runs_empty": (11, {"colorRuns": []}),
        "malformed_text_run_color_range": (11, {"colorRuns": [{"location": 0, "length": 2, "red": 1.5, "green": 0.0, "blue": 0.0}]}),
    }
    for index, (name, (version, extra)) in enumerate(negatives.items()):
        manifest, imgs = text_runs_manifest(version, extra, "2%d" % index)
        write_package(os.path.join(malformed_dir, name + ".comp"), manifest, images=imgs)

    # 13. v9_effects.comp (v9 document dedicated to all 6 layer effects)
    v9_eff_bg_id = "DDDDDDDD-1111-2222-3333-000000000001"
    v9_eff_layer_id = "DDDDDDDD-1111-2222-3333-000000000002"
    write_package(
        os.path.join(valid_dir, "v9_effects.comp"),
        {
            "activeLayerID": v9_eff_layer_id,
            "colorSpace": "sRGB",
            "documentID": "0000000D-0000-0000-0000-00000000000D",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": v9_eff_bg_id,
                    "imageFile": f"{v9_eff_bg_id}.png",
                    "isVisible": True,
                    "name": "Background",
                    "transform": default_transform(64, 64)
                },
                {
                    "effects": effects_record,
                    "id": v9_eff_layer_id,
                    "imageFile": f"{v9_eff_layer_id}.png",
                    "isVisible": True,
                    "name": "EffectsLayer",
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 9,
            "width": 64
        },
        images={
            f"{v9_eff_bg_id}.png": img64,
            f"{v9_eff_layer_id}.png": img64
        }
    )

    # 14. v9_shapes.comp (v9 document dedicated to Rectangle, Ellipse, and Line shapes)
    v9_shape_bg_id = "EEEEEEEE-1111-2222-3333-000000000001"
    v9_rect_id = "EEEEEEEE-1111-2222-3333-000000000002"
    v9_ellipse_id = "EEEEEEEE-1111-2222-3333-000000000003"
    v9_line_id = "EEEEEEEE-1111-2222-3333-000000000004"
    write_package(
        os.path.join(valid_dir, "v9_shapes.comp"),
        {
            "activeLayerID": v9_rect_id,
            "colorSpace": "sRGB",
            "documentID": "0000000E-0000-0000-0000-00000000000E",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": v9_shape_bg_id,
                    "imageFile": f"{v9_shape_bg_id}.png",
                    "isVisible": True,
                    "name": "Background",
                    "transform": default_transform(64, 64)
                },
                {
                    "id": v9_rect_id,
                    "imageFile": f"{v9_rect_id}.png",
                    "isVisible": True,
                    "name": "RectangleShape",
                    "shape": {
                        "cornerRadius": 12.0,
                        "kind": "Rectangle",
                        "red": 0.2,
                        "green": 0.8,
                        "blue": 0.3
                    },
                    "transform": default_transform(64, 64)
                },
                {
                    "id": v9_ellipse_id,
                    "imageFile": f"{v9_ellipse_id}.png",
                    "isVisible": True,
                    "name": "EllipseShape",
                    "shape": {
                        "kind": "Ellipse",
                        "red": 0.9,
                        "green": 0.3,
                        "blue": 0.2
                    },
                    "transform": default_transform(64, 64)
                },
                {
                    "id": v9_line_id,
                    "imageFile": f"{v9_line_id}.png",
                    "isVisible": True,
                    "name": "LineShape",
                    "shape": {
                        "end": [0.9, 0.9],
                        "kind": "Line",
                        "lineWidth": 4.0,
                        "red": 0.2,
                        "green": 0.5,
                        "blue": 0.9,
                        "start": [0.1, 0.1]
                    },
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 9,
            "width": 64
        },
        images={
            f"{v9_shape_bg_id}.png": img64,
            f"{v9_rect_id}.png": img64,
            f"{v9_ellipse_id}.png": img64,
            f"{v9_line_id}.png": img64
        }
    )

    # 15. Negative / Malformed fixtures
    # A. malformed_not_a_dir.comp
    not_dir_path = os.path.join(malformed_dir, "malformed_not_a_dir.comp")
    if os.path.isdir(not_dir_path):
        shutil.rmtree(not_dir_path)
    with open(not_dir_path, "w") as f:
        f.write("I am a file, not a directory package.\n")

    # B. malformed_missing_manifest.comp
    os.makedirs(os.path.join(malformed_dir, "malformed_missing_manifest.comp"), exist_ok=True)

    # C. malformed_corrupt_manifest.comp
    corrupt_dir = os.path.join(malformed_dir, "malformed_corrupt_manifest.comp")
    os.makedirs(corrupt_dir, exist_ok=True)
    with open(os.path.join(corrupt_dir, "manifest.json"), "w") as f:
        f.write("{ not valid json !!! }")

    # Helper for malformed variants
    def copy_manifest_variant(name, mutator, images=None, masks=None):
        m = {
            "activeLayerID": l1_id,
            "colorSpace": "sRGB",
            "documentID": "00000001-0000-0000-0000-000000000001",
            "format": "com.compositor.project",
            "height": 64,
            "layers": [
                {
                    "id": l1_id,
                    "imageFile": f"{l1_id}.png",
                    "isVisible": True,
                    "name": "Background",
                    "transform": default_transform(64, 64)
                }
            ],
            "resolution": 72.0,
            "version": 7,
            "width": 64
        }
        mutator(m)
        write_package(os.path.join(malformed_dir, f"{name}.comp"), m,
                      images=images if images is not None else {f"{l1_id}.png": img64},
                      masks=masks)

    # D. malformed_future_version.comp
    copy_manifest_variant("malformed_future_version", lambda m: m.update({"version": 42}))

    # E. malformed_zero_version.comp
    copy_manifest_variant("malformed_zero_version", lambda m: m.update({"version": 0}))

    # F. malformed_color_space.comp
    copy_manifest_variant("malformed_color_space", lambda m: m.update({"colorSpace": "Display P3"}))

    # G. malformed_canvas_size_zero.comp
    copy_manifest_variant("malformed_canvas_size_zero", lambda m: m.update({"width": 0}))

    # H. malformed_canvas_size_huge.comp
    copy_manifest_variant("malformed_canvas_size_huge", lambda m: m.update({"width": 50000}))

    # I. malformed_resolution_low.comp
    copy_manifest_variant("malformed_resolution_low", lambda m: m.update({"resolution": 0.5}))

    # J. malformed_path_traversal.comp
    copy_manifest_variant("malformed_path_traversal", lambda m: m["layers"][0].update({"imageFile": "../../outside.png"}))

    # K. malformed_missing_image.comp
    copy_manifest_variant("malformed_missing_image", lambda m: None, images={})

    # L. malformed_duplicate_layer_id.comp
    def dup_id(m):
        m["layers"].append({
            "id": l1_id,
            "imageFile": f"{l1_id}.png",
            "isVisible": True,
            "name": "Duplicate",
            "transform": default_transform(64, 64)
        })
    copy_manifest_variant("malformed_duplicate_layer_id", dup_id)

    # M. malformed_unknown_blend_mode.comp
    copy_manifest_variant("malformed_unknown_blend_mode", lambda m: m["layers"][0].update({"blendMode": "MysticBurn"}))

    # N. malformed_group_with_image.comp
    copy_manifest_variant("malformed_group_with_image", lambda m: m["layers"][0].update({"isGroup": True, "imageFile": f"{l1_id}.png"}))

    # O. malformed_group_cycle.comp
    g_a = "11111111-AAAA-0000-0000-000000000001"
    g_b = "11111111-BBBB-0000-0000-000000000002"
    def grp_cycle(m):
        m["layers"] = [
            {"id": g_a, "isGroup": True, "name": "A", "parentID": g_b, "transform": default_transform(64, 64), "isVisible": True},
            {"id": g_b, "isGroup": True, "name": "B", "parentID": g_a, "transform": default_transform(64, 64), "isVisible": True}
        ]
        m["activeLayerID"] = g_a
    copy_manifest_variant("malformed_group_cycle", grp_cycle, images={})

    # P. malformed_clipping_cycle.comp
    c_a = "22222222-AAAA-0000-0000-000000000001"
    c_b = "22222222-BBBB-0000-0000-000000000002"
    def clip_cycle(m):
        m["layers"] = [
            {"id": c_a, "imageFile": f"{c_a}.png", "maskSourceID": c_b, "name": "A", "transform": default_transform(64, 64), "isVisible": True},
            {"id": c_b, "imageFile": f"{c_b}.png", "maskSourceID": c_a, "name": "B", "transform": default_transform(64, 64), "isVisible": True}
        ]
        m["activeLayerID"] = c_a
    copy_manifest_variant("malformed_clipping_cycle", clip_cycle, images={f"{c_a}.png": img64, f"{c_b}.png": img64})

    # Q. malformed_clipping_to_group.comp
    g_target = "33333333-AAAA-0000-0000-000000000001"
    c_source = "33333333-BBBB-0000-0000-000000000002"
    def clip_to_grp(m):
        m["layers"] = [
            {"id": g_target, "isGroup": True, "name": "Group", "transform": default_transform(64, 64), "isVisible": True},
            {"id": c_source, "imageFile": f"{c_source}.png", "maskSourceID": g_target, "name": "Clipped", "transform": default_transform(64, 64), "isVisible": True}
        ]
        m["activeLayerID"] = c_source
    copy_manifest_variant("malformed_clipping_to_group", clip_to_grp, images={f"{c_source}.png": img64})

    # R. malformed_v1_with_group.comp
    def v1_grp(m):
        m["version"] = 1
        m["layers"][0]["isGroup"] = True
        del m["layers"][0]["imageFile"]
    copy_manifest_variant("malformed_v1_with_group", v1_grp, images={})

    # S. malformed_v1_with_mask.comp
    def v1_mask(m):
        m["version"] = 1
        m["layers"][0]["maskFile"] = f"{l1_id}.mask.png"
    copy_manifest_variant("malformed_v1_with_mask", v1_mask, masks={f"{l1_id}.mask.png": mask64})

    # T. malformed_v6_with_adjustment.comp
    def v6_adj(m):
        m["version"] = 6
        adj = default_adjustment_base()
        adj.update({"kind": "Invert"})
        m["layers"][0]["adjustment"] = adj
        del m["layers"][0]["imageFile"]
    copy_manifest_variant("malformed_v6_with_adjustment", v6_adj, images={})

    # U. malformed_v7_with_guides.comp
    def v7_guides(m):
        m["version"] = 7
        m["guides"] = [{"axis": "vertical", "id": "99999999-0000-0000-0000-000000000001", "position": 20.0}]
    copy_manifest_variant("malformed_v7_with_guides", v7_guides)

    # V. malformed_v7_with_dimmed_folder.comp
    def v7_dimmed(m):
        m["version"] = 7
        m["layers"] = [
            {"id": "44444444-0000-0000-0000-000000000001", "isGroup": True, "name": "Folder", "opacity": 0.5, "transform": default_transform(64, 64), "isVisible": True},
            {"id": l1_id, "imageFile": f"{l1_id}.png", "name": "Child", "parentID": "44444444-0000-0000-0000-000000000001", "transform": default_transform(64, 64), "isVisible": True}
        ]
        m["activeLayerID"] = l1_id
    copy_manifest_variant("malformed_v7_with_dimmed_folder", v7_dimmed)

    # W. malformed_v8_with_v9_blur.comp
    def v8_v9blur(m):
        m["version"] = 8
        adj = default_adjustment_base()
        adj.update({"blurRadius": 10.0, "kind": "Gaussian Blur"})
        m["layers"] = [
            {"id": "55555555-0000-0000-0000-000000000001", "adjustment": adj, "name": "Blur", "transform": default_transform(64, 64), "isVisible": True}
        ]
        m["activeLayerID"] = "55555555-0000-0000-0000-000000000001"
    copy_manifest_variant("malformed_v8_with_v9_blur", v8_v9blur, images={})

    print(f"Generated fixtures in {base_dir}")

if __name__ == "__main__":
    import sys
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(__file__)
    generate_all(target)
