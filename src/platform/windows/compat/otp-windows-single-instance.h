#pragma once
#include <gio/gio.h>

G_BEGIN_DECLS
int otp_windows_application_run (GApplication *app, int argc, char **argv);
G_END_DECLS

#ifndef OTP_WINDOWS_COMPAT_IMPLEMENTATION
#define g_application_run otp_windows_application_run
#endif
