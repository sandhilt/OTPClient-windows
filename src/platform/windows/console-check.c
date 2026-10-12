/* Hidden child consoles keep generated controls away from the runner's console.
 * The parent terminates a hung child and continues the remaining TAP tests. */
#include "compat/otp-windows.h"
#include "compat/termios.h"
#include <errno.h>

void otp_console_set_wait_event (HANDLE event);

static volatile sig_atomic_t received;
static HANDLE received_event;
static HANDLE pending_event;
static HANDLE finished_event;

static void
record_signal (int signo)
{
    received = signo;
    SetEvent (received_event);
}

typedef struct {
    DWORD control;
    gboolean early;
    ULONGLONG sent_at;
} ControlRequest;

static DWORD WINAPI
send_control (void *data)
{
    ControlRequest *request = data;
    if (!request->early) {
        if (WaitForSingleObject (pending_event, 2000) != WAIT_OBJECT_0) return 61;
        /* The shim handshake is after all queued noise/partial characters
         * have been consumed. No Enter was supplied; the read must still be
         * pending after 100 ms, rather than treating readiness as a line. */
        if (WaitForSingleObject (finished_event, 100) != WAIT_TIMEOUT) return 62;
    }
    request->sent_at = GetTickCount64 ();
    /* Injecting a ^C KEY_EVENT with WriteConsoleInputW does not produce a
     * console control event (confirmed by the native harness). Generate the
     * real event, in this child's isolated console, to exercise the callback. */
    return GenerateConsoleCtrlEvent (request->control, 0) ? 0 : 63;
}

static BOOL WINAPI
fallback_control (DWORD control)
{
    if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT) ExitProcess (77);
    return FALSE;
}

static void
write_records (HANDLE input, INPUT_RECORD *records, DWORD count)
{
    DWORD written = 0;
    g_assert_true (WriteConsoleInputW (input, records, count, &written));
    g_assert_cmpuint (written, ==, count);
}

static void
write_key (HANDLE input, WCHAR character, WORD repeat)
{
    INPUT_RECORD record = {0};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = repeat;
    record.Event.KeyEvent.uChar.UnicodeChar = character;
    write_records (input, &record, 1);
}

static void
write_noise (HANDLE input)
{
    INPUT_RECORD records[5] = {0};
    records[0].EventType = MOUSE_EVENT;
    records[1].EventType = FOCUS_EVENT;
    records[2].EventType = WINDOW_BUFFER_SIZE_EVENT;
    records[2].Event.WindowBufferSizeEvent.dwSize.X = 80;
    records[2].Event.WindowBufferSizeEvent.dwSize.Y = 25;
    records[3].EventType = KEY_EVENT;
    records[3].Event.KeyEvent.bKeyDown = FALSE;
    records[3].Event.KeyEvent.wRepeatCount = 1;
    records[3].Event.KeyEvent.uChar.UnicodeChar = L'x';
    records[4].EventType = KEY_EVENT;
    records[4].Event.KeyEvent.bKeyDown = TRUE; /* No character (e.g. a modifier). */
    records[4].Event.KeyEvent.wRepeatCount = 1;
    write_records (input, records, G_N_ELEMENTS (records));
}

static void
write_line (HANDLE input, const WCHAR *text)
{
    while (*text) write_key (input, *text++, 1);
    write_key (input, L'\r', 1);
}

static void
assert_line (int fd, const char *expected)
{
    /* Deliberately match get_pwd's one-byte reads, including UTF-8 split
     * across calls. Only dummy data is ever injected or compared here. */
    for (gsize i = 0; expected[i]; i++) {
        char byte = 0;
        g_assert_cmpint (otp_read (fd, &byte, 1), ==, 1);
        g_assert_cmpint ((guchar) byte, ==, (guchar) expected[i]);
    }
}

static void
assert_sender (HANDLE sender, ControlRequest *request)
{
    g_assert_cmpuint (WaitForSingleObject (sender, 1000), ==, WAIT_OBJECT_0);
    DWORD result = 0;
    g_assert_true (GetExitCodeThread (sender, &result));
    g_assert_cmpuint (result, ==, 0);
    g_assert_cmpuint (GetTickCount64 () - request->sent_at, <, 2000);
    CloseHandle (sender);
}

static void
assert_mode (int fd, DWORD expected)
{
    struct termios mode;
    g_assert_cmpint (tcgetattr (fd, &mode), ==, 0);
    g_assert_cmpuint (mode.c_lflag, ==, expected);
}

