#!/bin/sh

agent=/mnt/jffs2/ra8p1/sdr_capture_agent_d67e6d22
adapter=/mnt/jffs2/ra8p1/sdr_adapter_iio_mmap_b68f277d.so
supervisor_script=/mnt/jffs2/autorun.sh
agent_pidfile=/var/run/ra8p1_sdr_capture_agent.pid
supervisor_pidfile=/var/run/ra8p1_sdr_supervisor.pid
supervisor_lock=/var/run/ra8p1_sdr_supervisor.lock
supervisor_lock_owner=/var/run/ra8p1_sdr_supervisor.lock/owner
agent_log=/tmp/sdr_capture_agent.log
supervisor_log=/tmp/sdr_capture_supervisor.log
trace_flag=/mnt/jffs2/ra8p1/sdr_trace.enable
control_port_hex=138C
log_segment_bytes=262144
supervisor_revision=gain25-v2
fixed_gain_db=25
fixed_gains_csv=25,25,25,25

valid_pid()
{
    case "${1:-}" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

process_cmdline()
{
    tr '\000' ' ' <"/proc/$1/cmdline" 2>/dev/null
}

current_agent_running()
{
    pid=$1
    valid_pid "$pid" || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    [ "$(readlink "/proc/$pid/exe" 2>/dev/null)" = "$agent" ] || return 1
    cmdline=$(process_cmdline "$pid")
    case "$cmdline" in
        *"$agent 192.168.31.20 --adapter $adapter"*) return 0 ;;
        *) return 1 ;;
    esac
}

known_agent_running()
{
    pid=$1
    valid_pid "$pid" || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    executable=$(readlink "/proc/$pid/exe" 2>/dev/null)
    case "$executable" in
        /mnt/jffs2/ra8p1/sdr_capture_agent*|\
        /mnt/jffs2/sdr_capture_agent*|\
        /tmp/sdr_capture_agent*) return 0 ;;
        *) return 1 ;;
    esac
}

current_supervisor_running()
{
    pid=$1
    valid_pid "$pid" || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    cmdline=$(process_cmdline "$pid")
    case "$cmdline" in
        *"$supervisor_script --supervise $agent $adapter $supervisor_revision"*) return 0 ;;
        *) return 1 ;;
    esac
}

known_supervisor_running()
{
    pid=$1
    valid_pid "$pid" || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    cmdline=$(process_cmdline "$pid")
    case "$cmdline" in
        *"--supervise /mnt/jffs2/ra8p1/sdr_capture_agent_"*) return 0 ;;
        *) return 1 ;;
    esac
}

write_pidfile()
{
    destination=$1
    value=$2
    temporary="${destination}.part.$$"
    printf '%s\n' "$value" >"$temporary" && mv -f "$temporary" "$destination"
}

remove_owned_pidfile()
{
    destination=$1
    value=$2
    recorded=$(cat "$destination" 2>/dev/null)
    if [ "$recorded" = "$value" ]; then
        rm -f "$destination"
    fi
}

log_supervisor()
{
    rotate_log_file "$supervisor_log"
    timestamp=$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null)
    printf '%s %s\n' "${timestamp:-unknown-time}" "$*" >>"$supervisor_log"
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

configure_fixed_rx_gain()
{
    gain_phy=$(find_ad9361_phy) || {
        log_supervisor "ad9361-phy is unavailable"
        return 1
    }

    for gain_channel in 0 1; do
        gain_mode_path="$gain_phy/in_voltage${gain_channel}_gain_control_mode"
        gain_value_path="$gain_phy/in_voltage${gain_channel}_hardwaregain"
        if [ ! -w "$gain_mode_path" ] || [ ! -w "$gain_value_path" ]; then
            log_supervisor "RX${gain_channel} gain attributes are unavailable"
            return 1
        fi
        printf '%s\n' manual >"$gain_mode_path" || return 1
        printf '%s\n' "$fixed_gain_db" >"$gain_value_path" || return 1
        gain_mode_readback=$(cat "$gain_mode_path" 2>/dev/null)
        gain_value_readback=$(cat "$gain_value_path" 2>/dev/null)
        if [ "$gain_mode_readback" != "manual" ] ||
           ! fixed_gain_matches "$gain_value_readback"; then
            log_supervisor "RX${gain_channel} gain verify failed mode=$gain_mode_readback gain=$gain_value_readback"
            return 1
        fi
    done

    log_supervisor "fixed RX gain verified mode=manual gain_db=$fixed_gain_db"
    return 0
}

