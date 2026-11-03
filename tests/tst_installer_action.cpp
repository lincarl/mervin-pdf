#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <msi.h>
#include <msiquery.h>

#include "platform/InstallerCloseProtocol.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

extern "C" UINT __stdcall CloseMervin(MSIHANDLE install);
using namespace mervin::installerclose;
namespace fs = std::filesystem;

namespace {

// Assertions fail the native integration executable, without installing anything.
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::wstring childMode;
UINT closeMessage = 0;
HANDLE began = nullptr;
HANDLE cancelled = nullptr;
HANDLE cancelWhenBegan = nullptr;
std::uintptr_t request = 0;

// Models cooperative, refusing and legacy application processes for the real MSI action.
LRESULT CALLBACK childWindow(HWND window, UINT message, WPARAM parameter, LPARAM token)
{
    if (message == closeMessage && childMode != L"legacy") {
        const auto command = static_cast<Command>(parameter);
        if (command == Command::Probe)
            return static_cast<LRESULT>(Reply::Ready);
        if (command == Command::Begin) {
            request = static_cast<std::uintptr_t>(token);
            SetEvent(began);
            if (childMode == L"close")
                SetTimer(window, 1, 300, nullptr);
        }
        if (static_cast<std::uintptr_t>(token) != request)
            return static_cast<LRESULT>(Reply::Busy);
        if (command == Command::Cancel) {
            KillTimer(window, 1);
            SetEvent(cancelled);
            return static_cast<LRESULT>(Reply::Declined);
        }
        return static_cast<LRESULT>(childMode == L"decline" ? Reply::Declined
                                    : childMode == L"close" ? Reply::Closing : Reply::Pending);
    }
    if (message == WM_TIMER) {
        DestroyWindow(window);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, parameter, token);
}

// Runs a real top-level window in its own process, optionally hidden like a tray window.
int runChild(int argc, wchar_t **argv)
{
    require(argc == 7, "Invalid child arguments");
    childMode = argv[2];
    const auto handle = [](const wchar_t *value) {
        return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(value)));
    };
    HANDLE ready = handle(argv[3]);
    began = handle(argv[4]);
    cancelled = handle(argv[5]);
    closeMessage = RegisterWindowMessageW(messageName);
    require(closeMessage != 0, "RegisterWindowMessage failed");
    WNDCLASSW type{};
    type.lpfnWndProc = childWindow;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"MervinInstallerTest";
    require(RegisterClassW(&type) != 0, "RegisterClass failed");
    HWND window = CreateWindowW(type.lpszClassName, L"Installer test copy", WS_OVERLAPPEDWINDOW,
                                0, 0, 300, 200, nullptr, nullptr, type.hInstance, nullptr);
    require(window != nullptr, "CreateWindow failed");
    if (std::wstring(argv[6]) == L"visible")
        ShowWindow(window, SW_SHOW);
    SetEvent(ready);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

// Owns only a test-started process and its inherited synchronization events.
class Child {
public:
    Child(const fs::path &source, const fs::path &directory, const wchar_t *mode, bool visible = false)
    {
        try {
            fs::create_directories(directory);
            const fs::path executable = directory / L"MervinPDF.exe";
            if (!fs::exists(executable))
                fs::copy_file(source, executable);
            SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
            ready = CreateEventW(&security, TRUE, FALSE, nullptr);
            begin = CreateEventW(&security, TRUE, FALSE, nullptr);
            cancel = CreateEventW(&security, TRUE, FALSE, nullptr);
            require(ready && begin && cancel, "CreateEvent failed");
            const auto number = [](HANDLE value) {
                return std::to_wstring(reinterpret_cast<std::uintptr_t>(value));
            };
            std::wstring command = L"\"" + executable.wstring() + L"\" --child " + mode
                + L" " + number(ready) + L" " + number(begin) + L" " + number(cancel)
                + (visible ? L" visible" : L" hidden");
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, 0,
                                   nullptr, directory.c_str(), &startup, &process), "CreateProcess failed");
            require(WaitForSingleObject(ready, 15000) == WAIT_OBJECT_0, "Child did not create its window");
        } catch (...) {
            cleanup();
            throw;
        }
    }
    ~Child() { cleanup(); }
    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    bool exited() const { return WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0; }
    HANDLE begin = nullptr;
    HANDLE cancel = nullptr;
private:
    // Runs on construction failure too, so a failing test cannot leave fixture processes.
    void cleanup() noexcept
    {
        // Fixtures left running by refusal, cancellation or legacy fallback are disposable.
        if (process.hProcess && WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 99);
            WaitForSingleObject(process.hProcess, 10000);
        }
        for (HANDLE handle : {process.hThread, process.hProcess, ready, begin, cancel})
            if (handle)
                CloseHandle(handle);
    }
    HANDLE ready = nullptr;
    PROCESS_INFORMATION process{};
};

