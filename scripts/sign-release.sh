#!/usr/bin/env bash
# Signs a release with the release key (Ed25519) for the app's update check
# (core/ClientUpdate: signedMessage). Writes <nro>.sig.json beside it:
#   {"version":…,"size":…,"sha256":…,"signature":<128 hex>}
# The server passes "signature" on in /api/client/latest when its own
# version, size and sha256 are these.
#
#   scripts/sign-release.sh cookingfoil.nro 0.3.60 [key.pem]
#
# The private key stays off this repository (default below); without it no
# update can be offered to the app.
set -euo pipefail
nro=$1
version=$2
key=${3:-$HOME/app/cookingfoil-signing/release-ed25519.pem}
size=$(stat -c %s "$nro")
sha=$(sha256sum "$nro" | cut -d' ' -f1)
message=$(mktemp)
signature=$(mktemp)
trap 'rm -f "$message" "$signature"' EXIT
printf 'cookingfoil-update-v1\n%s\n%s\n%s\n' "$version" "$size" "$sha" > "$message"
openssl pkeyutl -sign -inkey "$key" -rawin -in "$message" -out "$signature"
# Checked with the public half before it is handed on.
openssl pkey -in "$key" -pubout | openssl pkeyutl -verify -pubin -inkey /dev/stdin -rawin -in "$message" -sigfile "$signature" >/dev/null
printf '{"version":"%s","size":%s,"sha256":"%s","signature":"%s"}\n' "$version" "$size" "$sha" "$(xxd -p -c 256 "$signature")" > "$nro.sig.json"
echo "signed $version ($size bytes) -> $nro.sig.json"
