# Windows tray integration and native checks

## Integration

Append this single line to `cmake/windows.cmake`, **after** its existing
`cmake_language(DEFER CALL otp_windows_targets)`:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/windows-tray.cmake")
```

The fragment queues its own deferred hook after the common compatibility hook.
`otp_windows_tray_apply(target)` requires exactly one absolute `src/gui/tray.c`
source, removes it and appends `tray-win.c`. With `ENABLE_MINIMIZE_TO_TRAY=ON`
it also adds `tray-win-shell.c` and user32/shell32/gdi32. With the option OFF,
the replacement is an empty translation unit and the application has no tray
backend. Linux inclusion returns without changing targets. No upstream edit
is required. `tray-menu-model.c` stays as unused portable code.

The public header remains `src/gui/tray.h`. All ten exports retain their
signatures: init, enable, disable, cleanup, begin_hidden,
notify_locked_changed, notify_session_locked_changed, notify_window_shown,
is_available and watcher_present_sync (each prefixed `otpclient_tray_`).
The GTK `close-request` connection and TRUE/FALSE interception contract remain.
There are no new public signals; Linux D-Bus signals are replaced by native
shell messages, not emulated on Windows.

## Implementation and limits

The creating GUI thread owns both the HWND_MESSAGE callback receiver and an
invisible WS_EX_TOOLWINDOW top-level window. Message-only windows cannot receive
broadcasts, so the second window receives TaskbarCreated and owns TrackPopupMenu.
The icon uses HWND + uID=1, avoiding global GUID collisions between processes.
Every NIM_ADD negotiates NOTIFYICON_VERSION_4; unsupported negotiation deletes
the icon and refuses to hide the application. NIN_SELECT/NIN_KEYSELECT toggle
the main window; WM_CONTEXTMENU offers the existing Show OTPClient/Quit actions.

GLib's message GIOChannel polls each HWND. Its callback dispatches only messages
for that HWND, leaving GTK messages and WM_QUIT alone. GDK's Win32 GSource also
dispatches thread messages and may get there first. GLib alone does not
automatically call DispatchMessage. Application actions are queued to GLib idles
instead of destroying GTK/application state from the native window procedure.
The standalone probe exercises the message channel without GTK.

Successful publication holds GApplication. Lost publication releases the hold,
retries once per second, and grants a hidden window the upstream three-second
recovery interval (ten seconds for initial hidden startup). Recovery is paused
according to lock_app_get_session_locked; its existing Linux D-Bus provider does
not report Windows locks. **WINDOWS-STUB:** native session-lock parity requires a
separate WTS provider calling notify_session_locked_changed. No new lock monitor
or database locking policy is implemented here.

The icon uses the same application icon name as Linux. GtkIconTheme resolves the
staged hicolor SVG, GdkPixbuf loads it at tray size, and the backend creates an
alpha HICON from premultiplied BGRA. No icon is present in gui.gresource.xml.
**WINDOWS-STUB:** artwork/loader failure warns and falls back to a generic Windows
application icon. Theme/DPI changes do not reload it. Dark mode, HiDPI, focus,
keyboard menu placement, SVG loading and the staged GTK/GLib event integration
are **UNVERIFIED**. Explorer's Shell_TrayWnd is a capability heuristic; alternate
shells and notification-area policy are **UNVERIFIED**. NIM_ADD acceptance does
not prove the icon is visible outside the notification overflow.

## Native checklist (MSYS2 UCRT64 and Windows PowerShell)

Use an expendable database and a dedicated staging directory. In the existing
portable MSYS2 development tree, retain its dependency/pkg-config setup. After
the integration line above, configure a separate build (adjust paths):

```sh
export PKG_CONFIG_PATH="$PWD/cmake/windows/pkgconfig:$OTP_PREFIX_ROOT/lib/pkgconfig:/ucrt64/lib/pkgconfig"
export PATH="$OTP_PREFIX_ROOT/bin:/ucrt64/bin:/usr/bin:$PATH"
cmake -S . -B /c/otp-tray-build -G Ninja \
  -DCMAKE_PROJECT_INCLUDE="$(cygpath -m "$PWD/cmake/windows.cmake")" \
  -DOTP_WINDOWS_GETTEXT_ROOT=C:/msys64/ucrt64 \
  -DBUILD_GUI=ON -DBUILD_CLI=OFF -DBUILD_SEARCH_PROVIDER=OFF \
  -DBUILD_TESTING=OFF -DIS_FLATPAK=OFF \
  -DENABLE_MINIMIZE_TO_TRAY=ON -DOTP_WINDOWS_SHIM_CHECKS=ON
