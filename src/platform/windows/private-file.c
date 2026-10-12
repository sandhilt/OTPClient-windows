#include "security.h"

typedef struct {
    GFileOutputStream parent;
    HANDLE handle;
    WCHAR *destination;
    DWORD destination_chars;
    TOKEN_USER *user;
    gboolean failed;
} OtpPrivateStream;
typedef struct { GFileOutputStreamClass parent; } OtpPrivateStreamClass;
GType otp_private_stream_get_type (void);
G_DEFINE_TYPE (OtpPrivateStream, otp_private_stream, G_TYPE_FILE_OUTPUT_STREAM)

static void
windows_error (GError **error, const char *operation)
{
    DWORD code = GetLastError ();
    g_autofree gchar *message = g_win32_error_message (code);
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s (%lu)",
                 operation, message, (unsigned long) code);
}

static void
discard_handle (HANDLE handle, GError **error)
{
    FILE_DISPOSITION_INFO disposition = { TRUE };
    /* Keep the primary failure, but do not silently lose cleanup errors. */
    if (!SetFileInformationByHandle (handle, FileDispositionInfo, &disposition, sizeof (disposition))) {
        g_autoptr(GError) cleanup = NULL;
        windows_error (&cleanup, "Private export cleanup failed");
        if (error && *error) g_prefix_error (error, "%s; ", cleanup->message);
        else g_propagate_error (error, g_steal_pointer (&cleanup));
    }
    CloseHandle (handle);
}

static gssize
private_write (GOutputStream *stream, const void *buffer, gsize size,
               GCancellable *cancellable, GError **error)
{
    OtpPrivateStream *self = (OtpPrivateStream *) stream;
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) {
        self->failed = TRUE;
        return -1;
    }
    DWORD written;
    if (WriteFile (self->handle, buffer, (DWORD) MIN (size, G_MAXINT), &written, NULL))
        return written;
    self->failed = TRUE;
    windows_error (error, "Private export write failed");
    return -1;
}

static gboolean
private_close (GOutputStream *stream, GCancellable *cancellable, GError **error)
{
    OtpPrivateStream *self = (OtpPrivateStream *) stream;
    if (self->handle == INVALID_HANDLE_VALUE) return TRUE;
    gboolean cancelled = g_cancellable_set_error_if_cancelled (cancellable, error);
    gboolean ok = FALSE;
    if (!cancelled && !self->failed) {
        gsize bytes = (gsize) self->destination_chars * sizeof (WCHAR);
        /* Include FileName[1], structure padding and a UTF-16 terminator.
         * FileNameLength counts only the name bytes, never the terminator. */
        gsize size = sizeof (FILE_RENAME_INFO) + bytes;
        g_autofree FILE_RENAME_INFO *rename = g_malloc0 (size);
        rename->ReplaceIfExists = TRUE;
        rename->RootDirectory = NULL;
        rename->FileNameLength = (DWORD) bytes;
        memcpy (rename->FileName, self->destination, bytes);
        rename->FileName[self->destination_chars] = L'\0';
        /* WINDOWS-STUB: file contents are flushed, but parent-directory
         * durability after replacement is not guaranteed on Windows. */
        /* Rename the held object, never reopen a temporary by name. Verify
         * its security again before reporting successful publication. */
        ok = FlushFileBuffers (self->handle) &&
             SetFileInformationByHandle (self->handle, FileRenameInfo, rename, (DWORD) size);
        if (!ok) windows_error (error, "Private export flush/replace failed");
        else if (!otp_private_handle (self->handle, self->user->User.Sid)) {
            ok = FALSE;
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                 "Private export DACL verification failed after replacement");
        }
    } else if (!cancelled) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Private export aborted after write failure");
    }
    if (ok) CloseHandle (self->handle);
    else discard_handle (self->handle, error);
    self->handle = INVALID_HANDLE_VALUE;
    return ok;
}

static void
private_finalize (GObject *object)
{
    OtpPrivateStream *self = (OtpPrivateStream *) object;
    if (self->handle != INVALID_HANDLE_VALUE) discard_handle (self->handle, NULL);
    g_free (self->destination);
    g_free (self->user);
    G_OBJECT_CLASS (otp_private_stream_parent_class)->finalize (object);
}

