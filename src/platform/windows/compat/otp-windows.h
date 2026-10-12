#pragma once
#ifndef _WIN32
#error "Windows compatibility must never be included in a native build"
#endif
#include <windows.h>
#undef NO_ERROR
#undef uuid_t
#include <glib.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <gcrypt.h>
#include <jansson.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>

#define O_NOFOLLOW 0x10000000
#define O_CLOEXEC _O_NOINHERIT
typedef int otp_sigset_t;
struct sigaction { void (*sa_handler)(int); otp_sigset_t sa_mask; int sa_flags; };
#define SIGHUP SIGBREAK
int otp_sigemptyset (otp_sigset_t *set);
int otp_sigaction (int signo, const struct sigaction *action, struct sigaction *old);
void otp_explicit_bzero (void *buffer, size_t length);
void otp_free (gpointer buffer);
ssize_t otp_getline (char **line, size_t *capacity, FILE *stream);
int otp_open (const char *path, int flags, ...);
int otp_chmod (const char *path, int mode);
int otp_umask (int mode);
int otp_fsync (int fd);
gint otp_mkstemp_full (gchar *template_name, gint flags, gint mode);
gboolean otp_file_get_contents (const gchar *path, gchar **contents, gsize *length, GError **error);
json_t *otp_json_load_file (const char *path, size_t flags, json_error_t *error);
GFileInputStream *otp_file_read (GFile *file, GCancellable *cancellable, GError **error);
GFileOutputStream *otp_file_replace (GFile *file, const char *etag, gboolean backup, GFileCreateFlags flags, GCancellable *cancellable, GError **error);
GFile *otp_file_new_for_path (const gchar *path);
gboolean otp_file_test (const gchar *path, GFileTest test);
GFileInfo *otp_file_query_info (GFile *file, const char *attributes, GFileQueryInfoFlags flags, GCancellable *cancellable, GError **error);

#ifndef OTP_WINDOWS_COMPAT_IMPLEMENTATION
#define sigemptyset otp_sigemptyset
#define sigaction(...) otp_sigaction (__VA_ARGS__)
#define explicit_bzero otp_explicit_bzero
#define g_free otp_free
#define getline otp_getline
#define open otp_open
#define g_open otp_open
#define g_chmod otp_chmod
#define umask otp_umask
#define fsync otp_fsync
#define g_mkstemp_full otp_mkstemp_full
#define g_file_get_contents otp_file_get_contents
#define json_load_file otp_json_load_file
#define g_file_read otp_file_read
#define g_file_replace otp_file_replace
#define g_file_new_for_path otp_file_new_for_path
#define g_file_test otp_file_test
#define g_file_query_info otp_file_query_info
#endif
