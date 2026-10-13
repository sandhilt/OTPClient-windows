#include <string.h>
#include <wchar.h>
#include "autostart-win.h"

static gboolean
win_error (const gchar *operation, LSTATUS status, GError **error)
{
    g_autofree gchar *message = g_win32_error_message ((gint) status);
    g_set_error (error, G_IO_ERROR, g_io_error_from_win32_error ((gint) status),
                 "%s: %s (Win32 %lu)", operation, message, (unsigned long) status);
    return FALSE;
}

static gboolean
valid_keys (const wchar_t *run_key, const wchar_t *approved_key, GError **error)
{
    if (run_key != NULL && approved_key != NULL
        && ((wcscmp (run_key, OTP_AUTOSTART_RUN_KEY) == 0
             && wcscmp (approved_key, OTP_AUTOSTART_APPROVED_KEY) == 0)
            || (wcscmp (run_key, OTP_AUTOSTART_TEST_RUN_KEY) == 0
                && wcscmp (approved_key, OTP_AUTOSTART_TEST_APPROVED_KEY) == 0)))
        return TRUE;

    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         "Autostart registry key is outside the allowed HKCU paths");
    return FALSE;
}

static gboolean
valid_path (const gchar *path)
{
    if (path == NULL || !g_utf8_validate (path, -1, NULL) || strlen (path) < 3
        || !((g_ascii_isalpha (path[0]) && path[1] == ':' && path[2] == '\\')
             || (path[0] == '\\' && path[1] == '\\' && path[2] != '\0'))
        || path[strlen (path) - 1] == '\\')
        return FALSE;
    if (path[0] == '\\')
    {
        const gchar *share = strchr (path + 2, '\\');
        if (share == NULL || share == path + 2 || share[1] == '\\')
            return FALSE;
        const gchar *file = strchr (share + 1, '\\');
        if (file == NULL || file == share + 1 || file[1] == '\0')
            return FALSE;
    }
    for (const gchar *p = path; *p != '\0'; p++)
        if ((guchar) *p < 32 || *p == '"')
            return FALSE;
    return TRUE;
}

gchar *
otp_windows_autostart_compose (const gchar *path, gboolean minimized, GError **error)
{
    if (!valid_path (path))
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "Autostart requires a full executable path without quotes or control characters");
        return NULL;
    }

    g_autofree gchar *command = g_strdup_printf ("\"%s\"%s", path,
                                                 minimized ? " --start-minimized" : "");
    glong units = 0;
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (command, -1, NULL, &units, error);
    if (wide == NULL)
        return NULL;
    if (units > 260)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG,
                             "Autostart command exceeds the Windows Run limit of 260 characters");
        return NULL;
    }
    return g_steal_pointer (&command);
}

gchar *
otp_windows_autostart_executable (GError **error)
{
    for (DWORD capacity = 256; capacity <= 32768; capacity *= 2)
    {
        g_autofree wchar_t *buffer = g_new0 (wchar_t, capacity);
        DWORD length = GetModuleFileNameW (NULL, buffer, capacity);
        if (length == 0)
        {
            win_error ("GetModuleFileNameW", (LSTATUS) GetLastError (), error);
            return NULL;
        }
        if (length < capacity)
            return g_utf16_to_utf8 ((const gunichar2 *) buffer, length, NULL, NULL, error);
    }
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG,
                         "Running executable path is too long");
    return NULL;
}

/* Bounded reads also defend against malformed/unterminated REG_SZ data.
 * UNVERIFIED: native Windows registry races and GTK caller behaviour. */
