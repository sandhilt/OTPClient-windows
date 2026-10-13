/* Windows source replacement for src/gui/lock-app.c at ec79d9f.
 * The dialog/manual-lock functions and inactivity_check below are copied
 * unchanged. Recheck these blocks whenever upstream lock-app.c changes.
 * Keep the public lock-app.h API; only the desktop event backend differs. */
#include <glib/gi18n.h>
#include "lock-app.h"
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "dialogs/password-dialog.h"
#include "db-common.h"
#include "tray.h"
#include "lock-events.h"
#include "lock-policy.h"

typedef struct
{
    OTPClientApplication *app;
    OtpWindowsLockEvents *events;
    OtpWindowsLockState session;
    gboolean session_locked;
    guint inactivity_timer_id;
    gint64 last_user_activity;
} LockData;

static LockData *lock_data = NULL;

static gboolean on_unlock_password (const gchar  *current_password,
                                    const gchar  *password,
                                    gchar       **error_message,
                                    gpointer      user_data);

static void
on_unlock_dialog_closed (AdwDialog *dialog,
                         gpointer   user_data)
{
    (void) dialog;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);

    /* An unlock submit is in flight: on_unlock_done loads the DB on success or
     * re-presents this dialog on failure. Don't pre-empt it. */
    if (otpclient_application_is_unlocking (app))
        return;

    /* Closed right after a successful unlock: the DB is open, nothing to do. */
    if (otpclient_application_is_db_unlocked (app))
        return;

    /* The user dismissed the prompt without unlocking (Escape, dialog X,
     * click-outside, or the parent window's close button). Drop to the locked
     * page instead of quitting or re-presenting, so the toolbar (Settings,
     * unlock) stays reachable (#467). */
    lock_app_enter_locked_state (app);
}

static void
on_unlock_dialog_quit_requested (PasswordDialog       *dlg,
                                 OTPClientApplication *app)
{
    (void) dlg;
    g_application_quit (G_APPLICATION (app));
}

void
lock_app_install_unlock_dialog_quit (PasswordDialog       *dlg,
                                     OTPClientApplication *app)
{
    g_return_if_fail (PASSWORD_IS_DIALOG (dlg));
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    password_dialog_set_locked_mode (dlg);
    g_signal_connect_object (dlg, "quit-requested",
                             G_CALLBACK (on_unlock_dialog_quit_requested),
                             app, 0);
    g_signal_connect_object (dlg, "closed",
                             G_CALLBACK (on_unlock_dialog_closed),
                             app, 0);
}

static void
present_unlock_dialog_with_error (OTPClientApplication *app,
                                  const gchar          *error_message)
{
    GtkWindow *win = otpclient_application_get_window (app);
    if (win == NULL)
        return;

    PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                               on_unlock_password,
                                               app);
    /* A retry after a failed unlock arrives here with the reason the last
     * attempt failed; without it the prompt would simply reappear with no
     * indication that the password was wrong. */
    password_dialog_set_initial_error (dlg, error_message);
    lock_app_install_unlock_dialog_quit (dlg, app);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (win));
}

void
lock_app_present_unlock_dialog (OTPClientApplication *app)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));
    if (otpclient_application_is_unlocking (app))
        return;
    present_unlock_dialog_with_error (app, NULL);
}

void
lock_app_present_unlock_dialog_with_error (OTPClientApplication *app,
                                           const gchar          *error_message)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));
    if (otpclient_application_is_unlocking (app))
        return;
    present_unlock_dialog_with_error (app, error_message);
}

static gboolean
on_unlock_password (const gchar  *current_password,
                    const gchar  *password,
                    gchar       **error_message,
                    gpointer      user_data)
{
    (void) current_password;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);
    if (password == NULL)
        return FALSE;
    return otpclient_application_submit_unlock_password (app, password,
                                                         error_message);
}

void
lock_app_enter_locked_state (OTPClientApplication *app)
{
    if (otpclient_application_get_app_locked (app)) {
        if (otpclient_application_is_unlocking (app))
            otpclient_application_purge_secrets (app);
        return;
    }

    otpclient_application_set_app_locked (app, TRUE);

    GtkWindow *win = otpclient_application_get_window (app);
    if (win != NULL && OTPCLIENT_IS_WINDOW (win))
    {
        otpclient_window_secure_lock_cleanup (OTPCLIENT_WINDOW (win));
        otpclient_window_set_locked_indicator (OTPCLIENT_WINDOW (win), TRUE);
        otpclient_window_set_db_actions_enabled (OTPCLIENT_WINDOW (win), FALSE);
        otpclient_window_refresh_content_page (OTPCLIENT_WINDOW (win));
    }

    otpclient_application_purge_secrets (app);
}

