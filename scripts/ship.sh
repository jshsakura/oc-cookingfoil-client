#!/usr/bin/env bash
# Ships a build: raises the version in makefiles/switch.mk and commits it,
# builds, checks the .nro for anything private, puts it where our server
# hands it out, then tags the commit and pushes. The tag (vX.Y.Z) makes
# GitHub Actions build the public release from the same source, with its
# licenses, marked latest (.github/workflows/release.yml).
#
#   scripts/ship.sh            the whole run
#   scripts/ship.sh --server   our server only (a build to try on the console);
#                              the commit stays here and goes with the next run
#   scripts/ship.sh --check    build and scan the current version; nothing
#                              is raised and nothing leaves this machine
#
# Where it goes is this machine's, kept out of the repository in
# scripts/ship.env (see scripts/ship.env.example):
#   COOK_CLIENT_DIR       the server's client folder (cookingfoil.nro goes in)
#   COOK_PRIVATE_PATTERN  more strings the build must not hold (this
#                         machine's domains, names), an extended regex
set -euo pipefail
cd "$(dirname "$0")/.."
mode=all
case "${1:-}" in
  --check) mode=check ;;
  --server) mode=server ;;
  "") ;;
  *) echo "usage: scripts/ship.sh [--server|--check]" >&2; exit 2 ;;
esac

env_file=scripts/ship.env
if [ -f "$env_file" ]; then
  # shellcheck source=/dev/null
  source "$env_file"
fi
[ "$mode" = check ] || : "${COOK_CLIENT_DIR:?set it in $env_file}"

# Only what is committed ships. The engine submodule is always modified in
# place by its patches.
if [ -n "$(git status --porcelain --ignore-submodules=all)" ]; then
  echo "uncommitted changes; commit first" >&2
  git status --short --ignore-submodules=all >&2
  exit 1
fi

# One more than the last build shipped, committed before it is built.
raised=false
if [ "$mode" != check ]; then
  current=$(sed -n 's/^APP_VERSION := //p' makefiles/switch.mk)
  next="${current%.*}.$(( ${current##*.} + 1 ))"
  sed -i "s/^APP_VERSION := .*/APP_VERSION := $next/" makefiles/switch.mk
  git commit -q -m "chore: release $next" makefiles/switch.mk
  raised=true
fi
undo() {
  # A build that does not ship takes its version back.
  if $raised; then git reset -q --keep HEAD~1; fi
}

version=$(make -s version 2>/dev/null | tail -n 1)
echo "== building $version"
# The full output (libnx's headers warn a lot) goes to a log; its end shows
# when the build fails.
log=build/ship.log
mkdir -p build
if ! make -j"$(nproc)" >"$log" 2>&1; then
  tail -n 40 "$log" >&2
  echo "build failed; the whole output is in $log" >&2
  undo
  exit 1
fi
built=$(python3 -c "import sys; d=open('cookingfoil.nacp','rb').read(); print(d[0x3060:0x3070].split(b'\0')[0].decode())")
if [ "$built" != "$version" ]; then
  echo "the build says $built, the commit $version" >&2
  undo
  exit 1
fi

# Nothing of this machine in the public file. Known harmless: devkitPro's
# build paths (/home/davem), curl's sample address, the NACP author.
echo "== scanning cookingfoil.nro"
hits=$(strings -n 6 cookingfoil.nro |
  grep -iE "192\\.168\\.|\\b100\\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\\.|/home/|/srv/|/mnt/|eyJ[A-Za-z0-9_-]{8,}\\.[A-Za-z0-9_-]{8,}\\.${COOK_PRIVATE_PATTERN:+|$COOK_PRIVATE_PATTERN}" |
  grep -v '/home/davem' || true)
if [ -n "$hits" ]; then
  echo "private strings in the build:" >&2
  echo "$hits" >&2
  undo
  exit 1
fi
if [ "$mode" = check ]; then
  echo "== $version built and clean (not shipped)"
  exit 0
fi

# To our server: a temporary name, then renamed, so a console downloading
# right then never gets half a file.
echo "== $COOK_CLIENT_DIR"
sudo cp cookingfoil.nro "$COOK_CLIENT_DIR/.cookingfoil.nro.tmp"
sudo mv "$COOK_CLIENT_DIR/.cookingfoil.nro.tmp" "$COOK_CLIENT_DIR/cookingfoil.nro"
if [ "$mode" = server ]; then
  echo "== $version on our server only"
  exit 0
fi

git tag -a "v$version" -m "CookingFoil $version"
git push -q origin HEAD
git push -q origin "v$version"
echo "== shipped $version; GitHub Actions builds the release from v$version"
