#include "hardening.h"
#include <windows.h>
#include <errno.h>

static LONG WINAPI
terminate_on_exception (EXCEPTION_POINTERS *exception)
{
    DWORD code = exception && exception->ExceptionRecord ?
        exception->ExceptionRecord->ExceptionCode : EXCEPTION_NONCONTINUABLE_EXCEPTION;
    /* Do not allocate, log, unwind, chain the previous filter or write a dump
     * from a potentially corrupted process. Successful self-termination does
     * not return. Let Windows terminate if that call unexpectedly fails. */
    TerminateProcess (GetCurrentProcess (), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

int
otp_windows_install_crash_protection (void)
{
    const UINT flags = SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX;
    SetErrorMode (GetErrorMode () | flags);
    if ((GetErrorMode () & flags) != flags) {
        errno = EIO;
        return -1;
    }
    /* NULL from SetUnhandledExceptionFilter is the previous filter, not an
     * error. Installing the same filter again lets us check its registration.
     * Reinstall on each request rather than caching an earlier success. */
    SetUnhandledExceptionFilter (terminate_on_exception);
    if (SetUnhandledExceptionFilter (terminate_on_exception) != terminate_on_exception) {
        errno = EIO;
        return -1;
    }
    /* WINDOWS-STUB: another library can replace the process filter/mode later;
     * debugger interception, fail-fast/invalid-stack paths and external dumps
     * (including independently configured WER LocalDumps) are not prevented. */
    return 0;
}
