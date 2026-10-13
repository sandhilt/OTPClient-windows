#include <windows.h>
#include <wtsapi32.h>
#include <glib.h>
#include <wchar.h>
#include "lock-events.h"
#include "lock-policy.h"

_Static_assert (OTP_WTS_CONSOLE_CONNECT == WTS_CONSOLE_CONNECT, "WTS event ABI");
_Static_assert (OTP_WTS_CONSOLE_DISCONNECT == WTS_CONSOLE_DISCONNECT, "WTS event ABI");
_Static_assert (OTP_WTS_REMOTE_CONNECT == WTS_REMOTE_CONNECT, "WTS event ABI");
_Static_assert (OTP_WTS_REMOTE_DISCONNECT == WTS_REMOTE_DISCONNECT, "WTS event ABI");
_Static_assert (OTP_WTS_SESSION_LOCK == WTS_SESSION_LOCK, "WTS event ABI");
_Static_assert (OTP_WTS_SESSION_UNLOCK == WTS_SESSION_UNLOCK, "WTS event ABI");

struct OtpWindowsLockEvents {
    grefcount refs;
    HWND window;
    HINSTANCE instance;
    ATOM window_class;
    HPOWERNOTIFY power;
    HPOWERNOTIFY suspend;
    GSource *pump;
    DWORD thread;
    gboolean wts_registered;
    gboolean stopped;
    OtpWindowsLockEventFunc callback;
    void *data;
};

static void
events_unref (gpointer data)
{
    OtpWindowsLockEvents *events = data;
    if (g_ref_count_dec (&events->refs))
        g_free (events);
}

static void
report_failure (const gchar *operation)
{
    g_warning ("Windows lock notifications: %s failed (Win32 %lu); "
               "this notification path is unavailable", operation,
               (unsigned long) GetLastError ());
}

static LRESULT CALLBACK
lock_window_proc (HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    OtpWindowsLockEvents *events = (void *) GetWindowLongPtrW (window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (void *) lparam;
        events = create->lpCreateParams;
        SetWindowLongPtrW (window, GWLP_USERDATA, (LONG_PTR) events);
    }
    if (events != NULL && !events->stopped) {
        /* Window procedures run on the creating main-loop thread. Delivery
         * through GTK's native pump or our filtered pump uses the same path.
         * UNVERIFIED: GTK/MSYS2 modal loops and native notification timing. */
        if (message == WM_WTSSESSION_CHANGE) {
            events->callback (OTP_LOCK_EVENT_SESSION, (unsigned) wparam, events->data);
            return 0;
        }
        if (message == WM_POWERBROADCAST) {
            if (wparam == PBT_APMSUSPEND)
                events->callback (OTP_LOCK_EVENT_SUSPEND, 0, events->data);
            /* Display power-setting changes and resume never unlock the DB.
             * WINDOWS-STUB: this is no suspend inhibitor; purging before the
             * OS suspends a busy main thread is UNVERIFIED, not guaranteed. */
            return TRUE;
        }
    }
    return DefWindowProcW (window, message, wparam, lparam);
}

