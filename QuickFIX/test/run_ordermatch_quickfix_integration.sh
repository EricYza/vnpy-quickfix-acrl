#!/usr/bin/env bash

set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${QUICKFIX_ORDERMATCH_BUILD_DIR:-${repo_dir}/build-direct-stage3}"
server_binary="${build_dir}/out/ordermatch"
client_binary="${build_dir}/out/ordermatch_integration_client"
dictionary="${repo_dir}/spec/FIX42.xml"
base_port="${QUICKFIX_ORDERMATCH_BASE_PORT:-54530}"

if [[ ! -x "${server_binary}" ]]; then
  echo "ordermatch binary not found: ${server_binary}" >&2
  exit 2
fi

if [[ ! -x "${client_binary}" ]]; then
  echo "QuickFIX integration client not found: ${client_binary}" >&2
  exit 2
fi

if [[ "$#" -gt 0 ]]; then
  modes=("$@")
else
  modes=(blocking poll0 direct)
fi

run_mode() {
  local mode="$1"
  local port="$2"
  local temp_dir
  temp_dir="$(mktemp -d "/tmp/quickfix-ordermatch-${mode}-XXXXXX")"
  local config="${temp_dir}/ordermatch.cfg"
  local server_log="${temp_dir}/ordermatch.log"
  local server_input="${temp_dir}/ordermatch.stdin"
  mkdir -p "${temp_dir}/store"
  mkfifo "${server_input}"

  {
    printf '%s\n' \
      "[DEFAULT]" \
      "ConnectionType=acceptor" \
      "SocketAcceptPort=${port}" \
      "SocketReuseAddress=Y" \
      "SocketNodelay=Y" \
      "SocketBusyPollMode=${mode}" \
      "FileStorePath=${temp_dir}/store" \
      "StartTime=00:00:00" \
      "EndTime=00:00:00" \
      "UseDataDictionary=Y" \
      "DataDictionary=${dictionary}" \
      "CheckLatency=N" \
      "ResetOnLogon=Y" \
      "ScreenLogShowIncoming=N" \
      "ScreenLogShowOutgoing=N" \
      "ScreenLogShowEvents=N" \
      "" \
      "[SESSION]" \
      "BeginString=FIX.4.2" \
      "SenderCompID=ORDERMATCH" \
      "TargetCompID=CLIENT1" \
      "" \
      "[SESSION]" \
      "BeginString=FIX.4.2" \
      "SenderCompID=ORDERMATCH" \
      "TargetCompID=CLIENT2"
  } >"${config}"

  exec 3<>"${server_input}"
  "${server_binary}" "${config}" <&3 >"${server_log}" 2>&1 &
  local server_pid=$!

  echo "running mode=${mode} client=quickfix port=${port}"
  set +e
  "${client_binary}" "${port}" "${dictionary}"
  local client_status=$?
  set -e

  if kill -0 "${server_pid}" 2>/dev/null; then
    printf '#quit\n' >&3
  fi
  exec 3>&-

  set +e
  wait "${server_pid}"
  local server_status=$?
  set -e

  if [[ "${client_status}" -ne 0 || "${server_status}" -ne 0 ]]; then
    echo "--- ordermatch ${mode} log ---" >&2
    tail -n 200 "${server_log}" >&2
    echo "mode=${mode} client_status=${client_status} server_status=${server_status}" >&2
    rm -rf -- "${temp_dir}"
    return 1
  fi

  echo "mode=${mode} client=quickfix ordermatch_integration=pass"
  rm -rf -- "${temp_dir}"
}

index=0
for mode in "${modes[@]}"; do
  run_mode "${mode}" "$((base_port + index))"
  index=$((index + 1))
done

echo "ordermatch_quickfix_integration=pass modes=$(IFS=,; echo "${modes[*]}")"
