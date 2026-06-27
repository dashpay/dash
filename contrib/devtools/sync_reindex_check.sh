#!/usr/bin/env bash
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Daily sync + reindex regression check.
#
# Boots dashd, waits for it to reach tip, then restarts with -reindex
# (or -reindex-chainstate when requested) and verifies the node converges back to the same
# (or a higher) tip. Designed to run on a stateful self-hosted runner where the
# datadir is preserved across invocations so each run resumes from yesterday's
# state rather than syncing from scratch.
#
# Communicates with the node via dash-cli RPC; log scraping is only used as a
# last resort when checking process liveness.
#
export LC_ALL=C

set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: sync_reindex_check.sh [options]

Options (all also configurable via environment variables):
  --dashd PATH               Path to dashd (env: DASHD, default: ./src/dashd)
  --dash-cli PATH            Path to dash-cli (env: DASH_CLI, default: ./src/dash-cli)
  --datadir PATH             Persistent data directory (env: DATADIR, required)
  --network NAME             test|main|devnet|regtest (env: NETWORK, default: test)
  --mode MODE                sync|reindex|both (env: MODE, default: both)
  --reindex-flag FLAG        -reindex or -reindex-chainstate
                             (env: REINDEX_FLAG, default: -reindex)
  --timeout SECONDS          Per-phase wall-clock timeout
                             (env: PHASE_TIMEOUT, default: 21600 = 6h)
  --poll-interval SECONDS    RPC poll interval (env: POLL_INTERVAL, default: 30)
  --min-headers N            Minimum acceptable header count when no external
                             tip source is available (env: MIN_HEADERS,
                             defaults: testnet=1000000, mainnet=2000000,
                             others=0). Ignored when --external-tip-url is set.
  --external-tip-url URL     HTTP endpoint returning JSON with .height or a
                             bare integer. When set, the local tip must be
                             within --tip-tolerance of this value; a fetch or
                             parse failure is fatal (the --min-headers floor
                             is NOT used as a silent fallback).
                             (env: EXTERNAL_TIP_URL)
  --tip-tolerance N          Allowed lag vs. external tip in blocks. Applies
                             only to the external-tip comparison; the
                             reindex-vs-sync baseline check is strict and has
                             no tolerance. (env: TIP_TOLERANCE, default: 50)
  --extra-args "..."         Extra args appended to every dashd invocation.
                             For --network=devnet, -port=<n> and -rpcport=<n>
                             are required, and -devnet=<name> may be included
                             to select a named devnet; -devnet=<name> and
                             -rpcport=<n> are also forwarded to dash-cli so it
                             can talk to the running node.
                             (env: EXTRA_DASHD_ARGS)
  --keep-running             Leave dashd running on success (for debugging)
  -h, --help                 Show this help

Exit codes:
  0  success
  1  configuration / startup failure
  2  sync phase failed (timeout or regression)
  3  reindex phase failed (timeout or regression)
  4  tip verification failed (height below expectation)
USAGE
}

# ---- defaults -----------------------------------------------------------

DASHD="${DASHD:-./src/dashd}"
DASH_CLI="${DASH_CLI:-./src/dash-cli}"
DATADIR="${DATADIR:-}"
NETWORK="${NETWORK:-test}"
MODE="${MODE:-both}"
REINDEX_FLAG="${REINDEX_FLAG:--reindex}"
PHASE_TIMEOUT="${PHASE_TIMEOUT:-21600}"
POLL_INTERVAL="${POLL_INTERVAL:-30}"
MIN_HEADERS="${MIN_HEADERS:-}"
EXTERNAL_TIP_URL="${EXTERNAL_TIP_URL:-}"
TIP_TOLERANCE="${TIP_TOLERANCE:-50}"
EXTRA_DASHD_ARGS="${EXTRA_DASHD_ARGS:-}"
KEEP_RUNNING=0

