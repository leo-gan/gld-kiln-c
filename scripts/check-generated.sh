#!/usr/bin/env bash
# Regenerate into a temp directory and diff against tests/generated.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "${root}"
make build/gld-kilngen-c
tmp="$(mktemp -d)"
trap 'rm -rf "${tmp}"' EXIT
shopt -s nullglob
for schema in testdata/schema/*.json; do
  ./build/gld-kilngen-c --schema "${schema}" --out "${tmp}"
done
diff -ru tests/generated "${tmp}"
