#pragma once
#include "otp-windows.h"

G_BEGIN_DECLS
/* WINDOWS-STUB: Windows has no POSIX uid. This placeholder is only for the
 * upstream document-portal diagnostic; it must never identify an ACL owner. */
guint otp_windows_getuid (void);
#ifndef OTP_WINDOWS_COMPAT_IMPLEMENTATION
#define getuid otp_windows_getuid
#endif
G_END_DECLS