while (( $# > 0 )); do
    case "$1" in
        --dashd) DASHD="$2"; shift 2;;
        --dash-cli) DASH_CLI="$2"; shift 2;;
        --datadir) DATADIR="$2"; shift 2;;
        --network) NETWORK="$2"; shift 2;;
        --mode) MODE="$2"; shift 2;;
        --reindex-flag) REINDEX_FLAG="$2"; shift 2;;
        --timeout) PHASE_TIMEOUT="$2"; shift 2;;
        --poll-interval) POLL_INTERVAL="$2"; shift 2;;
        --min-headers) MIN_HEADERS="$2"; shift 2;;
        --external-tip-url) EXTERNAL_TIP_URL="$2"; shift 2;;
        --tip-tolerance) TIP_TOLERANCE="$2"; shift 2;;
        --extra-args) EXTRA_DASHD_ARGS="$2"; shift 2;;
        --keep-running) KEEP_RUNNING=1; shift;;
        -h|--help) usage; exit 0;;
        *) echo "Unknown argument: $1" >&2; usage >&2; exit 1;;
    esac
done

# ---- validation ---------------------------------------------------------

if [[ -z "$DATADIR" ]]; then
    echo "ERROR: --datadir (or DATADIR env var) is required." >&2
    exit 1
fi
if [[ ! -x "$DASHD" ]]; then
    echo "ERROR: dashd not found or not executable at: $DASHD" >&2
    exit 1
fi
if [[ ! -x "$DASH_CLI" ]]; then
    echo "ERROR: dash-cli not found or not executable at: $DASH_CLI" >&2
    exit 1
fi
case "$NETWORK" in
    main|test|devnet|regtest) ;;
    *) echo "ERROR: --network must be one of main|test|devnet|regtest" >&2; exit 1;;
esac
case "$MODE" in
    sync|reindex|both) ;;
    *) echo "ERROR: --mode must be one of sync|reindex|both" >&2; exit 1;;
esac
case "$REINDEX_FLAG" in
    -reindex|-reindex-chainstate) ;;
    *) echo "ERROR: --reindex-flag must be -reindex or -reindex-chainstate" >&2; exit 1;;
esac

mkdir -p "$DATADIR"

if [[ -z "$MIN_HEADERS" ]]; then
    case "$NETWORK" in
        main) MIN_HEADERS=2000000;;
        test) MIN_HEADERS=1000000;;
        *) MIN_HEADERS=0;;
    esac
fi

PIDFILE="$DATADIR/sync_reindex_check.pid"
LOGFILE="$DATADIR/sync_reindex_check.log"
DASHD_PID=""

# shellcheck disable=SC2206  # word-splitting EXTRA_DASHD_ARGS is intentional
EXTRA_ARGS_ARR=( $EXTRA_DASHD_ARGS )

# Pick out args that dash-cli also needs to reach the node, so we can mirror
# them when invoking the CLI. -port is a dashd-only arg and is intentionally
# NOT forwarded. -devnet=<name> selects a named devnet (the bare -devnet flag
# from NETWORK_FLAG would otherwise duplicate / conflict with this).
DEVNET_NAME_ARG=""
RPCPORT_ARG=""
HAS_DEVNET_PORT=0
HAS_DEVNET_RPCPORT=0
for arg in "${EXTRA_ARGS_ARR[@]}"; do
    case "$arg" in
        -devnet=*) DEVNET_NAME_ARG="$arg";;
        -port=*)   HAS_DEVNET_PORT=1;;
        -rpcport=*)
            HAS_DEVNET_RPCPORT=1
            RPCPORT_ARG="$arg"
            ;;
    esac
done

if [[ -n "$DEVNET_NAME_ARG" && "$NETWORK" != "devnet" ]]; then
    echo "ERROR: -devnet=<name> in --extra-args is only valid with --network=devnet." >&2
    exit 1
fi

NETWORK_FLAG=""
case "$NETWORK" in
    test) NETWORK_FLAG="-testnet";;
    devnet)
        # If the user supplied -devnet=<name> in extras, don't also pass the
        # bare -devnet ourselves: dashd would reject the duplicate.
        [[ -z "$DEVNET_NAME_ARG" ]] && NETWORK_FLAG="-devnet"
        ;;
    regtest) NETWORK_FLAG="-regtest";;
