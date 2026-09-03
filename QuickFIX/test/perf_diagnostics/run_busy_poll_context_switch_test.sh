#!/bin/sh

set -eu

SCRIPT_PATH=$(realpath "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_PATH")
PROJECT_ROOT=$(realpath "$SCRIPT_DIR/../..")
RUNNER="$PROJECT_ROOT/test/run_parse_benchmark.sh"

MESSAGES=${MESSAGES:-200000}
REPETITIONS=${REPETITIONS:-3}
START_PORT=${START_PORT:-55301}
MESSAGE=${MESSAGE:-new-order-single}
VALIDATE=${VALIDATE:-no}
CPUSET=${CPUSET:-}
BUSY_POLL_CPU=${BUSY_POLL_CPU:-}
BUILD_DIR=${BUILD_DIR:-}
RESULTS_ROOT=${RESULTS_ROOT:-$SCRIPT_DIR/results}
RUN_ID=${RUN_ID:-$(date +%Y%m%d-%H%M%S)-busy-poll}
RUN_DIR="$RESULTS_ROOT/$RUN_ID"

if [ ! -x "$RUNNER" ]; then
  echo "benchmark runner is not executable: $RUNNER" >&2
  exit 1
fi

if [ ! -x /usr/bin/time ]; then
  echo "GNU time is required at /usr/bin/time" >&2
  exit 1
fi

case "$VALIDATE" in
  yes|no) ;;
  *) echo "VALIDATE must be yes or no" >&2; exit 1 ;;
esac

for value in "$MESSAGES" "$REPETITIONS" "$START_PORT"; do
  case "$value" in
    ''|*[!0-9]*) echo "MESSAGES, REPETITIONS, and START_PORT must be positive integers" >&2; exit 1 ;;
  esac
  if [ "$value" -eq 0 ]; then
    echo "MESSAGES, REPETITIONS, and START_PORT must be greater than zero" >&2
    exit 1
  fi
done

if [ -n "$BUILD_DIR" ]; then
  CACHE="$PROJECT_ROOT/$BUILD_DIR/CMakeCache.txt"
  if [ ! -f "$CACHE" ]; then
    echo "CMake cache not found: $CACHE" >&2
    exit 1
  fi
  if ! grep -q '^QUICKFIX_BUSY_POLL:BOOL=ON$' "$CACHE"; then
    echo "BUILD_DIR does not have QUICKFIX_BUSY_POLL=ON: $BUILD_DIR" >&2
    exit 1
  fi
fi

mkdir -p "$RUN_DIR"

SUMMARY="$RUN_DIR/summary.tsv"
printf 'label\tbusy_poll\tvalidate\trepetition\tport\tseconds\tmessages_per_second\ttime_user\ttime_system\tcpu_percent\tctx_voluntary\tctx_involuntary\tmax_rss_kib\n' > "$SUMMARY"

{
  echo "run_id=$RUN_ID"
  echo "timestamp=$(date --iso-8601=seconds)"
  echo "project_root=$PROJECT_ROOT"
  echo "messages=$MESSAGES"
  echo "repetitions=$REPETITIONS"
  echo "message=$MESSAGE"
  echo "validate=$VALIDATE"
  echo "start_port=$START_PORT"
  echo "cpuset=${CPUSET:-none}"
  echo "busy_poll_cpu=${BUSY_POLL_CPU:-none}"
  echo "build_dir=${BUILD_DIR:-unknown}"
  echo "uname=$(uname -a)"
  echo "online_cpus=$(getconf _NPROCESSORS_ONLN)"
  sha256sum "$PROJECT_ROOT/lib/fix_parse_benchmark" "$PROJECT_ROOT/lib/libquickfix.so.17.0.0"

  if [ -n "$BUILD_DIR" ]; then
    grep -E '^(CMAKE_BUILD_TYPE|QUICKFIX_(BUSY_POLL|FIXED_LAYOUT_PARSER|SIMD_FIELD_SCAN|SIMD_PATTERN_SCAN|SIMD_STREAM_PARSER)):' \
      "$PROJECT_ROOT/$BUILD_DIR/CMakeCache.txt"
  fi
} > "$RUN_DIR/metadata.txt"

read_value() {
  key=$1
  file=$2
  awk -F= -v key="$key" '$1 == key { print $2; exit }' "$file"
}

run_case() {
  busy_poll=$1
  repetition=$2
  port=$3
  label="busy-${busy_poll}-run-${repetition}"
  benchmark_log="$RUN_DIR/$label.benchmark.txt"
  resource_log="$RUN_DIR/$label.resources.txt"
  command_log="$RUN_DIR/$label.command.txt"

  set -- "$RUNNER" \
    --mode=server \
    --client=quickfix \
    --message="$MESSAGE" \
    --messages="$MESSAGES" \
    --port="$port" \
    --validate="$VALIDATE"

  if [ "$busy_poll" = "on" ]; then
    set -- "$@" --busy-poll
    if [ -n "$BUSY_POLL_CPU" ]; then
      set -- "$@" --busy-poll-cpu="$BUSY_POLL_CPU"
    fi
  else
    set -- "$@" --no-busy-poll
  fi

  printf '%s ' "$@" > "$command_log"
  printf '\n' >> "$command_log"

  if [ -n "$CPUSET" ]; then
    /usr/bin/time \
      -o "$resource_log" \
      -f 'time_user=%U\ntime_system=%S\ncpu_percent=%P\nelapsed_wall=%e\nctx_voluntary=%w\nctx_involuntary=%c\nmax_rss_kib=%M' \
      taskset -c "$CPUSET" "$@" \
      > "$benchmark_log"
  else
    /usr/bin/time \
      -o "$resource_log" \
      -f 'time_user=%U\ntime_system=%S\ncpu_percent=%P\nelapsed_wall=%e\nctx_voluntary=%w\nctx_involuntary=%c\nmax_rss_kib=%M' \
      "$@" \
      > "$benchmark_log"
  fi

  seconds=$(read_value seconds "$benchmark_log")
  messages_per_second=$(read_value messages_per_second "$benchmark_log")
  time_user=$(read_value time_user "$resource_log")
  time_system=$(read_value time_system "$resource_log")
  cpu_percent=$(read_value cpu_percent "$resource_log")
  ctx_voluntary=$(read_value ctx_voluntary "$resource_log")
  ctx_involuntary=$(read_value ctx_involuntary "$resource_log")
  max_rss_kib=$(read_value max_rss_kib "$resource_log")

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$label" "$busy_poll" "$VALIDATE" "$repetition" "$port" "$seconds" "$messages_per_second" \
    "$time_user" "$time_system" "$cpu_percent" "$ctx_voluntary" "$ctx_involuntary" "$max_rss_kib" \
    >> "$SUMMARY"

  echo "$label: seconds=$seconds ctx_voluntary=$ctx_voluntary time_system=$time_system"
}

repetition=1
port=$START_PORT
while [ "$repetition" -le "$REPETITIONS" ]; do
  if [ $((repetition % 2)) -eq 1 ]; then
    run_case off "$repetition" "$port"
    port=$((port + 1))
    run_case on "$repetition" "$port"
  else
    run_case on "$repetition" "$port"
    port=$((port + 1))
    run_case off "$repetition" "$port"
  fi

  port=$((port + 1))
  repetition=$((repetition + 1))
done

echo "results=$RUN_DIR"
echo "summary=$SUMMARY"
