#pragma once

#include <windows.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define OTP_AUTOSTART_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define OTP_AUTOSTART_APPROVED_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"
#define OTP_AUTOSTART_TEST_ROOT L"Software\\OTPClient-test"
#define OTP_AUTOSTART_TEST_RUN_KEY OTP_AUTOSTART_TEST_ROOT L"\\Run"
#define OTP_AUTOSTART_TEST_APPROVED_KEY OTP_AUTOSTART_TEST_ROOT L"\\StartupApproved\\Run"
#define OTP_AUTOSTART_VALUE L"OTPClient"

typedef enum {
    OTP_AUTOSTART_ABSENT,
    OTP_AUTOSTART_ENABLED,
    OTP_AUTOSTART_DISABLED_BY_USER
} OtpWindowsAutostartState;

/* Pure UTF-8 -> command-line composition, returned string owned by caller.
 * Accept only full drive/UNC paths without quotes/control characters; the
 * executable is always quoted, flags are fixed, and no shell is involved.
 * Reject commands over the documented Run limit of 260 UTF-16 code units. */
gchar *otp_windows_autostart_compose (const gchar *path, gboolean minimized,
                                      GError **error);
gchar *otp_windows_autostart_executable (GError **error);

/* Keys are injectable, but only the exact production pair or exact test pair
 * is accepted. All operations use HKCU, OTPClient and KEY_WOW64_64KEY.
 * Query returns FALSE on unreadable/unknown data, never claims it is enabled.
 * command may be NULL; otherwise receives a newly allocated UTF-8 REG_SZ.
 * StartupApproved is read-only. Disable removes only the Run value. */
gboolean otp_windows_autostart_query (const wchar_t *run_key,
                                      const wchar_t *approved_key,
                                      OtpWindowsAutostartState *state,
                                      gchar **command, GError **error);
gboolean otp_windows_autostart_write (const wchar_t *run_key,
                                      const wchar_t *approved_key,
                                      const gchar *path, gboolean minimized,
                                      gboolean enable, GError **error);
gboolean otp_windows_autostart_exists (GError **error);

G_END_DECLS
