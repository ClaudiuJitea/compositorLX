#pragma once

#include <QImage>
#include <QString>

namespace compositor {

struct SubjectRemovalSettings {
    bool advanced = false;
    double refineEdges = 0;
    double matteContrast = 0;
    double shiftEdge = 0;
};

class SubjectRemoval final {
public:
    [[nodiscard]] static QImage rawMask(const QImage &image, QString *error = nullptr);
    [[nodiscard]] static QImage refined(const QImage &mask, const QImage &guide,
                                        const SubjectRemovalSettings &settings);
};

} // namespace compositor
