#!/usr/bin/env bash
# Ships the committed build: builds it, checks the .nro for anything
# private, puts it where the server hands it out, publishes it on the
# GitHub release, then pushes the commits.
#
#   scripts/ship.sh            the whole run
#   scripts/ship.sh --check    build and scan only; nothing leaves this machine
#
# Where it goes is this machine's, kept out of the repository in
# scripts/ship.env (see scripts/ship.env.example):
#   COOK_CLIENT_DIR   the server's client folder (cookingfoil.nro goes in)
#   COOK_RELEASE_REPO the GitHub repository with the client-latest release
set -euo pipefail
cd "$(dirname "$0")/.."
check_only=false
[ "${1:-}" = "--check" ] && check_only=true

env_file=scripts/ship.env
if [ -f "$env_file" ]; then
  # shellcheck source=/dev/null
  source "$env_file"
fi
if ! $check_only; then
  : "${COOK_CLIENT_DIR:?set it in $env_file}"
  : "${COOK_RELEASE_REPO:?set it in $env_file}"
fi

# Only what is committed ships: the version comes from the commit count.
# The engine submodule is always modified in place by its patches.
if [ -n "$(git status --porcelain --ignore-submodules=all)" ]; then
  echo "uncommitted changes; commit first" >&2
  git status --short --ignore-submodules=all >&2
  exit 1
fi

version=$(make -s version 2>/dev/null | tail -n 1)
echo "== building $version"
# The full output (libnx's headers warn a lot) goes to a log; its end shows
# when the build fails.
log=build/ship.log
mkdir -p build
if ! make -j"$(nproc)" >"$log" 2>&1; then
  tail -n 40 "$log" >&2
  echo "build failed; the whole output is in $log" >&2
  exit 1
fi
built=$(python3 -c "import sys; d=open('cookingfoil.nacp','rb').read(); print(d[0x3060:0x3070].split(b'\0')[0].decode())")
if [ "$built" != "$version" ]; then
  echo "the build says $built, the commit $version" >&2
  exit 1
fi

# Nothing of this machine in the public file. Known harmless: devkitPro's
# build paths (/home/davem), curl's sample address, the NACP author.
echo "== scanning cookingfoil.nro"
hits=$(strings -n 6 cookingfoil.nro |
  grep -iE '192\.168\.|example\.com|jshsakura|gmail|/home/|tinfoil\.|eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.' |
  grep -v '/home/davem' || true)
if [ -n "$hits" ]; then
  echo "private strings in the build:" >&2
  echo "$hits" >&2
  exit 1
fi
if $check_only; then
  echo "== $version built and clean (not shipped)"
  exit 0
fi

# To the server: a temporary name, then renamed, so a console downloading
# right then never gets half a file.
echo "== $COOK_CLIENT_DIR"
sudo cp cookingfoil.nro "$COOK_CLIENT_DIR/.cookingfoil.nro.tmp"
sudo mv "$COOK_CLIENT_DIR/.cookingfoil.nro.tmp" "$COOK_CLIENT_DIR/cookingfoil.nro"

echo "== $COOK_RELEASE_REPO client-latest"
gh release upload client-latest cookingfoil.nro -R "$COOK_RELEASE_REPO" --clobber
notes=$(mktemp)
trap 'rm -f "$notes"' EXIT
gh release view client-latest -R "$COOK_RELEASE_REPO" --json body -q .body |
  sed -E "s/지금 버전: [0-9]+\.[0-9]+\.[0-9]+/지금 버전: $version/" > "$notes"
grep -q "지금 버전: $version" "$notes" || { echo "the notes name no version to bump" >&2; exit 1; }
gh release edit client-latest -R "$COOK_RELEASE_REPO" --notes-file "$notes" >/dev/null

git push -q origin HEAD
echo "== shipped $version"
