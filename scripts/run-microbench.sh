#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${root}"
make build/kiln-bench
bench="${root}/build/kiln-bench"
if command -v taskset >/dev/null 2>&1; then
  bench=(taskset -c 1 "${bench}")
else
  bench=("${bench}")
fi
for _ in 1 2 3 4 5; do
  "${bench[@]}"
done
