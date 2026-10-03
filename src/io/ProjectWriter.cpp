#include "io/ProjectWriter.h"
#include "io/ProjectReader.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>

namespace compositor {
namespace {

static ProjectWriter::WorkerDelayHook sSaveWorkerDelayHook = nullptr;

QString uuid(const QUuid &value) { return value.toString(QUuid::WithoutBraces).toUpper(); }

[[noreturn]] void fail(const QString &detail, bool isConflict = false)
{
    throw ProjectWriteError(QStringLiteral("Could not save project: %1").arg(detail), isConflict);
}

QJsonObject transformObject(const LayerTransform &value)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("flipX"), value.flipX);
    obj.insert(QStringLiteral("flipY"), value.flipY);
    obj.insert(QStringLiteral("origin"), QJsonArray{value.origin.x(), value.origin.y()});
    obj.insert(QStringLiteral("rotation"), value.rotation);
    obj.insert(QStringLiteral("sampling"), samplingToString(value.sampling));
    obj.insert(QStringLiteral("size"), QJsonArray{value.size.width(), value.size.height()});
    return obj;
}

QJsonObject textObject(const TextStyle &text)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("content"), text.content);
    obj.insert(QStringLiteral("fontName"), text.fontName);
    obj.insert(QStringLiteral("fontSize"), text.fontSize);
    obj.insert(QStringLiteral("red"), text.red);
    obj.insert(QStringLiteral("green"), text.green);
    obj.insert(QStringLiteral("blue"), text.blue);
    obj.insert(QStringLiteral("alignment"), textAlignmentToString(text.alignment));
    obj.insert(QStringLiteral("tracking"), text.tracking);
    obj.insert(QStringLiteral("leading"), text.leading);
    if (text.boxSize) {
        obj.insert(QStringLiteral("boxSize"), QJsonArray{text.boxSize->width(), text.boxSize->height()});
    }
    if (text.colorRuns) {
        QJsonArray runs;
        for (const TextColorRun &run : *text.colorRuns) {
            runs.append(QJsonObject{{QStringLiteral("location"), run.location}, {QStringLiteral("length"), run.length},
                                    {QStringLiteral("red"), run.red}, {QStringLiteral("green"), run.green}, {QStringLiteral("blue"), run.blue}});
        }
        obj.insert(QStringLiteral("colorRuns"), runs);
    }
    if (text.fontRuns) {
        QJsonArray runs;
        for (const TextFontRun &run : *text.fontRuns) {
            runs.append(QJsonObject{{QStringLiteral("location"), run.location}, {QStringLiteral("length"), run.length},
                                    {QStringLiteral("fontName"), run.fontName}});
        }
        obj.insert(QStringLiteral("fontRuns"), runs);
    }
    return obj;
}

QJsonObject shapeObject(const LayerShapeStyle &style)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("kind"), shapeKindToString(style.kind));
    obj.insert(QStringLiteral("red"), style.red);
    obj.insert(QStringLiteral("green"), style.green);
    obj.insert(QStringLiteral("blue"), style.blue);
    obj.insert(QStringLiteral("cornerRadius"), style.cornerRadius);
    if (style.lineWidth) obj.insert(QStringLiteral("lineWidth"), *style.lineWidth);
    if (style.start) obj.insert(QStringLiteral("start"), QJsonArray{style.start->x(), style.start->y()});
    if (style.end) obj.insert(QStringLiteral("end"), QJsonArray{style.end->x(), style.end->y()});
    return obj;
}

static QJsonObject strokeObject(const StrokeEffect &stroke)
{
    QJsonObject obj;
    if (stroke.enabled.has_value()) {
        obj.insert(QStringLiteral("enabled"), *stroke.enabled);
    }
    obj.insert(QStringLiteral("size"), stroke.size);
    obj.insert(QStringLiteral("red"), stroke.red);
    obj.insert(QStringLiteral("green"), stroke.green);
    obj.insert(QStringLiteral("blue"), stroke.blue);
    obj.insert(QStringLiteral("opacity"), stroke.opacity);
    obj.insert(QStringLiteral("inside"), stroke.inside);
    return obj;
}

static QJsonObject shadowObject(const ShadowEffect &shadow)
{
    QJsonObject obj;
    if (shadow.enabled.has_value()) {
        obj.insert(QStringLiteral("enabled"), *shadow.enabled);
    }
    obj.insert(QStringLiteral("angle"), shadow.angle);
    obj.insert(QStringLiteral("distance"), shadow.distance);
    obj.insert(QStringLiteral("blur"), shadow.blur);
    obj.insert(QStringLiteral("red"), shadow.red);
    obj.insert(QStringLiteral("green"), shadow.green);
    obj.insert(QStringLiteral("blue"), shadow.blue);
    obj.insert(QStringLiteral("opacity"), shadow.opacity);
    return obj;
}

