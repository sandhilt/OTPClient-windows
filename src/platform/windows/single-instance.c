#ifndef OTP_WINDOWS_COMPAT_IMPLEMENTATION
#define OTP_WINDOWS_COMPAT_IMPLEMENTATION
#endif
#include "compat/otp-windows-single-instance.h"
#include "single-instance.h"
#include <windows.h>
#include <string.h>

#define ACTIVATE_TAG 0x4f545031u
#define RETRY_COUNT 101
#define RETRY_MS 50
#define SEND_TIMEOUT_MS 1000

struct _OtpWindowsSingleInstance
{
    HANDLE mutex;
    HWND window;
    WCHAR *klass;
    DWORD thread;
    gboolean primary;
    gboolean class_registered;
    guint watch;
    guint idle;
    OtpWindowsSingleInstanceCallback callback;
    gpointer data;
};

static gboolean
win_error (GError **error, const gchar *message)
{
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                 "%s (Win32 error %lu)", message, GetLastError ());
    return FALSE;
}

static gboolean
activate_idle (gpointer data)
{
    OtpWindowsSingleInstance *self = data;
    self->idle = 0;
    self->callback (self->data);
    return G_SOURCE_REMOVE;
}

static LRESULT CALLBACK
window_proc (HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    OtpWindowsSingleInstance *self = (void *) GetWindowLongPtrW (window, GWLP_USERDATA);
    if (message == WM_NCCREATE)
    {
        self = ((const CREATESTRUCTW *) lparam)->lpCreateParams;
        SetWindowLongPtrW (window, GWLP_USERDATA, (LONG_PTR) self);
    }
    if (message == WM_COPYDATA && self != NULL)
    {
        const COPYDATASTRUCT *payload = (const COPYDATASTRUCT *) lparam;
        /* Check exact size BEFORE reading bytes. Never retain sender memory. */
        if (payload == NULL || payload->dwData != ACTIVATE_TAG ||
            payload->cbData != sizeof ("activate") || payload->lpData == NULL ||
            memcmp (payload->lpData, "activate", sizeof ("activate")) != 0)
            return FALSE;
        if (self->idle == 0)
            self->idle = g_idle_add (activate_idle, self);
        return TRUE; /* accepted/queued, not a promise of foreground focus */
    }
    if (message == WM_NCDESTROY)
        SetWindowLongPtrW (window, GWLP_USERDATA, 0);
    return DefWindowProcW (window, message, wparam, lparam);
}

static gboolean
dispatch_messages (GIOChannel *channel, GIOCondition condition, gpointer data)
{
    (void) channel;
    (void) condition;
    OtpWindowsSingleInstance *self = data;
    MSG message;
    /* Same watch as tray-win-shell.c: PeekMessage services nonqueued sends;
     * GDK may also dispatch to this WndProc. Only retrieve our posted messages.
     * WINDOWS-STUB: native GTK/GDK coexistence and foreground focus unverified.
     * Leave WM_QUIT for GTK even if PeekMessage returns it despite filtering. */
    for (guint i = 0; i < 32 &&
         PeekMessageW (&message, self->window, 0, 0, PM_NOREMOVE); i++)
    {
        if (message.message == WM_QUIT)
            break;
        if (PeekMessageW (&message, self->window, 0, 0, PM_REMOVE))
            DispatchMessageW (&message);
    }
    return G_SOURCE_CONTINUE;
}

OtpWindowsSingleInstance *
otp_windows_single_instance_acquire (const gchar *name, gboolean *primary, GError **error)
{
    *primary = FALSE;
    OtpWindowsSingleInstance *self = g_new0 (OtpWindowsSingleInstance, 1);
    self->thread = GetCurrentThreadId ();
    g_autofree gchar *mutex_name = g_strconcat ("Local\\", name, NULL);
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (mutex_name, -1, NULL, NULL, error);
    if (wide == NULL)
        goto fail;
    self->klass = (WCHAR *) g_utf8_to_utf16 (name, -1, NULL, NULL, error);
    if (self->klass == NULL)
        goto fail;
    SetLastError (ERROR_SUCCESS);
    self->mutex = CreateMutexW (NULL, FALSE, (LPCWSTR) wide);
    DWORD status = GetLastError ();
    if (self->mutex == NULL)
    {
        win_error (error, "Cannot create single-instance mutex");
        goto fail;
    }
    self->primary = status != ERROR_ALREADY_EXISTS;
    *primary = self->primary;
    return self;
fail:
    otp_windows_single_instance_free (self);
    return NULL;
}

gboolean
otp_windows_single_instance_listen (OtpWindowsSingleInstance *self,
                                     OtpWindowsSingleInstanceCallback callback,
                                     gpointer data, GError **error)
{
    g_return_val_if_fail (self->primary && self->window == NULL && callback != NULL &&
                         self->thread == GetCurrentThreadId (), FALSE);
    self->callback = callback;
    self->data = data;
    WNDCLASSW klass = { 0 };
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleW (NULL);
    klass.lpszClassName = self->klass;
    /* Do not accept a pre-existing class with an unknown window procedure. */
    if (!RegisterClassW (&klass))
        return win_error (error, "Cannot register single-instance window class");
    self->class_registered = TRUE;
    self->window = CreateWindowExW (0, self->klass, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, NULL, klass.hInstance, self);
    if (self->window == NULL)
        return win_error (error, "Cannot create single-instance message window");
    GIOChannel *channel = g_io_channel_win32_new_messages ((gsize) self->window);
    self->watch = g_io_add_watch (channel, G_IO_IN, dispatch_messages, self);
    g_io_channel_unref (channel);
    return TRUE;
}

