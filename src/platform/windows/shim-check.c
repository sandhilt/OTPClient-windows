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
assert_directory_entry (const char *dir, const char *expected)
{
    g_autofree gchar *pattern = g_build_filename (dir, "*", NULL);
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (pattern, -1, NULL, NULL, NULL);
    g_assert_nonnull (wide);
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW ((WCHAR *) wide, &data);
    guint count = 0;
    if (search == INVALID_HANDLE_VALUE) {
        g_assert_null (expected);
        g_assert_cmpuint (GetLastError (), ==, ERROR_FILE_NOT_FOUND);
        return;
    }
    do {
        g_autofree gchar *name = g_utf16_to_utf8 ((gunichar2 *) data.cFileName, -1, NULL, NULL, NULL);
        g_assert_nonnull (name);
        if (g_str_equal (name, ".") || g_str_equal (name, "..")) continue;
        g_assert_nonnull (expected);
        g_assert_cmpstr (name, ==, expected);
        count++;
    } while (FindNextFileW (search, &data));
    g_assert_cmpuint (GetLastError (), ==, ERROR_NO_MORE_FILES);
    g_assert_true (FindClose (search));
    g_assert_cmpuint (count, ==, expected ? 1 : 0);
}

static void
assert_private_export (const char *path, const char *text)
{
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (path, -1, NULL, NULL, NULL);
    g_assert_nonnull (wide);
    HANDLE handle = CreateFileW ((WCHAR *) wide, GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    g_assert_true (handle != INVALID_HANDLE_VALUE);
    LARGE_INTEGER length;
    g_assert_true (GetFileSizeEx (handle, &length));
    g_assert_cmpint (length.QuadPart, ==, strlen (text));
    char contents[128];
    DWORD read;
    g_assert_cmpuint (strlen (text), <, sizeof (contents));
    g_assert_true (ReadFile (handle, contents, sizeof (contents), &read, NULL));
    g_assert_cmpuint (read, ==, strlen (text));
    g_assert_cmpmem (contents, read, text, strlen (text));

    /* Audit independently of otp_private_handle: final owner, protection,
     * exactly one non-inherited full-access allow ACE for the token user. */
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PACL acl = NULL;
    PSID owner = NULL;
    g_assert_cmpuint (GetSecurityInfo (handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, NULL, &acl, NULL, &descriptor), ==, ERROR_SUCCESS);
    g_autofree TOKEN_USER *user = otp_current_user ();
    g_assert_nonnull (user);
    SECURITY_DESCRIPTOR_CONTROL control;
    DWORD revision;
    g_assert_true (GetSecurityDescriptorControl (descriptor, &control, &revision));
    g_assert_true ((control & SE_DACL_PROTECTED) != 0);
    g_assert_nonnull (owner);
    g_assert_true (EqualSid (owner, user->User.Sid));
    g_assert_nonnull (acl);
    g_assert_true (IsValidAcl (acl));
    g_assert_cmpuint (acl->AceCount, ==, 1);
    ACCESS_ALLOWED_ACE *ace;
    g_assert_true (GetAce (acl, 0, (void **) &ace));
    g_assert_cmpuint (ace->Header.AceType, ==, ACCESS_ALLOWED_ACE_TYPE);
    g_assert_cmpuint (ace->Header.AceFlags, ==, 0);
    g_assert_cmpuint (ace->Mask, ==, FILE_ALL_ACCESS);
    g_assert_true (EqualSid (&ace->SidStart, user->User.Sid));
    LocalFree (descriptor);
    g_assert_true (CloseHandle (handle));
}

static void
grant_inheritable_users_read (const char *dir)
{
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (dir, -1, NULL, NULL, NULL);
    g_assert_nonnull (wide);
    g_autofree BYTE *users = g_malloc0 (SECURITY_MAX_SID_SIZE);
    DWORD sid_size = SECURITY_MAX_SID_SIZE;
    g_assert_true (CreateWellKnownSid (WinBuiltinUsersSid, NULL, users, &sid_size));
    PACL old_acl = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    g_assert_cmpuint (GetNamedSecurityInfoW ((WCHAR *) wide, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, NULL, NULL, &old_acl, NULL, &descriptor), ==, ERROR_SUCCESS);
    g_assert_nonnull (old_acl);
    EXPLICIT_ACCESSW entry = { 0 };
    entry.grfAccessPermissions = FILE_GENERIC_READ;
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    BuildTrusteeWithSidW (&entry.Trustee, users);
    entry.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    PACL acl = NULL;
    g_assert_cmpuint (SetEntriesInAclW (1, &entry, old_acl, &acl), ==, ERROR_SUCCESS);
    g_assert_cmpuint (SetNamedSecurityInfoW ((WCHAR *) wide, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL), ==, ERROR_SUCCESS);
    LocalFree (acl);
    LocalFree (descriptor);

    /* Confirm the trigger was really installed as an explicit (OI)(CI) ACE. */
    descriptor = NULL;
    g_assert_cmpuint (GetNamedSecurityInfoW ((WCHAR *) wide, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION, NULL, NULL, &acl, NULL, &descriptor), ==, ERROR_SUCCESS);
    g_assert_nonnull (acl);
    gboolean found = FALSE;
    for (DWORD i = 0; i < acl->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace;
        g_assert_true (GetAce (acl, i, (void **) &ace));
        if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
            (ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERITED_ACE)) ==
                (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) &&
            (ace->Mask & FILE_GENERIC_READ) == FILE_GENERIC_READ &&
            EqualSid (&ace->SidStart, users)) found = TRUE;
    }
    g_assert_true (found);
    LocalFree (descriptor);
}

