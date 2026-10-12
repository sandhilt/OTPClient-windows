#include "compat/otp-windows.h"
#include "compat/sys/file.h"
#include "compat/termios.h"
#include "compat/uuid/uuid.h"
#include "compat/libsecret/secret.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

void
otp_explicit_bzero (void *buffer, size_t length)
{
    SecureZeroMemory (buffer, length);
}

void
otp_free (gpointer buffer)
{
    /* The fd contents adapter returns secure memory to the unchanged URI
     * parser. Its explicit g_free calls must retain the correct allocator. */
    if (buffer && gcry_is_secure (buffer)) gcry_free (buffer);
    else g_free (buffer);
}

int
otp_open (const char *path, int flags, ...)
{
    int mode = 0600;
    if (flags & O_CREAT) {
        va_list args;
        va_start (args, flags);
        mode = va_arg (args, int);
        va_end (args);
    }
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (path, -1, NULL, NULL, NULL);
    if (!wide) { errno = EINVAL; return -1; }
    if (!(flags & O_NOFOLLOW))
        return _wopen ((const wchar_t *) wide, flags | _O_BINARY, mode);

    /* O_NOFOLLOW users in this core are read-only. Refuse unsupported uses. */
    if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) != 0) {
        errno = EINVAL;
        return -1;
    }
    HANDLE handle = CreateFileW ((const wchar_t *) wide, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) { errno = EACCES; return -1; }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle (handle, &info) ||
        GetFileType (handle) != FILE_TYPE_DISK) {
        CloseHandle (handle);
        errno = EACCES;
        return -1;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        CloseHandle (handle);
        errno = ELOOP;
        return -1;
    }
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        CloseHandle (handle);
        errno = EISDIR;
        return -1;
    }
    int fd = _open_osfhandle ((intptr_t) handle, _O_RDONLY | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) CloseHandle (handle);
    return fd;
}

int
otp_chmod (const char *path, int mode)
{
    /* WINDOWS-STUB: chmod does not install a private DACL. Only private GIO
     * exports use the secure creation adapter; other creation paths do not. */
    (void) path;
    (void) mode;
    return 0;
}

int
otp_umask (int mode)
{
    /* WINDOWS-STUB: umask does not constrain CRT/GLib creation. The private
     * export adapter installs its own DACL independently of this function. */
    (void) mode;
    return 0;
}

int
otp_fsync (int fd)
{
    /* WINDOWS-STUB: _commit flushes files; the existing parent-directory open
     * fails on Windows and the upstream caller warns. Directory durability
     * is not implemented, and crash-safe replacement remains unverified. */
    return _commit (fd);
}

gint
otp_mkstemp_full (gchar *template_name, gint flags, gint mode)
{
    return g_mkstemp_full (template_name, flags | _O_BINARY, mode);
}