static gboolean
pump_messages (gpointer data)
{
    OtpWindowsLockEvents *events = data;
    /* PeekMessage can dispatch sent messages, whose callback can stop us.
     * Hold a reference independently of the timeout's destroy notifier. */
    g_ref_count_inc (&events->refs);
    MSG message;
    for (guint i = 0; i < 32 && !events->stopped; i++) {
        if (!PeekMessageW (&message, events->window, 0, 0, PM_REMOVE))
            break;
        if (events->stopped)
            break;
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
    gboolean keep = !events->stopped;
    events_unref (events);
    return keep;
}

OtpWindowsLockEvents *
otp_windows_lock_events_start (OtpWindowsLockEventFunc callback, void *data)
{
    g_return_val_if_fail (callback != NULL, NULL);
    OtpWindowsLockEvents *events = g_new0 (OtpWindowsLockEvents, 1);
    g_ref_count_init (&events->refs);
    events->callback = callback;
    events->data = data;
    events->thread = GetCurrentThreadId ();
    events->instance = GetModuleHandleW (NULL);
    wchar_t name[80];
    swprintf (name, G_N_ELEMENTS (name), L"OTPClientLockEvents-%p", (void *) events);
    WNDCLASSEXW window_class = { 0 };
    window_class.cbSize = sizeof (window_class);
    window_class.lpfnWndProc = lock_window_proc;
    window_class.hInstance = events->instance;
    window_class.lpszClassName = name;
    events->window_class = RegisterClassExW (&window_class);
    if (events->window_class == 0) {
        report_failure ("RegisterClassExW");
        otp_windows_lock_events_stop (events);
        return NULL;
    }
    /* Hidden top-level window, never ShowWindow. HWND_MESSAGE is not used:
     * message-only windows do not receive broadcast messages, and WTS's
     * documentation does not explicitly promise support for that HWND type. */
    events->window = CreateWindowExW (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        MAKEINTRESOURCEW (events->window_class), L"OTPClient lock notifications",
        WS_POPUP, 0, 0, 0, 0, NULL, NULL, events->instance, events);
    if (events->window == NULL) {
        report_failure ("CreateWindowExW");
        otp_windows_lock_events_stop (events);
        return NULL;
    }
    events->wts_registered = WTSRegisterSessionNotification (events->window,
                                                            NOTIFY_FOR_THIS_SESSION);
    if (!events->wts_registered)
        report_failure ("WTSRegisterSessionNotification");
    /* Power-setting registration is for display state, not suspend. Never
     * equate monitor-off with session lock. Suspend has its own registration. */
    static const GUID console_display_state = {
        0x6fe69556, 0x704a, 0x47a0, { 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47 }
    };
    events->power = RegisterPowerSettingNotification (events->window,
        &console_display_state, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (events->power == NULL)
        report_failure ("RegisterPowerSettingNotification");
    events->suspend = RegisterSuspendResumeNotification (events->window,
                                                        DEVICE_NOTIFY_WINDOW_HANDLE);
    if (events->suspend == NULL)
        report_failure ("RegisterSuspendResumeNotification");

    /* Do not install G_WIN32_MSG_HANDLE: GLib explicitly warns against a
     * second message-queue poll source in GTK programs. Only this HWND is
     * drained, at most 32 messages per tick; GDK owns other windows/messages.
     * This also makes the backend usable in a GLib-only native probe.
     * UNVERIFIED: idle CPU cost and latency alongside the MSYS2 GDK pump. */
    events->pump = g_timeout_source_new (50);
    g_source_set_name (events->pump, "otp-windows-lock-messages");
    g_ref_count_inc (&events->refs);
    g_source_set_callback (events->pump, pump_messages, events, events_unref);
    g_source_attach (events->pump, NULL);
    /* WINDOWS-STUB: initial WTS state is unknown until a notification arrives.
     * Startup while already locked/disconnected is UNVERIFIED; this backend
     * does not claim that a zero-initialized state proves an unlocked session.
     * Failed WTS registration (e.g. RDS not ready) is warned, without retry. */
    return events;
}

void
otp_windows_lock_events_stop (OtpWindowsLockEvents *events)
{
    if (events == NULL)
        return;
    g_return_if_fail (events->thread == GetCurrentThreadId ());
    events->stopped = TRUE;
    if (events->pump != NULL) {
        g_source_destroy (events->pump);
        g_source_unref (events->pump);
        events->pump = NULL;
    }
    if (events->suspend != NULL && !UnregisterSuspendResumeNotification (events->suspend))
        report_failure ("UnregisterSuspendResumeNotification");
    if (events->power != NULL && !UnregisterPowerSettingNotification (events->power))
        report_failure ("UnregisterPowerSettingNotification");
    if (events->wts_registered && !WTSUnRegisterSessionNotification (events->window))
        report_failure ("WTSUnRegisterSessionNotification");
    if (events->window != NULL) {
        SetWindowLongPtrW (events->window, GWLP_USERDATA, 0);
        if (!DestroyWindow (events->window))
            report_failure ("DestroyWindow");
    }
    if (events->window_class != 0 &&
        !UnregisterClassW (MAKEINTRESOURCEW (events->window_class), events->instance))
        report_failure ("UnregisterClassW");
    events_unref (events);
}
