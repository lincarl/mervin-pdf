# Bounded qpdf workers

Security operations, page probes and transformations, and measurement PDF reads
and writes run in a separate invocation of Mervin PDF. The internal
`--internal-qpdf-worker` mode installs OS limits before constructing
`QCoreApplication` or reading a request. It never starts `QApplication`, reads a
profile, claims the single instance, or starts the updater. Failure to install a
limit fails the operation. There is no in-process production fallback.

## Limits and compatibility

| Resource | Bound |
| --- | --- |
| Live helpers per application process | 2, including cleanup after Stop |
| Linux address space | 1 GiB, hard `RLIMIT_AS` |
| Windows committed memory | 1 GiB for the process and its Job Object |
| CPU time per helper | 60 seconds, Linux `RLIMIT_CPU`, Windows process and job user time |
| Parent watchdog | 120 seconds, excluding time in parent consent callbacks and file I/O |
| Request buffer | 32 MiB |
| IPC frame including message type | 64 KiB |
| Generated PDF bytes per operation | 1 GiB across all Split parts |
| Split outputs | 10,000 |
| Returned measurement data | 16 MiB, the existing measurement blob limit |
| Linux open descriptors | 64 |
| Linux direct file output and core dumps | Disabled with `RLIMIT_FSIZE` and `RLIMIT_CORE` |

Linux limits virtual address space, including loaded libraries. Windows Job
Objects limit committed memory, which is a different measure. Windows jobs also
limit active processes to one and terminate on the last job handle closing.
Linux helpers request termination when their parent exits. The parent discards
stderr instead of accumulating qpdf warning logs. Normal warnings still undergo
the existing qpdf warning checks inside the limited process.

Two slots let another tab save while a tool waits on a source, while limiting
aggregate helper memory to 2 GiB per application process. These limits deliberately
reject some legitimate large documents, merges with
many simultaneously open sources, and long operations. Split may already have
completed parts when it reaches a limit. Several separate application processes
each have their own helper allowance. Viewer parsing, rendering, OCR, OS file
caches, and parent-owned document snapshots are outside this qpdf budget.

This is resource containment, not a general hostile-input sandbox. It does not
remove the helper's OS file access rights or establish protection against every
library defect. It does not bound an uninterruptible kernel or network filesystem
call. A killed helper that is still stuck in such a call retains its admission
slot until it can be reaped, so repeated cancellation cannot accumulate helpers.
The GUI process is never placed in a resource-limited job or killed by this code.
Allocation exhaustion terminates the helper rather than allowing qpdf to recover
from `std::bad_alloc` by discarding document objects.

## Private transport and publication

The parent sends a bounded binary request through anonymous standard-input and
standard-output pipes. Passwords never appear in arguments, environment variables,
log messages, or a temporary request file. Each response frame requires an
acknowledgement before the helper sends more output. The parent validates frame
lengths before allocation and parses only bounded JSON metadata and byte chunks.
It does not deserialize child-controlled container lengths into application data.

The child names source and output indices, not arbitrary publication paths. The
parent accepts source indices only from its original input list. It constructs
Split output names itself from the original directory, base name and a bounded
part count. It rejects out-of-order writes, repeated plans, output after completion,
and unexpected message types. Canonical probe identities are resolved in the parent.

The parent retains destination locks, `QSaveFile`, output reservations, source and
destination validation callbacks, overwrite consent, and `job::beginCommit`.
Source-step messages preserve Reading and Writing notices and parent test hooks.
The helper finishes qpdf writing and checks deferred source failures and warnings
before declaring an output complete. Single-output operations additionally wait
for a successful result and a clean helper exit before running parent consent and
committing. A failure discards the parent's temporary output.

Split requests consent after parsing and the XFA checks, before opening any
output. It publishes complete parts sequentially. The parent records a path as
soon as its commit succeeds, including when the helper dies before receiving the
acknowledgement. Cancellation prevents every later commit, and the returned list
contains only paths the parent actually committed.

## Verification

`tst_qpdf` and the document, security, page-operation and save workflow tests run
the same executable helper used by the installed application. Test hooks for
reads, writes and commit admission run in the parent. The form-retarget fault and
Split projection counters cross the private protocol explicitly. Direct qpdf
fixture construction, warning classification and `AtomicPdfWriter` unit tests
remain algorithm-level checks and do not establish isolation.

`tst_qpdf_worker` checks process admission, resource configuration, killed helpers,
Stop while queued and running, parent event-loop responsiveness, preserved
destinations and accurate partial Split results. Windows Job Object enforcement
requires the native Windows CI test job. The same application executable ships in
the existing Linux and Windows packages, so no additional executable or runtime
inventory is needed.

AddressSanitizer reserves terabytes of virtual shadow memory before `main`, which
cannot fit under the production address-space limit. The sanitizer CI job first
builds the ordinary application from the same source revision, then supplies its
absolute path as `MERVIN_TEST_QPDF_EXECUTABLE`. Only ASan test builds accept this
setting. The parent transport and publication code remain instrumented, and the
helper still uses every production limit. This does not provide ASan coverage of
the helper's algorithms. Normal and packaged builds have no helper override.

OS reference material:

- [Linux getrlimit](https://man7.org/linux/man-pages/man2/getrlimit.2.html)
- [Windows extended job limits](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_extended_limit_information)
- [Windows basic job limits](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information)
