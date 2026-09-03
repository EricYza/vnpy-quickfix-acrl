#!/bin/sh

set -eu

SCRIPT_PATH=$(realpath "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
PROJECT_ROOT=$(realpath "$SCRIPT_DIR/../..")
RUNNER="$PROJECT_ROOT/test/run_parse_benchmark.sh"

MESSAGES=${MESSAGES:-100000}
REPETITIONS=${REPETITIONS:-3}
START_PORT=${START_PORT:-55411}
MESSAGE=${MESSAGE:-new-order-single}
BUILD_DIR=${BUILD_DIR:-build-bench-busy-poll}
RESULTS_ROOT=${RESULTS_ROOT:-$SCRIPT_DIR/results}
RUN_ID=${RUN_ID:-$(date +%Y%m%d-%H%M%S)-acceptor-network}
RUN_DIR="$RESULTS_ROOT/$RUN_ID"
PORT=$START_PORT

if [ ! -x "$RUNNER" ]; then
  echo "benchmark runner is not executable: $RUNNER" >&2
  exit 1
fi

if [ ! -x /usr/bin/time ]; then
  echo "GNU time is required at /usr/bin/time" >&2
  exit 1
fi

for value in "$MESSAGES" "$REPETITIONS" "$START_PORT"; do
  case "$value" in
    ''|*[!0-9]*) echo "MESSAGES, REPETITIONS, and START_PORT must be positive integers" >&2; exit 1 ;;
  esac
  if [ "$value" -eq 0 ]; then
    echo "MESSAGES, REPETITIONS, and START_PORT must be greater than zero" >&2
    exit 1
  fi
done

CACHE="$PROJECT_ROOT/$BUILD_DIR/CMakeCache.txt"
if [ ! -f "$CACHE" ]; then
  echo "CMake cache not found: $CACHE" >&2
  exit 1
fi
if ! grep -q '^QUICKFIX_NETWORK_DIAGNOSTICS:BOOL=ON$' "$CACHE"; then
  echo "BUILD_DIR does not have QUICKFIX_NETWORK_DIAGNOSTICS=ON: $BUILD_DIR" >&2
  exit 1
fi
if ! grep -q '^QUICKFIX_BUSY_POLL:BOOL=ON$' "$CACHE"; then
  echo "BUILD_DIR does not have QUICKFIX_BUSY_POLL=ON: $BUILD_DIR" >&2
  exit 1
fi

mkdir -p "$RUN_DIR"

SUMMARY="$RUN_DIR/summary.tsv"
printf 'state\trepetition\tseconds\tmessages_per_second\tpoll_calls\tpoll_wait_nanoseconds\tpoll_immediate_returns\tpoll_blocking_returns\tpoll_context_sample_failures\trecv_calls\trecv_bytes\tparsed_messages\tmessages_per_recv\taverage_bytes_per_recv\taverage_poll_wait_nanoseconds\ttime_user\ttime_system\tcpu_percent\tctx_voluntary\tctx_involuntary\n' > "$SUMMARY"

{
  echo "run_id=$RUN_ID"
  echo "timestamp=$(date --iso-8601=seconds)"
  echo "project_root=$PROJECT_ROOT"
  echo "messages=$MESSAGES"
  echo "repetitions=$REPETITIONS"
  echo "message=$MESSAGE"
  echo "start_port=$START_PORT"
  echo "build_dir=$BUILD_DIR"
  echo "uname=$(uname -a)"
  echo "online_cpus=$(getconf _NPROCESSORS_ONLN)"
  sha256sum "$PROJECT_ROOT/lib/fix_parse_benchmark" "$PROJECT_ROOT/lib/libquickfix.so.17.0.0"
  grep -E '^(CMAKE_BUILD_TYPE|QUICKFIX_(BUSY_POLL|NETWORK_DIAGNOSTICS|FIXED_LAYOUT_PARSER|SIMD_FIELD_SCAN|SIMD_PATTERN_SCAN|SIMD_STREAM_PARSER)):' "$CACHE"
} > "$RUN_DIR/metadata.txt"

read_value() {
  key=$1
  file=$2
  awk -F= -v key="$key" '$1 == key { print $2; exit }' "$file"
}

