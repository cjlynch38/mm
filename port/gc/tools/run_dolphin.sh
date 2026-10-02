#!/usr/bin/env bash
# WSL wrapper for run_dolphin.ps1: converts Linux paths to Windows paths and runs the
# script with Windows PowerShell. Exit code is the script's (see run_dolphin.ps1).
#
#   port/gc/tools/run_dolphin.sh port/gc/tests/sd_probe/sd_probe.dol -Seconds 15
#   port/gc/tools/run_dolphin.sh -Dol build/gc-n64-us/mm.dol -Screenshot /tmp/mm.png -NoSd
#   port/gc/tools/run_dolphin.sh build/gc-n64-us/mm-gc.iso -Seconds 60   (boots the disc image)
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

winpath() {
    case "$1" in
        [A-Za-z]:\\* | \\\\*) printf '%s\n' "$1" ;;            # already a Windows path
        *) wslpath -w "$(realpath -m -- "$1")" ;;
    esac
}

args=()
have_dol=0
while [ $# -gt 0 ]; do
    case "$1" in
        -Dol | -Iso | -Screenshot | -SdFolder | -Disc | -GeckoLog | -DolphinExe | -DolphinUserDir | -MemCard)
            [ $# -ge 2 ] || { echo "run_dolphin.sh: $1 needs a value" >&2; exit 1; }
            case "$1" in -Dol | -Iso) have_dol=1 ;; esac
            args+=("$1" "$(winpath "$2")")
            shift 2
            ;;
        -Seconds | -LogTail | -Distro | -ScreenshotEvery)
            [ $# -ge 2 ] || { echo "run_dolphin.sh: $1 needs a value" >&2; exit 1; }
            args+=("$1" "$2")
            shift 2
            ;;
        -*)
            args+=("$1")
            shift
            ;;
        *)
            if [ "$have_dol" = 0 ]; then
                args+=(-Dol "$(winpath "$1")")
                have_dol=1
            else
                args+=("$1")
            fi
            shift
            ;;
    esac
done

# Run from a Windows directory so powershell.exe does not start in a \\wsl.localhost cwd, and
# detach stdin: powershell.exe would otherwise swallow the rest of a calling script's input.
cd /mnt/c
exec powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass \
    -File "$(wslpath -w "$here/run_dolphin.ps1")" "${args[@]}" </dev/null