static QJsonObject colorOverlayObject(const ColorOverlayEffect &overlay)
{
    QJsonObject obj;
    if (overlay.enabled.has_value()) {
        obj.insert(QStringLiteral("enabled"), *overlay.enabled);
    }
    obj.insert(QStringLiteral("red"), overlay.red);
    obj.insert(QStringLiteral("green"), overlay.green);
    obj.insert(QStringLiteral("blue"), overlay.blue);
    obj.insert(QStringLiteral("opacity"), overlay.opacity);
    return obj;
}

static QJsonObject innerShadowObject(const InnerShadowEffect &shadow)
{
    QJsonObject obj;
    if (shadow.enabled.has_value()) {
        obj.insert(QStringLiteral("enabled"), *shadow.enabled);
    }
    obj.insert(QStringLiteral("angle"), shadow.angle);
    obj.insert(QStringLiteral("distance"), shadow.distance);
    obj.insert(QStringLiteral("blur"), shadow.blur);
    obj.insert(QStringLiteral("red"), shadow.red);
    obj.insert(QStringLiteral("green"), shadow.green);
    obj.insert(QStringLiteral("blue"), shadow.blue);
    obj.insert(QStringLiteral("opacity"), shadow.opacity);
    return obj;
}

static QJsonObject outerGlowObject(const OuterGlowEffect &glow)
{
    QJsonObject obj;
    if (glow.enabled.has_value()) {
        obj.insert(QStringLiteral("enabled"), *glow.enabled);
    }
    obj.insert(QStringLiteral("size"), glow.size);
    obj.insert(QStringLiteral("red"), glow.red);
    obj.insert(QStringLiteral("green"), glow.green);
    obj.insert(QStringLiteral("blue"), glow.blue);
    obj.insert(QStringLiteral("opacity"), glow.opacity);
    return obj;
}

static QJsonObject innerGlowObject(const InnerGlowEffect &glow)
{
    QJsonObject obj;
    if (glow.enabled.has_value()) {
        obj.insert(QStringLiteral("enabled"), *glow.enabled);
    }
    obj.insert(QStringLiteral("size"), glow.size);
    obj.insert(QStringLiteral("red"), glow.red);
    obj.insert(QStringLiteral("green"), glow.green);
    obj.insert(QStringLiteral("blue"), glow.blue);
    obj.insert(QStringLiteral("opacity"), glow.opacity);
    return obj;
}

static QJsonObject effectsObject(const LayerEffects &effects)
{
    QJsonObject obj;
    if (effects.stroke) obj.insert(QStringLiteral("stroke"), strokeObject(*effects.stroke));
    if (effects.shadow) obj.insert(QStringLiteral("shadow"), shadowObject(*effects.shadow));
    if (effects.colorOverlay) obj.insert(QStringLiteral("colorOverlay"), colorOverlayObject(*effects.colorOverlay));
    if (effects.innerShadow) obj.insert(QStringLiteral("innerShadow"), innerShadowObject(*effects.innerShadow));
    if (effects.outerGlow) obj.insert(QStringLiteral("outerGlow"), outerGlowObject(*effects.outerGlow));
    if (effects.innerGlow) obj.insert(QStringLiteral("innerGlow"), innerGlowObject(*effects.innerGlow));
    return obj;
}

void validateHierarchy(const QVector<Layer> &layers)
{
    QSet<QUuid> ids;
    QSet<QUuid> groups;
    for (const Layer &layer : layers) {
        if (ids.contains(layer.id)) fail(QStringLiteral("duplicate layer identifier"));
        ids.insert(layer.id);
        if (layer.group) {
            groups.insert(layer.id);
            if (!layer.image.isNull() || !layer.imageFile.isEmpty()) {
                fail(QStringLiteral("a group cannot contain image pixels"));
            }
        }
    }
    for (const Layer &layer : layers) {
        if (layer.parentId && !groups.contains(*layer.parentId)) {
            fail(QStringLiteral("layer parent is missing or is not a group"));
        }
        QSet<QUuid> visited{layer.id};
        std::optional<QUuid> parent = layer.parentId;
        int depth = 0;
        while (parent) {
            if (visited.contains(*parent) || ++depth > 64) {
                fail(QStringLiteral("cyclic or overly deep layer hierarchy"));
            }
            visited.insert(*parent);
            const auto it = std::find_if(layers.cbegin(), layers.cend(), [&](const Layer &candidate) {
                return candidate.id == *parent;
            });
            if (it == layers.cend() || !it->group) fail(QStringLiteral("missing parent group layer"));
            parent = it->parentId;
        }
        if (layer.group && visited.size() > 64) {
            fail(QStringLiteral("overly deep group nesting"));
        }
    }
}