esac

NET_SUBDIR=""
case "$NETWORK" in
    test) NET_SUBDIR="testnet3";;
    devnet)
        if [[ -n "$DEVNET_NAME_ARG" ]]; then
            NET_SUBDIR="devnet-${DEVNET_NAME_ARG#-devnet=}"
        else
            NET_SUBDIR="devnet"
        fi
        ;;
    regtest) NET_SUBDIR="regtest";;
esac
NET_DATADIR="$DATADIR"
if [[ -n "$NET_SUBDIR" ]]; then
    NET_DATADIR="$DATADIR/$NET_SUBDIR"
fi

if [[ "$NETWORK" == "devnet" ]]; then
    if (( ! HAS_DEVNET_PORT || ! HAS_DEVNET_RPCPORT )); then
        echo "ERROR: --network=devnet requires --extra-args with -port=<n> and -rpcport=<n>." >&2
        exit 1
    fi
fi

# ---- helpers ------------------------------------------------------------

log() {
    # All progress logging goes to stderr so that command substitutions
    # (e.g. capturing the post-sync tip from wait_for_sync) don't see it.
    # The launcher additionally tees stderr into $LOGFILE below.
    printf '[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2
    if [[ -n "${LOGFILE:-}" ]]; then
        printf '[%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$LOGFILE" 2>/dev/null || true
    fi
}

cli() {
    local args=()
    if [[ -n "$DEVNET_NAME_ARG" ]]; then
        args+=("$DEVNET_NAME_ARG")
    elif [[ -n "$NETWORK_FLAG" ]]; then
        args+=("$NETWORK_FLAG")
    fi
    args+=("-datadir=$DATADIR" "-rpcclienttimeout=120")
    [[ -n "$RPCPORT_ARG" ]] && args+=("$RPCPORT_ARG")
    "$DASH_CLI" "${args[@]}" "$@"
}

