# HS_Coop installer (run by Install.bat / Uninstall.bat; Windows PowerShell 5.1 or newer, nothing else needed).
# Finds Heat Signature through Steam, checks the game version, and puts three files into the game folder:
#   d3d9.dll, ModLoader.dll   the Heat Signature Mod Loader by piepieonline (loads mods when the game starts). Not part
#                             of this package: downloaded from its author's GitHub release v1.1.0 and checked against the
#                             SHA-256 below. If a HeatSignatureModLoader.zip sits next to Install.bat, that is used instead.
#   mods\HS_Coop.dll          the co-op mod
# Anything it replaces is kept next to it with a .coop-backup-<time> name. No other file of the game is changed.
param([string]$Game, [switch]$Uninstall, [switch]$NoPause)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$supportedExe = 'a5f8befe28786206ed1e2a52385051049524fea51e9e80660e3115e7126274d2'   # Steam build 6124596 (2021.1.20.1)
$loaderUrl = 'https://github.com/piepieonline/HeatSignatureModLoader/releases/download/v1.1.0/HeatSignatureModLoader.zip'
$loaderPage = 'https://github.com/piepieonline/HeatSignatureModLoader/releases/tag/v1.1.0'
$loaderSha = @{   # the files in that release
    'd3d9.dll'      = '6a7819f1259c90b59ebfa8c91393bfde627c2dc83046391e3c9b97c349126f2b'
    'ModLoader.dll' = 'fe05b103c273b4b0f66c0419b458aa8a4a60bf3839c2b890b6d6d51f9e8fcffa'
}
$loaderFiles = @('d3d9.dll', 'ModLoader.dll')

function Finish([int]$code) {
    if (-not $NoPause) { Write-Host ''; Read-Host 'Press Enter to close this window' | Out-Null }
    exit $code
}
function Fail([string]$message) { Write-Host ''; Write-Host $message -ForegroundColor Red; Finish 1 }
function Sha([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLower() }

function SteamGameFolders {
    $roots = @()
    foreach ($key in @('HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam')) {
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            foreach ($name in @('SteamPath', 'InstallPath')) { if ($p.$name) { $roots += [string]$p.$name } }
        } catch {}
    }
    if (${env:ProgramFiles(x86)}) { $roots += (Join-Path ${env:ProgramFiles(x86)} 'Steam') }
    $libraries = @()
    foreach ($root in $roots) {
        $libraries += $root
        $vdf = Join-Path $root 'steamapps\libraryfolders.vdf'
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libraries += $m.Groups[1].Value.Replace('\\', '\')
            }
        }
    }
    $libraries | ForEach-Object { Join-Path $_ 'steamapps\common\Heat Signature' }
}

Write-Host 'Heat Signature co-op mod (HS_Coop)' -ForegroundColor Cyan
Write-Host ''

# ---- Find the game ----
$candidates = @()
if ($Game) { $candidates += $Game }
$candidates += @(SteamGameFolders)
$candidates += @($here, (Split-Path -Parent $here), (Split-Path -Parent (Split-Path -Parent $here)))
$gameDir = $null
foreach ($c in $candidates) {
    if ($c -and (Test-Path -LiteralPath (Join-Path $c 'Heat_Signature.exe'))) { $gameDir = (Resolve-Path -LiteralPath $c).Path; break }
}
if (-not $gameDir -and -not $NoPause) {
    Write-Host 'Heat Signature was not found through Steam. Choose the folder that contains Heat_Signature.exe.'
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = 'Choose the Heat Signature folder (the one that contains Heat_Signature.exe)'
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK -and (Test-Path -LiteralPath (Join-Path $dialog.SelectedPath 'Heat_Signature.exe'))) {
        $gameDir = $dialog.SelectedPath
    }
}
if (-not $gameDir) { Fail 'Heat Signature was not found. Install the game with Steam first, then run this again.' }
Write-Host "Game folder: $gameDir"

# ---- The game must be closed: its files are in use while it runs ----
$running = @(Get-Process -Name 'Heat_Signature' -ErrorAction SilentlyContinue | Where-Object {
    try { $_.Path -and ((Split-Path -Parent $_.Path) -ieq $gameDir) } catch { $false } })
if ($running.Count -gt 0) { Fail 'Heat Signature is running. Close the game, then run this again.' }

$mods = Join-Path $gameDir 'mods'
$modTarget = Join-Path $mods 'HS_Coop.dll'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

# ---- Uninstall ----
if ($Uninstall) {
    $removed = $false
    if (Test-Path -LiteralPath $modTarget) { Remove-Item -LiteralPath $modTarget -Force; $removed = $true; Write-Host 'Removed mods\HS_Coop.dll' }
    foreach ($extra in @('HS_Coop.sig','HS_Coop.dll.old')) { $p = Join-Path $mods $extra; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }
    # The mod loader goes too, but only when it is the one this package installed and no other mod needs it.
    $otherMods = @()
    if (Test-Path -LiteralPath $mods) { $otherMods = @(Get-ChildItem -LiteralPath $mods -Filter '*.dll') }
    if ($otherMods.Count -eq 0) {
        foreach ($f in $loaderFiles) {
            $installed = Join-Path $gameDir $f
            if ((Test-Path -LiteralPath $installed) -and ((Sha $installed) -eq $loaderSha[$f])) {
                Remove-Item -LiteralPath $installed -Force; $removed = $true; Write-Host "Removed $f"
            }
        }
    } else { Write-Host 'Other mods are installed: the mod loader was left in place.' }
    Write-Host ''
    if ($removed) { Write-Host 'The co-op mod is removed. The game is back to normal.' -ForegroundColor Green }
    else { Write-Host 'The co-op mod was not installed in this game folder.' }
    Finish 0
}

