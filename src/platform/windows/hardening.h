#pragma once

/* WINDOWS-STUB: installs process-local best-effort crash handling, not the
 * Linux non-dumpable security boundary. See docs/windows-build.md. */
int otp_windows_install_crash_protection (void);