void validateLiveMaskGraph(const QVector<Layer> &layers)
{
    QHash<QUuid, const Layer *> byId;
    for (const Layer &layer : layers) {
        byId.insert(layer.id, &layer);
    }
    for (const Layer &layer : layers) {
        QSet<QUuid> visited;
        std::optional<QUuid> current = layer.id;
        while (current) {
            if (visited.size() >= 256 || visited.contains(*current)) {
                fail(QStringLiteral("cyclic or overly deep clipping mask chain"));
            }
            visited.insert(*current);
            const Layer *record = byId.value(*current, nullptr);
            if (!record) fail(QStringLiteral("referenced clipping mask layer missing"));
            if (record->maskSourceId) {
                if (record->group) fail(QStringLiteral("a group cannot have a clipping mask source"));
                if (*record->maskSourceId == record->id) fail(QStringLiteral("self-referencing clipping mask"));
                const Layer *source = byId.value(*record->maskSourceId, nullptr);
                if (!source) fail(QStringLiteral("clipping mask source layer does not exist"));
                if (source->group) fail(QStringLiteral("clipping mask source cannot be a group"));
                if (!source->adjustment.isEmpty()) fail(QStringLiteral("clipping mask source cannot be an adjustment layer"));
            }
            current = record->maskSourceId;
        }
    }
}

void validateGuides(const QVector<CanvasGuide> &guides, int version)
{
    if (version < 8) {
        if (!guides.isEmpty()) fail(QStringLiteral("guides require format version 8 or later"));
        return;
    }
    if (guides.size() > 1000) {
        fail(QStringLiteral("guide count exceeds 1,000 limit"));
    }
    QSet<QUuid> ids;
    for (const CanvasGuide &guide : guides) {
        if (!guide.isValid()) fail(QStringLiteral("invalid guide coordinates or identifier"));
        if (ids.contains(guide.id)) fail(QStringLiteral("duplicate guide identifier"));
        ids.insert(guide.id);
    }
}

void validateAdjustmentSettings(const QJsonObject &adj, int version)
{
    const QString kindStr = adj.value(QStringLiteral("kind")).toString();
    const auto kind = adjustmentKindFromString(kindStr);
    if (!kind) fail(QStringLiteral("unknown adjustment kind: %1").arg(kindStr));

    if (isVersion9Adjustment(*kind) && version < 9) {
        fail(QStringLiteral("%1 adjustment requires format version 9; version %2 cannot contain blurs or noise.").arg(kindStr).arg(version));
    }

    const double hue = adj.value(QStringLiteral("hue")).toDouble(0.0);
    const double saturation = adj.value(QStringLiteral("saturation")).toDouble(0.0);
    const double lightness = adj.value(QStringLiteral("lightness")).toDouble(0.0);
    if (std::abs(hue) > 360.0 || std::abs(saturation) > 100.0 || std::abs(lightness) > 100.0) {
        fail(QStringLiteral("adjustment HSV parameters out of range"));
    }

    if (*kind == AdjustmentKind::GaussianBlur) {
        const double radius = adj.value(QStringLiteral("blurRadius")).toDouble(10.0);
        if (radius < 0.1 || radius > 250.0) fail(QStringLiteral("blurRadius out of range (0.1..250)"));
    } else if (*kind == AdjustmentKind::MotionBlur) {
        const double angle = adj.value(QStringLiteral("motionAngle")).toDouble(0.0);
        const double dist = adj.value(QStringLiteral("motionDistance")).toDouble(10.0);
        if (angle < -90.0 || angle > 90.0 || dist < 1.0 || dist > 2000.0) {
            fail(QStringLiteral("motion blur parameters out of range"));
        }
    } else if (*kind == AdjustmentKind::AddNoise) {
        const double amount = adj.value(QStringLiteral("noiseAmount")).toDouble(10.0);
        if (amount < 0.1 || amount > 400.0) fail(QStringLiteral("noiseAmount out of range (0.1..400)"));
    } else if (*kind == AdjustmentKind::BlackWhite) {
        if (adj.contains(QStringLiteral("blackWhiteSettings"))) {
            const QJsonObject bw = adj.value(QStringLiteral("blackWhiteSettings")).toObject();
            for (const QString &colorKey : {QStringLiteral("reds"), QStringLiteral("yellows"), QStringLiteral("greens"),
                                            QStringLiteral("cyans"), QStringLiteral("blues"), QStringLiteral("magentas")}) {
                const double val = bw.value(colorKey).toDouble(0.0);
                if (!std::isfinite(val) || val < -200.0 || val > 300.0) {
                    fail(QStringLiteral("Black & White parameter %1 out of range (-200..300)").arg(colorKey));
                }
            }
            if (bw.contains(QStringLiteral("tintHue"))) {
                const double hue = bw.value(QStringLiteral("tintHue")).toDouble(0.0);
                if (!std::isfinite(hue) || hue < 0.0 || hue > 360.0) fail(QStringLiteral("Black & White tintHue out of range (0..360)"));
            }
            if (bw.contains(QStringLiteral("tintSaturation"))) {
                const double sat = bw.value(QStringLiteral("tintSaturation")).toDouble(0.0);
                if (!std::isfinite(sat) || sat < 0.0 || sat > 100.0) fail(QStringLiteral("Black & White tintSaturation out of range (0..100)"));
            }
        }
    } else if (*kind == AdjustmentKind::ColorBalance) {
        if (adj.contains(QStringLiteral("colorBalanceSettings"))) {
            const QJsonObject cb = adj.value(QStringLiteral("colorBalanceSettings")).toObject();
            for (const QString &cbKey : {QStringLiteral("shadowCyanRed"), QStringLiteral("shadowMagentaGreen"), QStringLiteral("shadowYellowBlue"),
                                         QStringLiteral("midCyanRed"), QStringLiteral("midMagentaGreen"), QStringLiteral("midYellowBlue"),
                                         QStringLiteral("highlightCyanRed"), QStringLiteral("highlightMagentaGreen"), QStringLiteral("highlightYellowBlue")}) {
                const double val = cb.value(cbKey).toDouble(0.0);
                if (!std::isfinite(val) || val < -100.0 || val > 100.0) {
                    fail(QStringLiteral("Color Balance parameter %1 out of range (-100..100)").arg(cbKey));
                }
            }
        }
    }
}

} // namespace

