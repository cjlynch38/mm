#!/usr/bin/env bash
# Builds and runs every host test of the GameCube port, builds every stand-alone test DOL, and reports
# pass/fail per test. Run inside WSL (Linux), from anywhere:
#
#   port/gc/tools/run_host_tests.sh            host tests (quick FP test) and test DOL builds
#   port/gc/tools/run_host_tests.sh --no-dols  host tests only
#   port/gc/tools/run_host_tests.sh --full     also the exhaustive FP test (every f32, about 25 s)
#
# Host tests: each port/gc/tests/*/Makefile with a `run` target (make run), and host_fp_test.c.
# Test DOLs: each other port/gc/tests/*/Makefile (make), and gfx_tev_host's `dol` target; running them
# needs Dolphin (port/gc/tools/run_dolphin.sh, see each Makefile). ultra_test links objects built by
# Makefile.gc. Output of each step goes to build/gc-host-tests/logs/<name>.log.
# Exit status: 0 if everything passed, 1 otherwise.
set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
tests="$root/port/gc/tests"
logs="$root/build/gc-host-tests/logs"
dols=1
fp_args=(--quick)

for arg in "$@"; do
    case "$arg" in
        --no-dols) dols=0 ;;
        --full) fp_args=() ;;
        -h | --help)
            sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "run_host_tests.sh: unknown option $arg" >&2
            exit 2
            ;;
    esac
done

mkdir -p "$logs"
jobs="$(nproc 2>/dev/null || echo 4)"
names=()
results=()
failed=0

# step <name> <command...>: runs the command with its output in the log, records PASS/FAIL
step() {
    local name="$1" log="$logs/$1.log" start end
    shift
    start=$(date +%s)
    printf '%-28s ' "$name"
    if "$@" >"$log" 2>&1; then
        end=$(date +%s)
        printf 'PASS  (%ds)\n' $((end - start))
        results+=("PASS")
    else
        end=$(date +%s)
        printf 'FAIL  (%ds, %s)\n' $((end - start)) "${log#"$root"/}"
        tail -n 15 "$log" | sed 's/^/    | /'
        results+=("FAIL")
        failed=$((failed + 1))
    fi
    names+=("$name")
}

has_target() {
    grep -q "^$2:" "$1/Makefile"
}

echo "== host tests"
for dir in "$tests"/*/; do
    dir="${dir%/}"
    [ -f "$dir/Makefile" ] && has_target "$dir" run || continue
    step "$(basename "$dir")" make --no-print-directory -C "$dir" -j"$jobs" run
done

fp_bin="$root/build/gc-host-tests/host_fp_test"
fp_test() {
    gcc -O2 -std=gnu17 -fwrapv -fno-strict-aliasing -ffp-contract=off -fopenmp -Wall -Wextra -Werror \
        -I"$root/include" -o "$fp_bin" "$tests/host_fp_test.c" -lm && "$fp_bin" "${fp_args[@]}"
}
step host_fp_test fp_test

if [ "$dols" = 1 ]; then
    echo "== test DOL builds (run them in Dolphin, see each Makefile)"
    for dir in "$tests"/*/; do
        dir="${dir%/}"
        [ -f "$dir/Makefile" ] || continue
        if has_target "$dir" run; then
            if has_target "$dir" dol; then
                step "$(basename "$dir").dol" make --no-print-directory -C "$dir" -j"$jobs" dol
            fi
            continue
        fi
        step "$(basename "$dir").dol" make --no-print-directory -C "$dir" -j"$jobs"
    done
fi

echo "== $(( ${#names[@]} - failed )) of ${#names[@]} passed"
[ "$failed" = 0 ]
