#include <QBuffer>
#include <QCborValue>
#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonValue>
#include <QRegularExpression>
#include <QtEndian>

#include <cstdint>
#include <iostream>

/// Calculate a PNG chunk CRC after deliberately corrupting its compressed data.
quint32 crc(const QByteArray &bytes)
{
    quint32 value = 0xffffffff;
    for (unsigned char byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320 : 0);
    }
    return value ^ 0xffffffff;
}

/// Append a complete ancillary chunk, including its correct length and CRC.
QByteArray chunk(const QByteArray &type, const QByteArray &payload)
{
    QByteArray bytes(4, '\0');
    qToBigEndian(quint32(payload.size()), bytes.data());
    bytes += type + payload;
    const auto checksum = crc(bytes.mid(4));
    const auto offset = bytes.size();
    bytes.resize(offset + 4);
    qToBigEndian(checksum, bytes.data() + offset);
    return bytes;
}

/// Exercise actual Qt entry points for each source patch before stamping the SDK.
int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QImage original(2, 2, QImage::Format_RGB32);
    original.fill(qRgb(31, 73, 127));
    QByteArray png;
    QBuffer output(&png);
    if (!output.open(QIODevice::WriteOnly) || !original.save(&output, "PNG"))
        return 1;
    if (QImage::fromData(png, "PNG") != original)
        return 2;
    // The libpng advisory concerns malformed compressed ancillary data followed
    // by png_read_end. Keep valid image data and CRCs to reach that decoder path.
    const QByteArray compressed = qCompress(QByteArray(32768, 'x')).mid(4);
    for (const QByteArray &type : {QByteArray("zTXt"), QByteArray("iTXt"), QByteArray("iCCP")}) {
        QByteArray metadata("note\0", 5);
        if (type == "iTXt")
            metadata += QByteArray("\1\0\0\0", 4); // Compressed, no language or translated keyword.
        else
            metadata += '\0'; // Compression method.
        if (type == "iCCP") {
            // Compress a real ICC profile, then truncate its compressed data.
            const auto profile = QColorSpace(QColorSpace::SRgb).iccProfile();
            const auto compressedProfile = qCompress(profile).mid(4);
            metadata += compressedProfile.left(compressedProfile.size() / 2);
        } else {
            metadata += compressed.left(compressed.size() / 2);
        }
        // iCCP must precede IDAT or libpng rejects its position before inflating.
        const auto offset = type == "iCCP" ? qsizetype(33) : png.size() - 12;
        const QByteArray malformed = png.left(offset) + chunk(type, metadata) + png.mid(offset);
        // Invalid ancillary metadata is discarded. Valid pixel data stays usable.
        if (QImage::fromData(malformed, "PNG") != original)
            return 3;
    }
    // Expand the PCRE2 upstream large-JIT-stack regression into a Qt pattern.
    // Qt provides a growable JIT stack and uses PCRE2's 16-bit implementation.
    const QString groups = QStringLiteral("()\\g{-1}").repeated(1400);
    QRegularExpression expression(QStringLiteral("((?(DEFINE)") + groups + QStringLiteral(").{1}(?R)|)"));
    if (!expression.isValid())
        return 4;
    expression.optimize();
    const auto match = expression.match(QStringLiteral("AAAAAA"));
    // A bounded JIT-stack failure is also an acceptable result for this input.
    if (match.hasMatch() && match.capturedLength() > 6)
        return 5;
    QRegularExpression ordinary(QStringLiteral("^(ab)+$"));
    ordinary.optimize();
    if (!ordinary.match(QStringLiteral("abab")).hasMatch() || ordinary.match(QStringLiteral("abX")).hasMatch())
        return 6;
    const auto emptyUrl = QCborValue::fromCbor(QByteArray::fromHex("d82060"));
    if (!emptyUrl.toJsonValue().isString() || !emptyUrl.toJsonValue().toString().isEmpty())
        return 7;
    std::cout << "Qt " << qVersion() << " PNG, malformed ancillary chunks, JIT regex and CBOR checks passed\n";
    return 0;
}