static void
check_repeated_exports (const char *dir)
{
    for (guint i = 0; i < 20; i++) {
        /* Include multibyte UTF-8 and a supplementary UTF-16 surrogate pair. */
        g_autofree gchar *name = i == 0 ? g_strdup ("freeotpplus-exports.txt") :
            g_strdup_printf ("export-%02u-ação-東京-🔐.txt", i);
        g_autofree gchar *path = g_build_filename (dir, name, NULL);
        g_autofree gchar *text = g_strdup_printf ("dummy export %u\r\n", i);
        write_private (path, "old");
        assert_directory_entry (dir, name);
        assert_private_export (path, "old");
        write_private (path, text);
        assert_directory_entry (dir, name);
        assert_private_export (path, text);
        g_autofree gunichar2 *wide = g_utf8_to_utf16 (path, -1, NULL, NULL, NULL);
        g_assert_nonnull (wide);
        g_assert_true (DeleteFileW ((WCHAR *) wide));
        assert_directory_entry (dir, NULL);
    }

    /* Force a genuine rename failure: the destination is an existing directory.
     * Neither the temporary nor plaintext may remain beside it. */
    g_autofree gchar *blocked = g_build_filename (dir, "blocked", NULL);
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (blocked, -1, NULL, NULL, NULL);
    g_assert_nonnull (wide);
    g_assert_true (CreateDirectoryW ((WCHAR *) wide, NULL));
    g_autoptr(GFile) file = g_file_new_for_path (blocked);
    g_autoptr(GError) error = NULL;
    g_autoptr(GFileOutputStream) output = otp_file_replace (file, NULL, FALSE,
        G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION, NULL, &error);
    g_assert_no_error (error);
    g_assert_nonnull (output);
    g_assert_true (g_output_stream_write_all (G_OUTPUT_STREAM (output), "discard", 7, NULL, NULL, &error));
    g_assert_no_error (error);
    g_assert_false (g_output_stream_close (G_OUTPUT_STREAM (output), NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull (strstr (error->message, "flush/replace failed"));
    assert_directory_entry (dir, "blocked");
    g_assert_true (RemoveDirectoryW ((WCHAR *) wide));
    assert_directory_entry (dir, NULL);
}

static void
test_private_inheritable_acl (void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *dir = g_dir_make_tmp ("otp-inherit-XXXXXX", &error);
    g_assert_no_error (error);
    grant_inheritable_users_read (dir);
    check_repeated_exports (dir);

    /* Explicit extended-length paths avoid dependence on the machine's
     * longPathAware manifest/registry policy. Each component stays <255. */
    g_strdelimit (dir, "/", '\\');
    g_autofree gchar *extended = g_str_has_prefix (dir, "\\\\") ?
        g_strconcat ("\\\\?\\UNC\\", dir + 2, NULL) : g_strconcat ("\\\\?\\", dir, NULL);
    g_autoptr(GPtrArray) parents = g_ptr_array_new_with_free_func (g_free);
    gchar *parent = extended;
    for (guint i = 0; i < 4; i++) {
        g_autofree gchar *component = g_strnfill (70, 'a' + i);
        gchar *child = g_build_filename (parent, component, NULL);
        g_ptr_array_add (parents, child);
        g_autofree gunichar2 *wide = g_utf8_to_utf16 (child, -1, NULL, NULL, NULL);
        g_assert_nonnull (wide);
        g_assert_true (CreateDirectoryW ((WCHAR *) wide, NULL));
        parent = child;
    }
    g_assert_cmpuint (strlen (parent), >, MAX_PATH);
    grant_inheritable_users_read (parent);
    check_repeated_exports (parent);
    for (guint i = parents->len; i > 0; i--) {
        g_autofree gunichar2 *wide = g_utf8_to_utf16 (g_ptr_array_index (parents, i - 1), -1, NULL, NULL, NULL);
        g_assert_nonnull (wide);
        g_assert_true (RemoveDirectoryW ((WCHAR *) wide));
    }
    g_assert_cmpint (g_rmdir (dir), ==, 0);
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
    g_test_add_func ("/windows/private-inheritable-acl", test_private_inheritable_acl);
    g_test_add_func ("/windows/fd-adapters", test_fd_adapters);
    return g_test_run ();
}
