#include "tray-win-shell.h"
#include <shellapi.h>
#include <windowsx.h>
#include <string.h>

#define TRAY_CALLBACK_MESSAGE (WM_APP + 71)
#define TRAY_ICON_ID 1
#define MENU_SHOW 1
#define MENU_QUIT 2

struct _OtpWindowsTrayShell
{
    HWND messages;
    HWND broadcast;
    HINSTANCE instance;
    NOTIFYICONDATAW icon;
    HMENU menu;
    guint message_watch;
    guint broadcast_watch;
    guint retry_source;
    UINT taskbar_created;
    DWORD thread_id;
    gboolean enabled;
    gboolean published;
    OtpWindowsTrayCallback callback;
    gpointer data;
};

static const WCHAR window_class[] = L"OTPClient.Windows.Tray";

static void
set_published (OtpWindowsTrayShell *shell, gboolean published)
{
    if (shell->published == published)
        return;
    shell->published = published;
    shell->callback (shell, OTP_TRAY_PUBLICATION_CHANGED, shell->data);
}

gboolean
otp_windows_tray_shell_is_available (void)
{
    /* WINDOWS-STUB: Shell_TrayWnd is an Explorer heuristic, not a documented
     * notification-area capability query. Alternate shells/policy hiding the
     * notification area are UNVERIFIED. Only NIM_ADD permits hiding a window. */
    return FindWindowW (L"Shell_TrayWnd", NULL) != NULL;
}

static void
publish (OtpWindowsTrayShell *shell)
{
    if (!shell->enabled || shell->published || !otp_windows_tray_shell_is_available ())
        return;
    if (!Shell_NotifyIconW (NIM_ADD, &shell->icon))
        return;
    shell->icon.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW (NIM_SETVERSION, &shell->icon))
    {
        Shell_NotifyIconW (NIM_DELETE, &shell->icon);
        g_warning ("Windows tray does not support notification icon version 4");
        return;
    }
    set_published (shell, TRUE);
}

static gboolean
retry_publish (gpointer data)
{
    OtpWindowsTrayShell *shell = data;
    if (shell->published &&
        (!otp_windows_tray_shell_is_available () ||
         !Shell_NotifyIconW (NIM_MODIFY, &shell->icon)))
    {
        Shell_NotifyIconW (NIM_DELETE, &shell->icon);
        set_published (shell, FALSE);
    }
    publish (shell);
    return G_SOURCE_CONTINUE;
}

static void
popup_menu (OtpWindowsTrayShell *shell, LPARAM position)
{
    POINT point = { GET_X_LPARAM (position), GET_Y_LPARAM (position) };
    if (point.x == -1 && point.y == -1)
        GetCursorPos (&point);
    /* A message-only window cannot be foreground. The hidden top-level window
     * is the menu owner; foreground + WM_NULL follow the shell menu recipe.
     * UNVERIFIED: focus restoration and keyboard dismissal in native GTK. */
    SetForegroundWindow (shell->broadcast);
    UINT command = TrackPopupMenu (shell->menu,
                                  TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                  point.x, point.y, 0, shell->broadcast, NULL);
    PostMessageW (shell->broadcast, WM_NULL, 0, 0);
    if (command == MENU_SHOW)
        shell->callback (shell, OTP_TRAY_SHOW, shell->data);
    else if (command == MENU_QUIT)
        shell->callback (shell, OTP_TRAY_QUIT, shell->data);
}

static LRESULT CALLBACK
window_proc (HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    LONG_PTR user_data = GetWindowLongPtrW (window, GWLP_USERDATA);
    OtpWindowsTrayShell *shell = (OtpWindowsTrayShell *) user_data;
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *) lparam;
        shell = create->lpCreateParams;
        SetWindowLongPtrW (window, GWLP_USERDATA, (LONG_PTR) shell);
    }
    if (shell != NULL && message == shell->taskbar_created)
    {
        /* Explorer discarded all icons. Re-negotiate version on every add. */
        Shell_NotifyIconW (NIM_DELETE, &shell->icon);
        set_published (shell, FALSE);
        publish (shell);
        return 0;
    }
    if (shell != NULL && message == TRAY_CALLBACK_MESSAGE && shell->published &&
        HIWORD (lparam) == TRAY_ICON_ID)
    {
        switch (LOWORD (lparam))
        {
            /* Version 4 sends NIN_SELECT rather than WM_LBUTTONUP. Handling
             * both would toggle twice; NIN_KEYSELECT covers keyboard access. */
            case NIN_SELECT:
            case NIN_KEYSELECT:
                shell->callback (shell, OTP_TRAY_TOGGLE, shell->data);
                break;
            case WM_CONTEXTMENU:
                popup_menu (shell, (LPARAM) wparam);
                break;
            default:
                break;
        }
        return 0;
    }
    if (message == WM_NCDESTROY)
        SetWindowLongPtrW (window, GWLP_USERDATA, 0);
    return DefWindowProcW (window, message, wparam, lparam);
}