static void
otp_private_stream_class_init (OtpPrivateStreamClass *klass)
{
    GOutputStreamClass *output = G_OUTPUT_STREAM_CLASS (klass);
    output->write_fn = private_write;
    output->close_fn = private_close;
    G_OBJECT_CLASS (klass)->finalize = private_finalize;
}

static void
otp_private_stream_init (OtpPrivateStream *self)
{
    self->handle = INVALID_HANDLE_VALUE;
}

GFileOutputStream *
otp_file_replace (GFile *file, const char *etag, gboolean backup, GFileCreateFlags flags,
                  GCancellable *cancellable, GError **error)
{
    if (!(flags & G_FILE_CREATE_PRIVATE))
        return g_file_replace (file, etag, backup, flags, cancellable, error);
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) return NULL;
    if (etag || backup || (flags & ~(G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION))) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Private Windows exports do not support etags, backups or extra flags");
        return NULL;
    }
    g_autofree gchar *path = g_file_get_path (file);
    g_autofree TOKEN_USER *user = otp_current_user ();
    g_autofree gunichar2 *wide = path ? g_utf8_to_utf16 (path, -1, NULL, NULL, error) : NULL;
    if (!wide || !user) {
        if (!error || !*error)
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot resolve private export path/user");
        return NULL;
    }
    G_STATIC_ASSERT (sizeof (gunichar2) == sizeof (WCHAR));
    DWORD length = GetFullPathNameW ((WCHAR *) wide, 0, NULL, NULL);
    if (!length) {
        windows_error (error, "Cannot resolve private export path");
        return NULL;
    }
    /* The sizing result includes NUL; the successful result excludes it. */
    if (length > (G_MAXUINT32 - sizeof (FILE_RENAME_INFO)) / sizeof (WCHAR)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                             "Private export path is too long");
        return NULL;
    }
    g_autofree WCHAR *destination = g_new0 (WCHAR, length);
    DWORD actual = GetFullPathNameW ((WCHAR *) wide, length, destination, NULL);
    if (!actual) {
        windows_error (error, "Cannot resolve private export path");
        return NULL;
    }
    if (actual >= length) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                             "Private export path changed during resolution");
        return NULL;
    }
    destination[actual] = L'\0';
    DWORD acl_size = sizeof (ACL) + sizeof (ACCESS_ALLOWED_ACE) - sizeof (DWORD) +
                     GetLengthSid (user->User.Sid);
    g_autofree ACL *acl = g_malloc0 (acl_size);
    SECURITY_DESCRIPTOR descriptor;
    if (!InitializeAcl (acl, acl_size, ACL_REVISION) ||
        !AddAccessAllowedAce (acl, ACL_REVISION, FILE_ALL_ACCESS, user->User.Sid) ||
        !InitializeSecurityDescriptor (&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner (&descriptor, user->User.Sid, FALSE) ||
        !SetSecurityDescriptorDacl (&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl (&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        windows_error (error, "Cannot construct private export DACL");
        return NULL;
    }
    SECURITY_ATTRIBUTES attributes = { sizeof (attributes), &descriptor, FALSE };
    g_autofree gchar *uuid = g_uuid_string_random ();
    g_autofree gchar *absolute = g_utf16_to_utf8 ((gunichar2 *) destination, actual, NULL, NULL, error);
    if (!absolute) return NULL;
    /* Use the same resolved directory even if the process cwd changes. */
    g_autofree gchar *temporary = g_strconcat (absolute, ".otp-private-", uuid, NULL);
    g_autofree gunichar2 *temp_wide = g_utf8_to_utf16 (temporary, -1, NULL, NULL, error);
    if (!temp_wide) return NULL;
    HANDLE handle = CreateFileW ((WCHAR *) temp_wide, GENERIC_WRITE | READ_CONTROL | DELETE,
        0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        windows_error (error, "Cannot create private export");
        return NULL;
    }
    if (!otp_private_handle (handle, user->User.Sid)) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "Private export requires a filesystem enforcing a protected user-only DACL");
        discard_handle (handle, error);
        return NULL;
    }
    OtpPrivateStream *stream = g_object_new (otp_private_stream_get_type (), NULL);
    stream->handle = handle;
    stream->destination = g_steal_pointer (&destination);
    stream->destination_chars = actual;
    stream->user = g_steal_pointer (&user);
    return G_FILE_OUTPUT_STREAM (stream);
}
