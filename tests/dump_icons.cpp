// Developer tool (not a test): renders every mervin::icons::Glyph through the
// real painter and writes a contact sheet PNG, so the house icon set can be
// checked by eye after a change to Icons.cpp.
//
//   dump_icons [out.png] [--dark] [--ocr-only]
//
// Each glyph is drawn at 48 px (inspection size) and again at 16/20/24 px, which
// is where thin strokes and small terminals actually fail.

#include "ui/Icons.h"
#include "ui/IconList.h"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>

#include <cstdio>

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);

    QString out = QStringLiteral("icons.png");
    bool dark = false;
    bool ocrOnly = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--dark"))
            dark = true;
        else if (a == QLatin1String("--ocr-only"))
            ocrOnly = true;
        else
            out = a;
    }

    const auto entries = mervin::icons::allGlyphs();
    const QColor bg  = dark ? QColor(0x1c, 0x22, 0x2e) : QColor(0xff, 0xff, 0xff);
    const QColor ink = dark ? QColor(0xc2, 0xc9, 0xd6) : QColor(0x2b, 0x30, 0x38);
    const QColor label = dark ? QColor(0x7c, 0x84, 0x94) : QColor(0x8b, 0x94, 0xa5);

    if (ocrOnly) {
        QImage preview(42, 28, QImage::Format_ARGB32_Premultiplied);
        preview.fill(bg);
        QPainter previewPainter(&preview);
        previewPainter.drawPixmap(QPoint(4, 4),
                                  mervin::icons::ocrWordmark(ink)
                                      .pixmap(QSize(34, 20), 1.0));
        previewPainter.end();
        if (!preview.save(out)) {
            std::fprintf(stderr, "failed to write %s\n", qPrintable(out));
            return 1;
        }
        std::printf("wrote %s (wordmark 34x20 OCR preview on 42x28)\n",
                    qPrintable(QDir::toNativeSeparators(out)));
        return 0;
    }

    constexpr int kCols = 6;
    constexpr int kCellW = 200;
    constexpr int kCellH = 96;
    const int rows = (int(entries.size()) + kCols - 1) / kCols;

    QImage sheet(kCols * kCellW, rows * kCellH, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(bg);
    QPainter p(&sheet);
    QFont f = p.font();
    f.setPixelSize(12);
    p.setFont(f);

    for (int i = 0; i < int(entries.size()); ++i) {
        const int cx = (i % kCols) * kCellW;
        const int cy = (i / kCols) * kCellH;
        const bool wideOcr = entries[i].id == mervin::icons::Glyph::Ocr;
        const QIcon icon = wideOcr ? mervin::icons::ocrWordmark(ink)
                                   : mervin::icons::glyph(entries[i].id, ink);

        // 48 px on the left, then the sizes the UI actually asks for.
        const auto iconSize = [wideOcr](int height) {
            return wideOcr ? QSize(qRound(height * 1.7), height)
                           : QSize(height, height);
        };
        p.drawPixmap(QPoint(cx + 8, cy + 8), icon.pixmap(iconSize(48)));
        int x = cx + (wideOcr ? 96 : 64);
        for (int sz : {24, 20, 16}) {
            const QSize requested = iconSize(sz);
            p.drawPixmap(QPoint(x, cy + 8 + (48 - sz) / 2), icon.pixmap(requested));
            x += requested.width() + 10;
        }
        p.setPen(label);
        p.drawText(cx + 8, cy + 78, QString::fromLatin1(entries[i].name));
    }
    p.end();

    if (!sheet.save(out)) {
        std::fprintf(stderr, "failed to write %s\n", qPrintable(out));
        return 1;
    }
    std::printf("wrote %s (%d glyphs, %dx%d)\n", qPrintable(QDir::toNativeSeparators(out)),
                int(entries.size()), sheet.width(), sheet.height());

    // What a theme switch costs: applyActionIcons() rebuilds the whole roster,
    // and each QIcon paints all five sizes. Printed so the price of the painted
    // set stays visible - whole-process startup timings cannot resolve it.
    QElapsedTimer t;
    t.start();
    constexpr int kRounds = 10;
    for (int i = 0; i < kRounds; ++i)
        for (const auto &e : entries)
            (void)mervin::icons::glyph(e.id, ink).availableSizes();
    std::printf("building all %d glyphs (5 sizes each): %.2f ms per round\n",
                int(entries.size()), double(t.nsecsElapsed()) / 1e6 / kRounds);
    return 0;
}
