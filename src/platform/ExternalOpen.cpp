#include "platform/ExternalOpen.h"

#include "io/Background.h"

#include <QDebug>
#include <QDesktopServices>
#include <QObject>

#include <utility>

#ifdef Q_OS_WIN
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <objbase.h>
#  include <shellapi.h>
#endif

namespace mervin::externalopen {

/// Keeps Qt's URL dispatch on the UI thread and hands only the native launch to a worker.
class Handler final : public QObject
{
    Q_OBJECT

public:
    Handler(QObject *owner, std::function<void(const QUrl &)> launcher)
        : QObject(owner), launcher_(std::move(launcher))
    {
        for (const QString &scheme : {QStringLiteral("http"), QStringLiteral("https"),
                                      QStringLiteral("ms-windows-store")})
            QDesktopServices::setUrlHandler(scheme, this, "open");
    }

public slots:
    /// Copies the URL and backend so an outstanding launch never uses this QObject.
    void open(const QUrl &url)
    {
        background::start([launcher = launcher_, url] { launcher(url); });
    }

private:
    const std::function<void(const QUrl &)> launcher_;
};

void install(QObject *owner, std::function<void(const QUrl &)> launcher)
{
#ifdef Q_OS_WIN
    if (!launcher) {
        launcher = [](const QUrl &url) {
            const auto target = url.toString(QUrl::FullyEncoded).toStdWString();
            const HRESULT com = CoInitializeEx(nullptr,
                COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            SHELLEXECUTEINFOW request{};
            request.cbSize = sizeof(request);
            request.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            request.lpVerb = L"open";
            request.lpFile = target.c_str();
            request.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&request))
                qWarning() << "Could not open the system URL handler, Windows error" << GetLastError();
            if (SUCCEEDED(com))
                CoUninitialize();
        };
    }
#endif
    if (owner && launcher)
        new Handler(owner, std::move(launcher));
}

} // namespace mervin::externalopen

#include "ExternalOpen.moc"
