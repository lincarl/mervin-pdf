#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <msi.h>
#include <msiquery.h>
#include <tlhelp32.h>

#include "../../src/platform/InstallerCloseProtocol.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(_MSC_VER) && defined(_M_IX86)
#pragma comment(linker, "/EXPORT:CloseMervin=_CloseMervin@4")
#endif

namespace {

using mervin::installerclose::Command;
using mervin::installerclose::Reply;

constexpr DWORD messageTimeoutMs = 1000;
constexpr ULONGLONG actionTimeoutMs = 5 * 60 * 1000;
constexpr ULONGLONG startupGraceMs = 3000;
constexpr ULONGLONG scanIntervalMs = 500;
constexpr ULONGLONG cancellationTimeoutMs = 5000;

// A process handle remains open until its exit has been observed or we cancel.
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) noexcept
        : value_(value == INVALID_HANDLE_VALUE ? nullptr : value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(Handle &&other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle &operator=(Handle &&other) noexcept
    {
        std::swap(value_, other.value_);
        return *this;
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    HANDLE value_;
};

// Only records created here are owned. Windows Installer owns the session handle.
class Record {
public:
    explicit Record(UINT fields) : value_(MsiCreateRecord(fields)) {}
    ~Record() { if (value_) MsiCloseHandle(value_); }
    Record(const Record &) = delete;
    Record &operator=(const Record &) = delete;
    MSIHANDLE get() const noexcept { return value_; }

private:
    MSIHANDLE value_;
};

struct Failure {
    const wchar_t *message;
    DWORD error;
};

/** Cancels installation when safe, cooperative closure cannot be established. */
[[noreturn]] void fail(const wchar_t *message, DWORD error = GetLastError())
{
    throw Failure{message, error};
}

/** Writes diagnostics without opening another dialog or exposing document paths. */
void log(MSIHANDLE install, const wchar_t *message, DWORD error = ERROR_SUCCESS) noexcept
{
    Record record(2);
    if (record.get()) {
        MsiRecordSetStringW(record.get(), 0, L"CloseMervin: [1] (Win32 code [2])");
        MsiRecordSetStringW(record.get(), 1, message);
        MsiRecordSetInteger(record.get(), 2, static_cast<int>(error));
        MsiProcessMessage(install, INSTALLMESSAGE_INFO, record.get());
    }
}

/** Reads a property without assuming a fixed installation-path length. */
std::wstring property(MSIHANDLE install, const wchar_t *name)
{
    std::wstring value(1, L'\0');
    for (;;) {
        DWORD length = static_cast<DWORD>(value.size());
        const UINT result = MsiGetPropertyW(install, name, value.data(), &length);
        if (result == ERROR_SUCCESS) {
            value.resize(length);
            return value;
        }
        if (result != ERROR_MORE_DATA)
            fail(L"Could not read an installer property.", result);
        value.resize(static_cast<size_t>(length) + 1);
    }
}

/** Makes an absolute extended-length path before opening files or directories. */
std::wstring fullPath(const std::wstring &path)
{
    if (path.starts_with(L"\\\\?\\"))
        return path;
    std::wstring full(512, L'\0');
    for (;;) {
        const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()),
                                              full.data(), nullptr);
        if (!length)
            fail(L"Could not resolve an executable path.");
        if (length < full.size()) {
            full.resize(length);
            break;
        }
        full.resize(static_cast<size_t>(length) + 1);
    }
    if (full.starts_with(L"\\\\"))
        return L"\\\\?\\UNC\\" + full.substr(2);
    return L"\\\\?\\" + full;
}

