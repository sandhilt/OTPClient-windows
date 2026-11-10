#include "single-instance.h"
#include "compat/otp-windows-single-instance.h"
#include <windows.h>
#include <string.h>

static DWORD main_thread;
static guint activations;

static void
activated (gpointer data)
{
    (void) data;
    g_assert_cmpuint (GetCurrentThreadId (), ==, main_thread);
    activations++;
}

static void
drain (void)
{
    while (g_main_context_iteration (NULL, FALSE))
        ;
}

static int
child (const gchar *name, const gchar *mode)
{
    g_autoptr (GError) error = NULL;
    gboolean primary = TRUE;
    OtpWindowsSingleInstance *self = otp_windows_single_instance_acquire (name, &primary, &error);
    g_assert_no_error (error);
    g_assert_nonnull (self);
    g_assert_false (primary);
    g_autofree gchar *event_name = g_strconcat ("Local\\", name, ".Ready", NULL);
    g_autofree gunichar2 *event_wide = g_utf8_to_utf16 (event_name, -1, NULL, NULL, NULL);
    HANDLE ready = OpenEventW (EVENT_MODIFY_STATE, FALSE, (LPCWSTR) event_wide);
    g_assert_nonnull (ready);
    g_assert_true (SetEvent (ready));
    CloseHandle (ready);
    if (strcmp (mode, "activate") == 0 || strcmp (mode, "absent") == 0 ||
        strcmp (mode, "hung") == 0)
    {
        gboolean sent = otp_windows_single_instance_notify (self, &error);
        if (strcmp (mode, "activate") == 0)
        {
            g_assert_no_error (error);
            g_assert_true (sent);
        }
        else
        {
            g_assert_false (sent);
            g_assert_nonnull (error);
        }
    }
    else
    {
        g_autofree gunichar2 *wide = g_utf8_to_utf16 (name, -1, NULL, NULL, NULL);
        HWND window = FindWindowExW (HWND_MESSAGE, NULL, (LPCWSTR) wide, NULL);
        g_assert_nonnull (window);
        char bytes[32] = "activate";
        COPYDATASTRUCT payload = { 0x4f545031u, sizeof ("activate"), bytes };
        if (strcmp (mode, "invalid") == 0)
            memcpy (bytes, "invalid!", sizeof ("invalid!"));
        else if (strcmp (mode, "truncated") == 0)
            payload.cbData--;
        else if (strcmp (mode, "oversized") == 0)
            payload.cbData = sizeof (bytes);
        else if (strcmp (mode, "tag") == 0)
            payload.dwData++;
        else
            g_assert_not_reached ();
        DWORD_PTR accepted = 1;
        g_assert_true (SendMessageTimeoutW (window, WM_COPYDATA, 0, (LPARAM) &payload,
                                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &accepted) != 0);
        g_assert_cmpuint (accepted, ==, FALSE);
    }
    otp_windows_single_instance_free (self);
    return 0;
}

