#include "compat/otp-windows.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

typedef struct {
    void (*handler)(int);
    DWORD thread_id;
    gboolean interrupted;
} ConsoleAction;

typedef struct {
    gsize capacity;
    gsize length;
    gsize offset;
    WCHAR high_surrogate;
    gboolean ready;
    gchar bytes[];
} ConsoleLine;

typedef BOOL (WINAPI *ConsoleInputNowait) (HANDLE, PINPUT_RECORD, DWORD, LPDWORD, USHORT);

static SRWLOCK console_lock = SRWLOCK_INIT;
static ConsoleAction console_actions[2] = {{SIG_DFL, 0, FALSE}, {SIG_DFL, 0, FALSE}};
static gboolean console_handler_installed;
static HANDLE interrupt_event; /* Process lifetime; never closed under a waiter. */
static ConsoleInputNowait console_input_nowait;
static ConsoleLine *console_line;
static HANDLE console_input;
static DWORD console_thread;
static gboolean console_read_active;

#ifdef OTP_WINDOWS_CONSOLE_CHECK
/* Test-only handshake: the input queue is empty and the read is about to wait.
 * No hook or exported test entry point is compiled into the CLI/shim checks. */
static HANDLE wait_event;
void
otp_console_set_wait_event (HANDLE event)
{
    wait_event = event;
}
#endif

static gboolean
real_handler (void (*handler)(int))
{
    return handler != SIG_DFL && handler != SIG_IGN;
}

static void
clear_line (void)
{
    if (console_line) {
        SecureZeroMemory (console_line, sizeof (*console_line) + console_line->capacity);
        gcry_free (console_line);
        console_line = NULL;
    }
}

static gboolean
any_interrupt (void)
{
    return console_actions[0].interrupted || console_actions[1].interrupted;
}

static gboolean
consume_interrupt (DWORD thread_id)
{
    gboolean interrupted = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS (console_actions); i++) {
        if (console_actions[i].thread_id == thread_id && console_actions[i].interrupted) {
            console_actions[i].interrupted = FALSE;
            interrupted = TRUE;
        }
    }
    if (interrupted) {
        clear_line ();
        if (!any_interrupt ()) ResetEvent (interrupt_event);
    }
    return interrupted;
}

static BOOL WINAPI
console_control (DWORD control)
{
    int index;
    int signo;
    if (control == CTRL_C_EVENT) { index = 0; signo = SIGINT; }
    else if (control == CTRL_BREAK_EVENT) { index = 1; signo = SIGBREAK; }
    else return FALSE;

    AcquireSRWLockExclusive (&console_lock);
    ConsoleAction *action = &console_actions[index];
    if (!real_handler (action->handler)) {
        ReleaseSRWLockExclusive (&console_lock);
        return FALSE; /* Leave SIG_DFL/SIG_IGN and other events to the CRT/OS. */
    }
    /* Invoke the password flag setter before making EINTR visible. Handlers
     * must be leaf handlers: no CRT I/O or shim reentry under this lock. */
    action->handler (signo);
    action->interrupted = TRUE;
    SetEvent (interrupt_event);
    ReleaseSRWLockExclusive (&console_lock);
    return TRUE;
}

static gboolean
ensure_event (void)
{
    if (!interrupt_event) interrupt_event = CreateEventW (NULL, TRUE, FALSE, NULL);
    if (interrupt_event) return TRUE;
    errno = EIO;
    return FALSE;
}

