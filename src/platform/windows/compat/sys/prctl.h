#pragma once
#include <errno.h>
#include "../../hardening.h"
#define PR_SET_DUMPABLE 4
/* WINDOWS-STUB: success means the best-effort process handler is installed,
 * not Linux PR_SET_DUMPABLE equivalence or a prohibition on external dumps. */
static inline int prctl (int option, unsigned long dumpable, ...)
{
    if (option != PR_SET_DUMPABLE || dumpable != 0) { errno = ENOSYS; return -1; }
    return otp_windows_install_crash_protection ();
}