static void
install_actions (struct sigaction *old_int, struct sigaction *old_break)
{
    struct sigaction action = {0};
    action.sa_handler = record_signal;
    received = 0;
    g_assert_true (ResetEvent (received_event));
    g_assert_cmpint (otp_sigaction (SIGINT, &action, old_int), ==, 0);
    g_assert_cmpint (otp_sigaction (SIGHUP, &action, old_break), ==, 0);
}

static void
restore_actions (struct sigaction *old_int, struct sigaction *old_break)
{
    g_assert_cmpint (otp_sigaction (SIGINT, old_int, NULL), ==, 0);
    g_assert_cmpint (otp_sigaction (SIGHUP, old_break, NULL), ==, 0);
}

static void
test_child_lines (int fd, HANDLE input, const struct termios *quiet, const struct termios *old)
{
    HANDLE output = GetStdHandle (STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO before, after;
    WCHAR screen_before[80], screen_after[80];
    DWORD screen_length = 0, after_length = 0;
    COORD origin = {0, 0};
    g_assert_true (GetConsoleScreenBufferInfo (output, &before));
    g_assert_true (ReadConsoleOutputCharacterW (output, screen_before, 80, origin, &screen_length));
    for (guint cycle = 0; cycle < 20; cycle++) {
        struct sigaction old_int = {0}, old_break = {0};
        install_actions (&old_int, &old_break);
        /* Typeahead in the OS queue must be discarded at prompt entry. */
        write_line (input, L"discard");
        g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, quiet), ==, 0);
        DWORD events = 1;
        g_assert_true (GetNumberOfConsoleInputEvents (input, &events));
        g_assert_cmpuint (events, ==, 0);
        assert_mode (fd, quiet->c_lflag);
        write_noise (input);
        write_key (input, L'\b', 1); /* Empty-line backspace. */
        write_key (input, L'a', 1);
        write_key (input, 0x00e9, 1);
        write_key (input, 0xd83d, 1);
        write_key (input, 0xde00, 1);
        write_key (input, L'\b', 1); /* Delete the entire supplementary code point. */
        write_key (input, 0x4e2d, 1);
        write_key (input, L'x', 3);
        write_key (input, L'\b', 2);
        write_key (input, L'\r', 1);
        assert_line (fd, "a\xc3\xa9\xe4\xb8\xadx\n");

        write_key (input, 0xd83d, 1);
        write_key (input, 0xde00, 1);
        write_key (input, L'\r', 1);
        assert_line (fd, "\xf0\x9f\x98\x80\n");
        write_key (input, 0xd83d, 1);
        write_key (input, L'\b', 1); /* Remove a pending, unpaired high surrogate. */
        write_key (input, 0xde00, 1);
        write_key (input, L'\r', 1);
        assert_line (fd, "\xef\xbf\xbd\n");

        /* get_pwd stops at 511 bytes. TCSAFLUSH must discard the unread shim
         * suffix as well as queued typeahead before the next prompt. */
        write_key (input, L'z', 600);
        write_key (input, L'\r', 1);
        for (guint i = 0; i < 511; i++) {
            char byte = 0;
            g_assert_cmpint (otp_read (fd, &byte, 1), ==, 1);
            g_assert_cmpint (byte, ==, 'z');
        }
        write_line (input, L"discard-again");
        g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, old), ==, 0);
        assert_mode (fd, old->c_lflag);
        restore_actions (&old_int, &old_break);
        install_actions (&old_int, &old_break);
        g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, quiet), ==, 0);
        write_line (input, L"next-prompt");
        assert_line (fd, "next-prompt\n");
        g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, old), ==, 0);
        assert_mode (fd, old->c_lflag);
        restore_actions (&old_int, &old_break);
    }
    g_assert_true (GetConsoleScreenBufferInfo (output, &after));
    g_assert_cmpint (before.dwCursorPosition.X, ==, after.dwCursorPosition.X);
    g_assert_cmpint (before.dwCursorPosition.Y, ==, after.dwCursorPosition.Y);
    g_assert_true (ReadConsoleOutputCharacterW (output, screen_after, 80, origin, &after_length));
    g_assert_cmpuint (screen_length, ==, after_length);
    g_assert_cmpmem (screen_before, screen_length * sizeof (WCHAR),
        screen_after, after_length * sizeof (WCHAR));
}