int
otp_sigaction (int signo, const struct sigaction *action, struct sigaction *old)
{
    /* WINDOWS-STUB: POSIX masks/flags, real SIGHUP, and asynchronous SIGTERM
     * interruption are unsupported. SIGHUP is the existing SIGBREAK alias.
     * Console registrations/readers are limited to one thread, as in the CLI. */
    if (!action) { errno = EINVAL; return -1; }
    int index = signo == SIGINT ? 0 : signo == SIGBREAK ? 1 : -1;
    if (index < 0) {
        void (*previous)(int) = signal (signo, action->sa_handler);
        if (previous == SIG_ERR) return -1;
        if (old) { old->sa_handler = previous; old->sa_mask = old->sa_flags = 0; }
        return 0;
    }

    AcquireSRWLockExclusive (&console_lock);
    DWORD thread_id = GetCurrentThreadId ();
    if (real_handler (action->sa_handler) &&
        real_handler (console_actions[1 - index].handler) &&
        console_actions[1 - index].thread_id != thread_id) {
        ReleaseSRWLockExclusive (&console_lock);
        errno = EBUSY;
        return -1;
    }
    if (!ensure_event ()) {
        ReleaseSRWLockExclusive (&console_lock);
        return -1;
    }
    void (*previous)(int) = signal (signo, action->sa_handler);
    if (previous == SIG_ERR) {
        ReleaseSRWLockExclusive (&console_lock);
        return -1;
    }
    gboolean needed = real_handler (action->sa_handler) ||
        real_handler (console_actions[1 - index].handler);
    if (needed != console_handler_installed &&
        !SetConsoleCtrlHandler (console_control, needed)) {
        signal (signo, previous);
        ReleaseSRWLockExclusive (&console_lock);
        errno = EIO;
        return -1;
    }
    console_handler_installed = needed;
    console_actions[index].handler = action->sa_handler;
    console_actions[index].thread_id = thread_id;
    console_actions[index].interrupted = FALSE;
    if (!any_interrupt ()) ResetEvent (interrupt_event);
    if (console_thread == thread_id && !console_read_active) clear_line ();
    if (old) { old->sa_handler = previous; old->sa_mask = old->sa_flags = 0; }
    ReleaseSRWLockExclusive (&console_lock);
    return 0;
}

void
otp_console_flush (HANDLE input)
{
    AcquireSRWLockExclusive (&console_lock);
    if (console_input == input) clear_line ();
    /* TCSAFLUSH discards line bytes, not a signal delivered before read(). */
    ReleaseSRWLockExclusive (&console_lock);
}