# ---- Install ----
$exeHash = Sha (Join-Path $gameDir 'Heat_Signature.exe')
if ($exeHash -ne $supportedExe) {
    Fail ("This copy of Heat Signature is a different version than the mod supports (patch 2021.1.20.1).`n" +
          "Nothing was installed. Let Steam update the game fully (no beta branch selected), then run this again.")
}
$modSource = $null
foreach ($c in @((Join-Path $here 'HS_Coop.dll'), (Join-Path $here 'build\HS_Coop.dll'))) { if (Test-Path -LiteralPath $c) { $modSource = $c; break } }
if (-not $modSource) { Fail 'HS_Coop.dll is missing from this package. Unzip the whole zip file first, then run Install.bat from the unzipped folder.' }

# ---- The mod loader: already in place, or fetched from its author's release and checked ----
$needLoader = @($loaderFiles | Where-Object {
    $t = Join-Path $gameDir $_; -not ((Test-Path -LiteralPath $t) -and ((Sha $t) -eq $loaderSha[$_])) })
$tmp = $null
if ($needLoader.Count -gt 0) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("hs_coop_loader_" + $stamp)
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    $zip = $null
    foreach ($c in @((Join-Path $here 'HeatSignatureModLoader.zip'), (Join-Path (Split-Path -Parent $here) 'HeatSignatureModLoader.zip'))) {
        if (Test-Path -LiteralPath $c) { $zip = $c; Write-Host "Using the mod loader from $c" }
    }
    if (-not $zip) {
        Write-Host 'Downloading the Heat Signature Mod Loader (by piepieonline) from GitHub...'
        $zip = Join-Path $tmp 'HeatSignatureModLoader.zip'
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            $ProgressPreference = 'SilentlyContinue'
            Invoke-WebRequest -Uri $loaderUrl -OutFile $zip -UseBasicParsing
        } catch {
            Fail ("Could not download the mod loader: " + $_.Exception.Message + "`n" +
                  "Download HeatSignatureModLoader.zip yourself from $loaderPage,`nput it next to Install.bat and run Install.bat again.")
        }
    }
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($zip, (Join-Path $tmp 'x'))
    } catch { Fail ("Could not unpack the mod loader zip: " + $_.Exception.Message) }
    foreach ($f in $loaderFiles) {
        $got = Get-ChildItem -LiteralPath (Join-Path $tmp 'x') -Recurse -Filter $f | Select-Object -First 1
        if (-not $got -or (Sha $got.FullName) -ne $loaderSha[$f]) {
            Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
            Fail "The mod loader download is not the expected v1.1.0 release ($f does not match). Nothing was installed."
        }
    }
}

try {
    foreach ($f in $needLoader) {
        $source = (Get-ChildItem -LiteralPath (Join-Path $tmp 'x') -Recurse -Filter $f | Select-Object -First 1).FullName
        $target = Join-Path $gameDir $f
        if (Test-Path -LiteralPath $target) {
            Rename-Item -LiteralPath $target -NewName "$f.coop-backup-$stamp"
            Write-Host "A different $f was already there: kept as $f.coop-backup-$stamp"
        }
        Copy-Item -LiteralPath $source -Destination $target
        Unblock-File -LiteralPath $target -ErrorAction SilentlyContinue
        Write-Host "Installed $f"
    }
    if (-not (Test-Path -LiteralPath $mods)) { New-Item -ItemType Directory -Path $mods | Out-Null }
    Copy-Item -LiteralPath $modSource -Destination $modTarget -Force
    Unblock-File -LiteralPath $modTarget -ErrorAction SilentlyContinue
    Write-Host 'Installed mods\HS_Coop.dll'
    # Left over from older versions of the mod, which could update themselves: not used any more.
    foreach ($extra in @('HS_Coop.sig','HS_Coop.dll.old')) { $p = Join-Path $mods $extra; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }
} catch {
    Fail ("Could not copy the files: " + $_.Exception.Message + "`nClose the game and try again. If the game is in a protected folder, right-click Install.bat and choose 'Run as administrator'.")
} finally {
    if ($tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}
if (Test-Path -LiteralPath (Join-Path $gameDir 'coop.ini')) {
    Write-Host 'Note: this game folder has a coop.ini from an earlier setup. It still works; lobby codes work as well.'
}

Write-Host ''
Write-Host 'Done. The co-op mod is installed.' -ForegroundColor Green
Write-Host ''
Write-Host 'To play together (both players need this same package installed):'
Write-Host '  1. Start Heat Signature from Steam, as usual, and pick your character.'
Write-Host '  2. Host: press Escape, then F5. A lobby code appears at the bottom left and is copied for you.'
Write-Host '     Send it to your partner.'
Write-Host '  3. Partner: copy the code, press Escape, then F6.'
Write-Host ''
Write-Host 'To remove the mod again, run Uninstall.bat.'
Finish 0
