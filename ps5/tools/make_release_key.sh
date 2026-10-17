#!/bin/bash
# Media Player Classic PS5: makes the release signing key (Ed25519), once, on the machine that makes the releases.
#
#   tools/make_release_key.sh <private key file>     e.g. ~/.mpcps5/release-ed25519.pem
#
# Writes the private key (owner-only permissions; keep it safe and out of the repository: whoever has it can sign
# updates the app will install) and keys/release-ed25519.pub, the public key the app is built with (64 hex
# digits; commit it). Needs OpenSSL 1.1.1 or newer.
set -euo pipefail
cd "$(dirname "$0")/.."
KEY=${1:?usage: tools/make_release_key.sh <private key file>}
if [ -e "$KEY" ]; then
	echo "$KEY already exists: not replaced (a new key can't install updates signed by the old one's users)" >&2
	exit 1
fi
mkdir -p "$(dirname "$KEY")" keys
umask 077
openssl genpkey -algorithm ed25519 -out "$KEY"
openssl pkey -in "$KEY" -pubout -outform DER | tail -c 32 | od -An -tx1 | tr -d ' \n' >keys/release-ed25519.pub
echo >>keys/release-ed25519.pub
echo "private key: $KEY (keep it secret)"
echo "public key:  keys/release-ed25519.pub = $(cat keys/release-ed25519.pub)"