static void
run_child (const gchar *exe, const gchar *name, const gchar *mode,
           OtpWindowsSingleInstance *self, gboolean race, gboolean pump)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *event_name = g_strconcat ("Local\\", name, ".Ready", NULL);
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (event_name, -1, NULL, NULL, NULL);
    HANDLE ready = CreateEventW (NULL, TRUE, FALSE, (LPCWSTR) wide);
    g_assert_nonnull (ready);
    gchar *args[] = { (gchar *) exe, "--child", (gchar *) name, (gchar *) mode, NULL };
    GPid pid = 0;
    g_assert_true (g_spawn_async (NULL, args, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &pid, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (WaitForSingleObject (ready, 5000), ==, WAIT_OBJECT_0);
    CloseHandle (ready);
    if (race)
    {
        /* Child already owns a secondary handle and has started its retries. */
        Sleep (250);
        g_assert_true (otp_windows_single_instance_listen (self, activated, NULL, &error));
        g_assert_no_error (error);
    }
    gint64 deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
    while (WaitForSingleObject (pid, 0) == WAIT_TIMEOUT)
    {
        g_assert_cmpint (g_get_monotonic_time (), <, deadline);
        if (pump)
            drain ();
        Sleep (5);
    }
    DWORD status = 1;
    g_assert_true (GetExitCodeProcess (pid, &status));
    g_assert_cmpuint (status, ==, 0);
    g_spawn_close_pid (pid);
    drain ();
}

static gint
version_option (GApplication *app, GVariantDict *options, gpointer data)
{
    (void) app;
    (void) data;
    return g_variant_dict_contains (options, "version") ? 0 : -1;
}

static void
check_options (void)
{
    GVariantDict options;
    g_variant_dict_init (&options, NULL);
    g_assert_true (otp_windows_single_instance_wants_activation (&options));
    g_variant_dict_insert (&options, "start-minimized", "b", TRUE);
    g_assert_false (otp_windows_single_instance_wants_activation (&options));
    g_variant_dict_remove (&options, "start-minimized");
    g_variant_dict_insert (&options, "start-minimised", "b", TRUE);
    g_assert_false (otp_windows_single_instance_wants_activation (&options));
    g_variant_dict_clear (&options);
    /* Real GApplication parser, no GTK: informational options must exit before
     * the single-instance hook. This also checks unknown --version behaviour. */
    g_autoptr (GApplication) help = g_application_new ("org.otpclient.SingleInstanceCheck.Help",
                                                      G_APPLICATION_DEFAULT_FLAGS);
    gchar *help_args[] = { "single-instance-check", "--help", NULL };
    g_assert_cmpint (otp_windows_application_run (help, 2, help_args), ==, 0);
    g_assert_false (g_application_get_is_registered (help));
    g_autoptr (GApplication) version = g_application_new ("org.otpclient.SingleInstanceCheck.Version",
                                                         G_APPLICATION_DEFAULT_FLAGS);
    gchar *version_args[] = { "single-instance-check", "--version", NULL };
    g_assert_cmpint (otp_windows_application_run (version, 2, version_args), !=, 0);
    g_assert_false (g_application_get_is_registered (version));
    /* A future real informational option must also bypass the hook. */
    g_autoptr (GApplication) info = g_application_new ("org.otpclient.SingleInstanceCheck.Info",
                                                      G_APPLICATION_DEFAULT_FLAGS);
    g_application_add_main_option (info, "version", 0, G_OPTION_FLAG_NONE,
                                   G_OPTION_ARG_NONE, "Print version", NULL);
    g_signal_connect (info, "handle-local-options", G_CALLBACK (version_option), NULL);
    g_assert_cmpint (otp_windows_application_run (info, 2, version_args), ==, 0);
    g_assert_false (g_application_get_is_registered (info));
}

int
main (int argc, char **argv)
{
    if (argc == 4 && strcmp (argv[1], "--child") == 0)
        return child (argv[2], argv[3]);
    main_thread = GetCurrentThreadId ();
    /* Environment injection exists only here. Fixed test prefix and PID keep
     * even an accidental app ID value separate from production names. */
    const gchar *suffix = g_getenv ("OTP_SINGLE_INSTANCE_CHECK_NAME");
    g_autofree gchar *digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256,
                                                             suffix != NULL ? suffix : "default", -1);
    g_autofree gchar *name = g_strdup_printf ("OTPClient.SingleInstance.Check.%lu.%s",
                                             GetCurrentProcessId (), digest);
    WCHAR executable[32768];
    DWORD length = GetModuleFileNameW (NULL, executable, G_N_ELEMENTS (executable));
    g_assert_cmpuint (length, >, 0);
    g_assert_cmpuint (length, <, G_N_ELEMENTS (executable));
    g_autofree gchar *exe = g_utf16_to_utf8 ((gunichar2 *) executable, -1, NULL, NULL, NULL);
    for (guint i = 0; i < 20; i++)
    {
        g_autoptr (GError) error = NULL;
        gboolean primary = FALSE;
        OtpWindowsSingleInstance *self = otp_windows_single_instance_acquire (name, &primary, &error);
        g_assert_no_error (error);
        g_assert_true (primary);
        gboolean other_primary = TRUE;
        OtpWindowsSingleInstance *other = otp_windows_single_instance_acquire (name, &other_primary, &error);
        g_assert_no_error (error);
        g_assert_false (other_primary);
        otp_windows_single_instance_free (other);
        guint before = activations;
        if (i == 0)
            run_child (exe, name, "activate", self, TRUE, TRUE);
        else
        {
            g_assert_true (otp_windows_single_instance_listen (self, activated, NULL, &error));
            g_assert_no_error (error);
            run_child (exe, name, "activate", self, FALSE, TRUE);
        }
        g_assert_cmpuint (activations, ==, before + 1);
        if (i == 0)
        {
            const gchar *invalid[] = { "invalid", "truncated", "oversized", "tag" };
            for (guint j = 0; j < G_N_ELEMENTS (invalid); j++)
                run_child (exe, name, invalid[j], self, FALSE, TRUE);
            g_assert_cmpuint (activations, ==, before + 1);
            run_child (exe, name, "hung", self, FALSE, FALSE);
            g_assert_cmpuint (activations, ==, before + 1);
        }
        if (i == 19)
        {
            /* Accepted activation pending in idle must be removed by cleanup. */
            g_autofree gunichar2 *wide = g_utf8_to_utf16 (name, -1, NULL, NULL, NULL);
            HWND window = FindWindowExW (HWND_MESSAGE, NULL, (LPCWSTR) wide, NULL);
            COPYDATASTRUCT payload = { 0x4f545031u, sizeof ("activate"), (void *) "activate" };
            g_assert_cmpint (SendMessageW (window, WM_COPYDATA, 0, (LPARAM) &payload), ==, TRUE);
            otp_windows_single_instance_free (self);
            drain ();
            g_assert_cmpuint (activations, ==, before + 1);
        }
        else
            otp_windows_single_instance_free (self);
    }
    g_autoptr (GError) error = NULL;
    gboolean primary = FALSE;
    OtpWindowsSingleInstance *absent = otp_windows_single_instance_acquire (name, &primary, &error);
    g_assert_no_error (error);
    g_assert_true (primary);
    run_child (exe, name, "absent", absent, FALSE, TRUE);
    otp_windows_single_instance_free (absent);
    check_options ();
    g_print ("Single-instance checks passed (20 cleanup/reinit cycles, IPC, retry, rejection, timeout, options)\n");
    return 0;
}
