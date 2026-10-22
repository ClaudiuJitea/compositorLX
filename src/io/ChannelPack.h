#pragma once
#include "core/Document.h"
#include <array>
namespace compositor {
// Empty id means a constant plane; black unless white is set.
struct ChannelSource { QUuid layer; bool white = false; };
class ChannelPack {
public:
    static QImage render(const Document &document, const std::array<ChannelSource, 4> &sources, const QSize &size);
    static QByteArray png(const QImage &straightRgba, double resolution);
};
}
