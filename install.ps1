# Memex one-shot installer (Windows x64).
# NOTE: keep this file pure ASCII. Windows PowerShell 5.1 reads remote scripts
# piped into iex using the system ANSI codepage; non-ASCII characters would be
# mangled and break parsing.
#
# Usage (one line, anonymous download, no GitHub login needed):
#   irm https://raw.githubusercontent.com/cuihairu/memex/main/install.ps1 | iex
#
# Downloads the nightly Inno Setup installer (MemexClient-<version>-win-x64.exe,
# A23 versioned client asset name - resolved from the nightly Release asset list
# via the GitHub API, exact regex match, never guessed) and installs it silently:
#   - installs to Program Files\Memex
#   - Start menu shortcut + optional desktop shortcut (interactive only)
#   - Add/Remove Programs uninstall entry (fixed AppId: re-run = upgrade)
#   - memex-client.exe is the WIN32 GUI-subsystem build: no console window
# UAC: the installer requests elevation itself (accept the UAC prompt).
# Verification: installed exe is launched with --version and its output is
# captured via handle redirection (GUI subsystem has no console attach).

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # irm|iex: IWR progress bar is very slow

$Repo = 'cuihairu/memex'
$InstallDir = Join-Path $env:ProgramFiles 'Memex'

function Info([string]$msg) { Write-Host ">>> $msg" }
function Die([string]$msg) { throw "install failed: $msg" }

# ---------- TLS 1.2 (WinPS 5.1 default may lack it; GitHub needs it) ----------
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch {}

# ---------- architecture (unknown combos reported clearly, never guessed) ----------
$arch = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
switch ($arch) {
    'AMD64' { }
    'ARM64' { Die "CPU architecture ${arch}: the daily build ships no native Windows ARM64 package yet." }
    'x86'   { Die "CPU architecture $arch (32-bit): only x64 builds are provided." }
    default { Die "Unsupported CPU architecture: $arch (supported: AMD64/x64)." }
}

# ---------- asset discovery + download (404 vs network failure reported separately) ----------
# A23: client assets carry the version in the name (MemexClient-x.y.z-win-x64.exe),
# so resolve the exact name from the nightly Release asset list instead of a fixed link.
$tmpRoot = Join-Path ([IO.Path]::GetTempPath()) ("memex-install-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null
try {
    $rel = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$Repo/releases/tags/nightly"
} catch {
    Die "nightly release lookup failed: $($_.Exception.Message)"
}
$assetHit = @($rel.assets) |
    Where-Object { $_.name -match '^MemexClient-[0-9]+\.[0-9]+\.[0-9]+-win-x64\.exe$' } |
    Select-Object -First 1
if (-not $assetHit) {
    Die "no MemexClient-<version>-win-x64.exe asset in the nightly Release - it may not be published yet; pick it manually at https://github.com/$Repo/releases/tag/nightly"
}
$Asset = $assetHit.name
$url = $assetHit.browser_download_url
$setupPath = Join-Path $tmpRoot $Asset
Info "resolved nightly asset: $Asset"

try {
    Info "Downloading $url"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $setupPath
    } catch {
        $code = $null
        try { $resp = $_.Exception.Response; if ($resp) { $code = [int]$resp.StatusCode } } catch {}
        if ($code -eq 404) {
            Die "asset not found (HTTP 404): $Asset - today's nightly may not be published yet; pick it manually at https://github.com/$Repo/releases/tag/nightly"
        }
        Die "download failed: $url ($($_.Exception.Message))"
    }
    if (-not (Test-Path $setupPath) -or (Get-Item $setupPath).Length -lt 1MB) {
        Die "downloaded installer is missing or suspiciously small: $Asset"
    }

    # ---------- PE integrity: MZ header ----------
    $fs = [IO.File]::OpenRead($setupPath)
    try {
        $mz = New-Object byte[] 2
        [void]$fs.Read($mz, 0, 2)
        if ($mz[0] -ne 0x4D -or $mz[1] -ne 0x5A) { Die "installer is not a valid PE file (MZ header missing): $Asset" }
    } finally { $fs.Dispose() }
    Info "verified: MZ header OK"

    # ---------- silent install (re-run = upgrade, same AppId) ----------
    Info "Running installer (/VERYSILENT; accept the UAC prompt)"
    $proc = Start-Process -FilePath $setupPath -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
    if ($proc.ExitCode -eq 740) {
        Die "elevation required (exit code 740): UAC was declined - re-run from an elevated PowerShell or accept the prompt"
    }
    if ($proc.ExitCode -ne 0) {
        Die "installer exited with code $($proc.ExitCode)"
    }

    $exe = Join-Path $InstallDir 'memex-client.exe'
    if (-not (Test-Path $exe)) { Die "installed exe not found: $exe" }
    $lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Memex\Memex.lnk'
    if (-not (Test-Path $lnk)) { Die "start menu shortcut not found: $lnk" }
    Info "installed to $InstallDir"

    # ---------- verify: memex-client --version (GUI subsystem, capture via handle redirect) ----------
    $verFile = Join-Path $tmpRoot 'version.txt'
    $verProc = Start-Process -FilePath $exe -ArgumentList '--version' -Wait -PassThru -RedirectStandardOutput $verFile
    $verText = ''
    if (Test-Path $verFile) { $verText = (Get-Content $verFile -Raw -ErrorAction SilentlyContinue) }
    if ($verProc.ExitCode -ne 0 -or -not ($verText -match 'memex-client')) {
        Die "post-install verification failed: $exe --version (exit=$($verProc.ExitCode), output='$($verText.Trim())')"
    }
    Info "verified: $($verText.Trim())"

    # ---------- user PATH (idempotent, registry scope) ----------
    try {
        $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
        if ([string]::IsNullOrEmpty($userPath)) {
            [Environment]::SetEnvironmentVariable('Path', $InstallDir, 'User')
        } elseif ($userPath -notlike "*$InstallDir*") {
            [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $InstallDir), 'User')
        }
        if (($env:Path -split ';') -notcontains $InstallDir) { $env:Path = "$env:Path;$InstallDir" }
        Info "user PATH contains $InstallDir (new terminals only)"
    } catch {
        Write-Warning "failed to update user PATH ($($_.Exception.Message)) - add it manually: $InstallDir"
    }

    Write-Host ''
    Info "done: $exe"
    Info "launch from the Start menu (Memex), or run memex-client in a new terminal"
    Info "uninstall: Apps & Features (Add/Remove Programs) -> Memex"
} finally {
    Remove-Item -Path $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
}
