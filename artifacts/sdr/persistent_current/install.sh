#!/bin/sh

set -u

agent_name=sdr_capture_agent_d67e6d22
adapter_name=sdr_adapter_iio_mmap_b68f277d.so
target_dir=/mnt/jffs2/ra8p1
autorun_target=/mnt/jffs2/autorun.sh
previous_pointer=$target_dir/autorun_previous_path
supervisor_revision=gain25-v2
fixed_gain_db=25
fixed_gains_csv=25,25,25,25

fail()
{
    printf 'RA8P1 SDR install: %s\n' "$*" >&2
    exit 1
}

file_hash()
{
    set -- $(sha256sum "$1" 2>/dev/null)
    printf '%s\n' "${1:-}"
}

find_ad9361_phy()
{
    for phy_candidate in /sys/bus/iio/devices/iio:device*; do
        if [ -r "$phy_candidate/name" ] &&
           [ "$(cat "$phy_candidate/name" 2>/dev/null)" = "ad9361-phy" ]; then
            printf '%s\n' "$phy_candidate"
            return 0
        fi
    done
    return 1
}

fixed_gain_matches()
{
    gain_value=${1%% *}
    [ "$gain_value" = "$fixed_gain_db" ] ||
        [ "$gain_value" = "$fixed_gain_db.000000" ]
}

publish_content_addressed()
{
    source_path=$1
    destination_path=$2
    mode=$3
    expected_hash=$(file_hash "$source_path")
    [ -n "$expected_hash" ] || fail "cannot hash $source_path"

    if [ -L "$destination_path" ]; then
        fail "refusing symbolic-link destination $destination_path"
    fi
    if [ -e "$destination_path" ]; then
        [ -f "$destination_path" ] || fail "destination is not a regular file: $destination_path"
        actual_hash=$(file_hash "$destination_path")
        [ "$actual_hash" = "$expected_hash" ] ||
            fail "content-addressed destination collision: $destination_path"
        chmod "$mode" "$destination_path" || fail "cannot set mode on $destination_path"
        return 0
    fi

    temporary_path="${destination_path}.part.$$"
    rm -f "$temporary_path"
    cp "$source_path" "$temporary_path" || fail "cannot stage $destination_path"
    chmod "$mode" "$temporary_path" || fail "cannot set staged mode for $destination_path"
    actual_hash=$(file_hash "$temporary_path")
    [ "$actual_hash" = "$expected_hash" ] || {
        rm -f "$temporary_path"
        fail "staged hash mismatch for $destination_path"
    }
    mv -f "$temporary_path" "$destination_path" || fail "cannot publish $destination_path"
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd)
[ -n "${script_dir:-}" ] || fail "cannot resolve bundle directory"
[ "$(id -u 2>/dev/null)" = 0 ] || fail "must run as root"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is unavailable"
command -v readlink >/dev/null 2>&1 || fail "readlink is unavailable"
[ -d /mnt/jffs2 ] && [ -w /mnt/jffs2 ] || fail "/mnt/jffs2 is not writable"

for required in "$agent_name" "$adapter_name" autorun.sh SHA256SUMS; do
    [ -f "$script_dir/$required" ] && [ ! -L "$script_dir/$required" ] ||
        fail "bundle file is missing or unsafe: $required"
done

(cd "$script_dir" && sha256sum -c SHA256SUMS) || fail "bundle SHA-256 verification failed"
mkdir -p "$target_dir" || fail "cannot create $target_dir"

publish_content_addressed "$script_dir/$agent_name" "$target_dir/$agent_name" 0755
publish_content_addressed "$script_dir/$adapter_name" "$target_dir/$adapter_name" 0644

if [ -L "$autorun_target" ]; then
    fail "refusing symbolic-link autorun target"
fi
if [ -f "$autorun_target" ]; then
    previous_hash=$(file_hash "$autorun_target")
    [ -n "$previous_hash" ] || fail "cannot hash current autorun"
    previous_short=$(printf '%s\n' "$previous_hash" | cut -c 1-12)
    backup_path="${autorun_target}.backup.${previous_short}"
    if [ -e "$backup_path" ]; then
        [ -f "$backup_path" ] && [ ! -L "$backup_path" ] || fail "unsafe existing backup $backup_path"
        [ "$(file_hash "$backup_path")" = "$previous_hash" ] || fail "backup hash collision $backup_path"
    else
        backup_part="${backup_path}.part.$$"
        cp "$autorun_target" "$backup_part" || fail "cannot stage autorun backup"
        chmod 0755 "$backup_part" || fail "cannot set autorun backup mode"
        [ "$(file_hash "$backup_part")" = "$previous_hash" ] || {
            rm -f "$backup_part"
            fail "autorun backup hash mismatch"
        }
        mv -f "$backup_part" "$backup_path" || fail "cannot publish autorun backup"
    fi
    pointer_part="${previous_pointer}.part.$$"
    printf '%s\n' "$backup_path" >"$pointer_part" || fail "cannot stage rollback pointer"
    mv -f "$pointer_part" "$previous_pointer" || fail "cannot publish rollback pointer"
