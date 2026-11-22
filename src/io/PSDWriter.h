#pragma once
#include "core/Document.h"
#include <QByteArray>
#include <QString>
#include <QVector>

namespace compositor {
// Export As › PSD (mac a0cc980 PSDWriter.swift): a document as a layered Photoshop file, from Adobe's Photoshop File
// Formats Specification, for work handed to Photoshop or After Effects. Pixel layers keep their names, places, opacity,
// visibility, blend modes and masks; folders become groups and clipping stacks stay clipped. Text, shapes and layer
// effects go out as the pixels they draw; adjustment layers are left out. A flattened copy goes in too, for apps that
// don't read layers. With a 16-bit (or deeper) layer, the whole file is 16 bits a channel.
namespace PSDWriter {
// Something that doesn't go into the file as it is in Compositor, said in the Export As dialog.
struct Note {
    enum class Change { TextAsPixels, EffectsAsPixels, AdjustmentLeftOut };
    QString layerName;
    Change change;
    bool operator==(const Note &) const = default;
};
// What `write` will change, in layer order, top first.
QVector<Note> notes(const Document &document);
// The notes as a line or two for the dialog; empty when nothing changes.
QString summary(const QVector<Note> &notes);
// Whether the file goes out at 16 bits a channel: when a layer is deeper than 8 bits.
bool sixteenBit(const Document &document);
// The file. Empty with `error` set when the document can't be written.
QByteArray write(const Document &document, QString *error = nullptr);
// Photoshop's run-length compression of one row.
QByteArray packBits(const uchar *row, int count);
// The key Photoshop stores for a blend mode (the reverse of PSDReader::blendModeFromPSD).
QString blendKey(BlendMode mode);
} // namespace PSDWriter
} // namespace compositor
