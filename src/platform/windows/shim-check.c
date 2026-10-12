/* Windows-only runtime checks; run under Wine and on native Windows. No real
 * credentials are used. --make-password-file writes a fixed smoke password. */
#include "security.h"
#include <aclapi.h>
#include <io.h>
#include <errno.h>

static void
write_private (const char *path, const char *text)
{
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFileOutputStream) output = otp_file_replace (file, NULL, FALSE,
        G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (output);
    g_assert_true (g_output_stream_write_all (G_OUTPUT_STREAM (output), text, strlen (text), NULL, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (g_output_stream_close (G_OUTPUT_STREAM (output), NULL, &error));
    g_assert_no_error (error);
}

static void
test_private_acl (void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp ("otp-shim-XXXXXX", &error);
    g_assert_no_error (error);
    g_autofree gchar *path = g_build_filename (dir, "export.txt", NULL);
    g_assert_true (g_file_set_contents (path, "old", -1, &error));
    g_assert_no_error (error);
    write_private (path, "replacement");
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (path, -1, NULL, NULL, NULL);
    HANDLE handle = CreateFileW ((wchar_t *) wide, GENERIC_READ | WRITE_DAC,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    g_assert_true (handle != INVALID_HANDLE_VALUE);
    g_autofree TOKEN_USER *user = otp_current_user ();
    g_assert_nonnull (user);
    g_assert_true (otp_private_handle (handle, user->User.Sid));
    unsigned mode;
    g_assert_true (otp_acl_mode (handle, user->User.Sid, &mode));
    g_assert_cmpuint (mode, ==, 0);
    CloseHandle (handle);
    int fd = otp_open (path, O_RDONLY | O_NOFOLLOW);
    g_assert_cmpint (fd, >=, 0);
    GStatBuf state;
    g_assert_cmpint (otp_fstat (fd, &state), ==, 0);
    g_assert_cmpuint (state.st_mode & 077, ==, 0);
    _close (fd);

    /* Read grants, write grants and a NULL DACL must fail the CLI mode test. */
    handle = CreateFileW ((wchar_t *) wide, READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    g_assert_true (handle != INVALID_HANDLE_VALUE);
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sid_size = sizeof (sid);
    g_assert_true (CreateWellKnownSid (WinWorldSid, NULL, sid, &sid_size));
    union {
        ACL alignment;
        BYTE bytes[sizeof (ACL) + sizeof (ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE];
    } acl_buffer;
    PACL acl = (PACL) acl_buffer.bytes;
    DWORD masks[] = { GENERIC_READ, GENERIC_ALL };
    unsigned expected[] = { 044, 066 };
    for (guint i = 0; i < G_N_ELEMENTS (masks); i++) {
        g_assert_true (InitializeAcl (acl, sizeof (acl_buffer), ACL_REVISION));
        g_assert_true (AddAccessAllowedAce (acl, ACL_REVISION, masks[i], sid));
        g_assert_cmpuint (SetSecurityInfo (handle, SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            NULL, NULL, acl, NULL), ==, ERROR_SUCCESS);
        g_assert_true (otp_acl_mode (handle, user->User.Sid, &mode));
        g_assert_cmpuint (mode, ==, expected[i]);
    }
    g_assert_cmpuint (SetSecurityInfo (handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        NULL, NULL, NULL, NULL), ==, ERROR_SUCCESS);
    g_assert_true (otp_acl_mode (handle, user->User.Sid, &mode));
    g_assert_cmpuint (mode, ==, 066);
    CloseHandle (handle);
    fd = otp_open (path, O_RDONLY | O_NOFOLLOW);
    g_assert_cmpint (fd, >=, 0);
    g_assert_cmpint (otp_fstat (fd, &state), ==, 0);
    g_assert_cmpuint (state.st_mode & 077, !=, 0);
    _close (fd);
    /* Replacing an existing permissive destination must restore privacy. */
    write_private (path, "replacement2");
    g_autofree gchar *contents = NULL;
    g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (contents, ==, "replacement2");
    fd = otp_open (path, O_RDONLY | O_NOFOLLOW);
    g_assert_cmpint (fd, >=, 0);
    g_assert_cmpint (otp_fstat (fd, &state), ==, 0);
    g_assert_cmpuint (state.st_mode & 077, ==, 0);
    _close (fd);
    /* A cancelled close must retain the existing export, not publish partial
     * content or leave an open handle that prevents directory cleanup. */
    g_autoptr(GFile) file = g_file_new_for_path (path);
    g_autoptr(GFileOutputStream) output = otp_file_replace (file, NULL, FALSE,
        G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (output);
    g_assert_true (g_output_stream_write_all (G_OUTPUT_STREAM (output), "discard", 7, NULL, NULL, &error));
    g_autoptr(GCancellable) cancel = g_cancellable_new ();
    g_cancellable_cancel (cancel);
    g_assert_false (g_output_stream_close (G_OUTPUT_STREAM (output), cancel, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&error);
    g_clear_object (&output);
    g_clear_pointer (&contents, g_free);
    g_assert_true (g_file_get_contents (path, &contents, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (contents, ==, "replacement2");
    g_assert_cmpint (g_remove (path), ==, 0);
    g_assert_cmpint (g_rmdir (dir), ==, 0);
}

static void
test_fd_adapters (void)
{
    g_autoptr(GError) error = NULL;
    gchar *path = NULL;
    int fd = g_file_open_tmp ("otp-fd-XXXXXX", &path, &error);
    g_assert_no_error (error);
    g_assert_cmpint (fd, >=, 0);
    g_assert_cmpint (_write (fd, "{\"a\":1}", 7), ==, 7);
    g_autofree gchar *token = g_strdup_printf ("/proc/self/fd/%d", fd);
    g_autoptr(GFile) file = otp_file_new_for_path (token);
    g_autoptr(GFileInputStream) input = otp_file_read (file, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (input);
    g_assert_true (g_seekable_can_seek (G_SEEKABLE (input)));
    g_assert_true (g_seekable_seek (G_SEEKABLE (input), 5, G_SEEK_SET, NULL, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_seekable_tell (G_SEEKABLE (input)), ==, 5);
    char byte;
    g_assert_cmpint (g_input_stream_read (G_INPUT_STREAM (input), &byte, 1, NULL, &error), ==, 1);
    g_assert_no_error (error);
    g_assert_cmpint (byte, ==, '1');
    g_assert_true (g_seekable_seek (G_SEEKABLE (input), -7, G_SEEK_END, NULL, &error));
    g_assert_true (g_seekable_seek (G_SEEKABLE (input), 1, G_SEEK_CUR, NULL, &error));
    g_assert_cmpint (g_seekable_tell (G_SEEKABLE (input)), ==, 1);
    g_assert_true (g_input_stream_close (G_INPUT_STREAM (input), NULL, &error));
    _close (fd);
    json_error_t json_error;
    memset (&json_error, 0xa5, sizeof (json_error));
    g_assert_null (otp_json_load_file (token, 0, &json_error));
    g_assert_true (g_str_has_prefix (json_error.text, "Cannot seek import descriptor:"));
    g_assert_cmpint (json_error.line, ==, 0);
    g_assert_cmpint (g_remove (path), ==, 0);
    g_free (path);
}

int
main (int argc, char **argv)
{
    if (argc == 3 && g_str_equal (argv[1], "--make-password-file")) {
        write_private (argv[2], "smoke-only-password\n");
        return 0;
    }
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/windows/private-acl", test_private_acl);
    g_test_add_func ("/windows/fd-adapters", test_fd_adapters);
    return g_test_run ();
}
