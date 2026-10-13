#include <windows.h>
#include <glib.h>
#include "lock-events.h"
#include "lock-policy.h"

typedef struct {
    OtpWindowsLockState state;
    DWORD thread;
} Probe;

static void
on_event (int kind, unsigned event, void *data)
{
    Probe *probe = data;
    g_assert_cmpuint (GetCurrentThreadId (), ==, probe->thread);
    if (kind == OTP_LOCK_EVENT_SESSION) {
        OtpWindowsLockAction action = otp_windows_lock_session_event (&probe->state, event, 1);
        g_print ("WTS event=%u session-locked=%d changed=%d lock-request=%d thread=main\n",
                 event, action.session_locked, action.changed, action.lock_database);
    } else {
        g_print ("SUSPEND lock-request=1 thread=main\n");
    }
}

static gboolean
quit_loop (gpointer data)
{
    g_main_loop_quit (data);
    return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
    guint seconds = 120;
    if (argc > 2)
        return 2;
    if (argc == 2) {
        gchar *end = NULL;
        guint64 value = g_ascii_strtoull (argv[1], &end, 10);
        if (end == argv[1] || *end != '\0' || value == 0 || value > 3600)
            return 2;
        seconds = (guint) value;
    }
    Probe probe = { .thread = GetCurrentThreadId () };
    OtpWindowsLockEvents *events = otp_windows_lock_events_start (on_event, &probe);
    if (events == NULL)
        return 1;
    GMainLoop *loop = g_main_loop_new (NULL, FALSE);
    g_timeout_add_seconds (seconds, quit_loop, loop);
    g_print ("Observe WTS/power events for %u seconds; no database or secrets loaded.\n", seconds);
    g_main_loop_run (loop);
    otp_windows_lock_events_stop (events);
    g_main_loop_unref (loop);
    return 0;
}
