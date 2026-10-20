#!/usr/bin/env bash
# Generate testdata/schema/*.json into tests/generated/.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${root}"
make generate
