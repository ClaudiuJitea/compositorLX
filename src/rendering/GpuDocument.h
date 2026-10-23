#pragma once
#include "core/Document.h"
#include <QImage>
namespace compositor {
// Renders the document as one GPU graph (or canvas tiles of one) when every layer it
// needs is supported; a null image means "use the CPU renderer". `base`, when given, is
// the composite of the first `firstTopLevel` top-level layers, which are then skipped.
QImage tryGpuDocument(const Document &document, const QImage &base = {}, int firstTopLevel = 0);
// Upper bound of the GPU working memory one graph over `region` would need, computed
// from the document alone. Exposed for tests.
quint64 estimateGpuDocumentBytes(const Document &document, QSize region);
} // namespace compositor
