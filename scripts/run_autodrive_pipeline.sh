#!/usr/bin/env bash
set -euo pipefail

source_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir="${source_dir}/build/debug"
scheduler_path="${source_dir}/config/autodrive/classic_sched.conf"
output_dir="${source_dir}/out/autodrive"
messages=1000
frequency=100
timeout_ms=30000
metrics_enabled=1

usage() {
  echo "Usage: $0 [--build-dir <path>] [--scheduler <path>] [--output-dir <path>] [--messages <count>] [--frequency <hz>] [--timeout-ms <ms>] [--metrics|--no-metrics]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --metrics)
      metrics_enabled=1
      shift
      ;;
    --no-metrics)
      metrics_enabled=0
      shift
      ;;
    --build-dir|--scheduler|--output-dir|--messages|--frequency|--timeout-ms)
      [[ $# -ge 2 ]] || { usage; exit 1; }
      case "$1" in
        --build-dir) build_dir=$2 ;;
        --scheduler) scheduler_path=$2 ;;
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

mainboard="${build_dir}/bin/mainboard"
sensor_source="${build_dir}/bin/sensor_source"
control_sink="${build_dir}/bin/control_sink"
dag_path="${source_dir}/config/autodrive/autodrive.dag"
for executable in "$mainboard" "$sensor_source" "$control_sink"; do
  [[ -x "$executable" ]] || { echo "Missing executable: $executable" >&2; exit 1; }
done
[[ -f "$scheduler_path" ]] || { echo "Missing Scheduler config: $scheduler_path" >&2; exit 1; }

mkdir -p "$output_dir"
export LD_LIBRARY_PATH="${build_dir}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

sink_pid=""
mainboard_pid=""
source_pid=""
ready_file="${output_dir}/control_sink.ready"
mainboard_evidence="${output_dir}/mainboard_evidence.txt"
sink_evidence="${output_dir}/control_sink_evidence.txt"
cleanup() {
  local status=$?
  trap - EXIT INT TERM
  # 正常结束和中断都先给每个进程 SIGTERM；各入口自行反转 Node、SHM 与
  # Discovery 的所有权顺序，避免脚本猜测或删除不属于本次运行的共享内存。
  for pid in "$source_pid" "$mainboard_pid" "$sink_pid"; do
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
    fi
  done
  for pid in "$source_pid" "$mainboard_pid" "$sink_pid"; do
    if [[ -n "$pid" ]]; then
      wait "$pid" 2>/dev/null || true
    fi
  done
  # 只有失败或中断路径使用精确名称兜底清理；成功路径必须由
  # PosixSegment 引用归零自然回收，否则无法验证生命周期语义。
  if (( status != 0 )); then
    if ! "$control_sink" --cleanup-shm; then
      echo "Failed to clean pipeline SHM after status ${status}." >&2
    fi
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM

rm -f "$ready_file" "${output_dir}/metrics.txt"
sink_args=(--messages "$messages" --timeout-ms "$timeout_ms" --ready-file "$ready_file" --evidence-file "$sink_evidence")
if (( metrics_enabled != 0 )); then
  sink_args+=(--metrics --output "${output_dir}/metrics.txt")
fi
"$control_sink" "${sink_args[@]}" >"${output_dir}/control_sink.log" 2>&1 &
sink_pid=$!

MINICYBER_AUTODRIVE_EVIDENCE_FILE="$mainboard_evidence" \
  "$mainboard" -d "$dag_path" -s "$scheduler_path" \
  >"${output_dir}/mainboard.log" 2>&1 &
mainboard_pid=$!

# 等待 Sink 观察到 mainboard 的 ControlCommand Writer Join。轮询只检查这一
# 有界业务条件，不能以固定 sleep 猜测三进程发现是否已经收敛。
ready_deadline=$((SECONDS + (timeout_ms + 999) / 1000))
while [[ ! -f "$ready_file" ]]; do
  if ! kill -0 "$sink_pid" 2>/dev/null || (( SECONDS >= ready_deadline )); then
    echo "ControlSink did not observe ControlCommand writer readiness." >&2
    exit 1
  fi
  sleep 0.01
done

# Source 在进程内继续等待两个远端输入 Reader Join；结合上方 Sink 的下游 Join
# 事实，使放行覆盖完整拓扑而不复制业务 DAG 或引入经验等待。
"$sensor_source" --messages "$messages" --frequency "$frequency" \
  --ready-timeout-ms "$timeout_ms" --await-shutdown \
  >"${output_dir}/sensor_source.log" 2>&1 &
source_pid=$!

wait "$sink_pid"
sink_pid=""

kill -TERM "$source_pid"
wait "$source_pid"
source_pid=""

kill -TERM "$mainboard_pid"
wait "$mainboard_pid"
mainboard_pid=""

# 正常退出后先检查四个精确频道已自然回收；检查失败会保留非零
# 退出码，再由 trap 做环境恢复，不能把兜底 unlink 冒充为验收证据。
"$control_sink" --check-shm-clean
if (( metrics_enabled != 0 )); then
  echo "Autodrive pipeline completed: ${output_dir}/metrics.txt"
else
  echo "Autodrive pipeline completed with metrics disabled."
fi
