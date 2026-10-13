/* Deliberate crash with no secrets. Build only with OTP_WINDOWS_SHIM_CHECKS;
 * never execute as part of a build or the ordinary shim regression suite. */
#include "compat/sys/prctl.h"
#include "compat/sys/resource.h"
#include <stdint.h>
#include <stdio.h>

int
main (void)
{
    struct rlimit limit = { 0, 0 };
    if (prctl (PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 || setrlimit (RLIMIT_CORE, &limit) != 0) {
        perror ("Cannot install best-effort crash protection");
        return 1;
    }
    puts ("Protection installed; deliberately writing to NULL. Expected exit: 0xC0000005.");
    fflush (stdout);
    volatile uintptr_t address = 0;
    *(volatile unsigned char *) address = 1;
    return 2;
}
