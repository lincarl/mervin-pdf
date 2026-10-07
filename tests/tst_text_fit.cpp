#include "TextFit.h"

#include <QApplication>
#include <QScrollArea>
#include <QSpinBox>
#include <QTest>
#include <QVBoxLayout>

class TstTextFit : public QObject
{
    Q_OBJECT

private slots:
    void fittingControls();
    void detectsClippedControls();
    void checksUnselectedComboOptions();
    void wrappingAndRichText();
    void scrollingContentAndEditableValues();
    void explicitElisionDoesNotHideVerticalClipping();
    void hiddenPagesAreCheckedWhenShown();
    void stylesheetPaddingReducesAvailableSpace();
    void buttonUsesPaintedContentsRatherThanPreferredSize();
    void detectsClippingByParent();
    void framedLabelMarginIncludesAutomaticIndent();
    void stylesheetGroupTitleUsesPaintedArea();
    void multilinePlaceholderMustFitWithoutScrolling();
    void placeholderAccountsForPersistentActionsButNotClearButton();
    void editableValuesCanScrollOnlyHorizontally();
};

namespace {

void showAndSettle(QWidget &widget)
{
    widget.show();
    QCoreApplication::processEvents();
}

} // namespace

void TstTextFit::fittingControls()
{
    QWidget root;
    auto *layout = new QVBoxLayout(&root);
    auto *label = new QLabel(QStringLiteral("English, Svenska, 简体中文"));
    layout->addWidget(label);
    auto *edit = new QLineEdit;
    edit->setPlaceholderText(QStringLiteral("Choose a file"));
    layout->addWidget(edit);
    label->setText(QStringLiteral("&Name"));
    label->setBuddy(edit);
    layout->addWidget(new QPushButton(QStringLiteral("&Open && inspect")));
    layout->addWidget(new QCheckBox(QStringLiteral("&Remember choice")));
    layout->addWidget(new QRadioButton(QStringLiteral("Use &default")));
    auto *combo = new QComboBox;
    combo->addItems({QStringLiteral("First"), QStringLiteral("Second option")});
    layout->addWidget(combo);
    auto *spin = new QSpinBox;
    spin->setRange(-99999, 99999);
    spin->setValue(-99999);
    layout->addWidget(spin);
    auto *group = new QGroupBox(QStringLiteral("&Options"));
    group->setCheckable(true);
    auto *groupLayout = new QVBoxLayout(group);
    groupLayout->addWidget(new QLabel(QStringLiteral("Visible setting")));
    layout->addWidget(group);
    root.resize(500, 400);
    showAndSettle(root);
    const QStringList errors = textfit::check(root);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
}

void TstTextFit::detectsClippedControls()
{
    QWidget root;
    root.resize(600, 500);
    const QString longText = QStringLiteral("A translated control that cannot fit");
    auto *label = new QLabel(longText, &root);
    label->setObjectName(QStringLiteral("clippedLabel"));
    auto *button = new QPushButton(longText, &root);
    button->setObjectName(QStringLiteral("clippedButton"));
    auto *check = new QCheckBox(longText, &root);
    check->setObjectName(QStringLiteral("clippedCheck"));
    auto *radio = new QRadioButton(longText, &root);
    radio->setObjectName(QStringLiteral("clippedRadio"));
    auto *edit = new QLineEdit(&root);
    edit->setObjectName(QStringLiteral("clippedPlaceholder"));
    edit->setPlaceholderText(longText);
    auto *spin = new QSpinBox(&root);
    spin->setObjectName(QStringLiteral("clippedSpin"));
    spin->setRange(0, 99999999);
    spin->setValue(99999999);
    auto *group = new QGroupBox(longText, &root);
    group->setObjectName(QStringLiteral("clippedGroup"));
    auto *list = new QListWidget(&root);
    list->setObjectName(QStringLiteral("clippedNavigation"));
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->addItem(longText);
    const QList<QWidget *> controls{label, button, check, radio, edit, spin, group, list};
    int y = 0;
    for (QWidget *control : controls) {
        control->setGeometry(0, y, 40, 45);
        y += 50;
    }
    showAndSettle(root);
    const QString errors = textfit::check(root).join('\n');
    for (QWidget *control : controls)
        QVERIFY2(errors.contains(control->objectName()), qPrintable(errors));
    QVERIFY(errors.contains(QStringLiteral("needs")));
    QVERIFY(errors.contains(QStringLiteral("available")));
}

