#pragma once

#include <QUrl>

#include <functional>

class QObject;

namespace mervin::externalopen {

/// Routes Windows browser and Store launches through workers. Register once on
/// the UI thread with an application-lifetime owner. Other platforms keep Qt's
/// desktop integration. A supplied launcher replaces the native call for tests
/// and runs on a worker on every platform.
void install(QObject *owner, std::function<void(const QUrl &)> launcher = {});

} // namespace mervin::externalopen
