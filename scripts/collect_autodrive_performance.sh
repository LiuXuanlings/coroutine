#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="${source_dir}/build/release"
output_dir="${source_dir}/docs/perf"
messages=1000
frequency=100
timeout_ms=30000

usage() {
  echo "Usage: $0 [--build-dir <path>] [--output-dir <path>] [--messages <count>] [--frequency <hz>] [--timeout-ms <ms>]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir|--output-dir|--messages|--frequency|--timeout-ms)
      [[ $# -ge 2 ]] || { usage; exit 1; }
      case "$1" in
        --build-dir) build_dir=$2 ;;
        --output-dir) output_dir=$2 ;;
        --messages) messages=$2 ;;
        --frequency) frequency=$2 ;;
        --timeout-ms) timeout_ms=$2 ;;
      esac
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      exit 1
      ;;
  esac
done

[[ -f "${build_dir}/CMakeCache.txt" ]] || { echo "Missing build cache: ${build_dir}" >&2; exit 1; }
grep -qx 'CMAKE_BUILD_TYPE:STRING=Release' "${build_dir}/CMakeCache.txt" || {
  echo "Performance collection requires a Release build." >&2
  exit 1
}
runner="${source_dir}/scripts/run_autodrive_pipeline.sh"
sink="${build_dir}/bin/control_sink"
[[ -x "${runner}" && -x "${sink}" ]] || { echo "Missing pipeline executable or launcher." >&2; exit 1; }

mkdir -p "${output_dir}"
commit=$(git -C "${source_dir}" rev-parse HEAD)
worktree_status=clean
if [[ -n $(git -C "${source_dir}" status --porcelain=v1) ]]; then
  worktree_status=dirty
fi
cpu=$(lscpu | awk -F: '/Model name/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}')
kernel=$(uname -sr)
compiler=$(c++ --version | head -1)
protobuf=$(protoc --version)
fastrtps=$(dpkg-query -W -f='${Version}' libfastrtps-dev 2>/dev/null || printf 'unknown')
system_load=$(cut -d' ' -f1-3 /proc/loadavg)
dag="${source_dir}/config/autodrive/autodrive.dag"
dag_hash=$(sha256sum "${dag}" | awk '{print $1}')

