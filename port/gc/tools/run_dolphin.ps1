<#
.SYNOPSIS
Runs a GameCube DOL in Dolphin 2609 for the mmgcport dev loop and collects its output.

.DESCRIPTION
Dolphin 2609 has no SD card on the GameCube side (no SD adapter EXI device; the
SD card settings in Dolphin.ini only feed the Wii), so a run gets the SD folder
(C:\_mmgcport\sdcard) as a DVD instead: the folder is packed into an ISO9660 dev
disc (port/gc/tools/mkdevdisc.py, rebuilt when the folder changes) that Dolphin
inserts while booting the DOL. The console side mounts it as dvd:/. The log
leaves the emulator through an emulated USB Gecko in slot B, which Dolphin
exposes as a TCP server; this script records that stream.

Steps: pad check, disc rebuild, launch Dolphin in batch mode with per-run
settings (-C, not saved to Dolphin.ini), record the Gecko stream, wait,
screenshot the render window, close Dolphin gracefully (WM_CLOSE), then print
the Gecko log and the new part of Dolphin's own log.

Options: -Seconds 0 waits until Dolphin is closed by hand. -NoSd boots without
the dev disc. -NoGecko leaves slot B empty (no log capture). -NoPadCheck skips
the DOL padding check (to reproduce "Failed to init core").

-ExtraConfig adds Dolphin settings for this run only, as <System>.<Section>.<Key>=<Value>
(passed as -C; several can be given separated by ';'), for example
'Dolphin.DSP.Volume=0'. -DumpAudio <file.wav> records the AI DMA output: it turns
on Dolphin's audio dump for the run (Dump\Audio\<id>_<date>_dspdump*.wav in the
user folder; Dolphin starts a new file whenever the AI sample rate changes) and
copies the file written last to <file.wav>, any earlier ones of the run next to it
as <file>-<n>.wav.

Exit codes: 0 ok, 1 setup error, 2 Dolphin showed a dialog (error/warning),
3 Dolphin exited before the time was up, 4 Dolphin had to be killed.

.EXAMPLE
powershell -ExecutionPolicy Bypass -File run_dolphin.ps1 -Dol C:\path\sd_probe.dol -Seconds 15

.EXAMPLE
run_dolphin.sh build/gc-n64-us/mm-gc.dol -Seconds 40 -DumpAudio 'C:\_mmgcport\dolphin\mm.wav' -ExtraConfig 'Dolphin.DSP.Volume=0'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Dol,
    [int]$Seconds = 20,
    [string]$Screenshot = 'C:\_mmgcport\dolphin\screenshot.png',
    [switch]$NoSd,
    [switch]$NoGecko,
    [string]$SdFolder = 'C:\_mmgcport\sdcard',
    [string]$Disc = 'C:\_mmgcport\dolphin\mmgcport-dev.iso',
    [string]$GeckoLog = 'C:\_mmgcport\dolphin\gecko.log',
    [string]$DolphinExe = "$env:LOCALAPPDATA\Programs\Dolphin-x64\Dolphin.exe",
    [string]$DolphinUserDir = "$env:APPDATA\Dolphin Emulator",
    [string]$Distro = 'Ubuntu-24.04',
    [int]$LogTail = 60,
    [switch]$NoPadCheck,
    [string[]]$ExtraConfig = @(),
    [string]$DumpAudio = ''
)

