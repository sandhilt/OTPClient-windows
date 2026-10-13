#pragma once
#include <stdint.h>
#include <errno.h>
#include "../../hardening.h"
typedef uint64_t rlim_t;
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };
#define RLIMIT_CORE 0
#define RLIMIT_MEMLOCK 1
#define RLIM_INFINITY UINT64_MAX
/* WINDOWS-STUB: fixed 64 MiB allocation budget, not a page-lock guarantee or a
 * measured Windows limit. libgcrypt secure memory remains responsible for keys. */
static inline int getrlimit (int resource, struct rlimit *limit)
{
    if (resource != RLIMIT_MEMLOCK) { errno = ENOSYS; return -1; }
    limit->rlim_cur = limit->rlim_max = 64 * 1024 * 1024;
    return 0;
}
/* WINDOWS-STUB: CORE zero installs best-effort crash handling, not a kernel
 * dump limit. Working-set/page-lock sizing remains unsupported. */
static inline int setrlimit (int resource, const struct rlimit *limit)
{
    if (!limit) { errno = EINVAL; return -1; }
    if (resource == RLIMIT_CORE && limit->rlim_cur == 0 && limit->rlim_max == 0)
        return otp_windows_install_crash_protection ();
    errno = ENOSYS;
    return -1;
}
