#include "compat/otp-windows-gui.h"
#include "compat/glib-unix.h"
#include <errno.h>

guint
otp_windows_unix_signal_add (gint signum, GSourceFunc handler, gpointer data)
{
    (void) handler;
    (void) data;
    /* WINDOWS-STUB: no asynchronous SIGINT/SIGTERM/SIGHUP cleanup. Calling
     * GTK from a console handler thread is unsafe. A future control handler
     * must dispatch to the owning main context and handle application lifetime.
     * SIGHUP is already aliased to SIGBREAK by the common compatibility header. */
    g_warning ("WINDOWS-STUB: GUI signal %d is unsupported; no source installed", signum);
    errno = ENOSYS;
    return 0;
}

guint
otp_windows_getuid (void)
{
    /* WINDOWS-STUB: a diagnostic sentinel, not a Windows user identity. The
     * unguarded Flatpak path predicate still needs a platform routing edit. */
    g_warning ("WINDOWS-STUB: no POSIX uid for document-portal path detection");
    errno = ENOSYS;
    return G_MAXUINT;
}