run_case() {
  state=$1
  repetition=$2

  case "$state" in
    no-block)
      validate=no
      busy_poll=no
      ;;
    yes-block)
      validate=yes
      busy_poll=no
      ;;
    no-busy)
      validate=no
      busy_poll=yes
      ;;
    *)
      echo "unknown diagnostic state: $state" >&2
      exit 1
      ;;
  esac

  label="$state-run-$repetition"
  benchmark_log="$RUN_DIR/$label.benchmark.txt"
  resource_log="$RUN_DIR/$label.resources.txt"
  command_log="$RUN_DIR/$label.command.txt"

  set -- "$RUNNER" \
    --mode=server \
    --client=quickfix \
    --message="$MESSAGE" \
    --messages="$MESSAGES" \
    --port="$PORT" \
    --validate="$validate" \
    --network-diagnostics

  if [ "$busy_poll" = "yes" ]; then
    set -- "$@" --busy-poll
  else
    set -- "$@" --no-busy-poll
  fi

  printf '%s ' "$@" > "$command_log"
  printf '\n' >> "$command_log"

  /usr/bin/time \
    -o "$resource_log" \
    -f 'time_user=%U\ntime_system=%S\ncpu_percent=%P\nelapsed_wall=%e\nctx_voluntary=%w\nctx_involuntary=%c\nmax_rss_kib=%M' \
    "$@" \
    > "$benchmark_log"

  seconds=$(read_value seconds "$benchmark_log")
  messages_per_second=$(read_value messages_per_second "$benchmark_log")
  poll_calls=$(read_value poll_calls "$benchmark_log")
  poll_wait_nanoseconds=$(read_value poll_wait_nanoseconds "$benchmark_log")
  poll_immediate_returns=$(read_value poll_immediate_returns "$benchmark_log")
  poll_blocking_returns=$(read_value poll_blocking_returns "$benchmark_log")
  poll_context_sample_failures=$(read_value poll_context_sample_failures "$benchmark_log")
  recv_calls=$(read_value recv_calls "$benchmark_log")
  recv_bytes=$(read_value recv_bytes "$benchmark_log")
  parsed_messages=$(read_value parsed_messages "$benchmark_log")
  messages_per_recv=$(read_value messages_per_recv "$benchmark_log")
  average_bytes_per_recv=$(read_value average_bytes_per_recv "$benchmark_log")
  average_poll_wait_nanoseconds=$(read_value average_poll_wait_nanoseconds "$benchmark_log")
  time_user=$(read_value time_user "$resource_log")
  time_system=$(read_value time_system "$resource_log")
  cpu_percent=$(read_value cpu_percent "$resource_log")
  ctx_voluntary=$(read_value ctx_voluntary "$resource_log")
  ctx_involuntary=$(read_value ctx_involuntary "$resource_log")

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$state" "$repetition" "$seconds" "$messages_per_second" "$poll_calls" "$poll_wait_nanoseconds" \
    "$poll_immediate_returns" "$poll_blocking_returns" "$poll_context_sample_failures" "$recv_calls" \
    "$recv_bytes" "$parsed_messages" "$messages_per_recv" "$average_bytes_per_recv" \
    "$average_poll_wait_nanoseconds" "$time_user" "$time_system" "$cpu_percent" "$ctx_voluntary" \
    "$ctx_involuntary" >> "$SUMMARY"

  echo "$label: seconds=$seconds blocking_polls=$poll_blocking_returns recv_calls=$recv_calls messages_per_recv=$messages_per_recv"
  PORT=$((PORT + 1))
}

repetition=1
while [ "$repetition" -le "$REPETITIONS" ]; do
  case $((repetition % 3)) in
    1)
      run_case no-block "$repetition"
      run_case yes-block "$repetition"
      run_case no-busy "$repetition"
      ;;
    2)
      run_case no-busy "$repetition"
      run_case no-block "$repetition"
      run_case yes-block "$repetition"
      ;;
    0)
      run_case yes-block "$repetition"
      run_case no-busy "$repetition"
      run_case no-block "$repetition"
      ;;
  esac
  repetition=$((repetition + 1))
done

echo "results=$RUN_DIR"
echo "summary=$SUMMARY"
