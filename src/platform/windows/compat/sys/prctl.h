#pragma once
#include <errno.h>
#define PR_SET_DUMPABLE 4
/* WINDOWS-STUB: dump prevention is unavailable. Return failure so the existing
 * caller warns rather than claiming that crash dumps have been disabled. */
static inline int prctl (int option, ...)
{
    (void) option;
    errno = ENOSYS;
    return -1;
}
