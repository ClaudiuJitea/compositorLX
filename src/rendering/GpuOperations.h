#pragma once
#include "rendering/GpuRenderer.h"
#include "rendering/RgbSplit.h"
#include <QTransform>
struct ProgressiveBlurSettings;
namespace compositor::GpuOperations {
QImage gaussian(const QImage &, double radius, bool byteAlgorithm);
QImage motion(const QImage &, double angle, double distance);
int appendProgressive(GpuGraph &, int input, const ProgressiveBlurSettings &, unsigned sourcePrecision);
int appendRgbSplit(GpuGraph &, int input, const RgbSplitSettings &, unsigned edges);
QImage progressive(const QImage &, const ProgressiveBlurSettings &);
QImage bloom(const QImage &, double amount, double radius, bool deep);
QImage rgbSplit(const QImage &, const RgbSplitSettings &, unsigned, GpuExecution *);
QImage lookup(const QImage &, const float *tables);
QImage cube(const QImage &, const float *table, int grid, int clipping = 0);
QImage simple(const QImage &, GpuOp, const std::array<float, 28> &parameters = {}, unsigned quantization = 1);
QImage masked(const QImage &, const QImage &, float factor = 1, bool alpha = false);
QImage composite(const QImage &, const QImage &, int mode, float opacity = 1, unsigned quantization = 0);
QImage transformed(const QImage &, QSize, const QTransform &inverse, bool smooth, float opacity = 1,
                   unsigned quantization = 0);
} // namespace compositor::GpuOperations