ProjectWriteError::ProjectWriteError(const QString &message, bool isConflict)
    : std::runtime_error(message.toStdString()), message_(message), isConflict_(isConflict)
{
}

int ProjectWriter::computeTargetVersion(const Document &document)
{
    int minRequired = 1;
    for (const Layer &layer : document.layers) {
        if (layer.group || layer.parentId.has_value()) minRequired = std::max(minRequired, 2);
        if (layer.opacity != 1.0 || layer.blendMode != BlendMode::Normal) minRequired = std::max(minRequired, 3);
        if (!layer.mask.isNull() || !layer.maskFile.isEmpty()) {
            minRequired = std::max(minRequired, layer.group ? 6 : 4);
        }
        if (layer.maskSourceId.has_value()) minRequired = std::max(minRequired, 5);
        if (!layer.adjustment.isEmpty()) {
            minRequired = std::max(minRequired, 7);
            const auto kind = adjustmentKindFromString(layer.adjustment.value(QStringLiteral("kind")).toString());
            if (kind && isVersion9Adjustment(*kind)) {
                minRequired = std::max(minRequired, 9);
            }
        }
        if (layer.group && layer.opacity < 0.999999) minRequired = std::max(minRequired, 8);
        if (layer.text) minRequired = std::max(minRequired, layer.text->requiredFormatVersion());
    }
    if (!document.guides.isEmpty()) minRequired = std::max(minRequired, 8);

    return (document.formatVersion >= 1 && document.formatVersion <= 11)
               ? document.formatVersion
               : std::max(minRequired, 9);
}

void ProjectWriter::setWorkerDelayHook(WorkerDelayHook hook)
{
    sSaveWorkerDelayHook = std::move(hook);
}

