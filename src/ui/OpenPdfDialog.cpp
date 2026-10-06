#include "ui/OpenPdfDialog.h"

#include "net/UrlOpen.h"

#ifdef Q_OS_WIN

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>

#include <QCoreApplication>
#include <QDialog>
#include <QFileInfo>
#include <QWidget>

#include <atomic>
#include <iterator>
#include <string>

namespace mervin {
namespace {

constexpr UINT_PTR kDialogSubclassId = 0x4d657276;
constexpr UINT_PTR kOpenButtonSubclassId = 0x4d657277;

class DialogEvents;

LRESULT CALLBACK dialogSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR subclassId, DWORD_PTR referenceData);
LRESULT CALLBACK openButtonSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR subclassId, DWORD_PTR referenceData);

QString translated(const char *text)
{
    return QCoreApplication::translate("OpenPdfDialog", text);
}

class DialogEvents final : public IFileDialogEvents
{
public:
    explicit DialogEvents(QString *internetUrl)
        : internetUrl_(internetUrl)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
    {
        if (!object)
            return E_POINTER;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) {
            *object = static_cast<IFileDialogEvents *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (remaining == 0)
            delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *dialog) override
    {
        captureUrl(dialog);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *dialog, IShellItem *) override
    {
        attachToWindow(dialog);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *dialog) override
    {
        attachToWindow(dialog);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *dialog) override
    {
        attachToWindow(dialog);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog *, IShellItem *,
                                               FDE_SHAREVIOLATION_RESPONSE *response) override
    {
        if (response)
            *response = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog *, IShellItem *,
                                          FDE_OVERWRITE_RESPONSE *response) override
    {
        if (response)
            *response = FDEOR_DEFAULT;
        return S_OK;
    }

    bool captureUrl(IFileDialog *dialog)
    {
        locateFileNameEdit();
        if (!dialog)
            dialog = dialog_;
        QString text;
        if (fileNameEdit_ && IsWindow(fileNameEdit_)) {
            const int length = GetWindowTextLengthW(fileNameEdit_);
            if (length > 0) {
                std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
                GetWindowTextW(fileNameEdit_, buffer.data(), length + 1);
                text = QString::fromWCharArray(buffer.c_str());
            }
        }
        if (text.isEmpty() && dialog) {
            PWSTR entered = nullptr;
            if (SUCCEEDED(dialog->GetFileName(&entered)) && entered) {
                text = QString::fromWCharArray(entered);
                CoTaskMemFree(entered);
            }
        }
        if (const auto url = urlopen::fromUserInput(text)) {
            *internetUrl_ = url->toString();
            return true;
        }
        return false;
    }

    void detachFromWindow()
    {
        if (openButton_) {
            RemoveWindowSubclass(openButton_, openButtonSubclassProc, kOpenButtonSubclassId);
            openButton_ = nullptr;
        }
        fileNameEdit_ = nullptr;
        if (window_) {
            RemoveWindowSubclass(window_, dialogSubclassProc, kDialogSubclassId);
            window_ = nullptr;
        }
        dialog_ = nullptr;
    }

    void openButtonDestroyed(HWND button)
    {
        if (openButton_ == button)
            openButton_ = nullptr;
    }

    void queueCloseForCapturedUrl()
    {
        if (window_)
            PostMessageW(window_, WM_CLOSE, 0, 0);
    }

private:
    void locateFileNameEdit()
    {
        if ((fileNameEdit_ && IsWindow(fileNameEdit_)) || !window_)
            return;
        HWND fileNameCombo = GetDlgItem(window_, 1148);
        if (!fileNameCombo)
            return;
        HWND innerCombo = FindWindowExW(fileNameCombo, nullptr, L"ComboBox", nullptr);
        if (innerCombo)
            fileNameEdit_ = FindWindowExW(innerCombo, nullptr, L"Edit", nullptr);
    }

    void attachToWindow(IFileDialog *dialog)
    {
        if (window_) {
            locateFileNameEdit();
            return;
        }
        IOleWindow *oleWindow = nullptr;
        if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&oleWindow))) || !oleWindow)
            return;
        HWND window = nullptr;
        const HRESULT result = oleWindow->GetWindow(&window);
        oleWindow->Release();
        if (FAILED(result) || !window)
            return;
        if (SetWindowSubclass(window, dialogSubclassProc, kDialogSubclassId,
                              reinterpret_cast<DWORD_PTR>(this))) {
            window_ = window;
            dialog_ = dialog;
            openButton_ = GetDlgItem(window, IDOK);
            if (openButton_
                && !SetWindowSubclass(openButton_, openButtonSubclassProc,
                                      kOpenButtonSubclassId,
                                      reinterpret_cast<DWORD_PTR>(this))) {
                openButton_ = nullptr;
            }
            locateFileNameEdit();
        }
    }

    std::atomic<ULONG> references_{1};
    QString *internetUrl_ = nullptr;
    HWND window_ = nullptr;
    HWND openButton_ = nullptr;
    HWND fileNameEdit_ = nullptr;
    IFileDialog *dialog_ = nullptr;
};

