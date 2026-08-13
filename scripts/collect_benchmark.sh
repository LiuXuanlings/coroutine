#!/usr/bin/env bash

set -euo pipefail

benchmark=${1:-build-cleanup/benchmark_pingpong}
output_dir=${2:-docs/refactor/perf/raw}
warmup=${WARMUP:-2000}
samples=${SAMPLES:-100000}

if [[ ! -x "$benchmark" ]]; then
  echo "benchmark executable not found: $benchmark" >&2
  exit 2
fi

mkdir -p "$output_dir"
stamp=$(date -u +%Y%m%dT%H%M%SZ)
csv="$output_dir/latency_${stamp}.csv"
json="$output_dir/latency_${stamp}.json"
commit=$(git rev-parse HEAD)
compiler=$(${CXX:-c++} --version 2>/dev/null | head -n 1 || true)

"$benchmark" --payload-bytes 64 --warmup "$warmup" --samples "$samples" > "$csv"
for payload_bytes in 1024 65536; do
  "$benchmark" --payload-bytes "$payload_bytes" --warmup "$warmup" \
    --samples "$samples" | tail -n +2 >> "$csv"
done

{
  printf '{\n'
  printf '  "schema": "minicyber_latency_v1",\n'
  printf '  "commit": "%s",\n' "$commit"
  printf '  "timestamp_utc": "%s",\n' "$stamp"
  printf '  "kernel": "%s",\n' "$(uname -sr)"
  printf '  "machine": "%s",\n' "$(uname -m)"
  printf '  "cpu_model": "%s",\n' "$(LC_ALL=C lscpu | awk -F: '/Model name:/ {sub(/^[[:space:]]+/, "", $2); print $2; exit}')"
  printf '  "compiler": "%s",\n' "$compiler"
  printf '  "warmup": %s,\n' "$warmup"
  printf '  "samples": %s,\n' "$samples"
  printf '  "payload_bytes": [64, 1024, 65536],\n'
  printf '  "csv": "%s"\n' "$(basename "$csv")"
  printf '}\n'
} > "$json"

printf 'csv=%s\njson=%s\n' "$csv" "$json"
