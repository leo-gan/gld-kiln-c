#!/usr/bin/env bash
# Install a C toolchain when gcc is not already on PATH.
set -euo pipefail

if command -v gcc >/dev/null 2>&1 && command -v make >/dev/null 2>&1; then
  gcc --version
  exit 0
fi

sudo apt-get update
sudo apt-get install -y --no-install-recommends build-essential ca-certificates
gcc --version
