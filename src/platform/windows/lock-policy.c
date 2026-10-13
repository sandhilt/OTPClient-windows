#include "lock-policy.h"

OtpWindowsLockAction
otp_windows_lock_session_event (OtpWindowsLockState *state,
                               unsigned event,
                               int auto_lock)
{
    int before = state->locked || state->disconnected;
    int lock_event = 0;
    switch (event) {
        case OTP_WTS_SESSION_LOCK:
            state->locked = 1;
            lock_event = 1;
            break;
        case OTP_WTS_SESSION_UNLOCK:
            state->locked = 0;
            break;
        case OTP_WTS_CONSOLE_DISCONNECT:
        case OTP_WTS_REMOTE_DISCONNECT:
            state->disconnected = 1;
            lock_event = 1;
            break;
        case OTP_WTS_CONSOLE_CONNECT:
        case OTP_WTS_REMOTE_CONNECT:
            state->disconnected = 0;
            break;
        default:
            break;
    }
    OtpWindowsLockAction action = {
        state->locked || state->disconnected,
        before != (state->locked || state->disconnected),
        lock_event && auto_lock
    };
    return action;
}