gboolean
otp_windows_single_instance_notify (OtpWindowsSingleInstance *self, GError **error)
{
    HWND window = NULL;
    for (guint i = 0; i < RETRY_COUNT; i++)
    {
        window = FindWindowExW (HWND_MESSAGE, NULL, self->klass, NULL);
        if (window != NULL)
            break;
        if (i + 1 < RETRY_COUNT)
            Sleep (RETRY_MS);
    }
    if (window == NULL)
    {
        g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                             "First instance has no activation window after 5 seconds; refusing another instance");
        return FALSE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId (window, &pid);
    /* Permit the existing GTK presentation funnel to request foreground. */
    if (pid != 0)
        AllowSetForegroundWindow (pid);
    COPYDATASTRUCT payload = { ACTIVATE_TAG, sizeof ("activate"), (void *) "activate" };
    DWORD_PTR accepted = 0;
    SetLastError (ERROR_SUCCESS);
    if (!SendMessageTimeoutW (window, WM_COPYDATA, 0, (LPARAM) &payload,
                              SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                              SEND_TIMEOUT_MS, &accepted))
        return win_error (error, "First instance did not respond to activation; refusing another instance");
    if (accepted != TRUE)
    {
        g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
                             "First instance rejected activation; refusing another instance");
        return FALSE;
    }
    return TRUE;
}

void
otp_windows_single_instance_free (OtpWindowsSingleInstance *self)
{
    if (self == NULL)
        return;
    g_return_if_fail (self->thread == GetCurrentThreadId ());
    g_clear_handle_id (&self->watch, g_source_remove);
    g_clear_handle_id (&self->idle, g_source_remove);
    if (self->window != NULL)
        DestroyWindow (self->window);
    if (self->class_registered)
        UnregisterClassW (self->klass, GetModuleHandleW (NULL));
    if (self->mutex != NULL)
        CloseHandle (self->mutex);
    g_free (self->klass);
    g_free (self);
}

/* SID partitions accounts even when they share an interactive session. Default
 * token DACL only: this is coordination, not an authenticated IPC boundary.
 * WINDOWS-STUB: elevated/non-elevated UIPI interoperability unverified; failures
 * are closed. No DB path, option arguments or secrets cross this transport. */
static gchar *
application_name (GApplication *app, GError **error)
{
    HANDLE token = NULL;
    DWORD size = 0;
    if (!OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &token))
    {
        win_error (error, "Cannot identify single-instance user");
        return NULL;
    }
    GetTokenInformation (token, TokenUser, NULL, 0, &size);
    g_autofree TOKEN_USER *user = g_malloc (size);
    if (!GetTokenInformation (token, TokenUser, user, size, &size))
    {
        win_error (error, "Cannot read single-instance user");
        CloseHandle (token);
        return NULL;
    }
    CloseHandle (token);
    const gchar *id = g_application_get_application_id (app);
    if (id == NULL)
    {
        g_set_error_literal (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Application ID is required");
        return NULL;
    }
    GChecksum *sum = g_checksum_new (G_CHECKSUM_SHA256);
    g_checksum_update (sum, (const guchar *) id, strlen (id) + 1);
    g_checksum_update (sum, user->User.Sid, GetLengthSid (user->User.Sid));
    gchar *name = g_strconcat ("OTPClient.SingleInstance.", g_checksum_get_string (sum), NULL);
    g_checksum_free (sum);
    return name;
}

typedef struct
{
    OtpWindowsSingleInstance *instance;
} RunState;

static void
activate_application (gpointer data)
{
    GApplication *app = data;
    if (g_application_get_is_registered (app))
        g_application_activate (app);
}

static gint
local_options (GApplication *app, GVariantDict *options, gpointer data)
{
    RunState *state = data;
    g_autoptr (GError) error = NULL;
    g_autofree gchar *name = application_name (app, &error);
    gboolean primary = FALSE;
    if (name != NULL)
        state->instance = otp_windows_single_instance_acquire (name, &primary, &error);
    if (state->instance == NULL)
        goto fail;
    if (!primary)
    {
        if (!otp_windows_single_instance_wants_activation (options))
            return 0;
        if (!otp_windows_single_instance_notify (state->instance, &error))
            goto fail;
        return 0;
    }
    if (!otp_windows_single_instance_listen (state->instance, activate_application, app, &error))
        goto fail;
    return -1;
fail:
    /* GUI subsystem builds often have no stderr attached. Always show a clear
     * diagnostic as well; never include command-line arguments or DB paths. */
    g_printerr ("OTPClient single instance: %s\n", error->message);
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (error->message, -1, NULL, NULL, NULL);
    MessageBoxW (NULL, (LPCWSTR) wide, L"OTPClient single instance", MB_OK | MB_ICONERROR);
    return 1;
}

gboolean
otp_windows_single_instance_wants_activation (GVariantDict *options)
{
    return !g_variant_dict_contains (options, "start-minimized") &&
           !g_variant_dict_contains (options, "start-minimised");
}

int
otp_windows_application_run (GApplication *app, int argc, char **argv)
{
    RunState state = { 0 };
    /* Let GLib parse help/errors and the class handle informational options
     * first. Only continuing launches acquire the mutex, before registration
     * and GTK startup. NON_UNIQUE avoids the absent Windows session D-Bus. */
    g_application_set_flags (app, g_application_get_flags (app) | G_APPLICATION_NON_UNIQUE);
    gulong handler = g_signal_connect_after (app, "handle-local-options",
                                             G_CALLBACK (local_options), &state);
    int status = g_application_run (app, argc, argv);
    g_signal_handler_disconnect (app, handler);
    otp_windows_single_instance_free (state.instance);
    return status;
}
