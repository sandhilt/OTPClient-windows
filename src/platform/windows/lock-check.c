#include <glib.h>
#include "lock-policy.h"
#ifdef _WIN32
#include <windows.h>
#include <wtsapi32.h>
#include <wchar.h>
#include "lock-events.h"
#endif

static void
test_disabled (void)
{
    const unsigned events[] = {
        OTP_WTS_SESSION_LOCK, OTP_WTS_SESSION_UNLOCK,
        OTP_WTS_CONSOLE_DISCONNECT, OTP_WTS_CONSOLE_CONNECT,
        OTP_WTS_REMOTE_DISCONNECT, OTP_WTS_REMOTE_CONNECT
    };
    OtpWindowsLockState state = { 0 };
    for (guint i = 0; i < G_N_ELEMENTS (events); i++) {
        OtpWindowsLockAction action = otp_windows_lock_session_event (&state, events[i], 0);
        g_assert_false (action.lock_database);
        g_assert_true (action.changed);
        g_assert_cmpint (action.session_locked, ==, i % 2 == 0);
    }
}

static void
test_overlap (void)
{
    OtpWindowsLockState state = { 0 };
    OtpWindowsLockAction action = otp_windows_lock_session_event (&state, OTP_WTS_SESSION_LOCK, 1);
    g_assert_true (action.lock_database);
    g_assert_true (action.changed);
    action = otp_windows_lock_session_event (&state, OTP_WTS_REMOTE_DISCONNECT, 1);
    g_assert_true (action.lock_database);
    g_assert_false (action.changed);
    action = otp_windows_lock_session_event (&state, OTP_WTS_SESSION_UNLOCK, 1);
    g_assert_true (action.session_locked);
    g_assert_false (action.lock_database);
    action = otp_windows_lock_session_event (&state, OTP_WTS_REMOTE_CONNECT, 1);
    g_assert_false (action.session_locked);
    g_assert_false (action.lock_database);
    g_assert_true (action.changed);
    /* A reconnect must not erase a still-locked desktop. */
    otp_windows_lock_session_event (&state, OTP_WTS_CONSOLE_DISCONNECT, 1);
    otp_windows_lock_session_event (&state, OTP_WTS_SESSION_LOCK, 1);
    action = otp_windows_lock_session_event (&state, OTP_WTS_CONSOLE_CONNECT, 1);
    g_assert_true (action.session_locked);
    g_assert_false (action.lock_database);
}

static void
test_duplicate_and_unknown (void)
{
    OtpWindowsLockState state = { 0 };
    otp_windows_lock_session_event (&state, OTP_WTS_SESSION_LOCK, 0);
    /* Auto-Lock may have changed: a fresh lock event still requests a lock. */
    OtpWindowsLockAction action = otp_windows_lock_session_event (&state, OTP_WTS_SESSION_LOCK, 1);
    g_assert_true (action.lock_database);
    g_assert_false (action.changed);
    action = otp_windows_lock_session_event (&state, 999, 1);
    g_assert_true (action.session_locked);
    g_assert_false (action.lock_database);
    g_assert_false (action.changed);
    action = otp_windows_lock_session_event (&state, OTP_WTS_SESSION_UNLOCK, 1);
    g_assert_false (action.lock_database);
    g_assert_false (action.session_locked);
}

#ifdef _WIN32
typedef struct {
    OtpWindowsLockEvents *events;
    guint session_count;
    guint suspend_count;
    unsigned last_event;
    DWORD thread;
    gboolean stop_on_event;
} EventCheck;

static void
on_event (int kind, unsigned event, void *data)
{
    EventCheck *check = data;
    g_assert_cmpuint (GetCurrentThreadId (), ==, check->thread);
    if (kind == OTP_LOCK_EVENT_SESSION) {
        check->session_count++;
        check->last_event = event;
    } else {
        g_assert_cmpint (kind, ==, OTP_LOCK_EVENT_SUSPEND);
        check->suspend_count++;
    }
    if (check->stop_on_event) {
        otp_windows_lock_events_stop (check->events);
        check->events = NULL;
    }
}

static BOOL CALLBACK
find_window (HWND window, LPARAM data)
{
    wchar_t title[64];
    GetWindowTextW (window, title, G_N_ELEMENTS (title));
    if (wcscmp (title, L"OTPClient lock notifications") == 0) {
        *(HWND *) data = window;
        return FALSE;
    }
    return TRUE;
}

static HWND
get_window (void)
{
    HWND window = NULL;
    EnumThreadWindows (GetCurrentThreadId (), find_window, (LPARAM) &window);
    g_assert_nonnull (window);
    g_assert_false (IsWindowVisible (window));
    return window;
}

static void
wait_for_event (EventCheck *check, guint count)
{
    gint64 deadline = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;
    while (check->session_count + check->suspend_count < count &&
           g_get_monotonic_time () < deadline) {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (1000);
    }
    g_assert_cmpuint (check->session_count + check->suspend_count, ==, count);
}

static void
test_message_delivery (void)
{
    EventCheck check = { .thread = GetCurrentThreadId () };
    check.events = otp_windows_lock_events_start (on_event, &check);
    g_assert_nonnull (check.events);
    HWND window = get_window ();
    g_assert_true (PostMessageW (window, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0));
    wait_for_event (&check, 1);
    g_assert_cmpuint (check.last_event, ==, WTS_SESSION_LOCK);
    g_assert_true (PostMessageW (window, WM_POWERBROADCAST, PBT_APMSUSPEND, 0));
    wait_for_event (&check, 2);
    g_assert_cmpuint (check.suspend_count, ==, 1);
    SendMessageW (window, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
    g_assert_cmpuint (check.suspend_count, ==, 1);
    otp_windows_lock_events_stop (check.events);
    g_assert_false (IsWindow (window));
}

static void
test_cleanup_and_reinit (void)
{
    EventCheck check = { .thread = GetCurrentThreadId (), .stop_on_event = TRUE };
    for (guint i = 0; i < 20; i++) {
        check.events = otp_windows_lock_events_start (on_event, &check);
        g_assert_nonnull (check.events);
        HWND window = get_window ();
        /* One callback destroys the source during dispatch; a second queued
         * event must never reach a freed or subsequently reused lifetime. */
        PostMessageW (window, WM_WTSSESSION_CHANGE, WTS_REMOTE_DISCONNECT, 0);
        PostMessageW (window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        wait_for_event (&check, i + 1);
        g_assert_null (check.events);
        g_assert_false (IsWindow (window));
    }
}
#endif

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/windows-lock/policy/auto-lock-off", test_disabled);
    g_test_add_func ("/windows-lock/policy/overlapping-session-events", test_overlap);
    g_test_add_func ("/windows-lock/policy/duplicate-and-unknown", test_duplicate_and_unknown);
#ifdef _WIN32
    /* Synthetic messages verify our bridge/lifetime, not actual OS delivery. */
    g_test_add_func ("/windows-lock/events/message-delivery", test_message_delivery);
    g_test_add_func ("/windows-lock/events/cleanup-and-reinit", test_cleanup_and_reinit);
#endif
    return g_test_run ();
}
