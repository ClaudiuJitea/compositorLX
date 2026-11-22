#!/usr/bin/env python3
"""scripts/convert-depth-model.py <DepthMini.mlpackage> <depthmini.onnx>

Needs coremltools, onnx and numpy (pip install coremltools onnx numpy); the package is built from the three files of
https://github.com/robbietilton/Compositor/releases/tag/depth-model-1 laid out as Core ML expects:
  DepthMini.mlpackage/Manifest.json
  DepthMini.mlpackage/Data/com.apple.CoreML/model.mlmodel
  DepthMini.mlpackage/Data/com.apple.CoreML/weights/weight.bin

Converts the DepthMini Core ML program (an ML Program of conv, relu, hard sigmoid, thresholded relu, add, mul, sub,
reduce_mean, bilinear upsampling, concat and 6-bit palettized weights) to an equivalent fp32 ONNX graph."""
import sys
import numpy as np
import onnx
from onnx import helper, TensorProto, numpy_helper
from coremltools.models.utils import load_spec
from coremltools.converters.mil.frontend.milproto.load import load as milload

package, out = sys.argv[1], sys.argv[2]
spec = load_spec(package)
prog = milload(spec, specification_version=spec.specificationVersion, file_weights_dir=package + "/Data/com.apple.CoreML/weights")
fn = prog.functions["main"]
nodes, inits, values = [], {}, {}

def value_of(var):
    if var.name in values: return values[var.name]
    v = var.val
    if v is None and var.op is not None and var.op.op_type.startswith("constexpr"):
        op = var.op
        v = op.materialized_val_inference() if hasattr(op, "materialized_val_inference") else op.value_inference()
    return v

def palettized(var):
    """A 6-bit palettized weight as uint8 indices into its table, expanded by a Gather when the model loads: the weights
    stay as small as Core ML keeps them, and take exactly the table's values."""
    op = var.op
    if op is None or not op.op_type.startswith("constexpr_lut"): return None
    dense = np.asarray(value_of(var)).astype(np.float32)
    lut = np.unique(dense)
    if lut.size > 256: return None
    name = var.name + "__lut"
    if name not in inits:
        index = np.searchsorted(lut, dense).astype(np.uint8)
        assert np.array_equal(lut[index], dense)
        inits[name + "_table"] = numpy_helper.from_array(lut, name + "_table")
        inits[name + "_index"] = numpy_helper.from_array(index, name + "_index")
        nodes.append(helper.make_node("Cast", [name + "_index"], [name + "_wide"], to=TensorProto.INT64))
        nodes.append(helper.make_node("Gather", [name + "_table", name + "_wide"], [name], axis=0))
        inits[name] = None
    return name

def ref(var):
    """The ONNX name for `var`: an initializer for a constant, else its tensor."""
    packed = palettized(var)
    if packed: return packed
    v = value_of(var)
    if v is None: return var.name
    name = var.name + "__c"
    if name not in inits:
        arr = np.asarray(v)
        arr = arr.astype(np.float32) if arr.dtype.kind == 'f' else arr.astype(np.int64)
        inits[name] = numpy_helper.from_array(arr, name)
    return name

for op in fn.operations:
    t = op.op_type
    if t == "const" or t.startswith("constexpr"): continue
    o = op.outputs[0].name
    i = op.inputs
    if t == "cast":
        nodes.append(helper.make_node("Identity", [ref(i["x"])], [o]))
    elif t in ("add", "mul", "sub"):
        nodes.append(helper.make_node({"add": "Add", "mul": "Mul", "sub": "Sub"}[t], [ref(i["x"]), ref(i["y"])], [o]))
    elif t == "relu":
        nodes.append(helper.make_node("Relu", [ref(i["x"])], [o]))
    elif t == "thresholded_relu":
        nodes.append(helper.make_node("ThresholdedRelu", [ref(i["x"])], [o], alpha=float(i["alpha"].val)))
    elif t == "sigmoid_hard":
        nodes.append(helper.make_node("HardSigmoid", [ref(i["x"])], [o], alpha=float(i["alpha"].val), beta=float(i["beta"].val)))
    elif t == "reduce_mean":
        axes = [int(a) for a in np.atleast_1d(i["axes"].val)]
        nodes.append(helper.make_node("ReduceMean", [ref(i["x"])], [o], axes=axes, keepdims=int(bool(i["keep_dims"].val))))
    elif t == "conv":
        pad_type = i["pad_type"].val
        kernel = list(value_of(i["weight"]).shape[2:])
        attrs = dict(strides=[int(s) for s in i["strides"].val], dilations=[int(d) for d in i["dilations"].val],
                     group=int(i["groups"].val), kernel_shape=kernel)
        if pad_type == "custom":
            p = [int(x) for x in i["pad"].val]           # MIL: h_begin, h_end, w_begin, w_end
            attrs["pads"] = [p[0], p[2], p[1], p[3]]     # ONNX: h_begin, w_begin, h_end, w_end
        elif pad_type == "valid":
            attrs["pads"] = [0, 0, 0, 0]
        else:
            raise SystemExit(f"pad_type {pad_type}")
        ins = [ref(i["x"]), ref(i["weight"])] + ([ref(i["bias"])] if "bias" in i and i["bias"] is not None else [])
        nodes.append(helper.make_node("Conv", ins, [o], **attrs))
    elif t == "upsample_bilinear":
        assert not bool(i["align_corners"].val)
        scales = o + "__scales"
        inits[scales] = numpy_helper.from_array(np.array([1, 1, float(i["scale_factor_height"].val), float(i["scale_factor_width"].val)], np.float32), scales)
        nodes.append(helper.make_node("Resize", [ref(i["x"]), "", scales], [o], mode="linear", coordinate_transformation_mode="half_pixel"))
    elif t == "concat":
        assert not bool(i["interleave"].val)
        nodes.append(helper.make_node("Concat", [ref(v) for v in i["values"]], [o], axis=int(i["axis"].val)))
    else:
        raise SystemExit(f"unhandled op {t}")

graph = helper.make_graph(nodes, "DepthMini",
    [helper.make_tensor_value_info("image", TensorProto.FLOAT, [1, 3, "height", "width"])],
    [helper.make_tensor_value_info("depth", TensorProto.FLOAT, [1, 1, "height", "width"])],
    initializer=[v for v in inits.values() if v is not None])
model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)], producer_name="compositorLX mil2onnx")
model.ir_version = 8
model.doc_string = ("DepthMini (macOS Compositor's depth model, round 5 'depth-model-1', distilled from Depth Anything V2 Small, "
                    "Apache-2.0), converted from its Core ML program to fp32 ONNX. Input: RGB 0-255 at 512x512, 512x384, "
                    "512x288, 384x512 or 288x512 (width x height). Output: relative inverse depth, higher nearer.")
onnx.checker.check_model(model)
onnx.save(model, out)
print("nodes", len(nodes), "initializers", len(inits))
