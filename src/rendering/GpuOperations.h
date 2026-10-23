#pragma once
#include "rendering/GpuRenderer.h"
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
} // namespace compositor::GpuOperations
