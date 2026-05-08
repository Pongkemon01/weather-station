#!/usr/bin/env bash
# Generate an Ed25519 key pair for firmware signing (S10-4).
#
# The private key stays on the signing workstation — NEVER copy it to the server.
# The server reads the private key via SIGNING_PRIVATE_KEY_PATH in iot.env and
# attaches a .sig file alongside every uploaded firmware binary.
#
# The public key (32 raw bytes, hex) must be embedded in the bootloader as a
# constant array for signature verification before Flash programming.
# Bootloader integration is a future firmware phase (see OTA_Firmware_Architecture.md §9).
#
# Usage:  bash scripts/generate_signing_key.sh <output_dir>
#
# Outputs:
#   <output_dir>/firmware_signing.key      — PEM private key (keep offline / air-gapped)
#   <output_dir>/firmware_signing.pub      — PEM public key
#   <output_dir>/firmware_signing_pub.hex  — raw 32-byte public key in hex (for C array)
#
# Annual key rotation:
#   1. Re-run this script with a new output directory.
#   2. Set SIGNING_PRIVATE_KEY_PATH to the new key in iot.env and restart the server.
#   3. Re-upload any in-progress firmware binaries so they carry the new signature.
#   4. Update the bootloader public key constant and reflash all devices before the old
#      key is retired.

set -euo pipefail
OUT="${1:?Usage: $0 <output_dir>}"
mkdir -p "$OUT"

openssl genpkey -algorithm ed25519 -out "$OUT/firmware_signing.key"
chmod 400 "$OUT/firmware_signing.key"

openssl pkey -in "$OUT/firmware_signing.key" -pubout -out "$OUT/firmware_signing.pub"

# Extract raw 32-byte public key (skip the 12-byte SubjectPublicKeyInfo DER header).
openssl pkey -in "$OUT/firmware_signing.key" -pubout -outform DER \
    | tail -c 32 | xxd -p -c 32 > "$OUT/firmware_signing_pub.hex"

echo "Keys written to $OUT/"
echo "  Private : firmware_signing.key  — configure SIGNING_PRIVATE_KEY_PATH in iot.env"
echo "  Public  : firmware_signing.pub"
echo "  Hex     : firmware_signing_pub.hex — embed in bootloader shared/ed25519_pubkey.h"
echo ""
echo "Public key (32 bytes): $(cat "$OUT/firmware_signing_pub.hex")"