rotate_log_file()
{
    log_path=$1
    log_size=$(wc -c <"$log_path" 2>/dev/null)
    case "${log_size:-}" in
        ''|*[!0-9]*) log_size=0 ;;
    esac
    if [ "$log_size" -ge "$log_segment_bytes" ]; then
        mv -f "$log_path" "${log_path}.previous"
    fi
}

pump_agent_log()
{
    fifo_path=$1
    if [ -f "$agent_log" ]; then
        log_size=$(wc -c <"$agent_log" 2>/dev/null)
    else
        log_size=0
    fi
    case "${log_size:-}" in
        ''|*[!0-9]*) log_size=0 ;;
    esac
    while IFS= read -r log_line || [ -n "${log_line:-}" ]; do
        line_bytes=$((${#log_line} + 1))
        if [ $((log_size + line_bytes)) -gt "$log_segment_bytes" ]; then
            mv -f "$agent_log" "${agent_log}.previous"
            log_size=0
        fi
        printf '%s\n' "$log_line" >>"$agent_log"
        log_size=$((log_size + line_bytes))
    done <"$fifo_path"
}

stop_verified_pid()
{
    pid=$1
    reason=$2
    log_supervisor "stopping pid=$pid reason=$reason"
    kill -TERM "$pid" 2>/dev/null || return 0
    attempts=0
    while kill -0 "$pid" 2>/dev/null && [ "$attempts" -lt 5 ]; do
        sleep 1
        attempts=$((attempts + 1))
    done
    if kill -0 "$pid" 2>/dev/null; then
        log_supervisor "forcing pid=$pid reason=$reason"
        kill -KILL "$pid" 2>/dev/null || true
    fi
}

stop_known_agents()
{
    for process_dir in /proc/[0-9]*; do
        pid=${process_dir##*/}
        if known_agent_running "$pid"; then
            stop_verified_pid "$pid" stale-content-addressed-agent
        fi
    done
    rm -f "$agent_pidfile"
}

process_owns_control_socket()
{
    owner_pid=$1
    valid_pid "$owner_pid" || return 1
    for owner_inode in $(
        awk '$2 ~ /:'"$control_port_hex"'$/ && $4 == "07" { print $10 }' \
            /proc/net/udp 2>/dev/null
    ); do
        for owner_descriptor in "/proc/$owner_pid/fd/"*; do
            if [ "$(readlink "$owner_descriptor" 2>/dev/null)" = \
                 "socket:[$owner_inode]" ]; then
                return 0
            fi
        done
    done
    return 1
}

stop_stale_control_port_agents()
{
    for process_dir in /proc/[0-9]*; do
        stale_pid=${process_dir##*/}
        if known_agent_running "$stale_pid" &&
           process_owns_control_socket "$stale_pid"; then
            stop_verified_pid "$stale_pid" stale-control-port-agent
        fi
    done
}

find_current_supervisor()
{
    for process_dir in /proc/[0-9]*; do
        candidate_pid=${process_dir##*/}
        [ "$candidate_pid" = "$$" ] && continue
        if current_supervisor_running "$candidate_pid"; then
            printf '%s\n' "$candidate_pid"
            return 0
        fi
    done
    return 1
}

find_known_supervisor()
{
    for process_dir in /proc/[0-9]*; do
        candidate_pid=${process_dir##*/}
        [ "$candidate_pid" = "$$" ] && continue
        if known_supervisor_running "$candidate_pid"; then
            printf '%s\n' "$candidate_pid"
            return 0
        fi
    done
    return 1
}

stop_known_supervisors()
{
    for process_dir in /proc/[0-9]*; do
        candidate_pid=${process_dir##*/}
        if known_supervisor_running "$candidate_pid"; then
            stop_verified_pid "$candidate_pid" stale-supervisor
        fi
    done
}

agent_control_socket_ready()
{
    pid=$1
    current_agent_running "$pid" || return 1
    for socket_inode in $(
        awk '$2 ~ /:'"$control_port_hex"'$/ && $4 == "07" { print $10 }' \
            /proc/net/udp 2>/dev/null
    ); do
        for descriptor in "/proc/$pid/fd/"*; do
            if [ "$(readlink "$descriptor" 2>/dev/null)" = \
                 "socket:[$socket_inode]" ]; then
                return 0
            fi
        done
    done
    return 1
}

acquire_supervisor_lock()
{
    if mkdir "$supervisor_lock" 2>/dev/null; then
        write_pidfile "$supervisor_lock_owner" "$$"
        return $?
    fi
    owner_pid=$(cat "$supervisor_lock_owner" 2>/dev/null)
    if known_supervisor_running "$owner_pid"; then
        return 1
    fi
    existing_pid=$(find_known_supervisor)
    if known_supervisor_running "$existing_pid"; then
        return 1
    fi
    rm -f "$supervisor_lock_owner"
    rmdir "$supervisor_lock" 2>/dev/null || return 1
    mkdir "$supervisor_lock" 2>/dev/null || return 1
    write_pidfile "$supervisor_lock_owner" "$$"
}

release_supervisor_lock()
{
    owner_pid=$(cat "$supervisor_lock_owner" 2>/dev/null)
    if [ "$owner_pid" = "$$" ]; then
        rm -f "$supervisor_lock_owner"
        rmdir "$supervisor_lock" 2>/dev/null || true
    fi
    remove_owned_pidfile "$supervisor_pidfile" "$$"
}

supervisor_stop_requested=0
child_pid=

request_supervisor_stop()
{
    supervisor_stop_requested=1
    if current_agent_running "${child_pid:-}"; then
        kill -TERM "$child_pid" 2>/dev/null || true
    fi
}

run_supervisor()
{
    if [ "${1:-}" != "$agent" ] || [ "${2:-}" != "$adapter" ] ||
       [ "${3:-}" != "$supervisor_revision" ]; then
        return 64
    fi
    if ! acquire_supervisor_lock; then
        return 0
    fi
    write_pidfile "$supervisor_pidfile" "$$" || {
        release_supervisor_lock
        return 1
    }
    trap 'request_supervisor_stop' INT TERM
    trap 'release_supervisor_lock' EXIT
    retry_delay_seconds=2

    if command -v ifconfig >/dev/null 2>&1; then
        ifconfig eth0 192.168.31.10 netmask 255.255.255.0 up \
            >>"$supervisor_log" 2>&1 ||
            log_supervisor "ifconfig eth0 failed"
    fi

    while [ "$supervisor_stop_requested" -eq 0 ]; do
        if [ ! -x "$agent" ] || [ ! -r "$adapter" ]; then
            log_supervisor "artifacts unavailable; retrying"
            sleep "$retry_delay_seconds"
            if [ "$retry_delay_seconds" -lt 30 ]; then
                retry_delay_seconds=$((retry_delay_seconds * 2))
                if [ "$retry_delay_seconds" -gt 30 ]; then
                    retry_delay_seconds=30
                fi
            fi
            continue
        fi

        if ! configure_fixed_rx_gain; then
            log_supervisor "fixed RX gain configuration failed; retrying"
            sleep "$retry_delay_seconds"
            continue
        fi

        if [ -f "$trace_flag" ]; then
            trace_enabled=1
            trace_argument=--trace
        else
            trace_enabled=0
            trace_argument=--no-trace
        fi

        log_supervisor "starting agent=$agent adapter=$adapter trace=$trace_enabled"
        fifo_path="/tmp/ra8p1_sdr_agent.$$.fifo"
        rm -f "$fifo_path"
        if ! mkfifo "$fifo_path"; then
            log_supervisor "cannot create agent log fifo=$fifo_path"
            sleep "$retry_delay_seconds"
            continue
        fi
        pump_agent_log "$fifo_path" &
        log_pump_pid=$!
        RA8P1_SDR_FIXED_GAINS_DB="$fixed_gains_csv" \
        RA8P1_SDR_IDENTITY=pluto-ethaddr-3E70EACC9791 \
        RA8P1_IIO_TUNE_SETTLE_US=1000 \
        RA8P1_IIO_TUNE_DISCARD_SAMPLES=4096 \
        RA8P1_SDR_UDP_GSO=1 \
        RA8P1_SDR_SEND_BATCH_OVERRIDE=16 \
        RA8P1_SDR_SNDBUF_BYTES=4194304 \
        RA8P1_SDR_CAPTURE_TRACE="$trace_enabled" \
        RA8P1_SDR_CRC_TRACE="$trace_enabled" \
        RA8P1_SDR_CRC_BACKEND=nibble \
            "$agent" 192.168.31.20 --adapter "$adapter" "$trace_argument" \
            >"$fifo_path" 2>&1 &
        child_pid=$!
        write_pidfile "$agent_pidfile" "$child_pid"

        stable_seconds=0
        ready_seen=0
        while current_agent_running "$child_pid" &&
              [ "$stable_seconds" -lt 60 ]; do
            if [ "$ready_seen" -eq 0 ] &&
               agent_control_socket_ready "$child_pid"; then
                ready_seen=1
                log_supervisor "ready agent_pid=$child_pid control_port=5004"
            fi
            sleep 1
            stable_seconds=$((stable_seconds + 1))
        done
        if [ "$ready_seen" -eq 1 ] && [ "$stable_seconds" -ge 60 ]; then
            retry_delay_seconds=2
        fi
        wait "$child_pid"
        child_status=$?
        if current_agent_running "$child_pid"; then
            stop_verified_pid "$child_pid" supervisor-stop
        fi
        remove_owned_pidfile "$agent_pidfile" "$child_pid"
        wait "$log_pump_pid" 2>/dev/null || true
        rm -f "$fifo_path"
        log_supervisor "agent exited pid=$child_pid status=$child_status"
        child_pid=

        if [ "$supervisor_stop_requested" -eq 0 ]; then
            sleep "$retry_delay_seconds"
            if [ "$retry_delay_seconds" -lt 30 ]; then
                retry_delay_seconds=$((retry_delay_seconds * 2))
                if [ "$retry_delay_seconds" -gt 30 ]; then
                    retry_delay_seconds=30
                fi
            fi
        fi
    done
    return 0
}

start_supervisor()
{
    if [ ! -x "$agent" ] || [ ! -r "$adapter" ]; then
        log_supervisor "persistent artifacts are missing"
        return 1
    fi

    supervisor_pid=$(cat "$supervisor_pidfile" 2>/dev/null)
    if current_supervisor_running "$supervisor_pid"; then
        return 0
    fi
    if known_supervisor_running "$supervisor_pid"; then
        stop_verified_pid "$supervisor_pid" old-supervisor
    fi
    rm -f "$supervisor_pidfile"

    supervisor_pid=$(find_current_supervisor)
    if current_supervisor_running "$supervisor_pid"; then
        write_pidfile "$supervisor_pidfile" "$supervisor_pid"
        return 0
    fi
    stop_known_supervisors
    stop_stale_control_port_agents
    stop_known_agents

    nohup "$supervisor_script" --supervise "$agent" "$adapter" \
        "$supervisor_revision" \
        </dev/null >>"$supervisor_log" 2>&1 &
    supervisor_pid=$!
    if kill -0 "$supervisor_pid" 2>/dev/null; then
        return 0
    fi
    log_supervisor "supervisor failed to start pid=$supervisor_pid"
    return 1
}

case "${1:-}" in
    --supervise)
        run_supervisor "${2:-}" "${3:-}" "${4:-}"
        ;;
    *)
        start_supervisor
        ;;
esac
