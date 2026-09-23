#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

manifest=${1:-build/arm64-debug/moss-artifacts.json}
runs=${2:-100}
output=${3:-build/arm64-debug/containers-smp-replay-$(date +%s)}

if [[ ! $runs =~ ^[1-9][0-9]*$ ]]; then
  echo "runs must be a positive integer" >&2
  exit 2
fi

mkdir -p "$output"
for ((run = 1; run <= runs; ++run)); do
  printf -v label '%04d' "$run"
  echo "containers.smp replay $run/$runs: $output/$label"
  uv run python scripts/kernel_validation.py run \
    --manifest "$manifest" --workload containers.smp --output "$output/$label"
  # A concurrent rebuild must not turn one repeat set into mixed-image evidence.
  hashes=$(uv run python -c 'import json, sys; p = json.load(open(sys.argv[1]))["provenance"]; print(*(p.get(k) for k in ("image_sha256", "fixture_sha256", "symbols_sha256")))' "$output/$label/results.json")
  if ((run == 1)); then
    baseline=$hashes
  elif [[ $hashes != "$baseline" ]]; then
    echo "validation artifacts changed during replay" >&2
    exit 1
  fi
done
