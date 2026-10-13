#ifdef ENABLE_MINIMIZE_TO_TRAY

#include "tray.h"
#include "lock-app.h"
#include "tray-win-shell.h"
#include <glib/gi18n.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define TRAY_LOST_GRACE_SECONDS 3
#define START_HIDDEN_DEADLINE_SECONDS 10
#define APP_ICON_NAME "com.github.paolostivanin.OTPClient"

typedef struct
{
    OTPClientApplication *app; /* borrowed, cleanup runs during app dispose */
    OtpWindowsTrayShell *shell;
    HICON icon;
    DWORD thread_id;
    gulong close_handler;
    guint hidden_deadline_source;
    guint hidden_deadline_seconds;
    guint action_source;
    GQueue actions;
    gboolean desired;
    gboolean holding;
    gboolean window_hidden;
} TrayData;

static TrayData *tray_data;

static void
clear_deadline (TrayData *td)
{
    g_clear_handle_id (&td->hidden_deadline_source, g_source_remove);
    td->hidden_deadline_seconds = 0;
}

static void
mark_window_shown (TrayData *td)
{
    td->window_hidden = FALSE;
    clear_deadline (td);
}

static void
show_window (TrayData *td)
{
    mark_window_shown (td);
    if (otpclient_application_get_window (td->app) != NULL)
        /* Preserve deferred unlock and startup dialogs. Never bypass this
         * funnel with gtk_window_present or a direct Win32 ShowWindow. */
        otpclient_application_present_window (td->app);
}

static gboolean
on_hidden_deadline (gpointer data)
{
    TrayData *td = data;
    td->hidden_deadline_source = 0;
    if (lock_app_get_session_locked (td->app))
        return G_SOURCE_REMOVE; /* retain purpose until session unlock */
    td->hidden_deadline_seconds = 0;
    if (td->window_hidden && !otp_windows_tray_shell_is_published (td->shell))
        show_window (td);
    return G_SOURCE_REMOVE;
}

static void
arm_deadline (TrayData *td, guint seconds)
{
    g_clear_handle_id (&td->hidden_deadline_source, g_source_remove);
    td->hidden_deadline_seconds = seconds;
    if (!lock_app_get_session_locked (td->app))
        td->hidden_deadline_source = g_timeout_add_seconds (seconds, on_hidden_deadline, td);
}

static gboolean
on_close_request (GtkWindow *window, gpointer data)
{
    TrayData *td = data;
    if (otp_windows_tray_shell_is_published (td->shell) &&
        otpclient_application_get_minimize_to_tray (td->app))
    {
        gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
        td->window_hidden = TRUE;
        return TRUE;
    }
    return FALSE;
}