// MSI session handles exercise property access and cancellation without executing an install sequence.
class Session {
public:
    Session(const wchar_t *package, const fs::path &directory, const fs::path &previous = {},
            const wchar_t *ui = L"5")
    {
        try {
            require(MsiOpenPackageExW(package, MSIOPENPACKAGEFLAGS_IGNOREMACHINESTATE, &handle) == ERROR_SUCCESS,
                    "MsiOpenPackageEx failed");
            set(L"UILevel", ui);
            set(L"INSTALLFOLDER", directory.c_str());
            set(L"PREVIOUSINSTALLFOLDER", previous.c_str());
            set(L"UPGRADINGPRODUCTCODE", L"");
        } catch (...) {
            if (handle)
                MsiCloseHandle(handle);
            throw;
        }
    }
    ~Session() { if (handle) MsiCloseHandle(handle); }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    void set(const wchar_t *name, const wchar_t *value)
    {
        require(MsiSetPropertyW(handle, name, value) == ERROR_SUCCESS, "MsiSetProperty failed");
    }
    MSIHANDLE handle = 0;
};

// Simulates clicking installer Cancel only after the target accepted Begin.
int CALLBACK installerMessages(void *, UINT type, LPCWSTR)
{
    if ((type & 0xff000000) == INSTALLMESSAGE_PROGRESS && cancelWhenBegan
        && WaitForSingleObject(cancelWhenBegan, 0) == WAIT_OBJECT_0)
        return IDCANCEL;
    return 0;
}

// Verifies real process messaging, path isolation and the MSI cancellation return value.
void runTests(const wchar_t *package, const fs::path &output)
{
    wchar_t image[32768]{};
    require(GetModuleFileNameW(nullptr, image, 32768) != 0, "GetModuleFileName failed");
    const fs::path source(image);
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    MsiSetExternalUIW(installerMessages, INSTALLLOGMODE_PROGRESS, nullptr);
    {
        const auto target = output / L"multiple" / L"installed";
        Child visible(source, target, L"close", true);
        Child hidden(source, target, L"close");
        Child portable(source, output / L"multiple" / L"portable", L"close");
        Session session(package, target);
        require(CloseMervin(session.handle) == ERROR_SUCCESS, "Cooperative closure failed");
        require(visible.exited() && hidden.exited(), "The action returned before installed processes exited");
        require(!portable.exited(), "The action closed an unrelated portable copy");
        std::puts("PASS visible and hidden copies closed, separate portable copy preserved");
    }
    {
        const auto target = output / L"declined";
        Child child(source, target, L"decline");
        Session session(package, target);
        require(CloseMervin(session.handle) == ERROR_INSTALL_USEREXIT, "Application refusal did not cancel MSI");
        require(!child.exited(), "Application refusal terminated the process");
        std::puts("PASS application refusal cancels MSI and preserves the process");
    }
    {
        const auto target = output / L"cancelled";
        Child child(source, target, L"pending");
        Session session(package, target);
        cancelWhenBegan = child.begin;
        const UINT result = CloseMervin(session.handle);
        cancelWhenBegan = nullptr;
        require(result == ERROR_INSTALL_USEREXIT, "Installer Cancel did not stop the action");
        require(WaitForSingleObject(child.cancel, 5000) == WAIT_OBJECT_0, "Installer Cancel did not cancel the pending Quit");
        require(!child.exited(), "Installer Cancel terminated the process");
        std::puts("PASS installer cancellation reaches the pending application request");
    }
    {
        const auto target = output / L"legacy";
        Child child(source, target, L"legacy");
        Session session(package, target);
        require(CloseMervin(session.handle) == ERROR_SUCCESS, "Legacy fallback failed");
        require(!child.exited(), "Legacy fallback terminated the process");
        require(WaitForSingleObject(child.begin, 0) == WAIT_TIMEOUT, "Legacy fallback sent Begin");
        std::puts("PASS unsupported legacy copies remain for MSI files-in-use handling");
    }
    {
        const auto previous = output / L"previous" / L"installed";
        Child child(source, previous, L"close");
        Session session(package, output / L"previous" / L"new destination", previous);
        require(CloseMervin(session.handle) == ERROR_SUCCESS && child.exited(), "Previous installation was not closed");
        std::puts("PASS changed destination still closes the previous installation");
    }
    {
        const auto target = output / L"passive";
        Child child(source, target, L"close");
        Session session(package, target, {}, L"3");
        require(CloseMervin(session.handle) == ERROR_SUCCESS && !child.exited(), "Passive installer ran interactive closure");
        session.set(L"UILevel", L"5");
        session.set(L"UPGRADINGPRODUCTCODE", L"{11111111-1111-1111-1111-111111111111}");
        require(CloseMervin(session.handle) == ERROR_SUCCESS && !child.exited(), "Nested upgrade removal ran closure");
        std::puts("PASS passive installation and nested old-product removal skip the action");
    }
}
} // namespace

// Runs against a built MSI on the disposable installer-test account, or as a fixture child.
int wmain(int argc, wchar_t **argv)
{
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"--child")
            return runChild(argc, argv);
        require(argc == 3, "Usage: tst_installer_action.exe package.msi output-directory");
        runTests(argv[1], fs::path(argv[2]));
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s (Win32 error %lu)\n", error.what(), GetLastError());
        return 1;
    }
}
