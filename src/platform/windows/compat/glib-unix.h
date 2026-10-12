#pragma once
#ifndef _WIN32
#error "Windows compatibility must never be included in a native build"
#endif
#include <glib.h>
#include <signal.h>

G_BEGIN_DECLS
/* WINDOWS-STUB: only the signal-add surface used by the GUI is provided.
 * No signal source or console-control handler is installed; zero means failure.
 * This header does not supply Unix fd sources, pipes or GUnixFDList. */
guint otp_windows_unix_signal_add (gint signum, GSourceFunc handler, gpointer data);
#define g_unix_signal_add otp_windows_unix_signal_add
G_END_DECLS