$ErrorActionPreference = 'Stop'
$WorkDir = 'C:\_mmgcport\dolphin'
$GeckoPorts = 55020..55030   # Dolphin's USB Gecko listens on the first free port from 0xD6EC
$WM_CLOSE = 0x0010
$WM_KEYDOWN = 0x0100
$WM_KEYUP = 0x0101
$VK_RETURN = 0x0D
$VK_ESCAPE = 0x1B

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public static class MmgcWin {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();

    // Top-level windows of a process.
    public static IntPtr[] Windows(int pid, bool visibleOnly) {
        var found = new List<IntPtr>();
        EnumWindows(delegate (IntPtr h, IntPtr l) {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == (uint)pid && (!visibleOnly || IsWindowVisible(h))) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }

    public static string Title(IntPtr h) {
        var sb = new StringBuilder(512);
        GetWindowText(h, sb, sb.Capacity);
        return sb.ToString();
    }

    // PW_RENDERFULLCONTENT (2) is needed for the D3D render window.
    public static string Capture(IntPtr h, string path) {
        RECT r;
        if (!GetWindowRect(h, out r)) return "GetWindowRect failed";
        int w = r.Right - r.Left, ht = r.Bottom - r.Top;
        if (w <= 0 || ht <= 0) return "window has no area";
        using (var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                bool ok = PrintWindow(h, hdc, 2);
                g.ReleaseHdc(hdc);
                if (!ok) return "PrintWindow failed";
            }
            bmp.Save(path, ImageFormat.Png);
        }
        return null;
    }
}
'@
[void][MmgcWin]::SetProcessDPIAware()

function Say([string]$Text) { Write-Host $Text }

function Fail([string]$Text) {
    Write-Host "run_dolphin: $Text"
    exit 1
}

# Same rule as dolpad.py: every section's 32-byte-rounded extent must be inside the file.
function Test-DolPadded([string]$Path) {
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x100) { return $false }
    $need = 0x100
    for ($i = 0; $i -lt 18; $i++) {
        $off = [System.Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes, 4 * $i))
        $size = [System.Net.IPAddress]::NetworkToHostOrder([BitConverter]::ToInt32($bytes, 0x90 + 4 * $i))
        if ($size -ne 0) { $need = [Math]::Max($need, [int64]$off + (([int64]$size + 31) -band -32)) }
    }
    return $bytes.Length -ge $need
}

function ConvertTo-WslPath([string]$Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full -match '^\\\\wsl(?:\.localhost|\$)\\[^\\]+(\\.*)?$') {
        $rest = $Matches[1]
        if (-not $rest) { return '/' }
        return $rest.Replace('\', '/')
    }
    if ($full -match '^([A-Za-z]):\\(.*)$') {
        return '/mnt/' + $Matches[1].ToLower() + '/' + $Matches[2].Replace('\', '/')
    }
    throw "cannot map $Path into WSL"
}

function Get-NewestWriteTime([string]$Folder) {
    $newest = (Get-Item -LiteralPath $Folder).LastWriteTimeUtc
    foreach ($item in Get-ChildItem -LiteralPath $Folder -Recurse -Force) {
        if ($item.LastWriteTimeUtc -gt $newest) { $newest = $item.LastWriteTimeUtc }
    }
    return $newest
}

function Get-DialogText([IntPtr]$Hwnd) {
    try {
        Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
        $root = [System.Windows.Automation.AutomationElement]::FromHandle($Hwnd)
        $all = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
            [System.Windows.Automation.Condition]::TrueCondition)
        $names = foreach ($el in $all) { $n = $el.Current.Name; if ($n) { $n.Trim() } }
        return (($names | Select-Object -Unique) -join ' | ')
    } catch {
        return "(dialog text unavailable: $($_.Exception.Message))"
    }
}

# The batch-mode render window is titled "Dolphin <ver> | <cpu> | <backend> | <dsp>"; the
# hidden main window is "Dolphin <ver>". Anything else visible is a dialog.
function Get-DolphinWindows([int]$ProcId) {
    $render = [IntPtr]::Zero
    $main = [IntPtr]::Zero
    $dialogs = @()
    foreach ($h in [MmgcWin]::Windows($ProcId, $true)) {
        $title = [MmgcWin]::Title($h)
        if (-not $title) { continue }
        if ($title -like 'Dolphin*|*') { $render = $h }
        elseif ($title -notlike 'Dolphin*') { $dialogs += $h }
    }
    foreach ($h in [MmgcWin]::Windows($ProcId, $false)) {
        if ([MmgcWin]::Title($h) -match '^Dolphin [^|]+$') { $main = $h; break }
    }
    return @{ Render = $render; Main = $main; Dialogs = $dialogs }
}

