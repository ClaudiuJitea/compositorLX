#pragma once
#include "rendering/GpuRenderer.h"
#include "rendering/RasterOperations.h"
#include "rendering/RgbSplit.h"
struct ProgressiveBlurSettings;
namespace compositor::GpuOperations {
QImage gaussian(const QImage &, double radius, bool byteAlgorithm);
QImage motion(const QImage &, double angle, double distance);
int appendProgressive(GpuGraph &, int input, const ProgressiveBlurSettings &, unsigned sourcePrecision);
// `origin` is the canvas position of the input's top-left pixel (for tiles), which keys the sample jitter.
int appendRgbSplit(GpuGraph &, int input, const RgbSplitSettings &, unsigned edges, QPoint origin = {});
QImage progressive(const QImage &, const ProgressiveBlurSettings &);
QImage bloom(const QImage &, double amount, double radius, bool deep);
QImage rgbSplit(const QImage &, const RgbSplitSettings &, unsigned, GpuExecution *);
QImage lookup(const QImage &, const float *tables);
QImage cube(const QImage &, const float *table, int grid, int clipping = 0);
QImage simple(const QImage &, GpuOp, const GpuParameters &parameters = {}, unsigned quantization = 1);
QImage composite(const QImage &, const QImage &, int mode, float opacity = 1, unsigned quantization = 0);
// Camera Raw's composed color table on 8-bit premultiplied pixels, in place: exactly what adjust_camera_raw makes
// (macOS MetalCameraRawLookup: the GPU's float lookup, with pixels near a rounding boundary redone by the CPU in
// double precision). False, with `pixels` unchanged, when it did not run; `backend` Auto decides by size.
bool cameraRawLookup(QImage &pixels, const float *table, int grid, RenderBackend backend = RenderBackend::Auto);
// The live adjustment kinds as passes on a graph, for the destructive filters and the document graph alike.
// `quantization` rounds the result as the source's format does (GpuQuantization). Noise and Grain take the document
// position of the input's top-left pixel, so tiles and the whole canvas draw the same pattern.
int appendHueSaturation(GpuGraph &, int input, const HueSaturationSettings &, unsigned quantization);
int appendGradientMap(GpuGraph &, int input, const std::array<uint8_t, 768> &table, unsigned quantization);
int appendBlackWhite(GpuGraph &, int input, const float *weights, bool tint, double tintHue, double tintSaturation,
                     unsigned quantization);
int appendColorBalance(GpuGraph &, int input, const float *shadows, const float *midtones, const float *highlights,
                       bool preserveLuminosity, unsigned quantization);
int appendMotion(GpuGraph &, int input, double angle, double distance, unsigned quantization, QPoint origin = {});
int appendNoise(GpuGraph &, int input, float amount, bool gaussian, bool monochromatic, quint32 seed, QPoint origin,
                unsigned quantization);
int appendGrain(GpuGraph &, int input, double amount, double size, double roughness, quint32 seed, QPointF origin,
                double unitsPerPixel, unsigned quantization);
} // namespace compositor::GpuOperations