SaveResult ProjectWriter::saveAtomicChecked(const Document &document,
                                           const QString &projectDirectory,
                                           const ExpectedDestinationState &expectedState,
                                           SaveFaultInjection fault)
{
    try {
        if (document.id.isNull() || document.canvasSize.width() < 1 || document.canvasSize.width() > 30000
            || document.canvasSize.height() < 1 || document.canvasSize.height() > 30000
            || !std::isfinite(document.resolution) || document.resolution < 1.0 || document.resolution > 9600.0
            || document.layers.size() > 10000) {
            fail(QStringLiteral("the document is outside supported limits"));
        }

    validateHierarchy(document.layers);
    validateLiveMaskGraph(document.layers);

    if (document.activeLayerId) {
        const bool found = std::any_of(document.layers.cbegin(), document.layers.cend(), [&](const Layer &layer) {
            return layer.id == *document.activeLayerId;
        });
        if (!found) fail(QStringLiteral("active layer is missing"));
    }
    const int targetVersion = computeTargetVersion(document);

    validateGuides(document.guides, targetVersion);

    qint64 totalImagePixels = 0;
    qint64 totalMaskPixels = 0;

    for (const Layer &layer : document.layers) {
        if (layer.id.isNull() || layer.name.trimmed().isEmpty() || layer.name.toUtf8().size() > 16384) {
            fail(QStringLiteral("invalid layer name or identifier"));
        }
        if (!layer.transform.isValid()) {
            fail(QStringLiteral("invalid layer transform"));
        }
        if (layer.opacity < 0.0 || layer.opacity > 1.0 || !std::isfinite(layer.opacity)) {
            fail(QStringLiteral("invalid layer opacity"));
        }
        if (layer.effects.has_value() && !layer.effects->isEmpty()) {
            if (layer.group) {
                fail(QStringLiteral("Folder layers cannot have layer effects."));
            }
            if (!layer.adjustment.isEmpty()) {
                fail(QStringLiteral("Adjustment layers cannot have layer effects."));
            }
            if (!layer.effects->isValid()) {
                fail(QStringLiteral("Invalid layer effects parameters."));
            }
        }
        if (layer.group) {
            if (layer.blendMode != BlendMode::Normal) {
                fail(QStringLiteral("Folder blend mode must be Normal."));
            }
            if (targetVersion < 8 && layer.opacity < 0.999999) {
                fail(QStringLiteral("Folder opacity below 1.0 requires format version 8; version 7 cannot contain dimmed folders."));
            }
            if (!layer.image.isNull() || !layer.imageFile.isEmpty()) {
                fail(QStringLiteral("a group cannot contain image pixels"));
            }
            if (!layer.adjustment.isEmpty()) {
                fail(QStringLiteral("groups cannot be adjustments"));
            }
            if (layer.maskSourceId.has_value()) {
                fail(QStringLiteral("groups cannot have clipping mask sources"));
            }
        }
        if (!layer.adjustment.isEmpty()) {
            if (targetVersion < 7) {
                fail(QStringLiteral("Adjustment layers require format version 7 or later."));
            }
            if (!layer.image.isNull() || !layer.imageFile.isEmpty()) {
                fail(QStringLiteral("adjustment layer cannot carry an image"));
            }
            validateAdjustmentSettings(layer.adjustment, targetVersion);
        }
        if (layer.text.has_value()) {
            if (targetVersion < layer.text->requiredFormatVersion()) {
                fail(QStringLiteral("text colorRuns/fontRuns require format version 10/11; this document is version %1.").arg(targetVersion));
            }
            if (!layer.text->isValid()) {
                fail(QStringLiteral("invalid text style"));
            }
            if (layer.group) {
                fail(QStringLiteral("groups cannot carry text"));
            }
            if (!layer.adjustment.isEmpty()) {
                fail(QStringLiteral("adjustments cannot carry text"));
            }
            if (layer.image.isNull() && layer.imageFile.isEmpty()) {
                fail(QStringLiteral("text layers require an image"));
            }
        }
        if (layer.shapeStyle.has_value() && !layer.shapeStyle->isValid()) {
            fail(QStringLiteral("invalid shape style"));
        }
        if (!layer.image.isNull() && !layer.group) {
            if (layer.image.width() < 1 || layer.image.height() < 1 || layer.image.width() > 30000 || layer.image.height() > 30000) {
                fail(QStringLiteral("layer image has invalid dimensions"));
            }
            totalImagePixels += qint64(layer.image.width()) * layer.image.height();
        }
        if (!layer.mask.isNull()) {
            if (targetVersion < 4) {
                fail(QStringLiteral("Layer masks require format version 4 or later."));
            }
            if (layer.group && targetVersion < 6) {
                fail(QStringLiteral("Folder masks require format version 6 or later."));
            }
            if (layer.mask.width() < 1 || layer.mask.height() < 1 || layer.mask.width() > 30000 || layer.mask.height() > 30000) {
                fail(QStringLiteral("layer mask has invalid dimensions"));
            }
            totalMaskPixels += qint64(layer.mask.width()) * layer.mask.height();
        }
        if (layer.maskPlacement.has_value()) {
            if (layer.mask.isNull() && layer.maskFile.isEmpty()) {
                fail(QStringLiteral("mask placement without a mask"));
            }
            if (!layer.maskPlacement->isValid()) {
                fail(QStringLiteral("invalid mask placement transform"));
            }
        }
        if (layer.maskSourceId.has_value()) {
            if (targetVersion < 5) {
                fail(QStringLiteral("Clipping masks require format version 5 or later."));
            }
        }
        if (targetVersion < 3 && (layer.opacity != 1.0 || layer.blendMode != BlendMode::Normal)) {
            fail(QStringLiteral("Layer opacity and blend mode require format version 3 or later."));
        }
        if (targetVersion < 2 && (layer.group || layer.parentId.has_value())) {
            fail(QStringLiteral("Groups and layer hierarchy require format version 2 or later."));
        }
    }

    if (totalImagePixels > 100000000LL) fail(QStringLiteral("the image pixel limit is exceeded"));
    if (totalMaskPixels > 100000000LL) fail(QStringLiteral("the mask pixel limit is exceeded"));

    const QFileInfo destinationInfo(projectDirectory);
    QDir parent = destinationInfo.absoluteDir();
    if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
        fail(QStringLiteral("the destination folder cannot be created"));
    }
    const QString stagingTemplate = parent.filePath(QStringLiteral(".staging-%1-XXXXXX").arg(destinationInfo.fileName()));
    QTemporaryDir staging(stagingTemplate);
    if (!staging.isValid()) fail(QStringLiteral("a temporary package cannot be created"));
    auto activeLock = std::make_unique<QLockFile>(QDir(staging.path()).filePath(QStringLiteral(".active.lock")));
    activeLock->lock();
    QDir root(staging.path());
    if (!root.mkdir(QStringLiteral("images"))) fail(QStringLiteral("the image folder cannot be created"));

    QJsonArray layers;

    for (const Layer &layer : document.layers) {
        if (layer.id.isNull() || layer.name.trimmed().isEmpty() || !layer.transform.isValid()) {
            fail(QStringLiteral("a layer is invalid"));
        }

        QJsonObject value;
        value.insert(QStringLiteral("id"), uuid(layer.id));
        value.insert(QStringLiteral("name"), layer.name);
        value.insert(QStringLiteral("isVisible"), layer.visible);
        value.insert(QStringLiteral("transform"), transformObject(layer.transform));

        if (targetVersion >= 2) {
            value.insert(QStringLiteral("isGroup"), layer.group);
            if (layer.parentId) value.insert(QStringLiteral("parentID"), uuid(*layer.parentId));
        }

        if (targetVersion >= 3) {
            value.insert(QStringLiteral("opacity"), layer.opacity);
            value.insert(QStringLiteral("blendMode"), blendModeToString(layer.blendMode));
        }

        if (targetVersion >= 5 && layer.maskSourceId) {
            value.insert(QStringLiteral("maskSourceID"), uuid(*layer.maskSourceId));
        }

        if (targetVersion >= 7 && !layer.adjustment.isEmpty()) {
            value.insert(QStringLiteral("adjustment"), layer.adjustment);
        }

        if (layer.text.has_value()) {
            value.insert(QStringLiteral("text"), textObject(*layer.text));
        }

        if (layer.shapeStyle.has_value()) {
            value.insert(QStringLiteral("shape"), shapeObject(*layer.shapeStyle));
        } else if (!layer.shape.isEmpty() && layer.shape.value(QStringLiteral("kind")).toString() != QStringLiteral("Text")) {
            value.insert(QStringLiteral("shape"), layer.shape);
        }

        if (layer.effects.has_value() && !layer.effects->isEmpty()) {
            value.insert(QStringLiteral("effects"), effectsObject(*layer.effects));
        }

        // Save image if present
        if (!layer.image.isNull() && !layer.group) {
            totalImagePixels += qint64(layer.image.width()) * layer.image.height();
            if (totalImagePixels > 100000000LL) fail(QStringLiteral("the image pixel limit is exceeded"));
            const QString fileName = uuid(layer.id) + QStringLiteral(".png");
            if (!layer.image.save(root.filePath(QStringLiteral("images/") + fileName), "PNG")) {
                fail(QStringLiteral("a layer image could not be encoded"));
            }
            value.insert(QStringLiteral("imageFile"), fileName);
        }

        // Save mask if present
        const bool canHaveMask = !layer.mask.isNull() && (targetVersion >= 6 || (targetVersion >= 4 && !layer.group));
        if (canHaveMask) {
            totalMaskPixels += qint64(layer.mask.width()) * layer.mask.height();
            if (totalMaskPixels > 100000000LL) fail(QStringLiteral("the mask pixel limit is exceeded"));
            const QString maskFileName = uuid(layer.id) + QStringLiteral(".mask.png");
            if (!layer.mask.save(root.filePath(QStringLiteral("images/") + maskFileName), "PNG")) {
                fail(QStringLiteral("a layer mask could not be encoded"));
            }
            value.insert(QStringLiteral("maskFile"), maskFileName);
            value.insert(QStringLiteral("maskEnabled"), layer.maskEnabled);
            value.insert(QStringLiteral("maskLinked"), layer.maskLinked);
            if (layer.maskPlacement) {
                value.insert(QStringLiteral("maskPlacement"), transformObject(*layer.maskPlacement));
            }
        }

        layers.append(value);
    }

    QJsonObject manifest;
    manifest.insert(QStringLiteral("format"), QStringLiteral("com.compositor.project"));
    manifest.insert(QStringLiteral("version"), targetVersion);
    manifest.insert(QStringLiteral("colorSpace"), QStringLiteral("sRGB"));
    manifest.insert(QStringLiteral("resolution"), document.resolution);
    manifest.insert(QStringLiteral("documentID"), uuid(document.id));
    manifest.insert(QStringLiteral("width"), document.canvasSize.width());
    manifest.insert(QStringLiteral("height"), document.canvasSize.height());
    manifest.insert(QStringLiteral("activeLayerID"), document.activeLayerId ? QJsonValue(uuid(*document.activeLayerId)) : QJsonValue(QJsonValue::Null));
    if (targetVersion >= 8 && !document.guides.isEmpty()) {
        QJsonArray guidesArray;
        for (const CanvasGuide &guide : document.guides) {
            QJsonObject g;
            g.insert(QStringLiteral("id"), uuid(guide.id));
            g.insert(QStringLiteral("axis"), guideAxisToString(guide.axis));
            g.insert(QStringLiteral("position"), guide.position);
            guidesArray.append(g);
        }
        manifest.insert(QStringLiteral("guides"), guidesArray);
    }
    manifest.insert(QStringLiteral("layers"), layers);

    const QByteArray metadata = QJsonDocument(manifest).toJson(QJsonDocument::Indented);
    if (metadata.size() > 4 * 1024 * 1024) fail(QStringLiteral("the manifest is too large"));

    QSaveFile manifestFile(root.filePath(QStringLiteral("manifest.json")));
    if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(metadata) != metadata.size() || !manifestFile.commit()) {
        fail(QStringLiteral("the manifest could not be written"));
    }

    const QString destination = destinationInfo.absoluteFilePath();
    if (sSaveWorkerDelayHook) {
        sSaveWorkerDelayHook(QStringLiteral("after_staging_before_install"), destination);
    }
    const bool existed = QFileInfo::exists(destination);

    if (expectedState.mode == ExpectedDestinationState::Mode::MustBeAbsent) {
        if (existed) {
            fail(QStringLiteral("conflict detected: destination was created externally during save"), true);
        }
    } else if (expectedState.mode == ExpectedDestinationState::Mode::MustMatchDigest) {
        if (!existed) {
            fail(QStringLiteral("conflict detected: destination was removed externally during save"), true);
        }
        const auto curDigest = ProjectDigest::compute(destination);
        if (!curDigest || !curDigest->isValid()) {
            fail(QStringLiteral("destination exists but cannot be fingerprinted (unreadable or malformed)"), false);
        }
        if (*curDigest != expectedState.digest) {
            fail(QStringLiteral("conflict detected: destination was modified externally during save"), true);
        }
    }

    const QString backup = destination + QStringLiteral(".previous-") + uuid(QUuid::createUuid());
    if (existed && (!destinationInfo.isDir() || destinationInfo.isSymLink() || !parent.rename(destination, backup))) {
        fail(QStringLiteral("the existing package cannot be replaced"));
    }

    if (fault == SaveFaultInjection::CrashAfterBackupBeforeInstall) {
        staging.setAutoRemove(false);
        fail(QStringLiteral("simulated crash after backup before install"));
    }

    if (fault == SaveFaultInjection::FailAfterBackupBeforeInstall) {
        if (activeLock) {
            activeLock->unlock();
            activeLock.reset();
        }
        staging.setAutoRemove(false);
        QDir(staging.path()).removeRecursively();
        if (existed) {
            const bool restored = parent.rename(backup, destination);
            if (!restored) {
                fail(QStringLiteral("failed after backup and failed to restore backup; original package preserved at: ") + backup);
            }
        }
        fail(QStringLiteral("simulated failure after backup before install; previous package was restored"));
    }

    if (activeLock) {
        activeLock->unlock();
        activeLock.reset();
    }

    staging.setAutoRemove(false);
    const bool installOk = (fault != SaveFaultInjection::FailInstallStaged && fault != SaveFaultInjection::FailRollbackRestore)
                           && parent.rename(staging.path(), destination);

    if (!installOk) {
        if (existed) {
            const bool restored = (fault != SaveFaultInjection::FailRollbackRestore)
                                  && parent.rename(backup, destination);
            if (!restored) {
                fail(QStringLiteral("the completed package cannot be installed and rollback failed; original package preserved at: ") + backup);
            }
        }
        QDir(staging.path()).removeRecursively();
        fail(QStringLiteral("the completed package cannot be installed; previous package was restored"));
    }

    if (existed) QDir(backup).removeRecursively();

    const auto installedDigest = ProjectDigest::compute(destination);
    return SaveResult{SaveResult::Status::Success, QString(), installedDigest};
    } catch (const ProjectWriteError &err) {
        return SaveResult{err.isConflict() ? SaveResult::Status::Conflict : SaveResult::Status::IoError,
                          err.message(), std::nullopt};
    } catch (const std::exception &err) {
        return SaveResult{SaveResult::Status::IoError, QString::fromUtf8(err.what()), std::nullopt};
    }
}

