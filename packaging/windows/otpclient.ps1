# Process-local runtime setup for the portable directory. Windows PowerShell 5.1.
[CmdletBinding()]
param(
    [ValidateSet('registry', 'keyfile')]
    [string]$SettingsBackend = 'registry', # One-line switch for the portable keyfile option.
    [switch]$InitializeOnly,
    [switch]$PassThru
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($PSScriptRoot)
$bin = Join-Path $root 'bin'
$exe = Join-Path $bin 'otpclient.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Missing executable: $exe" }
$env:PATH = "$bin;$env:SystemRoot\System32;$env:SystemRoot"
$env:GSETTINGS_SCHEMA_DIR = Join-Path $root 'share\glib-2.0\schemas'
$env:GSETTINGS_BACKEND = $SettingsBackend
$env:XDG_DATA_DIRS = Join-Path $root 'share'
$env:GIO_MODULE_DIR = Join-Path $root 'lib\gio\modules'
if (-not (Test-Path -LiteralPath (Join-Path $env:GSETTINGS_SCHEMA_DIR 'gschemas.compiled'))) {
    throw 'Missing compiled GSettings schemas; rerun stage-gui.sh'
}
if ($SettingsBackend -eq 'keyfile') {
    # GLib uses <user-config-dir>/glib-2.0/settings/keyfile. This also relocates
    # OTPClient's other configuration files, not its database/data directory.
    $env:XDG_CONFIG_HOME = Join-Path $root 'config'
    [IO.Directory]::CreateDirectory((Join-Path $env:XDG_CONFIG_HOME 'glib-2.0\settings')) | Out-Null
}

function Invoke-RuntimeTool {
    param([string]$File, [string[]]$Arguments, [string]$Output, [string]$ErrorOutput)
    # Windows filenames cannot contain double quotes. Quote each path, and
    # decode the native tool's UTF-8 output explicitly for Unicode zip paths.
    $quoted = @($Arguments | ForEach-Object { '"' + $_ + '"' })
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $File
    $info.Arguments = $quoted -join ' '
    $info.WorkingDirectory = $root
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
    $info.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    $tool = [Diagnostics.Process]::new()
    $tool.StartInfo = $info
    $started = $false
    try {
        $started = $tool.Start()
        if (-not $started) { throw "Cannot start runtime tool: $File" }
        $stdoutTask = $tool.StandardOutput.ReadToEndAsync()
        $stderrTask = $tool.StandardError.ReadToEndAsync()
        if (-not $tool.WaitForExit(15000)) { throw "Runtime tool timeout: $File" }
        [IO.File]::WriteAllText($Output, $stdoutTask.Result, [Text.UTF8Encoding]::new($false))
        [IO.File]::WriteAllText($ErrorOutput, $stderrTask.Result, [Text.UTF8Encoding]::new($false))
        if ($tool.ExitCode -ne 0) { throw "Runtime tool failed ($($tool.ExitCode)): $File; see $ErrorOutput" }
        if ((Get-Item -LiteralPath $ErrorOutput).Length -gt 0) {
            throw "Runtime tool reported diagnostics: $File; see $ErrorOutput"
        }
    } finally {
        if ($started -and -not $tool.HasExited) { $tool.Kill(); $tool.WaitForExit(3000) | Out-Null }
        $tool.Dispose()
    }
}

$relative = [IO.File]::ReadAllText((Join-Path $root 'pixbuf-loaders-dir.txt')).Trim()
if ($relative -notmatch '^lib/gdk-pixbuf-2\.0/[0-9.]+/loaders$') { throw 'Invalid pixbuf loader directory' }
$loaderDir = Join-Path $root $relative
$loaders = @(Get-ChildItem -LiteralPath $loaderDir -Filter '*.dll' -File | Sort-Object Name)
if ($loaders.Count -eq 0) { throw 'No staged pixbuf loaders' }
$cache = Join-Path (Split-Path $loaderDir) 'loaders.cache'
$env:GDK_PIXBUF_MODULE_FILE = $cache
$env:GDK_PIXBUF_MODULEDIR = $loaderDir
$suffix = [Guid]::NewGuid().ToString('N')
$tempCache = "$cache.$suffix.tmp"
$errors = "$cache.$suffix.stderr"
try {
    Invoke-RuntimeTool -File (Join-Path $bin 'gdk-pixbuf-query-loaders.exe') `
        -Arguments @($loaders | ForEach-Object { $_.FullName }) -Output $tempCache -ErrorOutput $errors
    $lines = [IO.File]::ReadAllLines($tempCache)
    $count = 0
    $normalized = foreach ($line in $lines) {
        if ($line -match '^#') { continue } # Drop build-prefix comments as well.
        if ($line -match '^"(.+\.dll)"\s*$') {
            $module = $Matches[1].Replace('\\', '\')
            if (-not [IO.Path]::IsPathRooted($module)) { $module = Join-Path $root $module }
            $module = [IO.Path]::GetFullPath($module)
            if (-not $module.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
                throw "Loader outside staged directory: $module"
            }
            if (-not (Test-Path -LiteralPath $module -PathType Leaf)) { throw "Missing loader: $module" }
            $count++
            # Forward slashes need no escaping in the cache string format.
            '"' + $module.Replace('\', '/') + '"'
        } else { $line }
    }
    if ($count -ne $loaders.Count) { throw "Only $count/$($loaders.Count) loaders queried successfully" }
    [IO.File]::WriteAllLines($tempCache, [string[]]$normalized, [Text.UTF8Encoding]::new($false))
    Move-Item -LiteralPath $tempCache -Destination $cache -Force
    Remove-Item -LiteralPath $errors -Force
} finally {
    if (Test-Path -LiteralPath $tempCache) { Remove-Item -LiteralPath $tempCache -Force }
}

$gioDir = Join-Path $root 'lib\gio\modules'
if (Test-Path -LiteralPath (Join-Path $bin 'gio-querymodules.exe')) {
    $out = Join-Path $root "gio-querymodules.$suffix.stdout"
    $err = Join-Path $root "gio-querymodules.$suffix.stderr"
    try {
        Invoke-RuntimeTool -File (Join-Path $bin 'gio-querymodules.exe') -Arguments @($gioDir) -Output $out -ErrorOutput $err
        Remove-Item -LiteralPath $err -Force
    } finally {
        if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Force }
    }
}
if ($InitializeOnly) { Write-Output 'PASS: runtime cache initialized'; return }

# Read the PE optional header: subsystem 2 is GUI, 3 is console.
$stream = [IO.File]::OpenRead($exe)
$reader = [IO.BinaryReader]::new($stream)
try {
    $stream.Position = 0x3c
    $peOffset = $reader.ReadInt32()
    $stream.Position = $peOffset + 24 + 68
    $subsystem = $reader.ReadUInt16()
} finally { $reader.Dispose(); $stream.Dispose() }
$info = [Diagnostics.ProcessStartInfo]::new()
$info.FileName = $exe
$info.WorkingDirectory = $root
$info.UseShellExecute = $false
# Suppress a console without sending SW_HIDE to the GTK window itself.
$info.CreateNoWindow = ($subsystem -eq 3)
$app = [Diagnostics.Process]::new()
$app.StartInfo = $info
try {
    if (-not $app.Start()) { throw 'Cannot start OTPClient' }
} catch { $app.Dispose(); throw }
if ($PassThru) { return $app }
$app.Dispose()
