# Capture the newly launched app's visible window; requires an unlocked desktop.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputPath,
    [string]$StageDir = $PSScriptRoot,
    [ValidateSet('registry', 'keyfile')][string]$SettingsBackend = 'registry',
    [ValidateRange(1, 120)][int]$TimeoutSeconds = 30
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class OtpWindowCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
}
'@
[OtpWindowCapture]::SetProcessDPIAware() | Out-Null
if (@(Get-Process -Name otpclient -ErrorAction SilentlyContinue).Count -gt 0) {
    throw 'Close existing OTPClient instances before capture; no existing process will be stopped'
}
$stage = [IO.Path]::GetFullPath($StageDir)
$output = [IO.Path]::GetFullPath($OutputPath)
if (Test-Path -LiteralPath $output) { throw "Output already exists: $output" }
$parent = Split-Path $output
[IO.Directory]::CreateDirectory($parent) | Out-Null
$app = $null
$bitmap = $null
$graphics = $null
try {
    $app = & (Join-Path $stage 'otpclient.ps1') -SettingsBackend $SettingsBackend -PassThru
    if ($app -isnot [Diagnostics.Process]) { throw 'Launcher did not return an application process' }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $rect = [OtpWindowCapture+Rect]::new()
    do {
        $app.Refresh()
        if ($app.HasExited) { throw "App exited before its window appeared: $($app.ExitCode)" }
        $hwnd = $app.MainWindowHandle
        if ($hwnd -ne [IntPtr]::Zero -and [OtpWindowCapture]::IsWindowVisible($hwnd) -and
            [OtpWindowCapture]::GetWindowRect($hwnd, [ref]$rect) -and
            ($rect.Right - $rect.Left) -gt 0 -and ($rect.Bottom - $rect.Top) -gt 0) { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($hwnd -eq [IntPtr]::Zero -or -not [OtpWindowCapture]::IsWindowVisible($hwnd) -or
        ($rect.Right - $rect.Left) -le 0 -or ($rect.Bottom - $rect.Top) -le 0) {
        throw "No visible main window within $TimeoutSeconds seconds"
    }
    if (-not [OtpWindowCapture]::SetForegroundWindow($hwnd)) {
        throw 'Cannot bring the app to the foreground; retry from the interactive desktop'
    }
    Start-Sleep -Seconds 2
    if (-not [OtpWindowCapture]::GetWindowRect($hwnd, [ref]$rect)) { throw 'Cannot read the window rectangle' }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    # Screen capture avoids PrintWindow's blank GPU-rendered GTK content.
    # Occlusion, remote desktop and off-screen parts still need owner review.
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    $bitmap.Save($output, [Drawing.Imaging.ImageFormat]::Png)
    Write-Output "PASS: screenshot saved to $output (visual review pending)"
} finally {
    if ($graphics) { $graphics.Dispose() }
    if ($bitmap) { $bitmap.Dispose() }
    if ($app) {
        try {
            if (-not $app.HasExited) {
                $closeSent = $app.CloseMainWindow()
                if (-not $closeSent -or -not $app.WaitForExit(5000)) {
                    $app.Kill()
                    $app.WaitForExit(3000) | Out-Null
                    throw 'FAIL: graceful shutdown timed out; capture process was forcibly stopped'
                }
            }
        } finally { $app.Dispose() }
    }
}
