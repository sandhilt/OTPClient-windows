#include "gettext.h"
#include <windows.h>
#include <glib.h>
#include <libintl.h>
#include <errno.h>

static gchar *locale_directory;

static gchar *
directory_from_executable (void)
{
    for (DWORD capacity = 256; capacity <= 32768; capacity *= 2) {
        g_autofree WCHAR *path = g_new0 (WCHAR, capacity);
        DWORD length = GetModuleFileNameW (NULL, path, capacity);
        if (!length) break;
        if (length >= capacity) continue;
        g_autofree gchar *utf8 = g_utf16_to_utf8 ((gunichar2 *) path, length, NULL, NULL, NULL);
        if (!utf8) break;
        g_autofree gchar *bin = g_path_get_dirname (utf8);
        g_autofree gchar *directory = g_build_filename (bin, "..", "share", "locale", NULL);
        return g_canonicalize_filename (directory, NULL);
    }
    errno = EIO;
    return NULL;
}

char *
otp_windows_bindtextdomain (const char *domain, const char *directory)
{
    /* Only the application's domain is relocated. libintl and toolkit domain
     * bindings in DLLs retain their own behaviour. NULL remains a query. */
    if (!directory || g_strcmp0 (domain, "OTPClient") != 0)
        return bindtextdomain (domain, directory);

    static gsize initialized;
    if (g_once_init_enter (&initialized)) {
        /* GNU libintl's Windows setlocale replacement honors LC_ALL, the
         * category variables and LANG, otherwise the native user locale.
         * Both upstream mains bind before GTK/GApplication initialization,
         * including CLI's first _(). */
        if (!setlocale (LC_ALL, ""))
            g_warning ("Windows locale initialization failed; check LANG/LC_ALL/LC_MESSAGES");
        locale_directory = directory_from_executable ();
        g_once_init_leave (&initialized, 1);
    }
    if (!locale_directory) {
        g_warning ("Cannot resolve Windows executable-relative locale directory");
        errno = EIO;
        return NULL;
    }
    /* Use libintl's UTF-16 API rather than assuming the current ANSI code page
     * can represent the extracted zip's directory. Return its UTF-8 spelling
     * with process lifetime, as callers expect from narrow bindtextdomain. */
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (locale_directory, -1, NULL, NULL, NULL);
    if (!wide || !wbindtextdomain (domain, (WCHAR *) wide)) {
        g_warning ("Cannot bind Windows application message catalogs");
        return NULL;
    }
    return locale_directory;
}