static gboolean
perform_action (gpointer data)
{
    TrayData *td = data;
    OtpWindowsTrayEvent event = GPOINTER_TO_UINT (g_queue_pop_head (&td->actions)) - 1;
    gboolean more = !g_queue_is_empty (&td->actions);
    if (!more)
        td->action_source = 0;
    switch (event)
    {
        case OTP_TRAY_TOGGLE:
        {
            GtkWindow *window = otpclient_application_get_window (td->app);
            if (window != NULL && gtk_widget_get_visible (GTK_WIDGET (window)))
                on_close_request (window, td);
            else
                show_window (td);
            break;
        }
        case OTP_TRAY_SHOW:
            show_window (td);
            break;
        case OTP_TRAY_QUIT:
            g_application_quit (G_APPLICATION (td->app));
            break;
        default:
            break;
    }
    return more ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

static void
on_shell_event (OtpWindowsTrayShell *shell, OtpWindowsTrayEvent event, gpointer data)
{
    TrayData *td = data;
    if (event != OTP_TRAY_PUBLICATION_CHANGED)
    {
        /* Win32's popup runs a nested message loop. Run GTK actions on a GLib
         * idle after it unwinds, so Quit cannot free an active window proc. */
        g_queue_push_tail (&td->actions, GUINT_TO_POINTER (event + 1));
        if (td->action_source == 0)
            td->action_source = g_idle_add (perform_action, td);
        return;
    }
    gboolean published = otp_windows_tray_shell_is_published (shell);
    if (published && !td->holding)
    {
        g_application_hold (G_APPLICATION (td->app));
        td->holding = TRUE;
    }
    else if (!published && td->holding)
    {
        td->holding = FALSE;
        g_application_release (G_APPLICATION (td->app));
    }
    if (published)
        clear_deadline (td);
    else if (td->window_hidden && td->desired && td->hidden_deadline_seconds == 0)
        arm_deadline (td, TRAY_LOST_GRACE_SECONDS);
}

static HICON
load_app_icon (void)
{
    /* gui.gresource.xml contains UI, not icons. stage-gui.sh ships this exact
     * hicolor SVG and otpclient.ps1 sets XDG_DATA_DIRS to the stage's share.
     * GtkIconTheme resolves it; GdkPixbuf uses the staged SVG loader. */
    GdkDisplay *display = gdk_display_get_default ();
    if (display != NULL)
    {
        gint size = CLAMP (GetSystemMetrics (SM_CXSMICON), 16, 256);
        GtkIconTheme *theme = gtk_icon_theme_get_for_display (display);
        g_autoptr (GtkIconPaintable) paintable = gtk_icon_theme_has_icon (theme, APP_ICON_NAME)
            ? gtk_icon_theme_lookup_icon (theme, APP_ICON_NAME, NULL, size, 1, GTK_TEXT_DIR_NONE, 0)
            : NULL;
        g_autoptr (GFile) file = paintable != NULL ? gtk_icon_paintable_get_file (paintable) : NULL;
        if (file != NULL)
        {
            g_autofree gchar *path = g_file_get_path (file);
            g_autoptr (GError) error = NULL;
            g_autoptr (GdkPixbuf) pixbuf = path != NULL
                ? gdk_pixbuf_new_from_file_at_scale (path, size, size, TRUE, &error) : NULL;
            if (pixbuf != NULL)
            {
                HICON icon = otp_windows_tray_icon_from_pixels (
                    gdk_pixbuf_read_pixels (pixbuf), gdk_pixbuf_get_width (pixbuf),
                    gdk_pixbuf_get_height (pixbuf), gdk_pixbuf_get_rowstride (pixbuf),
                    gdk_pixbuf_get_n_channels (pixbuf));
                if (icon != NULL)
                    return icon;
            }
        }
    }
    /* WINDOWS-STUB: missing artwork/loader falls back to a generic Windows
     * application icon. UNVERIFIED: portable SVG loading, HiDPI/monitor changes
     * and light/dark taskbar contrast. No automatic DPI/theme icon reload. */
    g_warning ("Windows tray artwork unavailable; using the generic application icon");
    return CopyIcon (LoadIconW (NULL, MAKEINTRESOURCEW (32512)));
}

void
otpclient_tray_init (OTPClientApplication *app)
{
    if (tray_data != NULL)
        return;
    tray_data = g_new0 (TrayData, 1);
    TrayData *td = tray_data;
    td->app = app;
    td->thread_id = GetCurrentThreadId ();
    td->desired = otpclient_application_get_minimize_to_tray (app);
    td->icon = load_app_icon ();
    if (td->icon != NULL)
        td->shell = otp_windows_tray_shell_new (td->icon, _("Show OTPClient"), _("Quit"),
                                                on_shell_event, td);
    GtkWindow *window = otpclient_application_get_window (app);
    if (window != NULL)
        td->close_handler = g_signal_connect (window, "close-request",
                                              G_CALLBACK (on_close_request), td);
    if (td->shell != NULL)
    {
        otpclient_tray_notify_locked_changed (app);
        otp_windows_tray_shell_set_enabled (td->shell, td->desired);
    }
}

void
otpclient_tray_enable (OTPClientApplication *app)
{
    if (tray_data == NULL)
    {
        otpclient_tray_init (app);
        return;
    }
    g_return_if_fail (tray_data->app == app && GetCurrentThreadId () == tray_data->thread_id);
    tray_data->desired = TRUE;
    if (tray_data->shell != NULL)
        otp_windows_tray_shell_set_enabled (tray_data->shell, TRUE);
}

void
otpclient_tray_disable (OTPClientApplication *app)
{
    if (tray_data == NULL)
        return;
    g_return_if_fail (tray_data->app == app && GetCurrentThreadId () == tray_data->thread_id);
    tray_data->desired = FALSE;
    if (tray_data->shell != NULL)
        otp_windows_tray_shell_set_enabled (tray_data->shell, FALSE);
    if (tray_data->window_hidden)
        show_window (tray_data);
}

void
otpclient_tray_begin_hidden (OTPClientApplication *app)
{
    if (tray_data == NULL || tray_data->window_hidden)
        return;
    g_return_if_fail (tray_data->app == app && GetCurrentThreadId () == tray_data->thread_id);
    tray_data->window_hidden = TRUE;
    if (!otp_windows_tray_shell_is_published (tray_data->shell))
        arm_deadline (tray_data, START_HIDDEN_DEADLINE_SECONDS);
}

void
otpclient_tray_notify_window_shown (OTPClientApplication *app)
{
    if (tray_data == NULL)
        return;
    g_return_if_fail (tray_data->app == app && GetCurrentThreadId () == tray_data->thread_id);
    mark_window_shown (tray_data);
}

void
otpclient_tray_notify_locked_changed (OTPClientApplication *app)
{
    if (tray_data == NULL || tray_data->shell == NULL)
        return;
    g_return_if_fail (tray_data->app == app && GetCurrentThreadId () == tray_data->thread_id);
    otp_windows_tray_shell_set_tooltip (tray_data->shell,
        otpclient_application_get_app_locked (app) ? _("OTPClient (locked)") : "OTPClient");
}

void
otpclient_tray_notify_session_locked_changed (OTPClientApplication *app)
{
    if (tray_data == NULL)
        return;
    g_return_if_fail (tray_data->app == app && GetCurrentThreadId () == tray_data->thread_id);
    /* WINDOWS-STUB: relies on lock-app.h's session-state provider. The Linux
     * D-Bus implementation cannot supply Windows lock notifications. Pair with
     * a WTS provider before claiming native session-lock recovery parity. */
    if (lock_app_get_session_locked (app))
        g_clear_handle_id (&tray_data->hidden_deadline_source, g_source_remove);
    else if (tray_data->window_hidden &&
             !otp_windows_tray_shell_is_published (tray_data->shell) &&
             tray_data->hidden_deadline_seconds != 0 && tray_data->hidden_deadline_source == 0)
        arm_deadline (tray_data, tray_data->hidden_deadline_seconds);
}

gboolean
otpclient_tray_is_available (void)
{
    return tray_data != NULL && tray_data->shell != NULL &&
           otp_windows_tray_shell_is_available ();
}

gboolean
otpclient_tray_watcher_present_sync (void)
{
    /* Startup has already attempted NIM_ADD synchronously. Refuse hidden
     * startup on an actual registration/resource failure, even with Explorer. */
    if (tray_data != NULL && tray_data->desired)
        return otp_windows_tray_shell_is_published (tray_data->shell);
    return otp_windows_tray_shell_is_available ();
}

void
otpclient_tray_cleanup (OTPClientApplication *app)
{
    if (tray_data == NULL)
        return;
    g_return_if_fail (tray_data->app == app && GetCurrentThreadId () == tray_data->thread_id);
    TrayData *td = tray_data;
    GtkWindow *window = otpclient_application_get_window (app);
    if (window != NULL && td->close_handler != 0 &&
        g_signal_handler_is_connected (window, td->close_handler))
        g_signal_handler_disconnect (window, td->close_handler);
    clear_deadline (td);
    g_clear_handle_id (&td->action_source, g_source_remove);
    g_queue_clear (&td->actions);
    otp_windows_tray_shell_free (td->shell);
    if (td->icon != NULL)
        DestroyIcon (td->icon);
    if (td->holding)
        g_application_release (G_APPLICATION (app));
    tray_data = NULL;
    g_free (td);
}

#endif
