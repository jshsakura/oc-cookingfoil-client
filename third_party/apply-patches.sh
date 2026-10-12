#!/usr/bin/env bash
# Brings the CyberFoil submodule to "pinned commit + every patch in order".
# Patches can touch the same lines, so instead of checking each one alone the
# whole series is fingerprinted: when the fingerprint changes, the submodule
# is reset to its pinned commit and the series applied from the start.
set -euo pipefail
cd "$(dirname "$0")/CyberFoil"
stamp=.cookingfoil-patches
want=$(cat ../patches/*.patch | sha256sum | cut -d' ' -f1)
if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$want" ]; then
  exit 0
fi
git checkout -- .
for patch in ../patches/*.patch; do
  git apply --check "$patch"
  git apply "$patch"
done
echo "$want" > "$stamp"
