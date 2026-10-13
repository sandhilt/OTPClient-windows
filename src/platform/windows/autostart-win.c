#include "autostart.h"
#include "autostart-policy.h"
#include "otpclient-application.h"
#include "autostart-win.h"

/* Same synchronous contract as the non-Flatpak backend. Called on the GUI
 * thread; registry APIs require no HWND, Win32 message pump or GLib source.
 * UNVERIFIED: GTK/GSettings notifications and native Windows execution.
 * No new signals: application setters retain the existing changed signals. */
static guint autostart_generation;

static gboolean
query_entry (OtpWindowsAutostartState *state, GError **error)
{
    return otp_windows_autostart_query (OTP_AUTOSTART_RUN_KEY,
                                        OTP_AUTOSTART_APPROVED_KEY,
                                        state, NULL, error);
}

static void
report_error (OTPClientApplication *app, const GError *error)
{
    g_warning ("Windows autostart: %s", error->message);
    otpclient_application_report_startup_change (app, error->message);
}

static void
report_disabled (OTPClientApplication *app)
{
    otpclient_application_set_autostart (app, FALSE);
    otpclient_application_set_startup_reconcile_pending (app, FALSE);
    /* WINDOWS-STUB: the upstream callback has no reason/error field; convey
     * the distinction through its existing startup-change report API. English
     * Windows-specific messages have no upstream translation entries yet. */
    const gchar *message = "Autostart is disabled by user in Windows Startup apps; enable OTPClient there first";
    g_message ("%s", message);
    otpclient_application_report_startup_change (app, message);
}

void
autostart_init (void)
{
    /* Like the Linux host backend, no asynchronous discovery is necessary.
     * Registry permission failures are reported per operation, not hidden. */
}

gboolean
autostart_is_supported (void)
{
    return TRUE;
}

gboolean
autostart_entry_may_exist (void)
{
    g_autoptr (GError) error = NULL;
    gboolean exists = otp_windows_autostart_exists (&error);
    if (error != NULL)
    {
        g_warning ("Windows autostart: %s", error->message);
        /* A failed read cannot prove absence; let reassert report/retry. */
        return TRUE;
    }
    return exists;
}

gboolean
autostart_adopt_existing_entry (OTPClientApplication *app)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (app), FALSE);
    if (!otpclient_application_autostart_key_is_default (app))
        return FALSE;

    OtpWindowsAutostartState state;
    g_autoptr (GError) error = NULL;
    if (!query_entry (&state, &error))
    {
        report_error (app, error);
        return FALSE;
    }
    if (state == OTP_AUTOSTART_DISABLED_BY_USER)
    {
        report_disabled (app);
        return FALSE;
    }
    if (state == OTP_AUTOSTART_ENABLED)
    {
        otpclient_application_set_autostart (app, TRUE);
        return TRUE;
    }
    return FALSE;
}

void
autostart_apply (OTPClientApplication *app, gboolean enable_autostart,
                 AutostartResultFunc done, gpointer user_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));
    const guint generation = ++autostart_generation;
    g_autoptr (GError) error = NULL;
    g_autofree gchar *path = NULL;
    if (enable_autostart)
        path = otp_windows_autostart_executable (&error);

    gboolean ok = error == NULL
        && otp_windows_autostart_write (OTP_AUTOSTART_RUN_KEY,
                                        OTP_AUTOSTART_APPROVED_KEY, path,
                                        otpclient_application_get_start_minimized (app),
                                        enable_autostart, &error);

    OtpWindowsAutostartState state;
    g_autoptr (GError) read_error = NULL;
    gboolean known = query_entry (&state, &read_error);
    AutostartResult result = {
        .autostart = ok && known
            && (enable_autostart ? state == OTP_AUTOSTART_ENABLED
                                 : state == OTP_AUTOSTART_ABSENT),
        .background = TRUE,
        .answered = known,
        .superseded = generation != autostart_generation
    };

    if (!result.superseded)
    {
        if (enable_autostart && known && state == OTP_AUTOSTART_DISABLED_BY_USER)
        {
            /* Do not use the portal policy's retry_removal here: removing a
             * Task Manager-disabled entry would erase the user's visible item.
             * Both effective settings and callback correctly say off. */
            report_disabled (app);
        }
        else
        {
            AutostartActions actions = autostart_policy_decide (
                enable_autostart, &result, autostart_is_supported (),
                otpclient_application_get_autostart (app),
                otpclient_application_get_minimize_to_tray (app));
            /* Unlike the portal, Windows exposes the actual effective state.
             * A failed overwrite may leave the old enabled value intact; a
             * failed removal may leave a disabled value. Do not infer that
             * either is simply the opposite of the requested setting. There
             * is no refused background grant requiring a cleanup request. */
            if (!result.autostart && known)
            {
                actions.autostart_value = state == OTP_AUTOSTART_ENABLED;
                actions.write_autostart = actions.autostart_value
                    != otpclient_application_get_autostart (app);
            }
            actions.pending_value = !result.autostart;
            if (actions.write_autostart)
                otpclient_application_set_autostart (app, actions.autostart_value);
            if (actions.write_pending)
                otpclient_application_set_startup_reconcile_pending (app, actions.pending_value);
            if (!result.autostart)
            {
                if (error != NULL)
                    report_error (app, error);
                else if (read_error != NULL)
                    report_error (app, read_error);
                else
                    otpclient_application_report_startup_change (app,
                        "Windows did not confirm the requested login-time entry");
            }
        }
    }
    if (done != NULL)
        done (&result, user_data);
}

void
autostart_ensure_background (OTPClientApplication *app,
                             AutostartResultFunc done, gpointer user_data)
{
    (void) app;
    /* Windows has no Flatpak Background grant. This does not create an entry
     * or claim that tray support itself is available. */
    if (done != NULL)
    {
        const AutostartResult result = { .autostart = TRUE, .background = TRUE,
                                         .answered = TRUE };
        done (&result, user_data);
    }
}

void
autostart_reassert (OTPClientApplication *app)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));
    OtpWindowsAutostartState state;
    g_autoptr (GError) error = NULL;
    if (query_entry (&state, &error) && state == OTP_AUTOSTART_DISABLED_BY_USER)
    {
        report_disabled (app);
        return;
    }
    /* Portable zip: after moving the directory, launch manually once. The
     * enabled Run value is refreshed only if path or --start-minimized changed;
     * a dead old path cannot repair itself at login. Disabled entries stay off.
     * UNVERIFIED: launch without the portable wrapper requires staged schemas,
     * settings backend and DLLs to be discoverable by the executable itself. */
    autostart_apply (app, otpclient_application_get_autostart (app), NULL, NULL);
}
