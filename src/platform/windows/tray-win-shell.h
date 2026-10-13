#pragma once

#include <windows.h>
#include <glib.h>

/* Private implementation API, not a replacement for src/gui/tray.h.
 * All calls and callbacks belong to the creating GUI thread/default context.
 * Callbacks must not destroy the shell inline (a Win32 menu can be nested). */
typedef struct _OtpWindowsTrayShell OtpWindowsTrayShell;
typedef enum
{
    OTP_TRAY_TOGGLE,
    OTP_TRAY_SHOW,
    OTP_TRAY_QUIT,
    OTP_TRAY_PUBLICATION_CHANGED
} OtpWindowsTrayEvent;
typedef void (*OtpWindowsTrayCallback) (OtpWindowsTrayShell *shell,
                                       OtpWindowsTrayEvent event,
                                       gpointer data);

OtpWindowsTrayShell *otp_windows_tray_shell_new (HICON icon,
                                                const gchar *show_label,
                                                const gchar *quit_label,
                                                OtpWindowsTrayCallback callback,
                                                gpointer data);
void otp_windows_tray_shell_set_enabled (OtpWindowsTrayShell *shell, gboolean enabled);
void otp_windows_tray_shell_set_tooltip (OtpWindowsTrayShell *shell, const gchar *text);
gboolean otp_windows_tray_shell_is_published (OtpWindowsTrayShell *shell);
gboolean otp_windows_tray_shell_is_available (void);
void otp_windows_tray_shell_free (OtpWindowsTrayShell *shell);

/* Converts public artwork only; caller owns the returned HICON. */
HICON otp_windows_tray_icon_from_pixels (const guchar *pixels, gint width,
                                        gint height, gint stride, gint channels);

#ifdef OTP_WINDOWS_TRAY_CHECK
/* Probe-only message injection; not exported by the application build. */
void otp_windows_tray_shell_check_post_selection (OtpWindowsTrayShell *shell, gboolean valid_id);
#endif
