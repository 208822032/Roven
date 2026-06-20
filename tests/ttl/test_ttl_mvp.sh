#!/usr/bin/env bash

# Compatibility wrapper only.
# Real TTL test logic lives in tests/ttl/test_ttl_all_backends.sh.

set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$DIR/test_ttl_all_backends.sh" "$@"
