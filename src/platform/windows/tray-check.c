#include "tray-win-shell.h"
#include <string.h>

typedef struct
{
    GMainLoop *loop;
    OtpWindowsTrayShell *shell;
    DWORD thread_id;
    guint selections;
    guint publications;
    guint stage;
    gboolean self_test;
    gboolean passed;
} Probe;

static void
on_event (OtpWindowsTrayShell *shell, OtpWindowsTrayEvent event, gpointer data)
{
    Probe *probe = data;
    g_assert_cmpuint (GetCurrentThreadId (), ==, probe->thread_id);
    if (event == OTP_TRAY_PUBLICATION_CHANGED)
    {
        probe->publications++;
        g_print ("published=%d (Shell_NotifyIcon result, not visual proof)\n",
                 otp_windows_tray_shell_is_published (shell));
    }
    else if (event == OTP_TRAY_TOGGLE)
    {
        probe->selections++;
        g_print ("toggle on creating thread\n");
    }
    else if (event == OTP_TRAY_SHOW)
        g_print ("Show OTPClient selected\n");
    else if (event == OTP_TRAY_QUIT)
    {
        g_print ("Quit selected\n");
        g_main_loop_quit (probe->loop);
    }
}

static gboolean
self_test_step (gpointer data)
{
    Probe *probe = data;
    switch (probe->stage++)
    {
        case 0:
            if (!otp_windows_tray_shell_is_published (probe->shell))
            {
                g_printerr ("FAIL: no native notification area/registration\n");
                g_main_loop_quit (probe->loop);
                return G_SOURCE_REMOVE;
            }
            otp_windows_tray_shell_check_post_selection (probe->shell, FALSE);
            otp_windows_tray_shell_check_post_selection (probe->shell, TRUE);
            break;
        case 1:
            g_assert_cmpuint (probe->selections, ==, 1);
            otp_windows_tray_shell_set_tooltip (probe->shell, "OTPClient (locked)");
            otp_windows_tray_shell_set_enabled (probe->shell, FALSE);
            g_assert_false (otp_windows_tray_shell_is_published (probe->shell));
            otp_windows_tray_shell_set_enabled (probe->shell, TRUE);
            g_assert_true (otp_windows_tray_shell_is_published (probe->shell));
            g_assert_cmpuint (probe->publications, ==, 3);
            probe->passed = TRUE;
            g_print ("PASS: queued dispatch, packed icon ID, modify, delete/re-add\n");
            g_main_loop_quit (probe->loop);
            return G_SOURCE_REMOVE;
        default:
            return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean
stop_probe (gpointer data)
{
    g_main_loop_quit (((Probe *) data)->loop);
    return G_SOURCE_REMOVE;
}

int
main (int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && strcmp (argv[1], "--self-test") != 0))
    {
        g_printerr ("Usage: otp-windows-tray-check.exe [--self-test]\n");
        return 2;
    }
    Probe probe = { 0 };
    probe.thread_id = GetCurrentThreadId ();
    probe.self_test = argc == 2;
    probe.loop = g_main_loop_new (NULL, FALSE);
    /* Public test artwork: blue tile with transparent border. */
    guchar pixels[32 * 32 * 4] = { 0 };
    for (gint y = 4; y < 28; y++)
        for (gint x = 4; x < 28; x++)
        {
            guchar *pixel = pixels + (y * 32 + x) * 4;
            pixel[0] = 40;
            pixel[1] = 120;
            pixel[2] = 220;
            pixel[3] = 255;
        }
    HICON icon = otp_windows_tray_icon_from_pixels (pixels, 32, 32, 128, 4);
    if (icon != NULL)
        probe.shell = otp_windows_tray_shell_new (icon, "Show OTPClient", "Quit", on_event, &probe);
    if (probe.shell == NULL)
    {
        if (icon != NULL)
            DestroyIcon (icon);
        g_main_loop_unref (probe.loop);
        return 1;
    }
    otp_windows_tray_shell_set_enabled (probe.shell, TRUE);
    guint timer = probe.self_test ? g_timeout_add (250, self_test_step, &probe)
                                 : g_timeout_add_seconds (180, stop_probe, &probe);
    g_print ("Interactive probe: click, keyboard/context menu, restart Explorer. Timeout 180s.\n");
    g_main_loop_run (probe.loop);
    /* Timer may already have removed itself when it stopped the loop. */
    GSource *source = g_main_context_find_source_by_id (NULL, timer);
    if (source != NULL)
        g_source_destroy (source);
    otp_windows_tray_shell_free (probe.shell);
    DestroyIcon (icon);
    g_main_loop_unref (probe.loop);
    return probe.self_test && !probe.passed ? 1 : 0;
}