void TstTextFit::checksUnselectedComboOptions()
{
    QComboBox combo;
    combo.addItems({QStringLiteral("OK"), QStringLiteral("A much longer translated option")});
    combo.setCurrentIndex(0);
    combo.setFixedSize(60, 40);
    showAndSettle(combo);
    const QString errors = textfit::check(combo).join('\n');
    QVERIFY2(errors.contains(QStringLiteral("option 1")), qPrintable(errors));
    QVERIFY(!errors.contains(QStringLiteral("option 0")));
}

void TstTextFit::wrappingAndRichText()
{
    for (const int mode : {0, 1, 2}) {
        const bool rich = mode == 1;
        QLabel label;
        label.setTextFormat(rich ? Qt::RichText : Qt::PlainText);
        label.setText(rich ? QStringLiteral("<b>Translated explanation</b> that wraps onto several lines.")
                           : QStringLiteral("Translated explanation that wraps onto several lines."));
        label.setWordWrap(true);
        if (mode == 2)
            label.setTextInteractionFlags(Qt::TextSelectableByMouse);
        label.setMargin(4);
        label.setFixedSize(150, 180);
        showAndSettle(label);
        const QStringList errors = textfit::check(label);
        QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
        label.setFixedHeight(20);
        QVERIFY(!textfit::check(label).isEmpty());
    }
}

void TstTextFit::scrollingContentAndEditableValues()
{
    QScrollArea root;
    auto *content = new QWidget;
    auto *layout = new QVBoxLayout(content);
    for (int i = 0; i < 20; ++i)
        layout->addWidget(new QLabel(QStringLiteral("Scrollable row %1").arg(i)));
    auto *edit = new QLineEdit(QString(300, 'x'));
    edit->setFixedWidth(80);
    layout->addWidget(edit);
    auto *combo = new QComboBox;
    combo->setEditable(true);
    combo->addItem(QString(300, 'x'));
    combo->setFixedWidth(80);
    layout->addWidget(combo);
    root.setWidget(content);
    root.resize(300, 100);
    showAndSettle(root);
    const QStringList errors = textfit::check(root);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
}

void TstTextFit::explicitElisionDoesNotHideVerticalClipping()
{
    QLabel label(QStringLiteral("A deliberately shortened document path"));
    label.setProperty("textFitAllowElision", QStringLiteral("Document paths are intentionally elided."));
    label.setFixedSize(40, 40);
    showAndSettle(label);
    QVERIFY(textfit::check(label).isEmpty());
    label.setFixedHeight(1);
    QVERIFY(!textfit::check(label).isEmpty());
}

void TstTextFit::hiddenPagesAreCheckedWhenShown()
{
    QWidget root;
    auto *label = new QLabel(QStringLiteral("An oversized hidden page label"), &root);
    label->setFixedSize(20, 30);
    label->hide();
    showAndSettle(root);
    QVERIFY(textfit::check(root).isEmpty());
    label->show();
    QVERIFY(!textfit::check(root).isEmpty());
}

void TstTextFit::stylesheetPaddingReducesAvailableSpace()
{
    QLabel label(QStringLiteral("Text with padding"));
    label.ensurePolished();
    label.setFixedSize(label.sizeHint() + QSize(20, 20));
    showAndSettle(label);
    const QStringList fitting = textfit::check(label);
    QVERIFY2(fitting.isEmpty(), qPrintable(fitting.join('\n')));
    label.setStyleSheet(QStringLiteral("QLabel { padding-left: 100px; padding-right: 30px; }"));
    QCoreApplication::processEvents();
    const QStringList errors = textfit::check(label);
    QVERIFY2(!errors.isEmpty(), "Stylesheet padding must reduce the measured text area");
}

void TstTextFit::buttonUsesPaintedContentsRatherThanPreferredSize()
{
    QPushButton button(QStringLiteral("OK"));
    button.setFixedSize(50, 40);
    showAndSettle(button);
    QVERIFY(button.sizeHint().width() > button.width());
    const QStringList errors = textfit::check(button);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    button.setStyleSheet(QStringLiteral("QPushButton { padding-left: 25px; padding-right: 20px; }"));
    QCoreApplication::processEvents();
    QVERIFY(!textfit::check(button).isEmpty());
}

