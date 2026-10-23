#pragma once
#include "core/Document.h"
#include <QImage>
namespace compositor {
QImage tryGpuDocument(const Document &document);
}