static int
console_child (const char *scenario)
{
    /* Do not inherit a runner's ignore-Ctrl+C attribute into the baseline. */
    g_assert_true (SetConsoleCtrlHandler (NULL, FALSE));
    received_event = CreateEventW (NULL, TRUE, FALSE, NULL);
    pending_event = CreateEventW (NULL, FALSE, FALSE, NULL);
    finished_event = CreateEventW (NULL, TRUE, FALSE, NULL);
    g_assert_nonnull (received_event);
    g_assert_nonnull (pending_event);
    g_assert_nonnull (finished_event);
    otp_console_set_wait_event (pending_event);
    HANDLE input = CreateFileW (L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    g_assert_true (input != INVALID_HANDLE_VALUE);
    int fd = _open_osfhandle ((intptr_t) input, _O_RDONLY | _O_TEXT);
    g_assert_cmpint (fd, >=, 0);
    struct termios old_mode, quiet_mode;
    g_assert_cmpint (tcgetattr (fd, &old_mode), ==, 0);
    quiet_mode = old_mode;
    quiet_mode.c_lflag |= ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT;
    quiet_mode.c_lflag &= ~ECHO;
    g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, &quiet_mode), ==, 0);

    if (g_str_equal (scenario, "lines")) {
        test_child_lines (fd, input, &quiet_mode, &old_mode);
        _close (fd);
        return 0;
    }
    if (g_str_equal (scenario, "ignored")) {
        struct sigaction ignored = {0}, old_int = {0}, old_break = {0};
        ignored.sa_handler = SIG_IGN;
        g_assert_cmpint (otp_sigaction (SIGINT, &ignored, &old_int), ==, 0);
        g_assert_cmpint (otp_sigaction (SIGHUP, &ignored, &old_break), ==, 0);
        g_assert_true (GenerateConsoleCtrlEvent (CTRL_C_EVENT, 0));
        g_assert_true (GenerateConsoleCtrlEvent (CTRL_BREAK_EVENT, 0));
        Sleep (100);
        g_assert_cmpint (received, ==, 0);
        write_line (input, L"ignored");
        assert_line (fd, "ignored\n");
        restore_actions (&old_int, &old_break);
        g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, &old_mode), ==, 0);
        _close (fd);
        return 0;
    }
    if (g_str_has_prefix (scenario, "default-") || g_str_has_prefix (scenario, "crt-")) {
        DWORD control = g_str_has_suffix (scenario, "break") ? CTRL_BREAK_EVENT : CTRL_C_EVENT;
        ControlRequest request = {control, FALSE, 0};
        HANDLE sender = CreateThread (NULL, 0, send_control, &request, 0, NULL);
        g_assert_nonnull (sender);
        char byte = 0;
        if (g_str_has_prefix (scenario, "crt-")) {
            /* Baseline: no shim registration. A sender waits 100 ms after
             * entry, then delivers the control during a real CRT line read. */
            SetEvent (pending_event);
            _read (fd, &byte, 1);
        } else otp_read (fd, &byte, 1);
        SetEvent (finished_event);
        assert_sender (sender, &request);
        return 71; /* A default control must terminate the process. */
    }

    /* Registered before the shim: reached only after dispositions restore. */
    g_assert_true (SetConsoleCtrlHandler (fallback_control, TRUE));
    DWORD control = g_str_equal (scenario, "break") ? CTRL_BREAK_EVENT : CTRL_C_EVENT;
    gboolean dual = g_str_equal (scenario, "dual");
    gboolean early = g_str_equal (scenario, "early") || dual;
    for (guint i = 0; i < 20; i++) {
        struct sigaction old_int = {0}, old_break = {0};
        install_actions (&old_int, &old_break);
        g_assert_true (old_int.sa_handler == SIG_DFL);
        g_assert_true (old_break.sa_handler == SIG_DFL);
        g_assert_true (ResetEvent (pending_event));
        g_assert_true (ResetEvent (finished_event));
        g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, &quiet_mode), ==, 0);
        if (!early) {
            write_noise (input);
            if (i % 2 == 0) {
                write_key (input, L'a', 3);
                write_key (input, 0xd83d, 1); /* Partial UTF-16 also gets wiped. */
            }
        }
        ControlRequest request = {control, early, 0};
        HANDLE sender = CreateThread (NULL, 0, send_control, &request, 0, NULL);
        g_assert_nonnull (sender);
        if (early) {
            g_assert_cmpuint (WaitForSingleObject (received_event, 1000), ==, WAIT_OBJECT_0);
            if (dual) {
                g_assert_true (ResetEvent (received_event));
                g_assert_true (GenerateConsoleCtrlEvent (CTRL_BREAK_EVENT, 0));
                g_assert_cmpuint (WaitForSingleObject (received_event, 1000), ==, WAIT_OBJECT_0);
            }
            /* A prompt's initial TCSAFLUSH must not clear a pending signal. */
            g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, &quiet_mode), ==, 0);
        }
        char byte = 0;
        errno = 0;
        g_assert_cmpint (otp_read (fd, &byte, 1), ==, -1);
        g_assert_cmpint (errno, ==, EINTR);
        SetEvent (finished_event);
        g_assert_cmpint (received, ==, dual || control == CTRL_BREAK_EVENT ? SIGBREAK : SIGINT);
        assert_sender (sender, &request);
        /* Consuming EINTR resets the event and BOTH per-signal latches.
         * An ordinary Enter must now work with the same handlers installed. */
        write_line (input, L"after-interrupt");
        assert_line (fd, "after-interrupt\n");
        g_assert_cmpint (tcsetattr (fd, TCSAFLUSH, &old_mode), ==, 0);
        assert_mode (fd, old_mode.c_lflag);
        restore_actions (&old_int, &old_break);
    }
    _close (fd);
    if (g_str_equal (scenario, "raise")) {
        raise (SIGINT); /* get_pwd's final step: CRT default must be restored. */
        return 71;
    }
    g_assert_true (GenerateConsoleCtrlEvent (control, 0));
    Sleep (1000);
    return 72; /* The lower fallback must have exited with 77. */
}