function Send-Key([IntPtr]$Hwnd, [int]$Vk) {
    [void][MmgcWin]::PostMessage($Hwnd, $WM_KEYDOWN, [IntPtr]$Vk, [IntPtr]::Zero)
    [void][MmgcWin]::PostMessage($Hwnd, $WM_KEYUP, [IntPtr]$Vk, [IntPtr]::Zero)
}

function Get-ShotPath([string]$Suffix) {
    $dir = [System.IO.Path]::GetDirectoryName($Screenshot)
    $base = [System.IO.Path]::GetFileNameWithoutExtension($Screenshot)
    return (Join-Path $dir "$base$Suffix.png")
}

# --- setup ---------------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $DolphinExe)) { Fail "Dolphin not found at $DolphinExe" }
if (-not (Test-Path -LiteralPath $Dol)) { Fail "DOL not found: $Dol" }
$Dol = (Resolve-Path -LiteralPath $Dol).ProviderPath
if (-not $NoPadCheck -and -not (Test-DolPadded $Dol)) {
    Fail "$Dol is not padded; Dolphin would fail with 'Failed to init core'. Run port/gc/tools/dolpad.py on it."
}
New-Item -ItemType Directory -Force -Path $WorkDir, (Join-Path $WorkDir 'run') | Out-Null
foreach ($p in @($Screenshot, $GeckoLog)) {
    $d = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($p))
    if ($d) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
}

# Run a private copy so a rebuild during the run cannot change the file under Dolphin and
# Dolphin never has to read from a \\wsl.localhost path.
$RunDol = Join-Path (Join-Path $WorkDir 'run') ([System.IO.Path]::GetFileName($Dol))
Copy-Item -LiteralPath $Dol -Destination $RunDol -Force

if (-not $NoSd) {
    if (-not (Test-Path -LiteralPath $SdFolder)) { Fail "SD folder not found: $SdFolder (use -NoSd to run without it)" }
    $stale = -not (Test-Path -LiteralPath $Disc)
    if (-not $stale) { $stale = (Get-Item -LiteralPath $Disc).LastWriteTimeUtc -lt (Get-NewestWriteTime $SdFolder) }
    if ($stale) {
        if (-not $PSBoundParameters.ContainsKey('Distro') -and $PSScriptRoot -match '^\\\\wsl(?:\.localhost|\$)\\([^\\]+)\\') {
            $Distro = $Matches[1]
        }
        $tool = ConvertTo-WslPath (Join-Path $PSScriptRoot 'mkdevdisc.py')
        Say "run_dolphin: rebuilding dev disc from $SdFolder"
        & wsl.exe -d $Distro -- python3 $tool (ConvertTo-WslPath $SdFolder) (ConvertTo-WslPath $Disc)
        if ($LASTEXITCODE -ne 0) { Fail "mkdevdisc.py failed (exit $LASTEXITCODE)" }
    }
}

$running = @(Get-Process -Name Dolphin -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    Say "run_dolphin: warning: Dolphin is already running (pid $($running.Id -join ', ')); its USB Gecko may own port 55020"
}

