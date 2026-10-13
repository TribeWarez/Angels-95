#!/bin/sh
# ---------------------------------------------------------------------------
# tools/check_editor_objects.sh
#
# Fails when the AngelEd object lists drift apart.
#
# The editor is built twice from two hand-maintained lists:
#   * AngelEd/Makefile          - what developers and the documented editor build use
#   * .github/workflows/ci.yml  - an inline `g++ -c` per object plus a link line
#
# CI does not use the Makefile, so the two can drift and the ONLY symptom is an
# "undefined reference" in one of them - or, worse, an object that is compiled but
# never linked, which builds cleanly and silently drops the code. AGENTS.md calls
# this out in three places ("A new source file must be added in THREE places");
# this script is the enforcement instead of the reminder.
#
# Run from the repository root. Text-only: needs no compiler and no raylib, so it
# runs on both CI jobs and locally. `make test` invokes it.
# ---------------------------------------------------------------------------
set -eu

MAKEFILE="AngelEd/Makefile"
CI=".github/workflows/ci.yml"

for f in "$MAKEFILE" "$CI"; do
    if [ ! -f "$f" ]; then
        echo "check_editor_objects: missing $f (run from the repo root)" >&2
        exit 1
    fi
done

# Objects the Makefile has a rule for: `$(BUILD_DIR)/Name.o:` at the start of a line.
makefile_objs=$(grep -oE '^\$\(BUILD_DIR\)/[A-Za-z0-9_]+\.o:' "$MAKEFILE" \
    | sed -E 's#^\$\(BUILD_DIR\)/##; s/:$//' | sort -u)

# Objects CI compiles: any `-o Name.o`.
ci_objs=$(sed -n 's/.*-o \([A-Za-z0-9_]*\.o\).*/\1/p' "$CI" | sort -u)

# AngelEd.res is linked but is a windres resource, not a compiled object; it has a
# Makefile rule but no `-o *.o` line in CI.
# OTCustom_stub.o exists only in CI: the Makefile links -l:Custom.so built from
# Source/OTCustom.cpp, while CI links a stub providing the same main_custom().
EXCLUDE='^\(AngelEd\.res\|OTCustom_stub\.o\)$'

only_makefile=$(printf '%s\n' "$makefile_objs" | grep -v "$EXCLUDE" | sort -u)
only_ci=$(printf '%s\n' "$ci_objs" | grep -v "$EXCLUDE" | sort -u)

# comm(1) takes FILE arguments, and process substitution is not available in every
# sh this runs under (busybox in w64devkit), so compare through temp files. TMPDIR
# is unset on Windows shells with no /tmp, hence the "." fallback; the trap removes
# both files either way.
tmpdir="${TMPDIR:-.}"
tmp_a="$tmpdir/edobjs_makefile.$$"
tmp_b="$tmpdir/edobjs_ci.$$"
trap 'rm -f "$tmp_a" "$tmp_b"' EXIT INT TERM
printf '%s\n' "$only_makefile" > "$tmp_a"
printf '%s\n' "$only_ci" > "$tmp_b"

missing_in_ci=$(comm -23 "$tmp_a" "$tmp_b")
missing_in_makefile=$(comm -13 "$tmp_a" "$tmp_b")

status=0

if [ -n "$missing_in_makefile" ]; then
    echo "check_editor_objects: CI compiles these, but $MAKEFILE has no rule:" >&2
    echo "$missing_in_makefile" | sed 's/^/    /' >&2
    status=1
fi

if [ -n "$missing_in_ci" ]; then
    echo "check_editor_objects: $MAKEFILE compiles these, but CI has no line:" >&2
    echo "$missing_in_ci" | sed 's/^/    /' >&2
    status=1
fi

# Every object CI COMPILES must also appear in its LINK line, or it is built and
# discarded - the failure mode that produces no error at all.
link_line=$(grep -E 'g\+\+ .*-o AngelEd\.exe' "$CI" || true)
if [ -z "$link_line" ]; then
    echo "check_editor_objects: no 'g++ ... -o AngelEd.exe' link line found in $CI" >&2
    status=1
else
    for obj in $only_ci; do
        case " $link_line " in
            *" $obj "*) ;;
            *) echo "check_editor_objects: CI compiles $obj but never links it" >&2
               status=1 ;;
        esac
    done
fi

if [ "$status" -eq 0 ]; then
    echo "check_editor_objects: AngelEd/Makefile and ci.yml agree ($(printf '%s\n' "$only_ci" | wc -l | tr -d ' ') objects)"
fi
exit "$status"