start_node() {
    local phase="$1"
    shift
    local extra_phase_args=("$@")

    log "Starting dashd for phase '$phase' (flags: ${extra_phase_args[*]:-none})"
    local cmd=(
        "$DASHD"
        "-datadir=$DATADIR"
        "-daemon=0"
        "-printtoconsole=0"
        "-debuglogfile=debug.log"
        "-pid=$PIDFILE"
    )
    [[ -n "$NETWORK_FLAG" ]] && cmd+=("$NETWORK_FLAG")
    cmd+=("${extra_phase_args[@]}")
    if (( ${#EXTRA_ARGS_ARR[@]} > 0 )); then
        cmd+=("${EXTRA_ARGS_ARR[@]}")
    fi

    # Run dashd in background; redirect stderr/stdout to phase log.
    "${cmd[@]}" >> "$LOGFILE" 2>&1 &
    local launcher_pid=$!
    DASHD_PID="$launcher_pid"
    echo "$launcher_pid" > "$PIDFILE.launcher"

    # Wait for RPC to become responsive.
    local waited=0
    local rpc_warmup_timeout=600
    while (( waited < rpc_warmup_timeout )); do
        if ! kill -0 "$launcher_pid" 2>/dev/null; then
            log "ERROR: dashd exited before RPC came up; tail of log:"
            tail -n 50 "$LOGFILE" >&2 || true
            return 1
        fi
        if cli getblockchaininfo >/dev/null 2>&1; then
            log "RPC is up after ${waited}s"
            return 0
        fi
        sleep 5
        waited=$((waited + 5))
    done
    log "ERROR: dashd RPC did not respond within ${rpc_warmup_timeout}s"
    return 1
}

stop_node() {
    log "Stopping dashd"
    if ! cli stop >/dev/null 2>&1; then
        log "WARN: 'dash-cli stop' failed; trying SIGTERM"
        if [[ -f "$PIDFILE" ]]; then
            kill "$(cat "$PIDFILE")" 2>/dev/null || true
        fi
    fi
    local waited=0
    while (( waited < 300 )); do
        if [[ ! -f "$PIDFILE" ]] || ! kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
            log "Node stopped after ${waited}s"
            return 0
        fi
        sleep 2
        waited=$((waited + 2))
    done
    log "ERROR: node did not stop cleanly within 300s; sending SIGKILL"
    [[ -f "$PIDFILE" ]] && kill -9 "$(cat "$PIDFILE")" 2>/dev/null || true
    return 1
}

# Fetch external tip; prints integer height on success.
fetch_external_tip() {
    [[ -z "$EXTERNAL_TIP_URL" ]] && return 1
    if ! command -v curl >/dev/null 2>&1; then
        log "ERROR: curl unavailable, cannot fetch external tip"
        return 1
    fi
    local body
    if ! body=$(curl -fsSL --max-time 30 "$EXTERNAL_TIP_URL" 2>/dev/null); then
        log "ERROR: external tip fetch failed: $EXTERNAL_TIP_URL"
        return 1
    fi
    [[ -z "$body" ]] && { log "ERROR: external tip fetch returned empty body"; return 1; }
    # Try JSON .height first, then a bare integer. Do not strip all digits
    # from arbitrary JSON; {"height":123,"time":456} must not become 123456.
    local h
    if command -v jq >/dev/null 2>&1; then
        h=$(printf '%s' "$body" | jq -r 'if type=="number" then . elif .height then .height else empty end' 2>/dev/null || true)
    fi
    if [[ -z "${h:-}" ]]; then
        if [[ "$body" =~ ^[[:space:]]*[0-9]+[[:space:]]*$ ]]; then
            h=$(printf '%s' "$body" | tr -dc '0-9')
        else
            h=$(printf '%s' "$body" | sed -n 's/.*"height"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -n1)
        fi
    fi
    if [[ "$h" =~ ^[0-9]+$ ]]; then
        printf '%s' "$h"
        return 0
    fi
    log "ERROR: external tip response did not contain a numeric height"
    return 1
}

INDEX_STATUS=""

indexes_synced() {
    local target_height="$1"
    local info
    if ! info=$(cli getindexinfo 2>/dev/null); then
        INDEX_STATUS="unavailable"
        log "WARN: getindexinfo failed; will retry"
        return 1
    fi

    local synced_values height_values
    synced_values=$(printf '%s\n' "$info" | sed -n 's/.*"synced":[[:space:]]*\(true\|false\).*/\1/p')
    height_values=$(printf '%s\n' "$info" | sed -n 's/.*"best_block_height":[[:space:]]*\([0-9]*\).*/\1/p')

    if [[ -z "$synced_values" && -z "$height_values" ]]; then
        INDEX_STATUS="none"
        return 0
    fi

    INDEX_STATUS="synced=$(printf '%s' "$synced_values" | tr '\n' ',') heights=$(printf '%s' "$height_values" | tr '\n' ',')"

    if printf '%s\n' "$synced_values" | grep -qx 'false'; then
        return 1
    fi

    local h
    while IFS= read -r h; do
        [[ -z "$h" ]] && continue
        if (( h < target_height )); then
            return 1
        fi
    done <<< "$height_values"

    return 0
}

block_hash_at_height() {
    local height="$1"
    if ! [[ "$height" =~ ^[0-9]+$ ]]; then
        log "ERROR: cannot query block hash for non-numeric height: '$height'"
        return 1
    fi
    cli getblockhash "$height" 2>/dev/null
}

# Poll the node until it is considered synced or timeout/regression occurs.
wait_for_sync() {
    local phase="$1"
    local deadline=$(( $(date +%s) + PHASE_TIMEOUT ))
    local last_blocks=-1
    local last_progress=""
    local last_index_status=""
    local stagnant_iters=0
    local stagnant_limit=$(( 1800 / POLL_INTERVAL ))  # ~30 min with no progress
    (( stagnant_limit < 5 )) && stagnant_limit=5

    log "Waiting for sync (phase=$phase, timeout=${PHASE_TIMEOUT}s)"
    while true; do
        local now
        now=$(date +%s)
        if (( now > deadline )); then
            log "ERROR: phase '$phase' exceeded timeout of ${PHASE_TIMEOUT}s"
            return 1
        fi

        local info
        if ! info=$(cli getblockchaininfo 2>/dev/null); then
            if [[ -n "$DASHD_PID" ]] && ! kill -0 "$DASHD_PID" 2>/dev/null; then
                log "ERROR: dashd exited while waiting for phase '$phase'; tail of log:"
                tail -n 50 "$LOGFILE" >&2 || true
                return 1
            fi
            log "WARN: getblockchaininfo failed; will retry"
            sleep "$POLL_INTERVAL"
            continue
        fi

        local blocks headers ibd progress
        blocks=$(printf '%s' "$info" | sed -n 's/.*"blocks":[[:space:]]*\([0-9]*\).*/\1/p' | head -n1)
        headers=$(printf '%s' "$info" | sed -n 's/.*"headers":[[:space:]]*\([0-9]*\).*/\1/p' | head -n1)
        progress=$(printf '%s' "$info" | sed -n 's/.*"verificationprogress":[[:space:]]*\([0-9.eE+-]*\).*/\1/p' | head -n1)
        ibd=$(printf '%s' "$info" | sed -n 's/.*"initialblockdownload":[[:space:]]*\(true\|false\).*/\1/p' | head -n1)

        local peers
        peers=$(cli getconnectioncount 2>/dev/null || echo 0)

        log "phase=$phase blocks=$blocks headers=$headers ibd=$ibd progress=$progress peers=$peers"

        # Detect when node has converged.
        if [[ -n "$blocks" && -n "$headers" && "$blocks" == "$headers" && "$ibd" == "false" && "$headers" -gt 0 ]]; then
            if indexes_synced "$blocks"; then
                if [[ -n "$EXTERNAL_TIP_URL" ]]; then
                    local external_tip
                    if ! external_tip=$(fetch_external_tip); then
                        return 1
                    fi
                    if (( blocks + TIP_TOLERANCE < external_tip )); then
                        log "Local chain is internally converged at $blocks, but external tip is $external_tip; continuing"
                        sleep "$POLL_INTERVAL"
                        continue
                    fi
                fi
                log "Node reports converged: blocks=$blocks headers=$headers ibd=false indexes=$INDEX_STATUS"
                printf '%s' "$blocks"
                return 0
            fi
            log "Node chain converged but indexes are not ready: blocks=$blocks indexes=$INDEX_STATUS"
        fi

        # Stagnation check: if blocks, validation progress, or index progress
        # changed, reset counter.
        if [[ "$blocks" != "$last_blocks" || "$progress" != "$last_progress" || "$INDEX_STATUS" != "$last_index_status" ]]; then
            stagnant_iters=0
            last_blocks="$blocks"
            last_progress="$progress"
            last_index_status="$INDEX_STATUS"
        else
            stagnant_iters=$((stagnant_iters + 1))
            if (( stagnant_iters >= stagnant_limit )); then
                log "ERROR: no progress for ${stagnant_iters} consecutive polls (~$(( stagnant_iters * POLL_INTERVAL ))s)"
                return 1
            fi
        fi

        sleep "$POLL_INTERVAL"
    done
}

verify_tip() {
    local phase="$1"
    local achieved="$2"
    local baseline="${3:-}"   # tip from earlier phase, if any
    local baseline_hash="${4:-}"

    if ! [[ "$achieved" =~ ^[0-9]+$ ]]; then
        log "ERROR: phase '$phase' produced non-numeric tip: '$achieved'"
        return 1
    fi

    # Compare to baseline from prior phase (reindex must not regress).
    if [[ -n "$baseline" && "$baseline" =~ ^[0-9]+$ ]]; then
        if (( achieved < baseline )); then
            log "ERROR: phase '$phase' tip $achieved is below baseline $baseline"
            return 1
        fi
        if [[ -n "$baseline_hash" ]]; then
            local current_baseline_hash
            if ! current_baseline_hash=$(block_hash_at_height "$baseline"); then
                return 1
            fi
            if [[ "$current_baseline_hash" != "$baseline_hash" ]]; then
                log "ERROR: phase '$phase' hash at baseline height $baseline changed"
                log "ERROR: expected $baseline_hash, got $current_baseline_hash"
                return 1
            fi
        fi
    fi

    # Compare to external source if configured.
    if [[ -n "$EXTERNAL_TIP_URL" ]]; then
        local external_tip
        if ! external_tip=$(fetch_external_tip); then
            return 1
        fi
        log "External tip reports height=$external_tip; local=$achieved (tolerance=$TIP_TOLERANCE)"
        if (( achieved + TIP_TOLERANCE < external_tip )); then
            log "ERROR: local tip $achieved trails external tip $external_tip by more than $TIP_TOLERANCE"
            return 1
        fi
        return 0
    fi

    # Conservative fallback: require headers/blocks above a minimum so that an
    # isolated node that "synced" to height 0 with no peers does not pass.
    if (( achieved < MIN_HEADERS )); then
        log "ERROR: tip $achieved is below MIN_HEADERS=$MIN_HEADERS for network=$NETWORK"
        log "Hint: set --min-headers or --external-tip-url to tune this check."
        return 1
    fi

    log "Tip $achieved meets conservative floor of $MIN_HEADERS for $NETWORK"
    return 0
}

# shellcheck disable=SC2329  # invoked indirectly via `trap`
cleanup() {
    if (( KEEP_RUNNING )) && [[ -f "$PIDFILE" ]]; then
        return
    fi
    if [[ -f "$PIDFILE" ]] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
        log "cleanup: stopping dashd"
        stop_node || true
    fi
}
trap cleanup EXIT

# ---- main ---------------------------------------------------------------

log "Config: dashd=$DASHD cli=$DASH_CLI datadir=$DATADIR net_datadir=$NET_DATADIR network=$NETWORK mode=$MODE reindex_flag=$REINDEX_FLAG"
log "Config: phase_timeout=${PHASE_TIMEOUT}s poll=${POLL_INTERVAL}s min_headers=$MIN_HEADERS tolerance=$TIP_TOLERANCE"
[[ -n "$EXTERNAL_TIP_URL" ]] && log "Config: external_tip_url=$EXTERNAL_TIP_URL"

# Refuse to run if a stale dashd is already holding the net-specific datadir.
if [[ -f "$NET_DATADIR/.lock" ]] && fuser "$NET_DATADIR/.lock" >/dev/null 2>&1; then
    log "ERROR: $NET_DATADIR appears to be in use by another dashd process. Aborting."
    exit 1
fi

SYNC_TIP=""
SYNC_HASH=""
if [[ "$MODE" == "sync" || "$MODE" == "both" ]]; then
    if ! start_node "sync"; then
        exit 1
    fi
    if ! SYNC_TIP=$(wait_for_sync "sync"); then
        log "Sync phase failed"
        stop_node || true
        exit 2
    fi
    if ! verify_tip "sync" "$SYNC_TIP" ""; then
        stop_node || true
        exit 4
    fi
    if ! SYNC_HASH=$(block_hash_at_height "$SYNC_TIP"); then
        stop_node || true
        exit 4
    fi
    log "Sync baseline: height=$SYNC_TIP hash=$SYNC_HASH"
    if ! stop_node; then
        exit 2
    fi
fi

REINDEX_TIP=""
if [[ "$MODE" == "reindex" || "$MODE" == "both" ]]; then
    if ! start_node "reindex" "$REINDEX_FLAG"; then
        exit 1
    fi
    if ! REINDEX_TIP=$(wait_for_sync "reindex"); then
        log "Reindex phase failed"
        stop_node || true
        exit 3
    fi
    if ! verify_tip "reindex" "$REINDEX_TIP" "$SYNC_TIP" "$SYNC_HASH"; then
        stop_node || true
        exit 4
    fi
    if ! stop_node; then
        exit 3
    fi
fi

log "OK: sync_tip=${SYNC_TIP:-skipped} reindex_tip=${REINDEX_TIP:-skipped}"
exit 0
