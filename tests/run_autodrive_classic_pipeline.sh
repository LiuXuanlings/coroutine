#!/usr/bin/env bash
set -euo pipefail

source_dir=$1
build_dir=$2
run_dir="${build_dir}/autodrive-classic-ctest-$(date +%s)-$$"

# DAG 中的 TextFormat 组件配置保持相对源码根目录，和 mainboard 的生产启动
# 语义一致；CTest 默认在构建目录运行，必须先恢复该工作目录而不能改写 DAG。
cd "${source_dir}"

# MC-618 只复用唯一业务入口；每次运行保留独立日志目录，使 CTest 失败时
# 可以按进程追溯 mainboard、Source 和 Sink，而不是新建替代数据管道。
"${source_dir}/scripts/run_autodrive_pipeline.sh" \
  --build-dir "${build_dir}" \
  --output-dir "${run_dir}" \
  --messages 1000 \
  --frequency 100 \
  --metrics

metrics_file="${run_dir}/metrics.txt"
mainboard_log="${run_dir}/mainboard.log"
for required_file in "${metrics_file}" "${mainboard_log}" \
                     "${run_dir}/sensor_source.log" \
                     "${run_dir}/control_sink.log"; do
  [[ -e "${required_file}" ]] || {
    echo "Missing pipeline evidence: ${required_file}" >&2
    exit 1
  }
done
[[ -s "${metrics_file}" && -s "${mainboard_log}" && -s "${run_dir}/sensor_source.log" ]] || {
  echo "Missing non-empty pipeline result evidence in ${run_dir}" >&2
  exit 1
}

# mainboard 必须在真实 DAG 中 dlopen 唯一组件库；任一组件遗漏都表示业务主干
# 被静态注册或不完整加载绕过，不能以 Sink 收到消息替代此项验收。
grep -Fq 'Loading library: libminicyber_autodrive_components.so' "${mainboard_log}"
for component in PerceptionComponent FusionComponent PlanningComponent \
                 ControlComponent ControlAuditComponent; do
  grep -Fq "Component loaded: ${component}" "${mainboard_log}"
done

# ControlSink 只有在两路 SHM 结果完整、序列单调且审计集合相等时才会令脚本成功；
# 此处固定验收记录字段，防止后续放宽脚本退出语义而让 CTest 失去业务断言。
grep -Eq '^METRICS received=1000 audit_received=1000 lost=0 duplicates=0 audit_duplicates=0 out_of_order=0 audit_difference=0 ' "${metrics_file}"

echo "Classic pipeline evidence: ${run_dir}"