void
lock_app_lock (OTPClientApplication *app)
{
    gboolean was_locked = otpclient_application_get_app_locked (app);

    lock_app_enter_locked_state (app);

    /* enter_locked_state is a no-op when already locked; only pop a fresh
     * unlock dialog when this call actually transitioned into the locked
     * state. */
    if (!was_locked)
        present_unlock_dialog_with_error (app, NULL);
}

void
lock_app_unlock (OTPClientApplication *app)
{
    otpclient_application_set_app_locked (app, FALSE);

    GtkWindow *win = otpclient_application_get_window (app);
    if (win != NULL && OTPCLIENT_IS_WINDOW (win))
    {
        otpclient_window_set_locked_indicator (OTPCLIENT_WINDOW (win), FALSE);
        otpclient_window_set_db_actions_enabled (OTPCLIENT_WINDOW (win), TRUE);
        otpclient_window_refresh_content_page (OTPCLIENT_WINDOW (win));
    }

    if (lock_data != NULL)
        lock_data->last_user_activity = g_get_monotonic_time ();
}

void
lock_app_reset_inactivity (OTPClientApplication *app)
{
    (void) app;
    if (lock_data != NULL)
        lock_data->last_user_activity = g_get_monotonic_time ();
}

gboolean
lock_app_get_session_locked (OTPClientApplication *app)
{
    return lock_data != NULL && lock_data->app == app && lock_data->session_locked;
}

static gboolean
inactivity_check (gpointer user_data)
{
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);

    if (!otpclient_application_get_auto_lock (app))
        return G_SOURCE_CONTINUE;

    if (otpclient_application_get_app_locked (app))
        return G_SOURCE_CONTINUE;

    gint timeout = otpclient_application_get_inactivity_timeout (app);
    if (timeout <= 0)
        return G_SOURCE_CONTINUE;

    gint64 now = g_get_monotonic_time ();
    gint64 elapsed = (now - lock_data->last_user_activity) / G_USEC_PER_SEC;

    if (elapsed >= timeout)
        lock_app_lock (app);

    return G_SOURCE_CONTINUE;
}

static void
on_windows_lock_event (int kind, unsigned event, void *user_data)
{
    LockData *data = user_data;
    if (lock_data != data)
        return;

    gboolean auto_lock = otpclient_application_get_auto_lock (data->app);
    if (kind == OTP_LOCK_EVENT_SESSION) {
        OtpWindowsLockAction action = otp_windows_lock_session_event (&data->session,
                                                                     event, auto_lock);
        data->session_locked = action.session_locked;
#ifdef ENABLE_MINIMIZE_TO_TRAY
        if (action.changed)
            otpclient_tray_notify_session_locked_changed (data->app);
#endif
        /* Track session state even when Auto-Lock is off, as on Linux.
         * Unlock/connect only update this state; never unlock the database. */
        if (action.lock_database)
            lock_app_lock (data->app);
    } else if (kind == OTP_LOCK_EVENT_SUSPEND && auto_lock) {
        lock_app_lock (data->app);
    }
}

void
lock_app_init_dbus_watchers (OTPClientApplication *app)
{
    /* Historical API name retained; this implementation does not use D-Bus. */
    /* WINDOWS-STUB: non-locking screensaver activation is not a WTS session
     * lock and is not observed. Session state means locked or disconnected
     * here, rather than the Linux union of screensaver ActiveChanged values. */
    if (lock_data != NULL)
        return;

    lock_data = g_new0 (LockData, 1);
    lock_data->app = app;
    lock_data->last_user_activity = g_get_monotonic_time ();
    lock_data->events = otp_windows_lock_events_start (on_windows_lock_event, lock_data);
    /* Manual and inactivity lock remain usable if native registration fails.
     * The backend emits explicit warnings for each unavailable event path. */
    lock_data->inactivity_timer_id = g_timeout_add_seconds (1, inactivity_check, app);
}

void
lock_app_cleanup (OTPClientApplication *app)
{
    (void) app;
    if (lock_data == NULL)
        return;

    otp_windows_lock_events_stop (lock_data->events);
    if (lock_data->inactivity_timer_id != 0)
        g_source_remove (lock_data->inactivity_timer_id);
    g_free (lock_data);
    lock_data = NULL;
}
