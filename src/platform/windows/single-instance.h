#pragma once
#include <gio/gio.h>

/* Internal transport interface. Names are supplied by the caller, never read
 * from the environment in the app. All receiver operations use the main thread
 * and the default GLib context. The mutex is a lifetime handle, not a lock. */
typedef struct _OtpWindowsSingleInstance OtpWindowsSingleInstance;
typedef void (*OtpWindowsSingleInstanceCallback) (gpointer data);

OtpWindowsSingleInstance *otp_windows_single_instance_acquire (const gchar *name,
                                                              gboolean *primary,
                                                              GError **error);
gboolean otp_windows_single_instance_listen (OtpWindowsSingleInstance *self,
                                             OtpWindowsSingleInstanceCallback callback,
                                             gpointer data, GError **error);
gboolean otp_windows_single_instance_notify (OtpWindowsSingleInstance *self, GError **error);
void otp_windows_single_instance_free (OtpWindowsSingleInstance *self);
gboolean otp_windows_single_instance_wants_activation (GVariantDict *options);
