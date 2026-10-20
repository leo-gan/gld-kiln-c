#!/usr/bin/env bash
# C ships as a GitHub release. This script does not upload to a package registry.
set -euo pipefail

echo "C ships as a GitHub release."
if [[ -z "${KILN_REGISTRY_TOKEN:-}" ]]; then
  echo "package registry secret is missing"
fi
exit 0
