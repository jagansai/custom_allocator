#!/usr/bin/env bash
set -euo pipefail

# Number of runs (default 10, override with first arg)
RUNS=${1:-10}

LOG_DIR="./logs/allocator"
RUN_LOG="$LOG_DIR/run.log"

mkdir -p "$LOG_DIR"
: > "$RUN_LOG"

# Arrays to collect per-run stats
runs=()
messages=()
first_sent=()
arena_time=()
heap_time=()
pool_time=()
winner=()

has_pool=false

for ((i=1; i<=RUNS; ++i)); do
  echo "Run #$i"

  ./build-release/fix_allocator_demo \
    --mode server \
    --server-config ./config/app.properties \
    --output-dir "$LOG_DIR" &
  server_pid=$!

  echo "Waiting for server to start..."
  sleep 3

  if ! kill -0 "$server_pid" 2>/dev/null; then
    echo "Server process not running; aborting run $i" >&2
    exit 1
  fi

  echo "Server started and sending messages"

  # Capture sender output while also appending to run.log
  output=$(python ./scripts/send_fix_messages.py \
    --config ./config/app.properties \
    --file ./data/fix_messages.txt \
    --log-dir "$LOG_DIR" 2>&1 | tee -a "$RUN_LOG")

  # Parse which allocator was sent first
  first_line=$(printf '%s\n' "$output" | grep -E '^Connecting to .*session at' | head -n 1 || true)
  if echo "$first_line" | grep -qi 'arena session'; then
    first="Arena"
  elif echo "$first_line" | grep -qi 'heap session'; then
    first="Heap"
  elif echo "$first_line" | grep -qi 'pool session'; then
    first="Pool"
  else
    first="Unknown"
  fi

  # Parse Arena/Heap/Pool timing lines
  arena_line=$(printf '%s\n' "$output" | grep -E '^Arena:' | tail -n 1 || true)
  heap_line=$(printf '%s\n' "$output" | grep -E '^Heap:'  | tail -n 1 || true)
  pool_line=$(printf '%s\n' "$output" | grep -E '^Pool:'  | tail -n 1 || true)

  a_msgs=""; a_time=""; h_msgs=""; h_time=""; p_msgs=""; p_time=""

  if [[ -n "$arena_line" ]]; then
    if [[ "$arena_line" =~ Arena:[[:space:]]+([0-9]+)[[:space:]]+messages[[:space:]]+over[[:space:]]+([0-9.]+)[[:space:]]+seconds ]]; then
      a_msgs="${BASH_REMATCH[1]}"
      a_time="${BASH_REMATCH[2]}"
    fi
  fi

  if [[ -n "$heap_line" ]]; then
    if [[ "$heap_line" =~ Heap:[[:space:]]+([0-9]+)[[:space:]]+messages[[:space:]]+over[[:space:]]+([0-9.]+)[[:space:]]+seconds ]]; then
      h_msgs="${BASH_REMATCH[1]}"
      h_time="${BASH_REMATCH[2]}"
    fi
  fi

  if [[ -n "$pool_line" ]]; then
    if [[ "$pool_line" =~ Pool:[[:space:]]+([0-9]+)[[:space:]]+messages[[:space:]]+over[[:space:]]+([0-9.]+)[[:space:]]+seconds ]]; then
      p_msgs="${BASH_REMATCH[1]}"
      p_time="${BASH_REMATCH[2]}"
    fi
  fi

  if [[ -n "$a_time" && -n "$h_time" ]]; then
    # Decide winner
    if [[ -n "$p_time" ]]; then
      # Arena vs Heap vs Pool
      awk_out=$(awk -v a="$a_time" -v h="$h_time" -v p="$p_time" 'BEGIN {
        if (a < h && a < p) print "Arena";
        else if (h < a && h < p) print "Heap";
        else if (p < a && p < h) print "Pool";
        else print "Tie";
      }')
      win="$awk_out"
      has_pool=true
    else
      # Arena vs Heap only
      awk_out=$(awk -v a="$a_time" -v h="$h_time" 'BEGIN { if (a < h) print "Arena"; else if (h < a) print "Heap"; else print "Tie"; }')
      win="$awk_out"
    fi

    msg_count="$a_msgs"
    if [[ -z "$msg_count" ]]; then
      msg_count="$h_msgs"
    fi

    runs+=("$i")
    messages+=("$msg_count")
    first_sent+=("$first")
    arena_time+=("$a_time")
    heap_time+=("$h_time")
    pool_time+=("$p_time")
    winner+=("$win")
  fi

  # Stop server
  kill "$server_pid" 2>/dev/null || true
  wait "$server_pid" 2>/dev/null || true
  echo "Server stopped."

done

if (( ${#runs[@]} > 0 )); then
  printf "\nSummary (per run):\n"
  if [[ "$has_pool" == true ]]; then
    printf "%-4s %-8s %-10s %-11s %-11s %-11s %-6s\n" "Run" "Messages" "FirstSent" "ArenaTimeS" "HeapTimeS" "PoolTimeS" "Winner"
    for idx in "${!runs[@]}"; do
      printf "%-4s %-8s %-10s %-11s %-11s %-11s %-6s\n" \
        "${runs[$idx]}" "${messages[$idx]}" "${first_sent[$idx]}" "${arena_time[$idx]}" "${heap_time[$idx]}" "${pool_time[$idx]}" "${winner[$idx]}"
    done
  else
    printf "%-4s %-8s %-10s %-11s %-11s %-6s\n" "Run" "Messages" "FirstSent" "ArenaTimeS" "HeapTimeS" "Winner"
    for idx in "${!runs[@]}"; do
      printf "%-4s %-8s %-10s %-11s %-11s %-6s\n" \
        "${runs[$idx]}" "${messages[$idx]}" "${first_sent[$idx]}" "${arena_time[$idx]}" "${heap_time[$idx]}" "${winner[$idx]}"
    done
  fi
fi