# Per-run settings. -C values live in Dolphin's command-line config layer and are not saved.
$discIso = ''
if (-not $NoSd) { $discIso = $Disc }
$slotB = 7                                         # EXIDeviceType::Gecko (USB Gecko, TCP 55020)
if ($NoGecko) { $slotB = 255 }                     # EXIDeviceType::None
$dolphinArgs = @(
    '-b',
    '-C', 'Dolphin.Interface.ConfirmStop=False',   # WM_CLOSE stops without a Yes/No box
    '-C', "Dolphin.Core.SlotB=$slotB",
    '-C', 'Dolphin.Core.FastDiscSpeed=True',
    '-C', "Dolphin.Core.DefaultISO=$discIso",      # inserted when booting a DOL
    # EFB copies reach RAM as on hardware (the renderer's framebuffer effects read them back);
    # Dolphin's default keeps them on the GPU only, which makes port/gc/gfx turn the effects off.
    '-C', 'Graphics.Hacks.EFBToTextureEnable=False'
)
$audioDumpDir = Join-Path $DolphinUserDir 'Dump\Audio'
$runStartUtc = [DateTime]::UtcNow
if ($DumpAudio) {
    # DumpAudioSilent: replace an old dump without asking (the question would be a dialog)
    $dolphinArgs += @('-C', 'Dolphin.DSP.DumpAudio=True', '-C', 'Dolphin.DSP.DumpAudioSilent=True')
}
foreach ($item in $ExtraConfig) {
    foreach ($setting in ($item -split ';')) {
        $setting = $setting.Trim()
        if (-not $setting) { continue }
        if ($setting -notmatch '^[^.=\s]+\.[^.=\s]+\.[^=\s]+=') { Fail "-ExtraConfig '$setting' is not <System>.<Section>.<Key>=<Value>" }
        $dolphinArgs += @('-C', $setting)
    }
}
$dolphinArgs += @('-e', $RunDol)
$argLine = ($dolphinArgs | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }) -join ' '

$dolphinLog = Join-Path $DolphinUserDir 'Logs\dolphin.log'
$logStart = 0
if (Test-Path -LiteralPath $dolphinLog) { $logStart = (Get-Item -LiteralPath $dolphinLog).Length }

# --- run -----------------------------------------------------------------------------------

$duration = "$Seconds s"
if ($Seconds -le 0) { $duration = 'until closed' }
Say "run_dolphin: $RunDol, $duration (disc: $(if ($discIso) { $discIso } else { 'none' }))"
$proc = Start-Process -FilePath $DolphinExe -ArgumentList $argLine -PassThru
$clock = [System.Diagnostics.Stopwatch]::StartNew()

# Gecko capture runs in its own runspace so the main loop can watch for dialogs.
$gecko = [hashtable]::Synchronized(@{ Stop = $NoGecko.IsPresent; Port = 0; Bytes = 0; Error = 'disabled (-NoGecko)' })
$geckoPs = [PowerShell]::Create()
[void]$geckoPs.AddScript({
    param($state, $ports, $procId, $path)
    if ($state.Stop) { return }
    $state.Error = $null
    $out = [System.IO.File]::Open($path, 'Create', 'Write', 'ReadWrite')
    $client = $null
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while (-not $client -and -not $state.Stop -and [DateTime]::UtcNow -lt $deadline) {
            $listening = @(Get-NetTCPConnection -State Listen -OwningProcess $procId -ErrorAction SilentlyContinue |
                Where-Object { $ports -contains $_.LocalPort } | Sort-Object LocalPort)
            if ($listening.Count -gt 0) {
                $c = New-Object System.Net.Sockets.TcpClient
                try { $c.Connect('127.0.0.1', $listening[0].LocalPort); $client = $c; $state.Port = $listening[0].LocalPort }
                catch { $c.Close() }
            }
            if (-not $client) { Start-Sleep -Milliseconds 200 }
        }
        if (-not $client) { $state.Error = 'no USB Gecko port found'; return }
        $sock = $client.Client
        $buf = New-Object byte[] 4096
        while ($true) {
            if ($sock.Poll(200000, [System.Net.Sockets.SelectMode]::SelectRead)) {
                $n = $sock.Receive($buf)
                if ($n -le 0) { break }
                $out.Write($buf, 0, $n)
                $out.Flush()
                $state.Bytes += $n
            } elseif ($state.Stop) {
                break
            }
        }
    } catch {
        $state.Error = $_.Exception.Message
    } finally {
        $out.Dispose()
        if ($client) { $client.Close() }
    }
}).AddArgument($gecko).AddArgument($GeckoPorts).AddArgument($proc.Id).AddArgument($GeckoLog)
$geckoHandle = $geckoPs.BeginInvoke()