int
flock (int fd, int operation)
{
    intptr_t raw_handle = _get_osfhandle (fd);
    HANDLE handle = (HANDLE) raw_handle;
    OVERLAPPED overlap = {0};
    BOOL ok;
    if (handle == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }
    if (operation & LOCK_UN) {
        ok = UnlockFileEx (handle, 0, MAXDWORD, MAXDWORD, &overlap);
    } else {
        DWORD flags = (operation & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
        if (operation & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;
        ok = LockFileEx (handle, flags, 0, MAXDWORD, MAXDWORD, &overlap);
    }
    if (ok) return 0;
    switch (GetLastError ()) {
    case ERROR_LOCK_VIOLATION: errno = EWOULDBLOCK; break;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
    case ERROR_CALL_NOT_IMPLEMENTED: errno = ENOSYS; break;
    default: errno = EIO; break;
    }
    return -1;
}

int
tcgetattr (int fd, struct termios *state)
{
    intptr_t raw_handle = _get_osfhandle (fd);
    if (GetConsoleMode ((HANDLE) raw_handle, &state->c_lflag)) return 0;
    errno = ENOTTY;
    return -1;
}

int
tcsetattr (int fd, int action, const struct termios *state)
{
    if (action != TCSANOW && action != TCSADRAIN && action != TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    intptr_t raw_handle = _get_osfhandle (fd);
    if (action == TCSAFLUSH) {
        if (!FlushConsoleInputBuffer ((HANDLE) raw_handle)) {
            errno = ENOTTY;
            return -1;
        }
        otp_console_flush ((HANDLE) raw_handle);
    }
    if (SetConsoleMode ((HANDLE) raw_handle, state->c_lflag)) return 0;
    errno = ENOTTY;
    return -1;
}

int
otp_sigemptyset (otp_sigset_t *set)
{
    *set = 0;
    return 0;
}

ssize_t
otp_getline (char **line, size_t *capacity, FILE *stream)
{
    size_t length = 0;
    int ch;
    while ((ch = fgetc (stream)) != EOF) {
        if (*line == NULL || length + 1 >= *capacity) {
            size_t next = *capacity < 128 ? 128 : *capacity * 2;
            char *resized = realloc (*line, next);
            if (!resized) { errno = ENOMEM; return -1; }
            *line = resized;
            *capacity = next;
        }
        (*line)[length++] = (char) ch;
        if (ch == '\n') break;
    }
    if (length == 0) return -1;
    (*line)[length] = '\0';
    return (ssize_t) length;
}

void
uuid_generate_random (uuid_t uuid)
{
    g_autofree gchar *text = g_uuid_string_random ();
    const gchar *p = text;
    for (int i = 0; i < 16; i++) {
        if (*p == '-') p++;
        uuid[i] = (g_ascii_xdigit_value (p[0]) << 4) | g_ascii_xdigit_value (p[1]);
        p += 2;
    }
}

void
uuid_unparse_lower (const uuid_t uuid, char *text)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) *text++ = '-';
        *text++ = hex[uuid[i] >> 4];
        *text++ = hex[uuid[i] & 15];
    }
    *text = '\0';
}

/* Keep the upstream fd-path spelling as a private transport token. These
 * adapters read the already checked descriptor; they never reopen its path.
 * Each sequential parser pass resets the offset, as /proc reopening did. */
static int
import_fd (const gchar *path)
{
    const char prefix[] = "/proc/self/fd/";
    if (!g_str_has_prefix (path, prefix)) return -1;
    char *end;
    gint64 value = g_ascii_strtoll (path + sizeof (prefix) - 1, &end, 10);
    if (*end || value < 0 || value > G_MAXINT) return -1;
    return (int) value;
}

GFile *
otp_file_new_for_path (const gchar *path)
{
    GFile *file = g_file_new_for_path (path);
    int fd = import_fd (path);
    if (fd >= 0) g_object_set_data (G_OBJECT (file), "otp-import-fd", GINT_TO_POINTER (fd + 1));
    return file;
}

gboolean
otp_file_test (const gchar *path, GFileTest test)
{
    int fd = import_fd (path);
    if (fd < 0) return g_file_test (path, test);
    struct stat st;
    if (OTP_CRT_FSTAT (fd, &st) != 0) return FALSE;
    return (test & G_FILE_TEST_EXISTS) ||
           ((test & G_FILE_TEST_IS_REGULAR) && S_ISREG (st.st_mode));
}

GFileInfo *
otp_file_query_info (GFile *file, const char *attributes, GFileQueryInfoFlags flags,
                     GCancellable *cancellable, GError **error)
{
    gpointer marker = g_object_get_data (G_OBJECT (file), "otp-import-fd");
    if (!marker) return g_file_query_info (file, attributes, flags, cancellable, error);
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) return NULL;
    struct stat st;
    if (OTP_CRT_FSTAT (GPOINTER_TO_INT (marker) - 1, &st) != 0) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot stat import descriptor");
        return NULL;
    }
    GFileInfo *info = g_file_info_new ();
    g_file_info_set_size (info, st.st_size);
    return info;
}

typedef struct { GFileInputStream parent; int fd; } OtpFdStream;
typedef struct { GFileInputStreamClass parent; } OtpFdStreamClass;
GType otp_fd_stream_get_type (void);
G_DEFINE_TYPE (OtpFdStream, otp_fd_stream, G_TYPE_FILE_INPUT_STREAM)

