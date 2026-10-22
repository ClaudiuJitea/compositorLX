#include "io/PdfImporter.h"
#include <poppler-qt6.h>
#include <cmath>
namespace compositor {
QVector<QImage> PdfImporter::read(const QString &path, const QSize &fit, QString *error, qint64 budget)
{
    const auto fail = [&](const QString &message) { if (error) *error = message; return QVector<QImage>{}; };
    auto pdf = Poppler::Document::load(path);
    if (!pdf || pdf->isLocked()) return fail(QStringLiteral("The PDF is unreadable or requires a password."));
    if (pdf->numPages() < 1 || pdf->numPages() > 10000) return fail(QStringLiteral("The PDF has too many pages."));
    pdf->setRenderHint(Poppler::Document::Antialiasing);
    pdf->setRenderHint(Poppler::Document::TextAntialiasing);
    pdf->setPaperColor(Qt::transparent);
    QVector<double> resolutions;
    // Check the entire import before allocating any pages. A failed import never leaves half a PDF in the document.
    for (int i = 0; i < pdf->numPages(); ++i) {
        auto page = pdf->page(i);
        if (!page) return fail(QStringLiteral("A PDF page could not be read."));
        const QSizeF points = page->pageSizeF();
        if (points.isEmpty()) return fail(QStringLiteral("Invalid PDF page size."));
        const double dpi = fit.isEmpty() ? 300 : 72 * std::min(fit.width() / points.width(), fit.height() / points.height());
        const double w = std::ceil(points.width() * dpi / 72), h = std::ceil(points.height() * dpi / 72);
        if (!std::isfinite(w) || !std::isfinite(h) || w < 1 || h < 1 || w > DocumentLimits::maxSide || h > DocumentLimits::maxSide || w * h > DocumentLimits::maxSurfacePixels || w * h > budget)
            return fail(QStringLiteral("The PDF exceeds the document's pixel budget."));
        budget -= qint64(w * h);
        resolutions.append(dpi);
    }
    QVector<QImage> pages;
    for (int i = 0; i < pdf->numPages(); ++i) {
        QImage image = pdf->page(i)->renderToImage(resolutions[i], resolutions[i]);
        if (image.isNull()) return fail(QStringLiteral("A PDF page could not be rendered."));
        pages.append(image.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    }
    return pages;
}
}
