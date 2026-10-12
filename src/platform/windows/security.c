#include "security.h"
#include <aclapi.h>
#include <errno.h>
#include <io.h>

TOKEN_USER *
otp_current_user (void)
{
    HANDLE token;
    if (!OpenThreadToken (GetCurrentThread (), TOKEN_QUERY, TRUE, &token)) {
        if (GetLastError () != ERROR_NO_TOKEN ||
            !OpenProcessToken (GetCurrentProcess (), TOKEN_QUERY, &token)) return NULL;
    }
    DWORD size = 0;
    GetTokenInformation (token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = size ? g_malloc (size) : NULL;
    if (!user || !GetTokenInformation (token, TokenUser, user, size, &size)) {
        g_free (user);
        user = NULL;
    }
    CloseHandle (token);
    return user;
}

static gboolean
trusted_sid (PSID sid, PSID user)
{
    return EqualSid (sid, user) || IsWellKnownSid (sid, WinLocalSystemSid) ||
           IsWellKnownSid (sid, WinBuiltinAdministratorsSid);
}

gboolean
otp_acl_mode (HANDLE handle, PSID user, unsigned *mode)
{
    PACL acl = NULL;
    PSID owner = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    DWORD status = GetSecurityInfo (handle, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
        &owner, NULL, &acl, NULL, &descriptor);
    if (status != ERROR_SUCCESS) return FALSE;
    *mode = 0;
    if (!acl || !owner || !trusted_sid (owner, user)) *mode = 066;
    /* WINDOWS-STUB: this is a conservative ACE audit, not a full Windows
     * AccessCheck/group/conditional-ACE evaluator. Unknown ACE forms reject
     * password files; deny ACEs never cancel a grant to another principal. */
    for (DWORD i = 0; acl && i < acl->AceCount; i++) {
        ACE_HEADER *header;
        if (!GetAce (acl, i, (void **) &header)) { *mode = 066; break; }
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
            header->AceSize < sizeof (ACCESS_ALLOWED_ACE)) { *mode = 066; break; }
        ACCESS_ALLOWED_ACE *ace = (ACCESS_ALLOWED_ACE *) header;
        PSID sid = &ace->SidStart;
        if (!IsValidSid (sid) || GetLengthSid (sid) >
            header->AceSize - G_STRUCT_OFFSET (ACCESS_ALLOWED_ACE, SidStart)) {
            *mode = 066;
            break;
        }
        if (!trusted_sid (sid, user) && ace->Mask) {
            DWORD write = GENERIC_ALL | GENERIC_WRITE | FILE_WRITE_DATA |
                FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                DELETE | WRITE_DAC | WRITE_OWNER;
            *mode |= (ace->Mask & write) ? 066 : 044;
        }
    }
    LocalFree (descriptor);
    return TRUE;
}

gboolean
otp_private_handle (HANDLE handle, PSID user)
{
    DWORD flags = 0;
    if (!GetVolumeInformationByHandleW (handle, NULL, 0, NULL, NULL, &flags, NULL, 0) ||
        !(flags & FS_PERSISTENT_ACLS)) return FALSE;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    PACL acl = NULL;
    PSID owner = NULL;
    if (GetSecurityInfo (handle, SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
        &owner, NULL, &acl, NULL, &descriptor) != ERROR_SUCCESS) return FALSE;
    SECURITY_DESCRIPTOR_CONTROL control;
    DWORD revision;
    gboolean private = GetSecurityDescriptorControl (descriptor, &control, &revision) &&
        (control & SE_DACL_PROTECTED) && owner && EqualSid (owner, user) &&
        acl && IsValidAcl (acl) && acl->AceCount == 1;
    if (private) {
        ACCESS_ALLOWED_ACE *ace;
        private = GetAce (acl, 0, (void **) &ace) &&
            ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
            ace->Header.AceSize >= sizeof (ACCESS_ALLOWED_ACE) &&
            ace->Header.AceFlags == 0 && ace->Mask == FILE_ALL_ACCESS &&
            IsValidSid (&ace->SidStart) && GetLengthSid (&ace->SidStart) <=
                ace->Header.AceSize - G_STRUCT_OFFSET (ACCESS_ALLOWED_ACE, SidStart) &&
            EqualSid (&ace->SidStart, user);
    }
    LocalFree (descriptor);
    return private;
}

int
otp_fstat (int fd, struct stat *state)
{
    if (OTP_CRT_FSTAT (fd, state) != 0) return -1;
    if (!S_ISREG (state->st_mode)) return 0;
    g_autofree TOKEN_USER *user = otp_current_user ();
    unsigned mode;
    intptr_t raw_handle = _get_osfhandle (fd);
    if (!user || !otp_acl_mode ((HANDLE) raw_handle, user->User.Sid, &mode)) {
        errno = EACCES;
        return -1;
    }
    state->st_mode = (state->st_mode & ~077) | mode;
    return 0;
}
