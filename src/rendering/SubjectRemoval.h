#pragma once

#include <QImage>
#include <QString>
#include <atomic>
#include <functional>

namespace compositor {

struct SubjectRemovalSettings {
    bool advanced = false;
    double refineEdges = 0;
    double matteContrast = 0;
    double shiftEdge = 0;
};

class SubjectRemoval final {
public:
    using SegmentationProvider = std::function<QImage(const QImage &image, QString *error, std::atomic<bool> *cancelled)>;

    /// Evaluates the segmentation model on @p image.
    /// Cancellation is checked before preprocessing, before native inference (Session::Run),
    /// immediately after Session::Run(), and after normalization/scaling.
    /// Note: ONNX Runtime CPU inference executes synchronously as an atomic compute kernel;
    /// cancellation takes effect immediately after Run() completes or at stage boundaries,
    /// safely discarding results before mask creation or document application.
    [[nodiscard]] static QImage rawMask(const QImage &image, QString *error = nullptr, std::atomic<bool> *cancelled = nullptr);
    [[nodiscard]] static QImage refined(const QImage &mask, const QImage &guide,
                                        const SubjectRemovalSettings &settings);
    [[nodiscard]] static QImage connectedInstance(const QImage &mask, const QPoint &clickPoint, std::atomic<bool> *cancelled = nullptr);
    [[nodiscard]] static QImage adjustEdgeOffset(const QImage &mask, int edgeOffset);
    [[nodiscard]] static QImage smoothBinaryMask(const QImage &mask);

    static void setSegmentationProvider(SegmentationProvider provider);
    static void resetSegmentationProvider();

    [[nodiscard]] static bool isModelAvailable();
    [[nodiscard]] static QString modelFilePath();
};

} // namespace compositor
