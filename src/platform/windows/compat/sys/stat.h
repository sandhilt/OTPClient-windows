#pragma once
#include_next <sys/stat.h>
/* Match the struct/function pair selected by GLib's gstdio.h. */
#if defined(_WIN64)
#define stat _stat64
#define OTP_CRT_FSTAT _fstat64
#else
#define stat _stat32
#define OTP_CRT_FSTAT _fstat32
#endif
int otp_fstat (int fd, struct stat *state);
#define fstat otp_fstat
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & _S_IFMT) == _S_IFREG)
#endif
