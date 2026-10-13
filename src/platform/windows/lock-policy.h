#pragma once

/* WTS notification values, checked against the SDK in lock-events.c. This
 * policy has no OS or UI dependency and can be exercised independently. */
enum {
    OTP_WTS_CONSOLE_CONNECT = 1,
    OTP_WTS_CONSOLE_DISCONNECT = 2,
    OTP_WTS_REMOTE_CONNECT = 3,
    OTP_WTS_REMOTE_DISCONNECT = 4,
    OTP_WTS_SESSION_LOCK = 7,
    OTP_WTS_SESSION_UNLOCK = 8
};

typedef struct {
    int locked;
    int disconnected;
} OtpWindowsLockState;

typedef struct {
    int session_locked;
    int changed;
    int lock_database;
} OtpWindowsLockAction;

OtpWindowsLockAction otp_windows_lock_session_event (OtpWindowsLockState *state,
                                                     unsigned event,
                                                     int auto_lock);