LRESULT CALLBACK dialogSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                    UINT_PTR, DWORD_PTR referenceData)
{
    auto *events = reinterpret_cast<DialogEvents *>(referenceData);
    if (message == WM_COMMAND && LOWORD(wParam) == IDOK
        && events->captureUrl(nullptr)) {
        events->queueCloseForCapturedUrl();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        events->detachFromWindow();
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK openButtonSubclassProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR, DWORD_PTR referenceData)
{
    auto *events = reinterpret_cast<DialogEvents *>(referenceData);
    bool activate = message == BM_CLICK;
    if (message == WM_LBUTTONUP) {
        const POINT point = {static_cast<short>(LOWORD(lParam)),
                             static_cast<short>(HIWORD(lParam))};
        RECT bounds = {};
        activate = GetClientRect(window, &bounds) && PtInRect(&bounds, point);
    }
    if (activate && events->captureUrl(nullptr)) {
        events->queueCloseForCapturedUrl();
        return 0;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, openButtonSubclassProc, kOpenButtonSubclassId);
        events->openButtonDestroyed(window);
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

QStringList fileSystemPaths(IFileOpenDialog *dialog)
{
    QStringList paths;
    IShellItemArray *items = nullptr;
    if (FAILED(dialog->GetResults(&items)) || !items)
        return paths;

    DWORD count = 0;
    if (SUCCEEDED(items->GetCount(&count))) {
        for (DWORD i = 0; i < count; ++i) {
            IShellItem *item = nullptr;
            if (FAILED(items->GetItemAt(i, &item)) || !item)
                continue;
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                paths.append(QFileInfo(QString::fromWCharArray(path)).absoluteFilePath());
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    items->Release();
    return paths;
}

} // namespace

OpenPdfDialog::OpenPdfDialog(QWidget *parent)
    : parent_(parent)
{
}

int OpenPdfDialog::exec()
{
    internetUrl_.clear();
    selectedFiles_.clear();

    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        return QDialog::Rejected;

    IFileOpenDialog *dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result) || !dialog) {
        if (uninitialize)
            CoUninitialize();
        return QDialog::Rejected;
    }

    const QString title = translated("Open PDF");
    dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()));

    const QString pdfLabel = translated("PDF documents (*.pdf)");
    const QString allLabel = translated("All files (*.*)");
    const COMDLG_FILTERSPEC filters[] = {
        {reinterpret_cast<LPCWSTR>(pdfLabel.utf16()), L"*.pdf"},
        {reinterpret_cast<LPCWSTR>(allLabel.utf16()), L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"pdf");

    FILEOPENDIALOGOPTIONS options = {};
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options &= ~(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        options |= FOS_ALLOWMULTISELECT | FOS_NOVALIDATE | FOS_NOCHANGEDIR;
        dialog->SetOptions(options);
    }

    // Give Windows a stable identity for the picker's persisted folder, view,
    // and size without coupling it to dialogs from unrelated applications.
    constexpr GUID clientGuid = {0x8d58f219,
                                 0x4e94,
                                 0x48ef,
                                 {0x8f, 0x1c, 0x52, 0xb7, 0x0e, 0xe2, 0x8f, 0x31}};
    dialog->SetClientGuid(clientGuid);

    auto *events = new DialogEvents(&internetUrl_);
    DWORD eventCookie = 0;
    const bool advised = SUCCEEDED(dialog->Advise(events, &eventCookie));

    HWND owner = nullptr;
    if (parent_)
        owner = reinterpret_cast<HWND>(parent_->window()->winId());
    result = dialog->Show(owner);

    events->detachFromWindow();
    if (advised)
        dialog->Unadvise(eventCookie);
    events->Release();

    if (SUCCEEDED(result) && internetUrl_.isEmpty())
        selectedFiles_ = fileSystemPaths(dialog);

    dialog->Release();
    if (uninitialize)
        CoUninitialize();

    return !internetUrl_.isEmpty() || (SUCCEEDED(result) && !selectedFiles_.isEmpty())
               ? QDialog::Accepted
               : QDialog::Rejected;
}

} // namespace mervin

#else

#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>

namespace mervin {

OpenPdfDialog::OpenPdfDialog(QWidget *parent)
    : QFileDialog(parent, tr("Open PDF"), QString(),
                  tr("PDF documents (*.pdf);;All files (*)"))
{
    setFileMode(QFileDialog::ExistingFiles);
    setAcceptMode(QFileDialog::AcceptOpen);
    setOption(QFileDialog::DontUseNativeDialog);
    setLabelText(QFileDialog::FileName, tr("File &name or URL:"));

    auto *nameEdit = findChild<QLineEdit *>(QStringLiteral("fileNameEdit"));
    auto *buttons = findChild<QDialogButtonBox *>();
    if (nameEdit && buttons) {
        connect(nameEdit, &QLineEdit::textChanged, this, [buttons](const QString &text) {
            if (urlopen::fromUserInput(text)) {
                if (auto *open = buttons->button(QDialogButtonBox::Open))
                    open->setEnabled(true);
            }
        });
    }
}

void OpenPdfDialog::accept()
{
    if (auto *nameEdit = findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
        if (const auto url = urlopen::fromUserInput(nameEdit->text())) {
            internetUrl_ = url->toString();
            QDialog::accept();
            return;
        }
    }
    QFileDialog::accept();
}

} // namespace mervin

#endif