fi

autorun_expected=$(file_hash "$script_dir/autorun.sh")
autorun_part="${autorun_target}.part.$$"
cp "$script_dir/autorun.sh" "$autorun_part" || fail "cannot stage autorun"
chmod 0755 "$autorun_part" || fail "cannot set autorun mode"
[ "$(file_hash "$autorun_part")" = "$autorun_expected" ] || {
    rm -f "$autorun_part"
    fail "staged autorun hash mismatch"
}
mv -f "$autorun_part" "$autorun_target" || fail "cannot publish autorun"
sync

"$autorun_target" || fail "new supervisor launch failed"

attempt=0
while [ "$attempt" -lt 15 ]; do
    supervisor_pid=$(cat /var/run/ra8p1_sdr_supervisor.pid 2>/dev/null)
    agent_pid=$(cat /var/run/ra8p1_sdr_capture_agent.pid 2>/dev/null)
    if [ -n "${supervisor_pid:-}" ] && [ -n "${agent_pid:-}" ] &&
       kill -0 "$supervisor_pid" 2>/dev/null && kill -0 "$agent_pid" 2>/dev/null &&
       [ "$(readlink "/proc/$agent_pid/exe" 2>/dev/null)" = "$target_dir/$agent_name" ]; then
        environment=$(tr '\000' '\n' <"/proc/$agent_pid/environ" 2>/dev/null)
        supervisor_command=$(tr '\000' ' ' <"/proc/$supervisor_pid/cmdline" 2>/dev/null)
        gain_phy=$(find_ad9361_phy 2>/dev/null) || gain_phy=
        gain_mode0=$(cat "$gain_phy/in_voltage0_gain_control_mode" 2>/dev/null)
        gain_value0=$(cat "$gain_phy/in_voltage0_hardwaregain" 2>/dev/null)
        gain_mode1=$(cat "$gain_phy/in_voltage1_gain_control_mode" 2>/dev/null)
        gain_value1=$(cat "$gain_phy/in_voltage1_hardwaregain" 2>/dev/null)
        case "$supervisor_command" in
            *"$autorun_target --supervise $target_dir/$agent_name $target_dir/$adapter_name $supervisor_revision"*)
                supervisor_current=1 ;;
            *) supervisor_current=0 ;;
        esac
        if [ "$supervisor_current" -eq 1 ] &&
           printf '%s\n' "$environment" | grep -qx "RA8P1_SDR_FIXED_GAINS_DB=$fixed_gains_csv" &&
           printf '%s\n' "$environment" | grep -qx 'RA8P1_IIO_TUNE_SETTLE_US=1000' &&
           printf '%s\n' "$environment" | grep -qx 'RA8P1_IIO_TUNE_DISCARD_SAMPLES=4096' &&
           printf '%s\n' "$environment" | grep -qx 'RA8P1_SDR_UDP_GSO=1' &&
           printf '%s\n' "$environment" | grep -qx 'RA8P1_SDR_CRC_BACKEND=nibble' &&
           [ "$gain_mode0" = "manual" ] && fixed_gain_matches "$gain_value0" &&
           [ "$gain_mode1" = "manual" ] && fixed_gain_matches "$gain_value1"; then
            break
        fi
    fi
    sleep 1
    attempt=$((attempt + 1))
done
[ "$attempt" -lt 15 ] || fail "new persistent agent did not become ready"

printf 'RA8P1 SDR install: PASS supervisor_pid=%s agent_pid=%s\n' "$supervisor_pid" "$agent_pid"
printf 'RA8P1 SDR install: agent=%s\n' "$target_dir/$agent_name"
printf 'RA8P1 SDR install: adapter=%s\n' "$target_dir/$adapter_name"
printf 'RA8P1 SDR install: gain=manual/%s dB RX0+RX1\n' "$fixed_gain_db"
if [ -f "$previous_pointer" ]; then
    printf 'RA8P1 SDR install: rollback=%s\n' "$(cat "$previous_pointer")"
fi