static gboolean
dispatch_messages (GIOChannel *channel, GIOCondition condition, gpointer data)
{
    (void) channel;
    (void) condition;
    MSG message;
    HWND window = data;
    /* Consume only our window's queue; never steal GTK or WM_QUIT messages.
     * GDK can dispatch these first too, using the same window procedure.
     * UNVERIFIED: coexistence with the staged MSYS2 GTK/GLib versions. */
    for (guint i = 0; i < 32 && PeekMessageW (&message, window, 0, 0, PM_REMOVE); i++)
    {
        TranslateMessage (&message);
        DispatchMessageW (&message);
    }
    return G_SOURCE_CONTINUE;
}

static guint
watch_messages (HWND window)
{
    GIOChannel *channel = g_io_channel_win32_new_messages ((gsize) window);
    guint id = g_io_add_watch (channel, G_IO_IN, dispatch_messages, window);
    g_io_channel_unref (channel);
    return id;
}

OtpWindowsTrayShell *
otp_windows_tray_shell_new (HICON icon, const gchar *show_label, const gchar *quit_label,
                            OtpWindowsTrayCallback callback, gpointer data)
{
    g_return_val_if_fail (icon != NULL && callback != NULL, NULL);
    OtpWindowsTrayShell *shell = g_new0 (OtpWindowsTrayShell, 1);
    g_autofree gunichar2 *show = NULL;
    g_autofree gunichar2 *quit = NULL;
    shell->instance = GetModuleHandleW (NULL);
    shell->thread_id = GetCurrentThreadId ();
    shell->callback = callback;
    shell->data = data;
    shell->taskbar_created = RegisterWindowMessageW (L"TaskbarCreated");
    WNDCLASSW klass = { 0 };
    klass.lpfnWndProc = window_proc;
    klass.hInstance = shell->instance;
    klass.lpszClassName = window_class;
    if (shell->taskbar_created == 0 ||
        (!RegisterClassW (&klass) && GetLastError () != ERROR_CLASS_ALREADY_EXISTS))
        goto fail;
    shell->messages = CreateWindowExW (0, window_class, L"", 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, NULL, shell->instance, shell);
    /* HWND_MESSAGE never receives broadcasts. No WS_VISIBLE and TOOLWINDOW
     * keep this separate receiver out of the taskbar and Alt+Tab. */
    shell->broadcast = CreateWindowExW (WS_EX_TOOLWINDOW, window_class, L"", WS_POPUP,
                                        0, 0, 0, 0, NULL, NULL, shell->instance, shell);
    if (shell->messages == NULL || shell->broadcast == NULL)
        goto fail;
    shell->menu = CreatePopupMenu ();
    show = g_utf8_to_utf16 (show_label, -1, NULL, NULL, NULL);
    quit = g_utf8_to_utf16 (quit_label, -1, NULL, NULL, NULL);
    if (shell->menu == NULL || show == NULL || quit == NULL ||
        !AppendMenuW (shell->menu, MF_STRING, MENU_SHOW, (LPCWSTR) show) ||
        !AppendMenuW (shell->menu, MF_STRING, MENU_QUIT, (LPCWSTR) quit))
        goto fail;
    SetMenuDefaultItem (shell->menu, MENU_SHOW, FALSE);
    shell->icon.cbSize = sizeof (shell->icon);
    shell->icon.hWnd = shell->messages;
    /* HWND + uID avoids a global GUID collision if two app processes run.
     * This does not choose or change the application's single-instance policy. */
    shell->icon.uID = TRAY_ICON_ID;
    shell->icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    shell->icon.uCallbackMessage = TRAY_CALLBACK_MESSAGE;
    shell->icon.hIcon = icon; /* borrowed; glue destroys after shell teardown */
    lstrcpynW (shell->icon.szTip, L"OTPClient", G_N_ELEMENTS (shell->icon.szTip));
    shell->message_watch = watch_messages (shell->messages);
    shell->broadcast_watch = watch_messages (shell->broadcast);
    return shell;

fail:
    g_warning ("Cannot create Windows tray resources (Win32 error %lu)", GetLastError ());
    otp_windows_tray_shell_free (shell);
    return NULL;
}

