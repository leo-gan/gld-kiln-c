#!/usr/bin/env bash
# Wire cases, generated structs, and the Python oracle.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${root}"
make test
echo "all test files passed"