json_escape() {
  local value=$1
  value=${value//\\/\\\\}
  value=${value//\"/\\\"}
  printf '%s' "${value}"
}

metric_value() {
  local line=$1
  local key=$2
  awk -v key="${key}" '{for (i = 1; i <= NF; ++i) { split($i, pair, "="); if (pair[1] == key) { print pair[2]; exit } }}' <<<"${line}"
}

collect_policy() {
  local policy=$1
  local scheduler=$2
  local run_dir
  run_dir=$(mktemp -d "${TMPDIR:-/tmp}/minicyber-perf-${policy}.XXXXXX")
  # 失败时保留进程日志以满足跨进程验收的复现要求；只有 JSON/CSV 写入后才删除
  # 这个精确 mktemp 目录，不能使用通配符清理其他性能现场。
  printf 'Performance run directory: %s\n' "${run_dir}"

  "${sink}" --check-shm-clean
  "${runner}" --build-dir "${build_dir}" --scheduler "${scheduler}" \
    --output-dir "${run_dir}" --messages "${messages}" --frequency "${frequency}" \
    --timeout-ms "${timeout_ms}" --metrics
  "${sink}" --check-shm-clean

  local metrics_line
  metrics_line=$(<"${run_dir}/metrics.txt")
  [[ ${metrics_line} == METRICS\ * ]] || { echo "Missing metrics output for ${policy}." >&2; exit 1; }
  local received audit_received lost duplicates audit_difference out_of_order p50 p95 p99 throughput duration
  received=$(metric_value "${metrics_line}" received)
  audit_received=$(metric_value "${metrics_line}" audit_received)
  lost=$(metric_value "${metrics_line}" lost)
  duplicates=$(metric_value "${metrics_line}" duplicates)
  audit_difference=$(metric_value "${metrics_line}" audit_difference)
  out_of_order=$(metric_value "${metrics_line}" out_of_order)
  p50=$(metric_value "${metrics_line}" latency_ns_p50)
  p95=$(metric_value "${metrics_line}" latency_ns_p95)
  p99=$(metric_value "${metrics_line}" latency_ns_p99)
  throughput=$(metric_value "${metrics_line}" throughput_mps)
  duration=$(metric_value "${metrics_line}" duration_ns)
  for value in "${received}" "${audit_received}" "${lost}" "${duplicates}" \
               "${audit_difference}" "${out_of_order}" "${p50}" "${p95}" "${p99}" \
               "${throughput}" "${duration}"; do
    [[ -n ${value} ]] || { echo "Incomplete metrics output for ${policy}." >&2; exit 1; }
  done
  [[ ${received} == "${messages}" && ${audit_received} == "${messages}" && ${lost} == 0 && \
     ${duplicates} == 0 && ${audit_difference} == 0 && ${out_of_order} == 0 ]] || {
    echo "Invalid performance measurement set for ${policy}: ${metrics_line}" >&2
    exit 1
  }

  local scheduler_hash csv json
  scheduler_hash=$(sha256sum "${scheduler}" | awk '{print $1}')
  csv="${output_dir}/autodrive_${policy}.csv"
  json="${output_dir}/autodrive_${policy}.json"
  printf '%s\n' 'schema,commit,policy,messages,frequency_hz,samples,p50_ns,p95_ns,p99_ns,throughput_msg_s,lost,duplicates,duration_ns,audit_difference,out_of_order' >"${csv}"
  printf 'minicyber_autodrive_v1,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "${commit}" "${policy}" "${messages}" "${frequency}" "${received}" "${p50}" "${p95}" \
    "${p99}" "${throughput}" "${lost}" "${duplicates}" "${duration}" "${audit_difference}" \
    "${out_of_order}" >>"${csv}"
  printf '{\n' >"${json}"
  printf '  "schema": "minicyber_autodrive_v1",\n' >>"${json}"
  printf '  "commit": "%s",\n' "$(json_escape "${commit}")" >>"${json}"
  printf '  "worktree_status": "%s",\n' "${worktree_status}" >>"${json}"
  printf '  "policy": "%s",\n' "${policy}" >>"${json}"
  printf '  "build_type": "Release",\n' >>"${json}"
  printf '  "cpu": "%s",\n' "$(json_escape "${cpu}")" >>"${json}"
  printf '  "kernel": "%s",\n' "$(json_escape "${kernel}")" >>"${json}"
  printf '  "compiler": "%s",\n' "$(json_escape "${compiler}")" >>"${json}"
  printf '  "fastrtps_version": "%s",\n' "$(json_escape "${fastrtps}")" >>"${json}"
  printf '  "protobuf_version": "%s",\n' "$(json_escape "${protobuf}")" >>"${json}"
  printf '  "system_load": "%s",\n' "${system_load}" >>"${json}"
  printf '  "dag": "config/autodrive/autodrive.dag",\n' >>"${json}"
  printf '  "dag_sha256": "%s",\n' "${dag_hash}" >>"${json}"
  printf '  "scheduler": "%s",\n' "$(json_escape "${scheduler#"${source_dir}"/}")" >>"${json}"
  printf '  "scheduler_sha256": "%s",\n' "${scheduler_hash}" >>"${json}"
  printf '  "arguments": "--messages %s --frequency %s --timeout-ms %s --metrics",\n' \
    "${messages}" "${frequency}" "${timeout_ms}" >>"${json}"
  printf '  "samples": %s,\n' "${received}" >>"${json}"
  printf '  "p50_ns": %s,\n' "${p50}" >>"${json}"
  printf '  "p95_ns": %s,\n' "${p95}" >>"${json}"
  printf '  "p99_ns": %s,\n' "${p99}" >>"${json}"
  printf '  "throughput_msg_s": %s,\n' "${throughput}" >>"${json}"
  printf '  "lost": %s,\n' "${lost}" >>"${json}"
  printf '  "duplicates": %s,\n' "${duplicates}" >>"${json}"
  printf '  "out_of_order": %s,\n' "${out_of_order}" >>"${json}"
  printf '  "audit_difference": %s,\n' "${audit_difference}" >>"${json}"
  printf '  "duration_ns": %s,\n' "${duration}" >>"${json}"
  printf '  "shm_reclaimed_before_fallback": true\n' >>"${json}"
  printf '}\n' >>"${json}"
  rm -rf "${run_dir}"
  printf 'Collected %s: %s\n' "${policy}" "${csv}"
}

collect_policy classic "${source_dir}/config/autodrive/classic_sched.conf"
collect_policy choreography "${source_dir}/config/autodrive/choreo_sched.conf"
