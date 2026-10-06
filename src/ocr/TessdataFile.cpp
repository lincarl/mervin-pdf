#include "ocr/TessdataFile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cctype>
#include <vector>

namespace mervin {

namespace {

// Translation context for this file's messages. TessdataFile is a namespace, so
// it cannot declare one itself.
struct Text
{
    Q_DECLARE_TR_FUNCTIONS(mervin::TessdataFile)
};

// Tesseract's kMaxNumTessdataEntries. A count above it means the container was
// written on the other endianness - which is exactly how Tesseract itself
// decides to byte-swap, so we follow it rather than rejecting such a file.
constexpr quint32 kMaxEntries = 1000;

// Component slots holding a text unicharset: TESSDATA_UNICHARSET (1) and
// TESSDATA_LSTM_UNICHARSET (21). tessdata_fast models carry only the LSTM one;
// legacy models carry both.
constexpr int kUnicharsetSlots[] = {1, 21};

// The first whitespace-delimited token of a unicharset line - the unichar itself.
QByteArray firstToken(const QByteArray &line)
{
    int i = 0;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line.at(i))) == 0)
        ++i;
    return line.left(i);
}

bool fail(QString *error, const QString &name, const QString &detail)
{
    if (error) {
        //: %1 is a language data file name such as swe.traineddata. %2 is a technical
        //: detail, one of the lower-case messages in this context.
        *error = Text::tr("%1 is damaged or incomplete (%2). Replace it with a fresh copy "
                          "of the language data.")
                     .arg(name, detail);
    }
    return false;
}

// A unicharset the loader can consume: the declared count must be backed by at
// least that many lines, and no unichar may repeat. Tesseract's
// UNICHARSET::load_via_fgets writes unichars[id] for every id it reads, but the
// insert behind it silently skips a duplicate - so one repeated entry leaves the
// rest of the load writing past the end of the vector.
bool checkUnicharset(const QByteArray &blob, int slot, const QString &name, QString *error)
{
    const QList<QByteArray> lines = blob.split('\n');
    bool ok = false;
    const int declared = lines.value(0).trimmed().toInt(&ok);
    if (!ok || declared <= 0) {
        //: Detail for the damaged language data message. %1 is a component number.
        //: A unichar is one character entry in Tesseract's character list.
        return fail(error, name,
                    Text::tr("component %1 does not start with a unichar count").arg(slot));
    }

    QList<QByteArray> entries;
    for (int i = 1; i < lines.size() && entries.size() < declared; ++i) {
        if (!lines.at(i).trimmed().isEmpty())
            entries << lines.at(i);
    }
    if (entries.size() < declared) {
        //: Detail for the damaged language data message. %1 is a component number,
        //: %2 and %3 are counts.
        return fail(error, name,
                    Text::tr("component %1 declares %2 unichars but holds %3")
                        .arg(slot)
                        .arg(declared)
                        .arg(entries.size()));
    }

    QSet<QByteArray> seen;
    for (const QByteArray &line : entries) {
        const QByteArray tok = firstToken(line);
        if (tok.isEmpty()) {
            //: Detail for the damaged language data message. %1 is a component number.
            return fail(error, name,
                        Text::tr("component %1 has an empty unichar entry").arg(slot));
        }
        if (seen.contains(tok)) {
            //: Detail for the damaged language data message. %1 is a component number,
            //: %2 the repeated character.
            return fail(error, name,
                        Text::tr("component %1 lists the unichar \"%2\" twice")
                            .arg(slot)
                            .arg(QString::fromUtf8(tok)));
        }
        seen.insert(tok);
    }
    return true;
}

} // namespace