static gssize
fd_stream_read (GInputStream *stream, void *buffer, gsize count,
                GCancellable *cancellable, GError **error)
{
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) return -1;
    int result = _read (((OtpFdStream *) stream)->fd, buffer, (unsigned) MIN (count, G_MAXINT));
    if (result < 0)
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno), "Import read failed: %s", g_strerror (errno));
    return result;
}

static gboolean
fd_stream_close (GInputStream *stream, GCancellable *cancellable, GError **error)
{
    (void) cancellable;
    (void) error;
    OtpFdStream *self = (OtpFdStream *) stream;
    if (self->fd >= 0) { _close (self->fd); self->fd = -1; }
    return TRUE;
}

static void
fd_stream_finalize (GObject *object)
{
    OtpFdStream *self = (OtpFdStream *) object;
    if (self->fd >= 0) _close (self->fd);
    G_OBJECT_CLASS (otp_fd_stream_parent_class)->finalize (object);
}

static goffset
fd_stream_tell (GFileInputStream *stream)
{
    return _lseeki64 (((OtpFdStream *) stream)->fd, 0, SEEK_CUR);
}

static gboolean
fd_stream_can_seek (GFileInputStream *stream)
{
    return fd_stream_tell (stream) >= 0;
}

static gboolean
fd_stream_seek (GFileInputStream *stream, goffset offset, GSeekType type,
                GCancellable *cancellable, GError **error)
{
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) return FALSE;
    int origin;
    switch (type) {
    case G_SEEK_SET: origin = SEEK_SET; break;
    case G_SEEK_CUR: origin = SEEK_CUR; break;
    case G_SEEK_END: origin = SEEK_END; break;
    default:
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid seek type");
        return FALSE;
    }
    if (_lseeki64 (((OtpFdStream *) stream)->fd, offset, origin) >= 0) return TRUE;
    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                 "Import seek failed: %s", g_strerror (errno));
    return FALSE;
}

static void
otp_fd_stream_class_init (OtpFdStreamClass *klass)
{
    GInputStreamClass *input = G_INPUT_STREAM_CLASS (klass);
    input->read_fn = fd_stream_read;
    input->close_fn = fd_stream_close;
    GFileInputStreamClass *file = G_FILE_INPUT_STREAM_CLASS (klass);
    file->tell = fd_stream_tell;
    file->can_seek = fd_stream_can_seek;
    file->seek = fd_stream_seek;
    G_OBJECT_CLASS (klass)->finalize = fd_stream_finalize;
}

static void
otp_fd_stream_init (OtpFdStream *self)
{
    self->fd = -1;
}

GFileInputStream *
otp_file_read (GFile *file, GCancellable *cancellable, GError **error)
{
    gpointer marker = g_object_get_data (G_OBJECT (file), "otp-import-fd");
    if (!marker) return g_file_read (file, cancellable, error);
    if (g_cancellable_set_error_if_cancelled (cancellable, error)) return NULL;
    int fd = _dup (GPOINTER_TO_INT (marker) - 1);
    if (fd < 0 || _lseeki64 (fd, 0, SEEK_SET) < 0) {
        if (fd >= 0) _close (fd);
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot duplicate import descriptor");
        return NULL;
    }
    OtpFdStream *stream = g_object_new (otp_fd_stream_get_type (), NULL);
    stream->fd = fd;
    return G_FILE_INPUT_STREAM (stream);
}

gboolean
otp_file_get_contents (const gchar *path, gchar **contents, gsize *length, GError **error)
{
    int fd = import_fd (path);
    if (fd < 0) return g_file_get_contents (path, contents, length, error);
    struct stat st;
    *contents = NULL;
    if (OTP_CRT_FSTAT (fd, &st) != 0 || st.st_size < 0 || st.st_size > 64 * 1024 * 1024 ||
        _lseeki64 (fd, 0, SEEK_SET) < 0) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Invalid import descriptor or size");
        return FALSE;
    }
    /* Do not introduce an ordinary-heap copy of imported secrets. The Windows
     * header routes the parser's explicit frees through otp_free. */
    gchar *data = gcry_malloc_secure ((gsize) st.st_size + 1);
    if (!data) {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Cannot allocate import buffer");
        return FALSE;
    }
    gsize used = 0;
    while (used < (gsize) st.st_size) {
        int n = _read (fd, data + used, (unsigned) MIN ((gsize) st.st_size - used, G_MAXINT));
        if (n <= 0) {
            SecureZeroMemory (data, used);
            gcry_free (data);
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Short import read");
            return FALSE;
        }
        used += n;
    }
    data[used] = '\0';
    *contents = data;
    if (length) *length = used;
    return TRUE;
}

