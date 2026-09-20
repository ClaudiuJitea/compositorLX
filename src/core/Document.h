#pragma once

#include <QImage>
#include <QJsonObject>
#include <QPainter>
#include <QPointF>
#include <QSize>
#include <QSizeF>
#include <QString>
#include <QUuid>
#include <QVector>

#include <optional>

namespace compositor {

enum class Sampling { Nearest, Smooth, HighQuality };

enum class BlendMode {
    Normal,
    Multiply,
    Screen,
    Overlay,
    Darken,
    Lighten,
    Difference,
    ColorDodge,
    ColorBurn,
    Hue,
    Saturation,
    Color,
    Luminosity
};

struct LayerTransform {
    QPointF origin;
    QSizeF size;
    double rotation = 0.0;
    bool flipX = false;
    bool flipY = false;
    Sampling sampling = Sampling::HighQuality;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QPointF center() const;
    [[nodiscard]] LayerTransform following(const LayerTransform &oldPlacement, const LayerTransform &newPlacement) const;
    [[nodiscard]] bool samePlacement(const LayerTransform &other) const;
    bool operator==(const LayerTransform &) const = default;
};

struct Layer {
    QUuid id;
    QString name;
    bool visible = true;
    LayerTransform transform;
    QString imageFile;
    QImage image;
    std::optional<QUuid> parentId;
    bool group = false;
    double opacity = 1.0;
    BlendMode blendMode = BlendMode::Normal;
    QString maskFile;
    QImage mask;
    bool maskEnabled = true;
    std::optional<LayerTransform> maskPlacement;
    bool maskLinked = true;
    std::optional<QUuid> maskSourceId;
    QJsonObject adjustment;
    QJsonObject shape;

    bool operator==(const Layer &) const = default;
};

struct Document {
    int formatVersion = 0;
    QUuid id;
    QSize canvasSize;
    double resolution = 72.0;
    std::optional<QUuid> activeLayerId;
    QVector<Layer> layers; // Bottom to top, matching the project manifest.
    /// Canvas-sized grayscale coverage. Null means no selection; an all-black image is an explicit empty selection.
    std::optional<QImage> selection;
    QString projectPath;

    bool operator==(const Document &) const = default;
};

[[nodiscard]] Sampling samplingFromString(const QString &value);
[[nodiscard]] BlendMode blendModeFromString(const QString &value);
[[nodiscard]] QPainter::CompositionMode compositionMode(BlendMode mode);

} // namespace compositor