/** Resolves junctions, symbolic links and short names using a file handle. */
std::wstring canonicalPath(const std::wstring &path, DWORD &error)
{
    const Handle file(CreateFileW(path.c_str(), 0,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (!file) {
        error = GetLastError();
        return {};
    }
    std::wstring result(512, L'\0');
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(file.get(), result.data(),
                                                       static_cast<DWORD>(result.size()),
                                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (!length) {
            error = GetLastError();
            return {};
        }
        if (length < result.size()) {
            result.resize(length);
            error = ERROR_SUCCESS;
            return result;
        }
        result.resize(static_cast<size_t>(length) + 1);
    }
}

/** Compares canonical Windows paths without locale-dependent case folding. */
bool samePath(const std::wstring &left, const std::wstring &right) noexcept
{
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
}

/** Captures hidden and visible top-level windows without unwinding through Win32. */
std::vector<HWND> windowsFor(DWORD processId)
{
    struct Enumeration {
        DWORD processId;
        std::vector<HWND> windows;
        std::exception_ptr exception;
    } enumeration{processId, {}, {}};
    SetLastError(ERROR_SUCCESS);
    const BOOL result = EnumWindows([](HWND window, LPARAM parameter) -> BOOL {
        auto &state = *reinterpret_cast<Enumeration *>(parameter);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner == state.processId) {
            try {
                state.windows.push_back(window);
            } catch (...) {
                state.exception = std::current_exception();
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&enumeration));
    if (enumeration.exception)
        std::rethrow_exception(enumeration.exception);
    if (!result)
        fail(L"Could not enumerate application windows.");
    return std::move(enumeration.windows);
}

struct Process {
    DWORD id;
    Handle handle;
    ULONGLONG discovered;
    std::uintptr_t token = 0;
    HWND endpoint = nullptr;
    bool closing = false;
    bool legacy = false;
};

/** Runs the protocol, retaining enough state to cancel even a timed-out Begin. */
class CloseAction {
public:
    explicit CloseAction(MSIHANDLE install)
        : install_(install), deadline_(GetTickCount64() + actionTimeoutMs), progress_(3) {}
    ~CloseAction() { cancelPending(); }

    /** Waits for supported processes to exit and leaves legacy handling to MSI. */
    void run()
    {
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_))
            fail(L"Could not identify the installer's Windows session.");
        message_ = RegisterWindowMessageW(mervin::installerclose::messageName);
        if (!message_)
            fail(L"Could not register the application close protocol.");
        const auto directory = property(install_, L"INSTALLFOLDER");
        if (directory.empty())
            fail(L"The installation directory has not been resolved.", ERROR_INVALID_DATA);
        addTarget(directory);
        addTarget(property(install_, L"PREVIOUSINSTALLFOLDER"));
        if (targets_.empty())
            return;
        if (!progress_.get()
            || MsiRecordSetInteger(progress_.get(), 1, 2) != ERROR_SUCCESS
            || MsiRecordSetInteger(progress_.get(), 2, 0) != ERROR_SUCCESS
            || MsiRecordSetInteger(progress_.get(), 3, 0) != ERROR_SUCCESS)
            fail(L"Could not initialize installer cancellation checks.", ERROR_NOT_ENOUGH_MEMORY);

        ULONGLONG nextScan = 0;
        ULONGLONG quietSince = 0;
        for (;;) {
            checkpoint();
            const ULONGLONG now = GetTickCount64();
            bool scanned = false;
            if (now >= nextScan) {
                discover();
                nextScan = GetTickCount64() + scanIntervalMs;
                scanned = true;
            }
            bool waiting = false;
            for (auto &process : processes_) {
                checkpoint();
                advance(process);
                if (!exited(process) && !process.legacy)
                    waiting = true;
            }
            if (waiting) {
                quietSince = 0;
            } else if (!quietSince) {
                quietSince = GetTickCount64();
            } else if (scanned && GetTickCount64() - quietSince >= scanIntervalMs) {
                // A fresh snapshot must agree before InstallValidate takes over.
                discover();
                if (std::any_of(processes_.begin(), processes_.end(), [this](const Process &process) {
                        return !exited(process) && !process.legacy;
                    })) {
                    quietSince = 0;
                    continue;
                }
                checkpoint();
                return;
            }
            Sleep(100);
        }
    }

private:
    /** Allows MSI to process Cancel without moving or resetting its progress bar. */
    void checkpoint()
    {
        if (GetTickCount64() >= deadline_)
            fail(L"Timed out waiting for the application to exit.", ERROR_TIMEOUT);
        const int result = MsiProcessMessage(install_, INSTALLMESSAGE_PROGRESS, progress_.get());
        if (result == IDCANCEL || result == IDABORT || result == -1)
            fail(L"Installation was canceled while waiting for the application.", ERROR_CANCELLED);
    }

    /** Missing files are normal for a first installation or a changed destination. */
    void addTarget(std::wstring directory)
    {
        if (directory.empty())
            return;
        if (directory.back() != L'\\' && directory.back() != L'/')
            directory += L'\\';
        DWORD error = ERROR_SUCCESS;
        auto path = canonicalPath(fullPath(directory + L"MervinPDF.exe"), error);
        if (path.empty()) {
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                return;
            fail(L"Could not resolve the installed application path.", error);
        }
        if (std::none_of(targets_.begin(), targets_.end(), [&path](const auto &target) {
                return samePath(path, target);
            }))
            targets_.push_back(std::move(path));
    }

    /** Process termination, rather than a protocol reply, authorizes replacement. */
    bool exited(const Process &process) const
    {
        const DWORD result = WaitForSingleObject(process.handle.get(), 0);
        if (result == WAIT_OBJECT_0)
            return true;
        if (result != WAIT_TIMEOUT)
            fail(L"Could not check whether the application exited.");
        return false;
    }

    /** Protected unrelated processes stay under Windows Installer's files-in-use check. */
    void unknownProcess()
    {
        if (!loggedUnknown_) {
            log(install_, L"Some process identities were unavailable. MSI files-in-use checks remain active.");
            loggedUnknown_ = true;
        }
    }

    /** Matches full canonical image paths, never process names from the snapshot. */
    void discover()
    {
        const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot)
            fail(L"Could not enumerate running processes.");
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.get(), &entry))
            fail(L"Could not read the running process snapshot.");
        do {
            checkpoint();
            DWORD session = 0;
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != session_)
                continue;
            if (std::any_of(processes_.begin(), processes_.end(), [&](const Process &process) {
                    return process.id == entry.th32ProcessID && !exited(process);
                }))
                continue;

            Handle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                                      FALSE, entry.th32ProcessID));
            const bool canWait = static_cast<bool>(handle);
            if (!handle)
                handle = Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!handle) {
                unknownProcess();
                continue;
            }
            // The snapshot's PID could have been reused before OpenProcess.
            if (!ProcessIdToSessionId(entry.th32ProcessID, &session) || session != session_)
                continue;
            if (canWait && WaitForSingleObject(handle.get(), 0) == WAIT_OBJECT_0)
                continue;
            std::wstring image(512, L'\0');
            for (;;) {
                DWORD length = static_cast<DWORD>(image.size());
                if (QueryFullProcessImageNameW(handle.get(), 0, image.data(), &length)) {
                    image.resize(length);
                    break;
                }
                if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || image.size() >= 32768) {
                    image.clear();
                    break;
                }
                image.resize(image.size() * 2);
            }
            if (image.empty()) {
                unknownProcess();
                continue;
            }
            DWORD error = ERROR_SUCCESS;
            const auto canonical = canonicalPath(fullPath(image), error);
            if (canonical.empty()) {
                unknownProcess();
                continue;
            }
            if (std::none_of(targets_.begin(), targets_.end(), [&](const auto &target) {
                    return samePath(canonical, target);
                }))
                continue;
            if (!canWait)
                fail(L"Cannot obtain an exit-wait handle for the installed application.", ERROR_ACCESS_DENIED);
            processes_.push_back(Process{entry.th32ProcessID, std::move(handle), GetTickCount64()});
        } while (Process32NextW(snapshot.get(), &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES)
            fail(L"The running process snapshot could not be completed.");
    }

    /** Checks ownership again because native windows can disappear or be recycled. */
    static bool ownsWindow(const Process &process, HWND window) noexcept
    {
        DWORD owner = 0;
        return GetWindowThreadProcessId(window, &owner) != 0 && owner == process.id;
    }

    /** A destroyed window is normal during Quit. A live unresponsive window is not. */
    bool send(Process &process, HWND window, Command command, Reply &reply, DWORD &error)
    {
        error = ERROR_SUCCESS;
        checkpoint();
        if (exited(process) || !ownsWindow(process, window))
            return false;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline_)
            fail(L"Timed out waiting for the application to exit.", ERROR_TIMEOUT);
        const auto timeout = static_cast<UINT>(std::min<ULONGLONG>(messageTimeoutMs, deadline_ - now));
        DWORD_PTR result = 0;
        SetLastError(ERROR_SUCCESS);
        const LRESULT sent = SendMessageTimeoutW(window, message_, static_cast<WPARAM>(command),
                                                 command == Command::Probe ? 0 : static_cast<LPARAM>(process.token),
                                                 SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                                                 timeout, &result);
        const DWORD sendError = GetLastError();
        if (!sent) {
            if (exited(process) || !ownsWindow(process, window))
                return false;
            error = sendError ? sendError : ERROR_TIMEOUT;
            return false;
        }
        reply = static_cast<Reply>(result);
        return true;
    }

    /** Random tokens separate this request from other installers and earlier runs. */
    std::uintptr_t newToken() const
    {
        std::uintptr_t token = 0;
        do {
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&token), sizeof(token),
                                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                fail(L"Could not create an application close request token.", ERROR_GEN_FAILURE);
            // Keep LPARAM positive, including when MSI and the app differ in bitness.
            token &= static_cast<std::uintptr_t>(std::numeric_limits<LONG>::max());
        } while (!token || std::any_of(processes_.begin(), processes_.end(), [token](const Process &process) {
            return process.token == token;
        }));
        return token;
    }

    /** Uses any protocol-capable window for the one process-wide close request. */
    void advance(Process &process)
    {
        if (exited(process) || process.closing)
            return;
        auto windows = windowsFor(process.id);
        const auto endpoint = std::find(windows.begin(), windows.end(), process.endpoint);
        if (endpoint != windows.end())
            std::iter_swap(windows.begin(), endpoint);
        bool unsupported = false;
        DWORD probeError = ERROR_SUCCESS;
        for (const HWND window : windows) {
            Reply reply = Reply::Unsupported;
            DWORD error = ERROR_SUCCESS;
            if (!send(process, window, Command::Probe, reply, error)) {
                if (error != ERROR_SUCCESS)
                    probeError = error;
                continue;
            }
            if (reply == Reply::Unsupported) {
                unsupported = true;
                continue;
            }
            if (reply != Reply::Ready) {
                probeError = ERROR_INVALID_DATA;
                continue;
            }
            process.endpoint = window;
            process.legacy = false;
            Command command = Command::Poll;
            if (!process.token) {
                checkpoint();
                process.token = newToken();
                // Save the token before Begin, which may be delivered even if its send times out.
                if (!AllowSetForegroundWindow(process.id))
                    log(install_, L"Windows did not grant foreground permission to the application.", GetLastError());
                command = Command::Begin;
            }
            if (!send(process, window, command, reply, error)) {
                if (error != ERROR_SUCCESS)
                    fail(L"The application close protocol did not respond.", error);
                return;
            }
            if (exited(process))
                return;
            if (reply == Reply::Closing) {
                process.closing = true;
                return;
            }
            if (reply == Reply::Pending)
                return;
            if (reply == Reply::Declined || reply == Reply::Busy)
                fail(L"The application declined the installer close request or is busy.", ERROR_CANCELLED);
            fail(L"The application returned an invalid close protocol reply.", ERROR_INVALID_DATA);
        }
        if (probeError != ERROR_SUCCESS && !exited(process))
            fail(L"No application window answered the protocol probe reliably.", probeError);
        // No windows can mean startup or final shutdown. Neither proves process exit.
        // Give newly initialized windows time to install their protocol handler.
        process.legacy = !process.token && unsupported
            && GetTickCount64() - process.discovered >= startupGraceMs;
        if (process.legacy && !loggedLegacy_) {
            log(install_, L"An application does not support cooperative close. MSI files-in-use checks remain active.");
            loggedLegacy_ = true;
        }
    }

    /** Best-effort cancellation also queues Cancel if a receiver is temporarily hung. */
    void cancelPending() noexcept
    {
        struct Cancellation {
            Process *process;
            UINT message;
            ULONGLONG deadline;
            bool acknowledged = false;
        };
        const ULONGLONG deadline = std::min(GetTickCount64() + cancellationTimeoutMs, deadline_);
        for (auto &process : processes_) {
            if (!process.token || WaitForSingleObject(process.handle.get(), 0) == WAIT_OBJECT_0)
                continue;
            Cancellation cancellation{&process, message_, deadline};
            const auto cancelWindow = [](HWND window, LPARAM parameter) -> BOOL {
                auto &state = *reinterpret_cast<Cancellation *>(parameter);
                if (state.acknowledged || !ownsWindow(*state.process, window)
                    || WaitForSingleObject(state.process->handle.get(), 0) == WAIT_OBJECT_0)
                    return TRUE;
                DWORD_PTR reply = 0;
                const ULONGLONG now = GetTickCount64();
                const auto timeout = static_cast<UINT>(std::min<ULONGLONG>(
                    messageTimeoutMs, now < state.deadline ? state.deadline - now : 0));
                if (timeout && SendMessageTimeoutW(window, state.message, static_cast<WPARAM>(Command::Cancel),
                                                   static_cast<LPARAM>(state.process->token),
                                                   SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                                                   timeout, &reply)) {
                    const auto result = static_cast<Reply>(reply);
                    if (result == Reply::Declined || result == Reply::Busy || result == Reply::Closing)
                        state.acknowledged = true;
                } else if (ownsWindow(*state.process, window)
                           && WaitForSingleObject(state.process->handle.get(), 0) != WAIT_OBJECT_0) {
                    // A timed-out Begin can still be delivered when the UI thread resumes.
                    PostMessageW(window, state.message, static_cast<WPARAM>(Command::Cancel),
                                 static_cast<LPARAM>(state.process->token));
                }
                return TRUE;
            };
            if (process.endpoint)
                cancelWindow(process.endpoint, reinterpret_cast<LPARAM>(&cancellation));
            const BOOL enumerated = EnumWindows(cancelWindow, reinterpret_cast<LPARAM>(&cancellation));
            if ((!enumerated || !cancellation.acknowledged)
                && WaitForSingleObject(process.handle.get(), 0) != WAIT_OBJECT_0)
                log(install_, L"Cancellation of a pending close request could not be confirmed.");
        }
    }

    MSIHANDLE install_;
    ULONGLONG deadline_;
    Record progress_;
    DWORD session_ = 0;
    UINT message_ = 0;
    std::vector<std::wstring> targets_;
    std::vector<Process> processes_;
    bool loggedUnknown_ = false;
    bool loggedLegacy_ = false;
};

} // namespace

/** Immediate full-UI action. No exception may unwind into Windows Installer. */
extern "C" __declspec(dllexport) UINT __stdcall CloseMervin(MSIHANDLE install)
{
    try {
        if (property(install, L"UILevel") != L"5"
            || !property(install, L"UPGRADINGPRODUCTCODE").empty())
            return ERROR_SUCCESS;
        CloseAction action(install);
        action.run();
        return ERROR_SUCCESS;
    } catch (const Failure &failure) {
        log(install, failure.message, failure.error);
        return ERROR_INSTALL_USEREXIT;
    } catch (const std::exception &) {
        log(install, L"An unexpected C++ exception prevented safe application closure.", ERROR_INSTALL_FAILURE);
        return ERROR_INSTALL_FAILURE;
    } catch (...) {
        log(install, L"An unexpected exception prevented safe application closure.", ERROR_INSTALL_FAILURE);
        return ERROR_INSTALL_FAILURE;
    }
}
