#!/bin/sh

set -u

autorun_target=/mnt/jffs2/autorun.sh
previous_pointer=/mnt/jffs2/ra8p1/autorun_previous_path

fail()
{
    printf 'RA8P1 SDR rollback: %s\n' "$*" >&2
    exit 1
}

file_hash()
{
    set -- $(sha256sum "$1" 2>/dev/null)
    printf '%s\n' "${1:-}"
}

[ "$(id -u 2>/dev/null)" = 0 ] || fail "must run as root"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is unavailable"

if [ "$#" -gt 1 ]; then
    fail "usage: $0 [/mnt/jffs2/autorun.sh.backup.<sha-prefix>]"
fi
if [ "$#" -eq 1 ]; then
    backup_path=$1
else
    [ -f "$previous_pointer" ] || fail "rollback pointer is missing"
    backup_path=$(cat "$previous_pointer" 2>/dev/null)
fi

case "$backup_path" in
    /mnt/jffs2/autorun.sh.backup.*) ;;
    *) fail "unsafe rollback path: $backup_path" ;;
esac
[ -f "$backup_path" ] && [ ! -L "$backup_path" ] || fail "rollback file is missing or unsafe"

if [ -f "$autorun_target" ] && [ ! -L "$autorun_target" ]; then
    current_hash=$(file_hash "$autorun_target")
    current_short=$(printf '%s\n' "$current_hash" | cut -c 1-12)
    recovery_path="${autorun_target}.rollback-from.${current_short}"
    if [ ! -e "$recovery_path" ]; then
        recovery_part="${recovery_path}.part.$$"
        cp "$autorun_target" "$recovery_part" || fail "cannot stage current autorun recovery copy"
        chmod 0755 "$recovery_part" || fail "cannot set recovery-copy mode"
        [ "$(file_hash "$recovery_part")" = "$current_hash" ] || {
            rm -f "$recovery_part"
            fail "current autorun recovery hash mismatch"
        }
        mv -f "$recovery_part" "$recovery_path" || fail "cannot publish recovery copy"
    fi
fi

expected_hash=$(file_hash "$backup_path")
rollback_part="${autorun_target}.part.$$"
cp "$backup_path" "$rollback_part" || fail "cannot stage rollback autorun"
chmod 0755 "$rollback_part" || fail "cannot set rollback autorun mode"
[ "$(file_hash "$rollback_part")" = "$expected_hash" ] || {
    rm -f "$rollback_part"
    fail "staged rollback hash mismatch"
}
mv -f "$rollback_part" "$autorun_target" || fail "cannot publish rollback autorun"
sync

"$autorun_target" || fail "restored supervisor launch failed"
printf 'RA8P1 SDR rollback: PASS restored=%s sha256=%s\n' "$backup_path" "$expected_hash"