cmake --build /c/otp-tray-build --target OTPClient otp-windows-tray-check
OTP_GUI_BUILD=/c/otp-tray-build bash packaging/windows/stage-gui.sh /c/otp-tray-stage
cp /c/otp-tray-build/otp-windows-tray-check.exe /c/otp-tray-stage/bin/
```

Inspect `build.ninja`/verbose commands: tray-win.c and tray-win-shell.c must be
compiled, tray.c must not be compiled, and Shell_NotifyIcon symbols must link.
An independent OFF build must omit both the tray backend and tray caller code.

In PowerShell, load the stage's runtime environment and run the probe:

```powershell
& C:\otp-tray-stage\otpclient.ps1 -SettingsBackend keyfile -InitializeOnly
& C:\otp-tray-stage\bin\otp-windows-tray-check.exe --self-test
$LASTEXITCODE  # expected 0 and PASS, not a visual test
& C:\otp-tray-stage\bin\otp-windows-tray-check.exe
```

The interactive probe lasts at most 180 seconds. Observe the blue test icon,
toggle output on left click/keyboard selection, the two menu labels, Escape and
outside-click dismissal, Show output, and Quit exiting/removing the icon.
Restart Explorer while the probe is alive, from another PowerShell terminal:

```powershell
Stop-Process -Name explorer -Force
Start-Process explorer.exe
```

Expect one restored icon, no duplicates, working callbacks/menu, and publication
transitions. This real restart is required to verify TaskbarCreated reception.

Launch the application through `C:\otp-tray-stage\otpclient.ps1`. Verify:

1. Preference off: no icon and X closes normally. Preference on: icon appears;
   X hides the main window while the process survives; left click toggles;
   Show always presents, and Quit removes the icon and ends the process.
2. Lock the test database: tooltip becomes OTPClient (locked); showing from tray
   uses the normal password prompt and preserves canceled/unlocked behavior.
3. Enable hidden startup, exit normally, initialize the same runtime environment,
   then run `& C:\otp-tray-stage\bin\otpclient.exe --start-minimized`. Expect a
   hidden window with reachable icon and deferred unlock on first show.
4. Stop Explorer with the main window hidden; delay restarting it for at least
   six seconds. Expect recovery after loss detection plus the three-second
   grace. A quick restart should restore the icon without an unsolicited show.
   Start without Explorer: hidden startup must be refused. Always restart it.
5. With a Windows session-lock provider integrated, repeat tray loss during
   Win+L: no recovery presentation while locked; after unlock, one fresh grace
   interval. Without that provider, record this case as UNVERIFIED.
6. Toggle the preference repeatedly and quit with an open popup. Check for stale
   icons, callbacks after cleanup, process leftovers and growing USER/GDI counts.
7. Test 100/150/200% DPI, mixed-DPI monitors, dark/light themes, negative monitor
   coordinates, keyboard context menus and a relocated stage with Unicode/spaces.
   The application icon must match OTPClient, not the generic fallback.

## Recorded validation

The MinGW x64/UCRT backend and standalone probe compiled and linked with C11 and
strict warnings including -Werror. The backend also compiled without the probe
definition. A temporary isolated CMake fixture verified ON/OFF source selection,
rejected missing/duplicate Linux sources, and built the probe through the new
fragment. The cross pkg-config metadata emitted missing static libpcre2-8.pc
diagnostics; the dynamic probe build still succeeded.

tray-win.c could not be compiled: the first missing GUI header was adwaita.h.
GTK signatures/ownership were checked against the public API; this is static
review, not compiler validation. No Windows executable was run, no full GUI or
Linux test suite was built/run, and no native result above is claimed.

Reference implementations: GLib glib/giowin32.c (message-channel watch and
PeekMessage), GTK gdk/win32/gdkevents-win32.c (GSource/DispatchMessage), Microsoft
NOTIFYICONDATAW/Shell_NotifyIconW and Window Features documentation.
