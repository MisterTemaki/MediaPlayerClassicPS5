#!/bin/bash
# Media Player Classic PS5: signs a release zip with the release key (Ed25519, RFC 8032): <zip>.sig, 64 bytes, to
# attach to the GitHub release next to the zip. Checks the signature against keys/release-ed25519.pub too.
#
#   tools/sign_release.sh <release zip> <private key file>
#   (make dist RELEASE_SIGNING_KEY=<private key file> does it for the release zip)
set -euo pipefail
ZIP=${1:?usage: tools/sign_release.sh <zip> <private key>}
KEY=${2:?usage: tools/sign_release.sh <zip> <private key>}
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$ZIP" -out "$ZIP.sig"
[ "$(stat -c %s "$ZIP.sig")" = 64 ] || { echo "the signature isn't 64 bytes" >&2; exit 1; }
PUB=$(openssl pkey -in "$KEY" -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n')
WANT=$(tr -d ' \n' <"$(dirname "$0")/../keys/release-ed25519.pub" 2>/dev/null || true)
if [ -n "$WANT" ] && [ "$PUB" != "$WANT" ]; then
	echo "this private key isn't the one keys/release-ed25519.pub is made from: the app wouldn't accept it" >&2
	rm -f "$ZIP.sig"
	exit 1
fi
openssl pkey -in "$KEY" -pubout -out "$ZIP.pub.pem.tmp"
openssl pkeyutl -verify -pubin -inkey "$ZIP.pub.pem.tmp" -rawin -in "$ZIP" -sigfile "$ZIP.sig" >/dev/null
rm -f "$ZIP.pub.pem.tmp"
echo "signed: $ZIP.sig"
