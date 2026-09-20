#pragma once

#include <QImage>
#include <QString>

namespace compositor {

class ImageImporter final {
public:
    static QImage read(const QString &path, QString *error = nullptr);
};

} // namespace compositor