$status = 0
$dialogsSeen = @{}
$renderSeen = $false
try {
    while ($Seconds -le 0 -or $clock.Elapsed.TotalSeconds -lt $Seconds) {
        Start-Sleep -Milliseconds 500
        if ($proc.HasExited) {
            Say "run_dolphin: Dolphin exited after $([int]$clock.Elapsed.TotalSeconds) s (exit code $($proc.ExitCode))"
            if ($Seconds -gt 0) { $status = 3 }
            break
        }
        $wins = Get-DolphinWindows $proc.Id
        if ($wins.Render -ne [IntPtr]::Zero) { $renderSeen = $true }
        foreach ($h in $wins.Dialogs) {
            $key = $h.ToInt64()
            if ($dialogsSeen.ContainsKey($key)) { continue }
            $title = [MmgcWin]::Title($h)
            $text = Get-DialogText $h
            $shot = Get-ShotPath "-dialog$($dialogsSeen.Count)"
            $err = [MmgcWin]::Capture($h, $shot)
            if ($err) { $shot = "(no screenshot: $err)" }
            $dialogsSeen[$key] = $true
            Say "run_dolphin: DIALOG '$title': $text"
            Say "run_dolphin:   screenshot $shot"
            $status = 2
        }
        if ($status -eq 2) { break }
    }

    $wins = Get-DolphinWindows $proc.Id
    if (-not $proc.HasExited -and $wins.Render -ne [IntPtr]::Zero) {
        $err = [MmgcWin]::Capture($wins.Render, $Screenshot)
        if ($err) { Say "run_dolphin: screenshot failed: $err" } else { Say "run_dolphin: screenshot $Screenshot" }
    } elseif (-not $renderSeen) {
        Say 'run_dolphin: no render window appeared'
    }
} finally {
    # Graceful stop: WM_CLOSE on the render window -> RequestStop -> Core::Stop -> exit (batch
    # mode). Without a render window (boot failed) the hidden main window gets the WM_CLOSE.
    # Dialogs are answered with Escape (Enter for a stop confirmation); kill as a last resort.
    $closeClock = [System.Diagnostics.Stopwatch]::StartNew()
    $lastClose = -10
    $weClosed = -not $proc.HasExited
    while (-not $proc.HasExited -and $closeClock.Elapsed.TotalSeconds -lt 20) {
        $wins = Get-DolphinWindows $proc.Id
        foreach ($h in $wins.Dialogs) {
            if ([MmgcWin]::Title($h) -eq 'Confirm') { Send-Key $h $VK_RETURN } else { Send-Key $h $VK_ESCAPE }
        }
        if ($wins.Dialogs.Count -eq 0 -and $closeClock.Elapsed.TotalSeconds - $lastClose -ge 5) {
            $target = $wins.Render
            if ($target -eq [IntPtr]::Zero) { $target = $wins.Main }
            if ($target -ne [IntPtr]::Zero) {
                [void][MmgcWin]::PostMessage($target, $WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
                $lastClose = $closeClock.Elapsed.TotalSeconds
            }
        }
        [void]$proc.WaitForExit(500)
    }
    if (-not $proc.HasExited) {
        Say 'run_dolphin: Dolphin did not shut down; killing it'
        Stop-Process -Id $proc.Id -Force
        [void]$proc.WaitForExit(5000)
        if ($status -eq 0) { $status = 4 }
    } elseif ($weClosed) {
        Say "run_dolphin: Dolphin closed gracefully in $([Math]::Round($closeClock.Elapsed.TotalSeconds, 1)) s"
    }

    $gecko.Stop = $true
    [void]$geckoHandle.AsyncWaitHandle.WaitOne(3000)
    try { $geckoPs.Stop(); $geckoPs.Dispose() } catch { }
}

# Audio dump: Dolphin has closed the files (and written their headers) by now. The run's files
# are moved out of Dolphin's dump folder: the DSP (AI DMA) dump to -DumpAudio, the disc
# streaming (DTK) dump, which this port never uses, next to it as <file>-dtk.wav.
if ($DumpAudio) {
    $dumps = @()
    $dtk = @()
    if (Test-Path -LiteralPath $audioDumpDir) {
        $new = @(Get-ChildItem -LiteralPath $audioDumpDir -Filter '*.wav' |
            Where-Object { $_.LastWriteTimeUtc -ge $runStartUtc } | Sort-Object LastWriteTimeUtc)
        $dumps = @($new | Where-Object { $_.Name -like '*dspdump*' })
        $dtk = @($new | Where-Object { $_.Name -like '*dtkdump*' })
    }
    if ($dumps.Count -eq 0) {
        Say "run_dolphin: no audio dump was written to $audioDumpDir"
    } else {
        $d = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($DumpAudio))
        if ($d) { New-Item -ItemType Directory -Force -Path $d | Out-Null }
        $base = Join-Path $d ([System.IO.Path]::GetFileNameWithoutExtension($DumpAudio))
        for ($i = 0; $i -lt $dumps.Count; $i++) {
            $dest = if ($i -eq $dumps.Count - 1) { $DumpAudio } else { "$base-$i.wav" }
            Move-Item -LiteralPath $dumps[$i].FullName -Destination $dest -Force
            Say "run_dolphin: audio dump $($dumps[$i].Name) ($($dumps[$i].Length) bytes) -> $dest"
        }
        for ($i = 0; $i -lt $dtk.Count; $i++) {
            $dest = if ($i -eq 0) { "$base-dtk.wav" } else { "$base-dtk-$i.wav" }
            Move-Item -LiteralPath $dtk[$i].FullName -Destination $dest -Force
        }
    }
}