void
otp_windows_tray_shell_set_enabled (OtpWindowsTrayShell *shell, gboolean enabled)
{
    g_return_if_fail (GetCurrentThreadId () == shell->thread_id);
    shell->enabled = enabled;
    if (enabled)
    {
        publish (shell);
        if (shell->retry_source == 0)
            shell->retry_source = g_timeout_add_seconds (1, retry_publish, shell);
    }
    else
    {
        g_clear_handle_id (&shell->retry_source, g_source_remove);
        Shell_NotifyIconW (NIM_DELETE, &shell->icon);
        set_published (shell, FALSE);
    }
}

void
otp_windows_tray_shell_set_tooltip (OtpWindowsTrayShell *shell, const gchar *text)
{
    g_return_if_fail (GetCurrentThreadId () == shell->thread_id);
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (text, -1, NULL, NULL, NULL);
    if (wide == NULL)
        return;
    lstrcpynW (shell->icon.szTip, (LPCWSTR) wide, G_N_ELEMENTS (shell->icon.szTip));
    if (shell->published && !Shell_NotifyIconW (NIM_MODIFY, &shell->icon))
    {
        Shell_NotifyIconW (NIM_DELETE, &shell->icon);
        set_published (shell, FALSE);
    }
}

gboolean
otp_windows_tray_shell_is_published (OtpWindowsTrayShell *shell)
{
    return shell != NULL && shell->published;
}

#ifdef OTP_WINDOWS_TRAY_CHECK
void
otp_windows_tray_shell_check_post_selection (OtpWindowsTrayShell *shell, gboolean valid_id)
{
    PostMessageW (shell->messages, TRAY_CALLBACK_MESSAGE, 0,
                  MAKELPARAM (NIN_SELECT, valid_id ? TRAY_ICON_ID : TRAY_ICON_ID + 1));
}
#endif

void
otp_windows_tray_shell_free (OtpWindowsTrayShell *shell)
{
    if (shell == NULL)
        return;
    g_return_if_fail (GetCurrentThreadId () == shell->thread_id);
    g_clear_handle_id (&shell->retry_source, g_source_remove);
    g_clear_handle_id (&shell->message_watch, g_source_remove);
    g_clear_handle_id (&shell->broadcast_watch, g_source_remove);
    if (shell->icon.hWnd != NULL)
        Shell_NotifyIconW (NIM_DELETE, &shell->icon);
    /* No callback during teardown, and no PostQuitMessage: GTK owns the loop. */
    if (shell->messages != NULL)
        DestroyWindow (shell->messages);
    if (shell->broadcast != NULL)
        DestroyWindow (shell->broadcast);
    if (shell->menu != NULL)
        DestroyMenu (shell->menu);
    g_free (shell);
}

HICON
otp_windows_tray_icon_from_pixels (const guchar *pixels, gint width, gint height,
                                  gint stride, gint channels)
{
    g_return_val_if_fail (pixels != NULL && width > 0 && width <= 256 &&
                         height > 0 && height <= 256 &&
                         (channels == 3 || channels == 4) && stride >= width * channels, NULL);
    BITMAPINFO info = { 0 };
    info.bmiHeader.biSize = sizeof (BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    guchar *bits = NULL;
    HBITMAP color = CreateDIBSection (NULL, &info, DIB_RGB_COLORS, (void **) &bits, NULL, 0);
    if (color == NULL)
        return NULL;
    for (gint y = 0; y < height; y++)
        for (gint x = 0; x < width; x++)
        {
            const guchar *p = pixels + y * stride + x * channels;
            guchar *out = bits + (y * width + x) * 4;
            guint alpha = channels == 4 ? p[3] : 255;
            /* Win32 32-bit alpha icons use premultiplied BGRA. */
            out[0] = (p[2] * alpha + 127) / 255;
            out[1] = (p[1] * alpha + 127) / 255;
            out[2] = (p[0] * alpha + 127) / 255;
            out[3] = alpha;
        }
    /* Zero AND mask; transparency comes from the 32-bit alpha channel. */
    gsize mask_size = ((width + 15) / 16) * 2 * height;
    g_autofree guchar *mask_bits = g_malloc0 (mask_size);
    HBITMAP mask = CreateBitmap (width, height, 1, 1, mask_bits);
    HICON result = NULL;
    if (mask != NULL)
    {
        ICONINFO icon_info = { 0 };
        icon_info.fIcon = TRUE;
        icon_info.hbmColor = color;
        icon_info.hbmMask = mask;
        result = CreateIconIndirect (&icon_info);
        DeleteObject (mask);
    }
    DeleteObject (color);
    /* UNVERIFIED: alpha rendering, DPI transitions and dark-mode contrast. */
    return result;
}