gboolean
otp_windows_autostart_query (const wchar_t *run_key, const wchar_t *approved_key,
                            OtpWindowsAutostartState *state, gchar **command,
                            GError **error)
{
    g_return_val_if_fail (state != NULL, FALSE);
    *state = OTP_AUTOSTART_ABSENT;
    if (command != NULL)
        *command = NULL;
    if (!valid_keys (run_key, approved_key, error))
        return FALSE;

    HKEY key;
    LSTATUS status = RegOpenKeyExW (HKEY_CURRENT_USER, run_key, 0,
                                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_FILE_NOT_FOUND)
        return TRUE;
    if (status != ERROR_SUCCESS)
        return win_error ("Open autostart Run key", status, error);

    wchar_t buffer[262] = { 0 };
    DWORD bytes = sizeof buffer;
    status = RegGetValueW (key, NULL, OTP_AUTOSTART_VALUE, RRF_RT_REG_SZ,
                           NULL, buffer, &bytes);
    RegCloseKey (key);
    if (status == ERROR_FILE_NOT_FOUND)
        return TRUE;
    if (status != ERROR_SUCCESS)
        return win_error ("Read autostart Run value", status, error);
    if (bytes < sizeof (wchar_t) || bytes % sizeof (wchar_t) != 0
        || buffer[0] == L'\0' || wcslen (buffer) > 260
        || wcslen (buffer) + 1 != bytes / sizeof (wchar_t))
        return win_error ("Invalid autostart Run value", ERROR_INVALID_DATA, error);

    g_autofree gchar *value = g_utf16_to_utf8 ((const gunichar2 *) buffer, -1,
                                              NULL, NULL, error);
    if (value == NULL)
        return FALSE;

    status = RegOpenKeyExW (HKEY_CURRENT_USER, approved_key, 0,
                            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        return win_error ("Open StartupApproved key", status, error);
    if (status == ERROR_SUCCESS)
    {
        BYTE approved[12];
        bytes = sizeof approved;
        status = RegGetValueW (key, NULL, OTP_AUTOSTART_VALUE, RRF_RT_REG_BINARY,
                               NULL, approved, &bytes);
        RegCloseKey (key);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return win_error ("Read StartupApproved value", status, error);
        if (status == ERROR_SUCCESS)
        {
            /* WINDOWS-STUB / UNVERIFIED: StartupApproved has no documented
             * stable binary contract. Recognise only observed DWORD 2/3 plus
             * an eight-byte timestamp; unknown layouts fail closed. Never
             * write this key or override Task Manager's user decision. */
            if (bytes != 12 || approved[1] != 0 || approved[2] != 0
                || approved[3] != 0 || (approved[0] != 2 && approved[0] != 3))
                return win_error ("Unknown StartupApproved state", ERROR_INVALID_DATA, error);
            if (approved[0] == 3)
                *state = OTP_AUTOSTART_DISABLED_BY_USER;
        }
    }
    if (*state != OTP_AUTOSTART_DISABLED_BY_USER)
        *state = OTP_AUTOSTART_ENABLED;
    if (command != NULL)
        *command = g_steal_pointer (&value);
    return TRUE;
}

gboolean
otp_windows_autostart_exists (GError **error)
{
    HKEY key;
    LSTATUS status = RegOpenKeyExW (HKEY_CURRENT_USER, OTP_AUTOSTART_RUN_KEY, 0,
                                    KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_FILE_NOT_FOUND)
        return FALSE;
    if (status != ERROR_SUCCESS)
        return win_error ("Open autostart Run key", status, error);
    status = RegQueryValueExW (key, OTP_AUTOSTART_VALUE, NULL, NULL, NULL, NULL);
    RegCloseKey (key);
    if (status == ERROR_FILE_NOT_FOUND)
        return FALSE;
    if (status != ERROR_SUCCESS)
        return win_error ("Check autostart Run value", status, error);
    return TRUE;
}

gboolean
otp_windows_autostart_write (const wchar_t *run_key, const wchar_t *approved_key,
                            const gchar *path, gboolean minimized, gboolean enable,
                            GError **error)
{
    if (!valid_keys (run_key, approved_key, error))
        return FALSE;

    HKEY key;
    LSTATUS status;
    if (!enable)
    {
        status = RegOpenKeyExW (HKEY_CURRENT_USER, run_key, 0,
                                KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
        if (status == ERROR_FILE_NOT_FOUND)
            return TRUE;
        if (status != ERROR_SUCCESS)
            return win_error ("Open autostart Run key for removal", status, error);
        status = RegDeleteValueW (key, OTP_AUTOSTART_VALUE);
        RegCloseKey (key);
        if (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND)
            return TRUE;
        return win_error ("Remove autostart Run value", status, error);
    }

    g_autofree gchar *command = otp_windows_autostart_compose (path, minimized, error);
    if (command == NULL)
        return FALSE;
    OtpWindowsAutostartState state;
    g_autofree gchar *previous = NULL;
    if (!otp_windows_autostart_query (run_key, approved_key, &state, &previous, error))
        return FALSE;
    /* Query approval even if no Run value exists: Task Manager's decision can
     * outlive a removed entry. A temporary, read-only query would otherwise
     * miss it, so inspect approval directly below before creating the value. */
    if (state == OTP_AUTOSTART_ABSENT)
    {
        BYTE approved[12];
        DWORD bytes = sizeof approved;
        status = RegGetValueW (HKEY_CURRENT_USER, approved_key, OTP_AUTOSTART_VALUE,
                               RRF_RT_REG_BINARY | RRF_SUBKEY_WOW6464KEY,
                               NULL, approved, &bytes);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            return win_error ("Read StartupApproved before enabling", status, error);
        if (status == ERROR_SUCCESS)
        {
            if (bytes != 12 || approved[1] != 0 || approved[2] != 0
                || approved[3] != 0 || (approved[0] != 2 && approved[0] != 3))
                return win_error ("Unknown StartupApproved state", ERROR_INVALID_DATA, error);
            if (approved[0] == 3)
                state = OTP_AUTOSTART_DISABLED_BY_USER;
        }
    }
    if (state == OTP_AUTOSTART_DISABLED_BY_USER)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                             "Autostart is disabled by user in Windows Startup apps; enable OTPClient there first");
        return FALSE;
    }
    if (g_strcmp0 (command, previous) == 0)
        return TRUE;

    g_autofree gunichar2 *wide = g_utf8_to_utf16 (command, -1, NULL, NULL, error);
    if (wide == NULL)
        return FALSE;
    status = RegCreateKeyExW (HKEY_CURRENT_USER, run_key, 0, NULL,
                              REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_WOW64_64KEY,
                              NULL, &key, NULL);
    if (status != ERROR_SUCCESS)
        return win_error ("Create autostart Run key", status, error);
    status = RegSetValueExW (key, OTP_AUTOSTART_VALUE, 0, REG_SZ, (const BYTE *) wide,
                             (DWORD) ((wcslen ((const wchar_t *) wide) + 1) * sizeof (wchar_t)));
    RegCloseKey (key);
    if (status != ERROR_SUCCESS)
        return win_error ("Write autostart Run value", status, error);
    return TRUE;
}