static gboolean
reserve_line (gsize extra)
{
    gsize length = console_line ? console_line->length : 0;
    if (extra > G_MAXSIZE - sizeof (ConsoleLine) - length) {
        errno = EOVERFLOW;
        return FALSE;
    }
    gsize needed = length + extra;
    if (console_line && needed <= console_line->capacity) return TRUE;
    gsize capacity = console_line ? console_line->capacity : 512;
    while (capacity < needed) {
        if (capacity > (G_MAXSIZE - sizeof (ConsoleLine)) / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    ConsoleLine *line = gcry_calloc_secure (1, sizeof (*line) + capacity);
    if (!line || !gcry_is_secure (line)) {
        if (line) {
            SecureZeroMemory (line, sizeof (*line) + capacity);
            gcry_free (line);
        }
        errno = ENOMEM;
        return FALSE;
    }
    if (console_line) memcpy (line, console_line, sizeof (*line) + length);
    clear_line ();
    line->capacity = capacity;
    console_line = line;
    return TRUE;
}

static gboolean
append_character (gunichar character)
{
    if (!reserve_line (4)) return FALSE;
    console_line->length += g_unichar_to_utf8 (character,
        console_line->bytes + console_line->length);
    return TRUE;
}

static gboolean
process_character (WCHAR character, DWORD mode)
{
    gboolean line_mode = (mode & ENABLE_LINE_INPUT) != 0;
    if (line_mode && character == L'\b') {
        if (console_line->high_surrogate) console_line->high_surrogate = 0;
        else if (console_line->length > 0) {
            gsize start = console_line->length - 1;
            while (start > 0 && ((guchar) console_line->bytes[start] & 0xc0) == 0x80) start--;
            SecureZeroMemory (console_line->bytes + start, console_line->length - start);
            console_line->length = start;
        }
        return TRUE;
    }
    if (console_line->high_surrogate) {
        WCHAR high = console_line->high_surrogate;
        console_line->high_surrogate = 0;
        if (character >= 0xdc00 && character <= 0xdfff) {
            gunichar codepoint = 0x10000 + ((high - 0xd800) << 10) + character - 0xdc00;
            if (!append_character (codepoint)) return FALSE;
            if (!line_mode) console_line->ready = TRUE;
            return TRUE;
        }
        if (!append_character (0xfffd)) return FALSE;
    }
    if (character >= 0xd800 && character <= 0xdbff) {
        console_line->high_surrogate = character;
        return TRUE;
    }
    if (line_mode && (character == L'\r' || character == L'\n')) {
        /* Text-mode CRT _read translates a console CRLF into LF. get_pwd's
         * one-byte reads must see one newline, with no CR left for a prompt. */
        if (!append_character ('\n')) return FALSE;
        console_line->ready = TRUE;
        return TRUE;
    }
    if (!append_character (character >= 0xdc00 && character <= 0xdfff ? 0xfffd : character))
        return FALSE;
    if (!line_mode) console_line->ready = TRUE;
    return TRUE;
}

ssize_t
otp_read (int fd, void *buffer, unsigned count)
{
    DWORD mode;
    intptr_t raw_handle = _get_osfhandle (fd);
    HANDLE input = (HANDLE) raw_handle;
    if (count == 0 || !GetConsoleMode (input, &mode))
        return _read (fd, buffer, count); /* Preserve pipe/file/NUL CRT semantics. */
    if (!buffer || count > INT_MAX) { errno = EINVAL; return -1; }

    AcquireSRWLockExclusive (&console_lock);
    DWORD thread_id = GetCurrentThreadId ();
    /* WINDOWS-STUB: one console reader/handle at a time; no cooked history,
     * cursor editing, code-page output, echo or binary-console CRLF emulation.
     * Passwords use line mode, no echo, UTF-8 and normalized LF. */
    if (console_read_active || (console_line &&
        (console_thread != thread_id || console_input != input))) {
        ReleaseSRWLockExclusive (&console_lock);
        errno = EBUSY;
        return -1;
    }
    for (guint i = 0; i < G_N_ELEMENTS (console_actions); i++) {
        if (real_handler (console_actions[i].handler) && console_actions[i].thread_id != thread_id) {
            ReleaseSRWLockExclusive (&console_lock);
            errno = EBUSY;
            return -1;
        }
    }
    if (!ensure_event ()) {
        ReleaseSRWLockExclusive (&console_lock);
        return -1;
    }
    if (!console_input_nowait) {
        /* ReadConsoleInputW after Peek can still block if Ctrl+Break (or an
         * external reader) empties the queue between calls. Microsoft's ExW
         * NOWAIT variant closes that race; never fall back to a cooked read. */
        union { FARPROC address; ConsoleInputNowait function; } proc;
        proc.address = GetProcAddress (GetModuleHandleW (L"kernel32.dll"), "ReadConsoleInputExW");
        console_input_nowait = proc.function;
        if (!console_input_nowait) {
            ReleaseSRWLockExclusive (&console_lock);
            errno = ENOSYS;
            return -1;
        }
    }
    console_thread = thread_id;
    console_input = input;
    console_read_active = TRUE;
    ssize_t result = -1;
    for (;;) {
        if (consume_interrupt (thread_id)) { errno = EINTR; break; }
        if (console_line && console_line->ready) {
            gsize length = MIN ((gsize) count, console_line->length - console_line->offset);
            memcpy (buffer, console_line->bytes + console_line->offset, length);
            SecureZeroMemory (console_line->bytes + console_line->offset, length);
            console_line->offset += length;
            result = (ssize_t) length;
            if (console_line->offset == console_line->length) clear_line ();
            break;
        }

        INPUT_RECORD record = {0};
        DWORD events = 0;
        BOOL ok = PeekConsoleInputW (input, &record, 1, &events);
        SecureZeroMemory (&record, sizeof (record));
        if (!ok) { errno = EIO; break; }
        if (events != 0) {
            ok = console_input_nowait (input, &record, 1, &events, 0x0002 /* CONSOLE_READ_NOWAIT */);
            if (ok && events != 0 && record.EventType == KEY_EVENT &&
                record.Event.KeyEvent.bKeyDown && record.Event.KeyEvent.uChar.UnicodeChar != 0) {
                ok = reserve_line (4);
                for (WORD i = 0; ok && i < record.Event.KeyEvent.wRepeatCount; i++) {
                    ok = process_character (record.Event.KeyEvent.uChar.UnicodeChar, mode);
                    if (ok && console_line->ready) break;
                }
            } else if (!ok) errno = EIO;
            SecureZeroMemory (&record, sizeof (record));
            if (!ok) break;
            /* Yield the lock after every record so a busy input queue cannot
             * starve the control handler. Every next iteration checks EINTR. */
            ReleaseSRWLockExclusive (&console_lock);
            AcquireSRWLockExclusive (&console_lock);
            continue;
        }

        HANDLE handles[] = {interrupt_event, input}; /* Interrupt wins ties. */
#ifdef OTP_WINDOWS_CONSOLE_CHECK
        if (wait_event) SetEvent (wait_event);
#endif
        ReleaseSRWLockExclusive (&console_lock);
        DWORD wait = WaitForMultipleObjects (G_N_ELEMENTS (handles), handles, FALSE, INFINITE);
        AcquireSRWLockExclusive (&console_lock);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_OBJECT_0 + 1) { errno = EIO; break; }
    }
    if (result < 0) clear_line ();
    console_read_active = FALSE;
    ReleaseSRWLockExclusive (&console_lock);
    return result;
}