# --- report --------------------------------------------------------------------------------

Say ''
if ($gecko.Port -ne 0) {
    Say "== USB Gecko log ($GeckoLog, port $($gecko.Port), $($gecko.Bytes) bytes) =="
    if (Test-Path -LiteralPath $GeckoLog) { Get-Content -LiteralPath $GeckoLog | ForEach-Object { Say $_ } }
} else {
    Say "== USB Gecko log: not captured ($($gecko.Error)) =="
}

Say ''
if (Test-Path -LiteralPath $dolphinLog) {
    # Share read/write: another Dolphin instance (a concurrent run) may still have the log open
    $stream = [System.IO.File]::Open($dolphinLog, 'Open', 'Read', 'ReadWrite')
    try {
        $raw = New-Object byte[] $stream.Length
        $got = 0
        while ($got -lt $raw.Length) {
            $n = $stream.Read($raw, $got, $raw.Length - $got)
            if ($n -le 0) { break }
            $got += $n
        }
    } finally {
        $stream.Dispose()
    }
    if ($raw.Length -lt $logStart) { $logStart = 0 }   # Dolphin truncated the log at startup
    $text = [System.Text.Encoding]::UTF8.GetString($raw, [int]$logStart, $raw.Length - [int]$logStart)
    $lines = $text -split "`r?`n" | Where-Object { $_ -and $_ -notmatch 'warning X\d{4}:|Shader@0x|compilation succeeded with warnings' }
    Say "== $dolphinLog (last $LogTail of $(@($lines).Count) new lines, shader noise removed) =="
    $lines | Select-Object -Last $LogTail | ForEach-Object { Say $_ }
} else {
    Say "== ${dolphinLog}: missing (enable WriteToFile in Config\Logger.ini) =="
}

Say ''
Say "run_dolphin: exit $status"
exit $status
