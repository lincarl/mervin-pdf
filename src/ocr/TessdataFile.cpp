#include "ocr/TessdataFile.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cctype>
#include <vector>

namespace mervin {

namespace {

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
        *error = QObject::tr("%1 is damaged or incomplete (%2). Replace it with a fresh copy "
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
        return fail(error, name,
                    QObject::tr("component %1 does not start with a unichar count").arg(slot));
    }

    QList<QByteArray> entries;
    for (int i = 1; i < lines.size() && entries.size() < declared; ++i) {
        if (!lines.at(i).trimmed().isEmpty())
            entries << lines.at(i);
    }
    if (entries.size() < declared) {
        return fail(error, name,
                    QObject::tr("component %1 declares %2 unichars but holds %3")
                        .arg(slot)
                        .arg(declared)
                        .arg(entries.size()));
    }

    QSet<QByteArray> seen;
    for (const QByteArray &line : entries) {
        const QByteArray tok = firstToken(line);
        if (tok.isEmpty()) {
            return fail(error, name,
                        QObject::tr("component %1 has an empty unichar entry").arg(slot));
        }
        if (seen.contains(tok)) {
            return fail(error, name,
                        QObject::tr("component %1 lists the unichar \"%2\" twice")
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
        if (error)
            *error = QObject::tr("Cannot read %1: %2").arg(name, f.errorString());
        return false;
    }

    const qint64 size = f.size();
    if (size < static_cast<qint64>(sizeof(quint32)))
        return fail(error, name, QObject::tr("file is too small to hold a header"));

    QByteArray head = f.read(sizeof(quint32));
    if (head.size() != static_cast<int>(sizeof(quint32)))
        return fail(error, name, QObject::tr("header is truncated"));

    quint32 count = qFromLittleEndian<quint32>(head.constData());
    const bool swapped = count > kMaxEntries;
    if (swapped)
        count = qFromBigEndian<quint32>(head.constData());
    if (count == 0 || count > kMaxEntries) {
        return fail(error, name,
                    QObject::tr("implausible component count %1 - this is probably not "
                                "Tesseract language data")
                        .arg(count));
    }

    const qint64 tableBytes = static_cast<qint64>(count) * static_cast<qint64>(sizeof(qint64));
    const qint64 tableEnd = static_cast<qint64>(sizeof(quint32)) + tableBytes;
    if (size < tableEnd) {
        return fail(error, name,
                    QObject::tr("offset table needs %1 bytes but the file is only %2")
                        .arg(tableEnd)
                        .arg(size));
    }

    const QByteArray table = f.read(tableBytes);
    if (table.size() != tableBytes)
        return fail(error, name, QObject::tr("offset table is truncated"));

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
            return fail(error, name,
                        QObject::tr("component %1 starts at %2, outside the %3-byte file")
                            .arg(i)
                            .arg(offsets[i])
                            .arg(size));
        }
        if (offsets[i] <= prev) {
            return fail(error, name,
                        QObject::tr("component %1 starts at %2, before the previous component")
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
            return fail(error, name,
                        QObject::tr("component %1 has a non-positive length %2").arg(i).arg(len));
        }

        const bool isUnicharset =
            std::find(std::begin(kUnicharsetSlots), std::end(kUnicharsetSlots),
                      static_cast<int>(i))
            != std::end(kUnicharsetSlots);
        if (!isUnicharset)
            continue; // only the unicharsets are parsed; the rest is opaque model data

        if (!f.seek(offsets[i]))
            return fail(error, name, QObject::tr("cannot seek to component %1").arg(i));
        const QByteArray blob = f.read(len);
        if (blob.size() != len)
            return fail(error, name, QObject::tr("component %1 is truncated").arg(i));
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