SaveResult ProjectWriter::saveAtomicChecked(const Document &document,
                                           const QString &projectDirectory,
                                           const std::optional<ProjectDigest> &expectedDigest,
                                           bool checkConflict,
                                           SaveFaultInjection fault)
{
    ExpectedDestinationState expectedState = ExpectedDestinationState::any();
    if (checkConflict) {
        if (expectedDigest.has_value() && expectedDigest->isValid()) {
            expectedState = ExpectedDestinationState::mustMatch(*expectedDigest);
        } else if (QFileInfo::exists(projectDirectory)) {
            const auto d = ProjectDigest::compute(projectDirectory);
            if (d && d->isValid()) {
                expectedState = ExpectedDestinationState::mustMatch(*d);
            } else {
                return SaveResult{SaveResult::Status::IoError,
                                  QStringLiteral("destination exists but cannot be fingerprinted (unreadable or malformed)"),
                                  std::nullopt};
            }
        } else {
            expectedState = ExpectedDestinationState::mustBeAbsent();
        }
    }
    return saveAtomicChecked(document, projectDirectory, expectedState, fault);
}

void ProjectWriter::save(const Document &document, const QString &projectDirectory, SaveFaultInjection fault)
{
    const auto result = saveAtomicChecked(document, projectDirectory, std::nullopt, false, fault);
    if (result.status != SaveResult::Status::Success) {
        throw ProjectWriteError(result.errorMessage, result.status == SaveResult::Status::Conflict);
    }
}

