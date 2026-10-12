#pragma once
#include "compat/otp-windows.h"
TOKEN_USER *otp_current_user (void);
gboolean otp_acl_mode (HANDLE handle, PSID user, unsigned *mode);
gboolean otp_private_handle (HANDLE handle, PSID user);