static size_t
json_fd_read (void *buffer, size_t length, void *user_data)
{
    int n = _read (GPOINTER_TO_INT (user_data), buffer, (unsigned) MIN (length, G_MAXINT));
    return n < 0 ? (size_t) -1 : (size_t) n;
}

json_t *
otp_json_load_file (const char *path, size_t flags, json_error_t *error)
{
    if (error) memset (error, 0, sizeof (*error));
    int fd = import_fd (path);
    if (fd >= 0 && _lseeki64 (fd, 0, SEEK_SET) < 0) {
        if (error) {
            g_strlcpy (error->source, path, sizeof (error->source));
            g_snprintf (error->text, sizeof (error->text), "Cannot seek import descriptor: %s", g_strerror (errno));
        }
        return NULL;
    }
    json_t *result = fd < 0 ? json_load_file (path, flags, error) :
        json_load_callback (json_fd_read, GINT_TO_POINTER (fd), flags, error);
    if (!result && error && !error->text[0])
        g_strlcpy (error->text, "Cannot read import JSON", sizeof (error->text));
    return result;
}

/* WINDOWS-STUB: no Secret Service or native credential backend. All operations
 * fail explicitly; async failures preserve GTask callback/cancellation flow. */
static void
secret_unavailable (GCancellable *cancellable, GError **error)
{
    if (!g_cancellable_set_error_if_cancelled (cancellable, error))
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "WINDOWS-STUB: secret service unavailable");
}

gboolean
secret_password_store_sync (const SecretSchema *schema, const gchar *collection,
    const gchar *label, const gchar *password, GCancellable *cancellable, GError **error, ...)
{
    (void) schema; (void) collection; (void) label; (void) password;
    secret_unavailable (cancellable, error);
    return FALSE;
}

gchar *
secret_password_lookup_sync (const SecretSchema *schema, GCancellable *cancellable, GError **error, ...)
{
    (void) schema;
    secret_unavailable (cancellable, error);
    return NULL;
}

gboolean
secret_password_clear_sync (const SecretSchema *schema, GCancellable *cancellable, GError **error, ...)
{
    (void) schema;
    secret_unavailable (cancellable, error);
    return FALSE;
}

static void
secret_fail_async (GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
    GTask *task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             "WINDOWS-STUB: secret service unavailable");
    g_object_unref (task);
}

void
secret_password_store (const SecretSchema *schema, const gchar *collection,
    const gchar *label, const gchar *password, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data, ...)
{
    /* WINDOWS-STUB: fail asynchronously without retaining/copying secrets. */
    (void) schema; (void) collection; (void) label; (void) password;
    secret_fail_async (cancellable, callback, user_data);
}

void
secret_password_lookup (const SecretSchema *schema, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data, ...)
{
    (void) schema;
    secret_fail_async (cancellable, callback, user_data);
}

void
secret_password_clear (const SecretSchema *schema, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data, ...)
{
    (void) schema;
    secret_fail_async (cancellable, callback, user_data);
}

gchar *
secret_password_lookup_finish (GAsyncResult *result, GError **error)
{
    g_task_propagate_boolean (G_TASK (result), error);
    return NULL;
}

gboolean
secret_password_clear_finish (GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
secret_password_store_finish (GAsyncResult *result, GError **error)
{
    return g_task_propagate_boolean (G_TASK (result), error);
}

void
secret_password_free (gchar *password)
{
    if (password) { SecureZeroMemory (password, strlen (password)); gcry_free (password); }
}
