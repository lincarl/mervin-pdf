#include "print/PageRange.h"

#include <QTest>

class TstPrintRange : public QObject
{
    Q_OBJECT

private slots:
    void singlePage();
    void simpleRange();
    void mixedTokensKeepOrder();
    void whitespaceTolerated();
    void strayCommasTolerated();
    void duplicatesPreserved();
    void reversedTokenOrderPreserved();
    void openEndedRanges();
    void emptyIsError();
    void nonNumericIsError();
    void outOfRangeIsError();
    void backwardsRangeIsError();
    void bareDashIsError();
    void multiDashIsError();
    void overflowIsError();
    void allowingAllExpandsAll();
    void allowingAllIsOtherwiseParse();
};

void TstPrintRange::singlePage()
{
    QString err = QStringLiteral("untouched");
    QCOMPARE(PageRange::parse(QStringLiteral("5"), 10, &err), (QList<int>{5}));
    QVERIFY(err.isEmpty()); // cleared on success
}

void TstPrintRange::simpleRange()
{
    QString err;
    QCOMPARE(PageRange::parse(QStringLiteral("1-3,5-8"), 10, &err),
             (QList<int>{1, 2, 3, 5, 6, 7, 8}));
    QVERIFY(err.isEmpty());
}

void TstPrintRange::mixedTokensKeepOrder()
{
    QString err;
    QCOMPARE(PageRange::parse(QStringLiteral("2,4-6,9"), 10, &err),
             (QList<int>{2, 4, 5, 6, 9}));
    QVERIFY(err.isEmpty());
}

void TstPrintRange::whitespaceTolerated()
{
    QString err;
    QCOMPARE(PageRange::parse(QStringLiteral("  1 - 3 ,  5 "), 10, &err),
             (QList<int>{1, 2, 3, 5}));
    QVERIFY(err.isEmpty());
}

void TstPrintRange::strayCommasTolerated()
{
    QString err;
    QCOMPARE(PageRange::parse(QStringLiteral("1-3,,5,"), 10, &err),
             (QList<int>{1, 2, 3, 5}));
    QVERIFY(err.isEmpty());
}

void TstPrintRange::duplicatesPreserved()
{
    QString err;
    QCOMPARE(PageRange::parse(QStringLiteral("1,1,2-3,3"), 10, &err),
             (QList<int>{1, 1, 2, 3, 3}));
    QVERIFY(err.isEmpty());
}

void TstPrintRange::reversedTokenOrderPreserved()
{
    QString err;
    QCOMPARE(PageRange::parse(QStringLiteral("5-6,1"), 10, &err), (QList<int>{5, 6, 1}));
    QVERIFY(err.isEmpty());
}

void TstPrintRange::openEndedRanges()
{
    QString err;
    QCOMPARE(PageRange::parse(QStringLiteral("8-"), 10, &err), (QList<int>{8, 9, 10}));
    QVERIFY(err.isEmpty());
    QCOMPARE(PageRange::parse(QStringLiteral("-3"), 10, &err), (QList<int>{1, 2, 3}));
    QVERIFY(err.isEmpty());
}

void TstPrintRange::emptyIsError()
{
    QString err;
    QVERIFY(PageRange::parse(QStringLiteral("   "), 10, &err).isEmpty());
    QVERIFY(!err.isEmpty());
}

void TstPrintRange::nonNumericIsError()
{
    QString err;
    QVERIFY(PageRange::parse(QStringLiteral("1,abc,3"), 10, &err).isEmpty());
    QVERIFY(!err.isEmpty());
}

void TstPrintRange::outOfRangeIsError()
{
    QString err;
    QVERIFY(PageRange::parse(QStringLiteral("1-12"), 10, &err).isEmpty()); // 12 > pageCount
    QVERIFY(!err.isEmpty());
    QVERIFY(PageRange::parse(QStringLiteral("0"), 10, &err).isEmpty()); // 0 < 1
    QVERIFY(!err.isEmpty());
}

void TstPrintRange::backwardsRangeIsError()
{
    QString err;
    QVERIFY(PageRange::parse(QStringLiteral("8-3"), 10, &err).isEmpty());
    QVERIFY(!err.isEmpty());
}

void TstPrintRange::bareDashIsError()
{
    QString err;
    QVERIFY(PageRange::parse(QStringLiteral("-"), 10, &err).isEmpty());
    QVERIFY(!err.isEmpty());
}

void TstPrintRange::multiDashIsError()
{
    // QString::toInt() is all-or-nothing (unlike C atoi): it rejects trailing
    // junk, so a multi-dash token cannot silently truncate. "1-2-3" splits into
    // lhs="1", rhs="2-3" -> toInt("2-3") fails. "1--3"/"5--3" yield rhs "-3" which
    // parses to -3 and is rejected by the bounds check. All must error, not
    // partially print.
    for (const auto &spec : {QStringLiteral("1-2-3"), QStringLiteral("1--3"),
                             QStringLiteral("5--3"), QStringLiteral("1-2-")}) {
        QString err;
        QVERIFY2(PageRange::parse(spec, 10, &err).isEmpty(),
                 qPrintable(QStringLiteral("should reject: %1").arg(spec)));
        QVERIFY(!err.isEmpty());
    }
}

void TstPrintRange::overflowIsError()
{
    QString err;
    // Out-of-int-range digits: toInt() fails rather than wrapping.
    QVERIFY(PageRange::parse(QStringLiteral("99999999999999"), 10, &err).isEmpty());
    QVERIFY(!err.isEmpty());
}

void TstPrintRange::allowingAllExpandsAll()
{
    QString err = QStringLiteral("untouched");
    QCOMPARE(PageRange::parseAllowingAll(QStringLiteral("all"), 4, &err), (QList<int>{1, 2, 3, 4}));
    QVERIFY(err.isEmpty());
    // Any case, any padding - this is what the Document menu's prompts accept.
    QCOMPARE(PageRange::parseAllowingAll(QStringLiteral("ALL"), 2, &err), (QList<int>{1, 2}));
    QCOMPARE(PageRange::parseAllowingAll(QStringLiteral("  All  "), 2, &err), (QList<int>{1, 2}));
    // plain parse() still rejects it - that divergence is the reason this exists.
    QVERIFY(PageRange::parse(QStringLiteral("all"), 4, &err).isEmpty());
}

void TstPrintRange::allowingAllIsOtherwiseParse()
{
    QString err;
    QCOMPARE(PageRange::parseAllowingAll(QStringLiteral("1-3,7"), 10, &err),
             (QList<int>{1, 2, 3, 7}));
    QVERIFY(err.isEmpty());
    QVERIFY(PageRange::parseAllowingAll(QString(), 10, &err).isEmpty()); // empty is still an error
    QVERIFY(!err.isEmpty());
    QVERIFY(PageRange::parseAllowingAll(QStringLiteral("allsorts"), 10, &err).isEmpty());
    QVERIFY(!err.isEmpty());
    QVERIFY(PageRange::parseAllowingAll(QStringLiteral("40"), 10, &err).isEmpty());
    QVERIFY2(err.contains(QStringLiteral("1-10")), qPrintable(err)); // plain hyphen, no en dash
}

QTEST_GUILESS_MAIN(TstPrintRange)
#include "tst_print_range.moc"
