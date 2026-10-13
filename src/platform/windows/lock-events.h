#pragma once

typedef struct OtpWindowsLockEvents OtpWindowsLockEvents;

enum {
    OTP_LOCK_EVENT_SESSION,
    OTP_LOCK_EVENT_SUSPEND
};

/* Start/stop and callbacks belong to the GTK/default GLib main-loop thread.
 * A session callback carries a WTS event; suspend carries zero. No secrets
 * cross this interface. Stop prevents further callbacks before returning. */
typedef void (*OtpWindowsLockEventFunc) (int kind, unsigned event, void *data);
OtpWindowsLockEvents *otp_windows_lock_events_start (OtpWindowsLockEventFunc callback,
                                                    void *data);
void otp_windows_lock_events_stop (OtpWindowsLockEvents *events);
