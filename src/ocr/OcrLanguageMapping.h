#pragma once

#include <QString>
#include <QStringList>

namespace mervin::ocr {

// Match a UI or OS locale to an explicitly supported tessdata_best model.
// Unknown languages and writing scripts have no fallback model.
QString modelForLanguage(const QString &localeTag);

// Selected language first, then the OS language if it needs a different model.
QStringList initialModels(const QString &selectedLanguage, const QString &osLanguage);

} // namespace mervin::ocr