static gboolean
run_child (const char *scenario, DWORD *exit_code)
{
    WCHAR executable[32768];
    DWORD length = GetModuleFileNameW (NULL, executable, G_N_ELEMENTS (executable));
    if (length == 0 || length >= G_N_ELEMENTS (executable)) {
        g_test_fail_printf ("GetModuleFileNameW failed: error=%lu", (unsigned long) GetLastError ());
        return FALSE;
    }
    g_autofree gchar *utf8 = g_utf16_to_utf8 ((gunichar2 *) executable, -1, NULL, NULL, NULL);
    g_autofree gchar *command = g_strdup_printf ("\"%s\" --console-child %s", utf8, scenario);
    g_autofree gunichar2 *wide = g_utf8_to_utf16 (command, -1, NULL, NULL, NULL);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof (startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {0};
    ULONGLONG start = GetTickCount64 ();
    if (!CreateProcessW (executable, (WCHAR *) wide, NULL, NULL, FALSE,
        CREATE_NEW_CONSOLE, NULL, NULL, &startup, &process)) {
        g_test_fail_printf ("console child %s: CreateProcessW error=%lu",
            scenario, (unsigned long) GetLastError ());
        return FALSE;
    }
    DWORD wait = WaitForSingleObject (process.hProcess, 8000);
    gboolean ok = wait == WAIT_OBJECT_0;
    if (!ok) {
        DWORD error = GetLastError ();
        BOOL killed = TerminateProcess (process.hProcess, 73);
        DWORD reaped = WaitForSingleObject (process.hProcess, 1000);
        g_test_fail_printf ("console child %s exceeded 8000 ms or wait failed: "
            "wait=%lu error=%lu killed=%d reap=%lu",
            scenario, (unsigned long) wait, (unsigned long) error,
            (int) killed, (unsigned long) reaped);
    }
    if (!GetExitCodeProcess (process.hProcess, exit_code)) {
        g_test_fail_printf ("console child %s: GetExitCodeProcess error=%lu",
            scenario, (unsigned long) GetLastError ());
        ok = FALSE;
    }
    CloseHandle (process.hThread);
    CloseHandle (process.hProcess);
    g_test_message ("console child %s: wait=%lu exit=%lu elapsed=%llu ms",
        scenario, (unsigned long) wait, (unsigned long) *exit_code,
        (unsigned long long) (GetTickCount64 () - start));
    return ok;
}

static void
assert_child (const char *scenario, DWORD expected)
{
    DWORD exit_code = 0;
    if (run_child (scenario, &exit_code) && exit_code != expected)
        g_test_fail_printf ("console child %s: exit=%lu expected=%lu",
            scenario, (unsigned long) exit_code, (unsigned long) expected);
}

static void
test_console_interrupt (void)
{
    assert_child ("int", 77);
    assert_child ("break", 77);
    assert_child ("early", 77);
    assert_child ("dual", 77);
    assert_child ("raise", 3);
    assert_child ("ignored", 0);
}

static void
test_console_lines (void)
{
    assert_child ("lines", 0);
}

static void
test_console_default (void)
{
    const char *crt[] = {"crt-int", "crt-break"};
    const char *shim[] = {"default-int", "default-break"};
    for (guint i = 0; i < G_N_ELEMENTS (crt); i++) {
        DWORD expected = 0, actual = 0;
        gboolean baseline_ok = run_child (crt[i], &expected);
        gboolean shim_ok = run_child (shim[i], &actual);
        if (baseline_ok && shim_ok &&
            (expected == 0 || expected == 71 || actual != expected)) {
            g_test_fail_printf ("default control changed: %s exit=%lu %s exit=%lu",
                crt[i], (unsigned long) expected, shim[i], (unsigned long) actual);
        }
    }
}

static void
check_crt_read (int fd)
{
    const char bytes[] = "a\r\nb\032c";
    char buffer[sizeof (bytes)] = {0};
    g_assert_cmpint (_setmode (fd, _O_TEXT), >=, 0);
    g_assert_cmpint (otp_read (fd, buffer, sizeof (buffer)), ==, 3);
    g_assert_cmpmem (buffer, 3, "a\nb", 3);
    g_assert_cmpint (otp_read (fd, buffer, sizeof (buffer)), ==, 0);
}

static void
test_nonconsole_read (void)
{
    const char bytes[] = "a\r\nb\032c";
    int pipe_fds[2];
    g_assert_cmpint (_pipe (pipe_fds, 512, _O_BINARY), ==, 0);
    g_assert_cmpint (_write (pipe_fds[1], bytes, sizeof (bytes) - 1), ==, sizeof (bytes) - 1);
    _close (pipe_fds[1]);
    check_crt_read (pipe_fds[0]);
    _close (pipe_fds[0]);

    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    int fd = g_file_open_tmp ("otp-console-check-XXXXXX", &path, &error);
    g_assert_no_error (error);
    g_assert_cmpint (fd, >=, 0);
    g_assert_cmpint (_setmode (fd, _O_BINARY), >=, 0);
    g_assert_cmpint (_write (fd, bytes, sizeof (bytes) - 1), ==, sizeof (bytes) - 1);
    g_assert_cmpint (_lseek (fd, 0, SEEK_SET), ==, 0);
    check_crt_read (fd);
    g_assert_cmpint (_setmode (fd, _O_BINARY), >=, 0);
    g_assert_cmpint (_lseek (fd, 0, SEEK_SET), ==, 0);
    char buffer[sizeof (bytes)] = {0};
    g_assert_cmpint (otp_read (fd, buffer, sizeof (buffer)), ==, sizeof (bytes) - 1);
    g_assert_cmpmem (buffer, sizeof (bytes) - 1, bytes, sizeof (bytes) - 1);
    g_assert_cmpint (otp_read (fd, buffer, 0), ==, 0);
    _close (fd);
    g_assert_cmpint (g_remove (path), ==, 0);
    fd = _open ("NUL", _O_RDONLY | _O_TEXT);
    g_assert_cmpint (fd, >=, 0);
    g_assert_cmpint (otp_read (fd, buffer, sizeof (buffer)), ==, 0);
    g_assert_cmpint (otp_read (fd, buffer, 0), ==, 0);
    _close (fd);
}

int
main (int argc, char **argv)
{
    g_assert_nonnull (gcry_check_version (NULL));
    g_assert_cmpint (gcry_control (GCRYCTL_INIT_SECMEM, 1024 * 1024, 0), ==, 0);
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
    if (argc == 3 && g_str_equal (argv[1], "--console-child"))
        return console_child (argv[2]);
    g_test_init (&argc, &argv, NULL);
    g_test_set_nonfatal_assertions ();
    g_test_add_func ("/windows/console-interrupt", test_console_interrupt);
    g_test_add_func ("/windows/console-lines", test_console_lines);
    g_test_add_func ("/windows/console-default", test_console_default);
    g_test_add_func ("/windows/nonconsole-read", test_nonconsole_read);
    return g_test_run ();
}