bool ProjectWriter::recoverInterruptedPackage(const QString &projectDirectory)
{
    try {
        if (QFileInfo::exists(projectDirectory)) {
            (void)ProjectReader::load(projectDirectory);
            return true;
        }
    } catch (...) {
        // Destination exists but is invalid or incomplete (e.g. from an interrupted install)
    }

    const QFileInfo destinationInfo(projectDirectory);
    QDir parent = destinationInfo.absoluteDir();
    const QString baseName = destinationInfo.fileName();
    const QString prefix = baseName + QStringLiteral(".previous-");
    const QStringList backups = parent.entryList({prefix + QStringLiteral("*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
    for (const QString &entry : backups) {
        const QString candidate = parent.filePath(entry);
        try {
            // Validate candidate backup by fully loading it, not merely by finding manifest.json
            (void)ProjectReader::load(candidate);
        } catch (...) {
            // Corrupt backup package: reject and skip to next candidate
            continue;
        }

        // If invalid destination directory exists on disk, do not delete it before successful restoration!
        // Move it aside temporarily to a staging area so we can attempt restoration without destroying user data.
        QString invalidBackup;
        bool movedInvalid = false;
        if (destinationInfo.exists()) {
            invalidBackup = parent.filePath(QStringLiteral(".corrupt-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
            movedInvalid = parent.rename(destinationInfo.absoluteFilePath(), invalidBackup);
            if (!movedInvalid) {
                // Cannot safely move invalid destination aside; skip candidate
                continue;
            }
        }

        const bool restored = parent.rename(candidate, destinationInfo.absoluteFilePath());
        if (restored) {
            // Restoration succeeded! Now we can safely remove the temporary invalid backup.
            if (movedInvalid) {
                QDir(invalidBackup).removeRecursively();
            }

            // Clean up stale staging directories belonging strictly to THIS project (leaving active staging directories untouched)
            const QString stagingPrefix = QStringLiteral(".staging-") + baseName + QStringLiteral("-");
            const QStringList stagings = parent.entryList({stagingPrefix + QStringLiteral("*")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
            for (const QString &s : stagings) {
                const QString stagingPath = parent.filePath(s);
                const QString lockFile = stagingPath + QStringLiteral("/.active.lock");
                if (QFile::exists(lockFile)) {
                    QLockFile checkLock(lockFile);
                    qint64 pid = 0;
                    QString host, app;
                    if (checkLock.getLockInfo(&pid, &host, &app) && pid > 0) {
#if defined(Q_OS_UNIX)
                        if (::kill(pid, 0) == 0 || errno == EPERM) {
                            // An active save process currently holds this lock; do not delete!
                            continue;
                        }
#else
                        continue;
#endif
                    }
                }
                QDir(stagingPath).removeRecursively();
            }
            return true;
        } else {
            // Restoration failed: restore the invalid destination back into place!
            if (movedInvalid) {
                parent.rename(invalidBackup, destinationInfo.absoluteFilePath());
            }
        }
    }
    return false;
}

} // namespace compositor
