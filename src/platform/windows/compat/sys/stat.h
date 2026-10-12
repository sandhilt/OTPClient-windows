#pragma once
#include_next <sys/stat.h>
/* Match GLib's Windows GStatBuf and retain 64-bit file sizes. */
#define stat _stat64
#define fstat _fstat64
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
