#!/bin/sh
set -eu

RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
STATE=${ANLAND_VOLUME_STATE:-$RUNTIME_DIR/anland-volume-state}
LOCK=${STATE}.lock
STEP=${ANLAND_VOLUME_STEP:-5}
DEFAULT_VOLUME=${ANLAND_VOLUME_DEFAULT:-100}

mkdir -p "$RUNTIME_DIR"

read_state() {
    if [ -r "$STATE" ]; then
        set -- $(cat "$STATE")
        vol=${1:-$DEFAULT_VOLUME}
        muted=${2:-0}
    else
        vol=$DEFAULT_VOLUME
        muted=0
    fi
    case "$vol" in ''|*[!0-9]*) vol=$DEFAULT_VOLUME ;; esac
    case "$muted" in 1) muted=1 ;; *) muted=0 ;; esac
    if [ "$vol" -lt 0 ]; then vol=0; fi
    if [ "$vol" -gt 150 ]; then vol=150; fi
}

write_state() {
    tmp="$STATE.$$"
    printf '%s %s\n' "$vol" "$muted" > "$tmp"
    mv "$tmp" "$STATE"
}

with_lock() {
    exec 9>"$LOCK"
    flock 9
    read_state
    case "$1" in
        get)
            printf '%s %s\n' "$vol" "$muted"
            ;;
        up)
            vol=$((vol + STEP))
            if [ "$vol" -gt 150 ]; then vol=150; fi
            muted=0
            write_state
            printf '%s %s\n' "$vol" "$muted"
            ;;
        down)
            vol=$((vol - STEP))
            if [ "$vol" -lt 0 ]; then vol=0; fi
            muted=0
            write_state
            printf '%s %s\n' "$vol" "$muted"
            ;;
        toggle)
            if [ "$muted" = 1 ]; then muted=0; else muted=1; fi
            write_state
            printf '%s %s\n' "$vol" "$muted"
            ;;
        set)
            vol=${2:-}
            case "$vol" in ''|*[!0-9]*) echo "Usage: $0 set <0-150>" >&2; exit 2 ;; esac
            if [ "$vol" -gt 150 ]; then vol=150; fi
            muted=0
            write_state
            printf '%s %s\n' "$vol" "$muted"
            ;;
        *)
            echo "Usage: $0 {get|up|down|toggle|set <0-150>}" >&2
            exit 2
            ;;
    esac
}

with_lock "${1:-get}" "${2:-}"