void TstTextFit::detectsClippingByParent()
{
    QWidget root;
    auto *holder = new QWidget(&root);
    auto *label = new QLabel(QStringLiteral("The label fits itself"), holder);
    label->ensurePolished();
    const QSize labelSize = label->sizeHint() + QSize(20, 20);
    label->resize(labelSize);
    holder->resize(labelSize.width() / 2, labelSize.height());
    root.resize(labelSize + QSize(40, 40));
    showAndSettle(root);
    QVERIFY(textfit::check(root).join('\n').contains(QStringLiteral("extends outside its parent")));
    holder->resize(labelSize);
    const QStringList fitting = textfit::check(root);
    QVERIFY2(fitting.isEmpty(), qPrintable(fitting.join('\n')));
}

void TstTextFit::framedLabelMarginIncludesAutomaticIndent()
{
    QLabel label(QStringLiteral("Framed text"));
    label.setFrameStyle(QFrame::Box);
    label.setMargin(8);
    const QSize text = label.fontMetrics().boundingRect(QRect(0, 0, 10000, 10000), 0, label.text()).size();
    label.setFixedSize(text + QSize(18, 18));
    showAndSettle(label);
    const QStringList errors = textfit::check(label);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    label.setFixedWidth(label.width() - 10);
    QVERIFY(!textfit::check(label).isEmpty());
}

void TstTextFit::stylesheetGroupTitleUsesPaintedArea()
{
    QGroupBox group(QStringLiteral("Current security"));
    group.setStyleSheet(QStringLiteral("QGroupBox { border:1px solid gray; margin-top:10px; }"
                                      "QGroupBox::title { subcontrol-origin:margin;"
                                      " subcontrol-position:top left; left:10px; padding:0 4px; }"));
    group.setFixedSize(300, 100);
    showAndSettle(group);
    const QStringList errors = textfit::check(group);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    group.setFixedWidth(30);
    QVERIFY(!textfit::check(group).isEmpty());
}

void TstTextFit::multilinePlaceholderMustFitWithoutScrolling()
{
    QPlainTextEdit editor;
    editor.setPlaceholderText(QStringLiteral("Recognised text will appear here. You can edit it before copying."));
    editor.setFixedSize(200, 130);
    showAndSettle(editor);
    const QStringList errors = textfit::check(editor);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    editor.setFixedHeight(20);
    QVERIFY(!textfit::check(editor).isEmpty());
    editor.setPlainText(QString(1000, 'x'));
    QVERIFY(textfit::check(editor).isEmpty());
}

void TstTextFit::placeholderAccountsForPersistentActionsButNotClearButton()
{
    QLineEdit editor;
    editor.setPlaceholderText(QStringLiteral("Clearable input"));
    editor.setClearButtonEnabled(true);
    const int textWidth = editor.fontMetrics().size(0, editor.placeholderText()).width();
    editor.setFixedSize(textWidth + 10, 40);
    showAndSettle(editor);
    QVERIFY(textfit::check(editor).isEmpty());
    editor.setText(QStringLiteral("A populated field has no placeholder"));
    QCoreApplication::processEvents();
    QVERIFY(textfit::check(editor).isEmpty());
    editor.clear();
    QVERIFY(textfit::check(editor).isEmpty());
    editor.addAction(editor.style()->standardIcon(QStyle::SP_FileIcon), QLineEdit::LeadingPosition);
    QCoreApplication::processEvents();
    QVERIFY(!textfit::check(editor).isEmpty());
    editor.setFixedWidth(editor.width() + 60);
    QVERIFY(textfit::check(editor).isEmpty());
}

void TstTextFit::editableValuesCanScrollOnlyHorizontally()
{
    QLineEdit editor(QString(200, 'x'));
    editor.setFixedSize(70, 40);
    showAndSettle(editor);
    QVERIFY(textfit::check(editor).isEmpty());
    editor.setFixedHeight(8);
    const QString problems = textfit::check(editor).join('\n');
    QVERIFY2(problems.contains(QStringLiteral("scrollable value")), qPrintable(problems));
}

QTEST_MAIN(TstTextFit)
#include "tst_text_fit.moc"
