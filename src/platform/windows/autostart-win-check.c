#include <stdlib.h>
#include <string.h>
#include "autostart-win.h"

static void
test_executable (void)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *path = otp_windows_autostart_executable (&error);
    g_assert_no_error (error);
    g_assert_nonnull (path);
    g_assert_true (g_utf8_validate (path, -1, NULL));
    g_assert_true (g_path_is_absolute (path));
}

static void
test_compose (void)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *normal = otp_windows_autostart_compose (
        "C:\\Program Files\\OTPClient\\otpclient.exe", FALSE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (normal, ==, "\"C:\\Program Files\\OTPClient\\otpclient.exe\"");
    g_autofree gchar *unicode = otp_windows_autostart_compose (
        "C:\\Usuários\\João 😀\\otpclient.exe", TRUE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (unicode, ==,
        "\"C:\\Usuários\\João 😀\\otpclient.exe\" --start-minimized");
    g_autofree gchar *unc = otp_windows_autostart_compose (
        "\\\\server\\share name\\otpclient.exe", FALSE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (unc, ==, "\"\\\\server\\share name\\otpclient.exe\"");
    g_autofree gchar *metachar = otp_windows_autostart_compose (
        "C:\\100% & (portable)\\otpclient.exe", FALSE, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (metachar, ==, "\"C:\\100% & (portable)\\otpclient.exe\"");
}

static void
test_invalid (void)
{
    const gchar *paths[] = { NULL, "", "otpclient.exe", "C:otpclient.exe",
        "C:\\a\"b.exe", "C:\\a\nb.exe", "C:\\dir\\", "C:\\\xff.exe",
        "\\\\server", "\\\\server\\share", "\\\\server\\\\file.exe" };
    for (guint i = 0; i < G_N_ELEMENTS (paths); i++)
    {
        g_autoptr (GError) error = NULL;
        g_autofree gchar *value = otp_windows_autostart_compose (paths[i], FALSE, &error);
        g_assert_null (value);
        g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    }
}

static void
test_limit (void)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *tail = g_strnfill (255, 'a');
    g_autofree gchar *path = g_strconcat ("C:\\", tail, NULL);
    g_autofree gchar *value = otp_windows_autostart_compose (path, FALSE, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (strlen (value), ==, 260);
    g_clear_pointer (&value, g_free);
    value = otp_windows_autostart_compose (path, TRUE, &error);
    g_assert_null (value);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG);
    g_clear_error (&error);
    g_autofree gchar *non_bmp = g_strconcat (path, "😀", NULL);
    value = otp_windows_autostart_compose (non_bmp, FALSE, &error);
    g_assert_null (value);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FILENAME_TOO_LONG);
}

static void
test_key_restriction (void)
{
    g_autoptr (GError) error = NULL;
    g_assert_false (otp_windows_autostart_write (L"Software\\Elsewhere",
        OTP_AUTOSTART_TEST_APPROVED_KEY, "C:\\otpclient.exe", FALSE, TRUE, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_assert_false (otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_APPROVED_KEY, NULL, FALSE, FALSE, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

/* No fatal assertion after owning the temporary key: failures go through
 * cleanup, including unexpected native status/error results. A process crash
 * cannot guarantee cleanup; the next run refuses to overwrite a leftover. */
#define CHECK(condition) do { \
    if (!(condition)) { \
        g_test_message ("Failed: %s (line %d)", #condition, __LINE__); \
        g_test_fail (); \
        goto cleanup; \
    } \
} while (0)

static gboolean owns_test_key;

static void
cleanup_test_key (void)
{
    if (!owns_test_key)
        return;
    const wchar_t *keys[] = { OTP_AUTOSTART_TEST_APPROVED_KEY,
        OTP_AUTOSTART_TEST_ROOT L"\\StartupApproved", OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_ROOT };
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    {
        LSTATUS status = RegDeleteKeyExW (HKEY_CURRENT_USER, keys[i], KEY_WOW64_64KEY, 0);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
        {
            g_test_message ("Test-key cleanup failed: Win32 %lu", (unsigned long) status);
            g_test_fail ();
        }
    }
    owns_test_key = FALSE;
}

static void
test_registry (void)
{
    HKEY root = NULL;
    HKEY approved = NULL;
    HKEY run = NULL;
    DWORD disposition;
    OtpWindowsAutostartState state;
    g_autoptr (GError) error = NULL;
    g_autofree gchar *command = NULL;
    BYTE approval[12] = { 2 };
    const gchar *path = "C:\\Portable João\\otpclient.exe";
    LSTATUS status = RegCreateKeyExW (HKEY_CURRENT_USER, OTP_AUTOSTART_TEST_ROOT,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
        NULL, &root, &disposition);
    CHECK (status == ERROR_SUCCESS);
    if (disposition != REG_CREATED_NEW_KEY)
    {
        g_test_message ("Refusing existing HKCU\\Software\\OTPClient-test; inspect/remove it before retrying");
        g_test_fail ();
        goto cleanup;
    }
    owns_test_key = TRUE;
    CHECK (otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, NULL, &error));
    CHECK (state == OTP_AUTOSTART_ABSENT);
    CHECK (otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, path, TRUE, TRUE, &error));
    CHECK (otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, &command, &error));
    CHECK (state == OTP_AUTOSTART_ENABLED);
    CHECK (g_strcmp0 (command, "\"C:\\Portable João\\otpclient.exe\" --start-minimized") == 0);
    g_clear_pointer (&command, g_free);
    CHECK (otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, "D:\\Moved folder\\otpclient.exe", FALSE, TRUE, &error));
    CHECK (otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, &command, &error));
    CHECK (g_strcmp0 (command, "\"D:\\Moved folder\\otpclient.exe\"") == 0);
    g_clear_pointer (&command, g_free);
    CHECK (RegCreateKeyExW (HKEY_CURRENT_USER, OTP_AUTOSTART_TEST_APPROVED_KEY,
        0, NULL, REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY,
        NULL, &approved, NULL) == ERROR_SUCCESS);
    CHECK (RegSetValueExW (approved, OTP_AUTOSTART_VALUE, 0, REG_BINARY,
        approval, sizeof approval) == ERROR_SUCCESS);
    CHECK (otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, NULL, &error));
    CHECK (state == OTP_AUTOSTART_ENABLED);
    approval[0] = 3;
    CHECK (RegSetValueExW (approved, OTP_AUTOSTART_VALUE, 0, REG_BINARY,
        approval, sizeof approval) == ERROR_SUCCESS);
    CHECK (otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, NULL, &error));
    CHECK (state == OTP_AUTOSTART_DISABLED_BY_USER);
    CHECK (!otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, path, FALSE, TRUE, &error));
    CHECK (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED));
    g_clear_error (&error);
    CHECK (otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, NULL, FALSE, FALSE, &error));
    CHECK (otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, NULL, &error));
    CHECK (state == OTP_AUTOSTART_ABSENT);
    /* Stale approval still blocks recreation; do not undo Task Manager. */
    CHECK (!otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, path, FALSE, TRUE, &error));
    CHECK (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED));
    g_clear_error (&error);
    approval[0] = 2;
    CHECK (RegSetValueExW (approved, OTP_AUTOSTART_VALUE, 0, REG_BINARY,
        approval, sizeof approval) == ERROR_SUCCESS);
    CHECK (otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, path, FALSE, TRUE, &error));
    approval[0] = 7;
    CHECK (RegSetValueExW (approved, OTP_AUTOSTART_VALUE, 0, REG_BINARY,
        approval, sizeof approval) == ERROR_SUCCESS);
    CHECK (!otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, NULL, &error));
    CHECK (error != NULL);
    g_clear_error (&error);
    CHECK (RegDeleteValueW (approved, OTP_AUTOSTART_VALUE) == ERROR_SUCCESS);
    CHECK (RegOpenKeyExW (HKEY_CURRENT_USER, OTP_AUTOSTART_TEST_RUN_KEY, 0,
        KEY_SET_VALUE | KEY_WOW64_64KEY, &run) == ERROR_SUCCESS);
    DWORD wrong = 42;
    CHECK (RegSetValueExW (run, OTP_AUTOSTART_VALUE, 0, REG_DWORD,
        (const BYTE *) &wrong, sizeof wrong) == ERROR_SUCCESS);
    CHECK (!otp_windows_autostart_query (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, &state, NULL, &error));
    CHECK (error != NULL);
    g_clear_error (&error);
    CHECK (otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, NULL, FALSE, FALSE, &error));
    CHECK (otp_windows_autostart_write (OTP_AUTOSTART_TEST_RUN_KEY,
        OTP_AUTOSTART_TEST_APPROVED_KEY, NULL, FALSE, FALSE, &error));

cleanup:
    if (run != NULL)
        RegCloseKey (run);
    if (approved != NULL)
        RegCloseKey (approved);
    if (root != NULL)
        RegCloseKey (root);
    cleanup_test_key ();
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    atexit (cleanup_test_key);
    g_test_add_func ("/autostart/compose", test_compose);
    g_test_add_func ("/autostart/executable", test_executable);
    g_test_add_func ("/autostart/invalid", test_invalid);
    g_test_add_func ("/autostart/limit", test_limit);
    g_test_add_func ("/autostart/key-restriction", test_key_restriction);
    g_test_add_func ("/autostart/registry", test_registry);
    return g_test_run ();
}