bool TessdataFile::validate(const QString &path, QString *error)
{
    const QString name = QFileInfo(path).fileName();

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            //: %1 is a language data file name, %2 the system's reason.
            *error = Text::tr("Cannot read %1: %2").arg(name, f.errorString());
        }
        return false;
    }

    const qint64 size = f.size();
    if (size < static_cast<qint64>(sizeof(quint32))) {
        //: Detail for the damaged language data message.
        return fail(error, name, Text::tr("file is too small to hold a header"));
    }

    QByteArray head = f.read(sizeof(quint32));
    if (head.size() != static_cast<int>(sizeof(quint32))) {
        //: Detail for the damaged language data message.
        return fail(error, name, Text::tr("header is truncated"));
    }

    quint32 count = qFromLittleEndian<quint32>(head.constData());
    const bool swapped = count > kMaxEntries;
    if (swapped)
        count = qFromBigEndian<quint32>(head.constData());
    if (count == 0 || count > kMaxEntries) {
        //: Detail for the damaged language data message. %1 is a number.
        return fail(error, name,
                    Text::tr("implausible component count %1 - this is probably not "
                             "Tesseract language data")
                        .arg(count));
    }

    const qint64 tableBytes = static_cast<qint64>(count) * static_cast<qint64>(sizeof(qint64));
    const qint64 tableEnd = static_cast<qint64>(sizeof(quint32)) + tableBytes;
    if (size < tableEnd) {
        //: Detail for the damaged language data message. %1 and %2 are byte counts.
        return fail(error, name,
                    Text::tr("offset table needs %1 bytes but the file is only %2")
                        .arg(tableEnd)
                        .arg(size));
    }

    const QByteArray table = f.read(tableBytes);
    if (table.size() != tableBytes) {
        //: Detail for the damaged language data message.
        return fail(error, name, Text::tr("offset table is truncated"));
    }

    // Negative offsets mark absent components; the present ones must sit inside
    // the file and ascend, because Tesseract derives each component's length
    // from the next present offset (or EOF for the last one).
    std::vector<qint64> offsets(count);
    qint64 prev = -1;
    for (quint32 i = 0; i < count; ++i) {
        const char *p = table.constData() + i * sizeof(qint64);
        offsets[i] = swapped ? qFromBigEndian<qint64>(p) : qFromLittleEndian<qint64>(p);
        if (offsets[i] < 0)
            continue;
        if (offsets[i] < tableEnd || offsets[i] > size) {
            //: Detail for the damaged language data message. %1 is a component number,
            //: %2 a byte offset, %3 the file size in bytes.
            return fail(error, name,
                        Text::tr("component %1 starts at %2, outside the %3-byte file")
                            .arg(i)
                            .arg(offsets[i])
                            .arg(size));
        }
        if (offsets[i] <= prev) {
            //: Detail for the damaged language data message. %1 is a component number,
            //: %2 a byte offset.
            return fail(error, name,
                        Text::tr("component %1 starts at %2, before the previous component")
                            .arg(i)
                            .arg(offsets[i]));
        }
        prev = offsets[i];
    }

    for (quint32 i = 0; i < count; ++i) {
        if (offsets[i] < 0)
            continue;
        qint64 len = size - offsets[i];
        for (quint32 j = i + 1; j < count; ++j) {
            if (offsets[j] >= 0) {
                len = offsets[j] - offsets[i];
                break;
            }
        }
        if (len <= 0) {
            //: Detail for the damaged language data message. %1 is a component number,
            //: %2 a length in bytes.
            return fail(error, name,
                        Text::tr("component %1 has a non-positive length %2").arg(i).arg(len));
        }

        const bool isUnicharset =
            std::find(std::begin(kUnicharsetSlots), std::end(kUnicharsetSlots),
                      static_cast<int>(i))
            != std::end(kUnicharsetSlots);
        if (!isUnicharset)
            continue; // only the unicharsets are parsed; the rest is opaque model data

        if (!f.seek(offsets[i])) {
            //: Detail for the damaged language data message. %1 is a component number.
            return fail(error, name, Text::tr("cannot seek to component %1").arg(i));
        }
        const QByteArray blob = f.read(len);
        if (blob.size() != len) {
            //: Detail for the damaged language data message. %1 is a component number.
            return fail(error, name, Text::tr("component %1 is truncated").arg(i));
        }
        if (!checkUnicharset(blob, static_cast<int>(i), name, error))
            return false;
    }

    return true;
}

bool TessdataFile::validateLanguages(const QString &dir, const QStringList &languages,
                                     QString *error)
{
    QDir d(dir);
    for (const QString &lang : languages) {
        if (lang.isEmpty())
            continue;
        const QString path = d.filePath(lang + QStringLiteral(".traineddata"));
        if (!QFileInfo::exists(path))
            continue; // see the header: Tesseract reports a missing language cleanly
        if (!validate(path, error))
            return false;
    }
    return true;
}

} // namespace mervin
