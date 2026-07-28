#!/usr/bin/env bash
# Regenerate the fleet-wide client certificate and the three *_der.c arrays
# embedded in firmware (lib/A7670/{client,client_key,server}_der.c).
#
# BACKGROUND
# ──────────
# All IoT devices share a single client certificate (the "fleet cert"). During
# mTLS handshake the modem presents this cert; nginx validates it against
# pki/private_ca_chain.pem. If either PKI side changes (provision_ca.sh was
# re-run, intermediate CA rotated, server cert re-issued), the firmware arrays
# must be regenerated from the CURRENT state of pki/.
#
# This script:
#   1. Issues (or re-issues) a fleet client cert signed by the current
#      intermediate CA, CN="iot-fleet" (matching the prior convention).
#   2. Converts the cert + private key from PEM to DER (modem uses DER).
#   3. Copies the intermediate CA cert to DER (modem uses it to verify the
#      SERVER's TLS cert chain — Arch §2.3).
#   4. Runs scripts/pem_to_c_array.sh (repo scripts/ on the build host) to
#      regenerate lib/A7670/{client,client_key,server}_der.c.
#
# PREREQUISITES
# ─────────────
#   • Run from the SERVER (where pki/ lives), not the build host.
#   • pki/intermediate/ca.conf must exist (run provision_ca.sh first).
#   • PEM→C script must be available — either copy scripts/pem_to_c_array.sh
#     from the repo, or set PEM_TO_C to an alternate path.
#   • After running, copy the regenerated lib/A7670/*.c to the build host
#     and rebuild the firmware (platformio run).
#
# USAGE
# ─────
#   regen_fleet_certs.sh [PKI_DIR] [FW_DIR]
#
#   PKI_DIR  pki/ root on server           (default: html/pki next to scripts/)
#   FW_DIR   firmware lib/A7670/ directory  (default: lib/A7670 relative to repo root)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HTML_DIR="$(dirname "$SCRIPT_DIR")"
REPO_ROOT="$(cd "$HTML_DIR/.." && pwd)"

PKI_DIR="${1:-$HTML_DIR/pki}"
FW_DIR="${2:-$REPO_ROOT/lib/A7670}"
PEM_TO_C="${PEM_TO_C:-$REPO_ROOT/scripts/pem_to_c_array.sh}"

INT_DIR="$PKI_DIR/intermediate"
FLEET_DIR="$PKI_DIR/devices/iot-fleet"

FLEET_CN="iot-fleet"
FLEET_KEY="$FLEET_DIR/$FLEET_CN.key"
FLEET_CSR="$FLEET_DIR/$FLEET_CN.csr"
FLEET_CERT="$FLEET_DIR/$FLEET_CN.crt"
FLEET_CERT_DER="$FLEET_DIR/$FLEET_CN.der"
FLEET_KEY_DER="$FLEET_DIR/$FLEET_CN-key.der"
INT_CA_DER="$FLEET_DIR/intermediate.der"

# ── Sanity checks ──────────────────────────────────────────────────────────
for required in "$INT_DIR/ca.conf" "$INT_DIR/intermediate.crt" "$INT_DIR/intermediate.key" "$PEM_TO_C"; do
    if [ ! -f "$required" ]; then
        echo "ERROR: missing prerequisite: $required" >&2
        exit 1
    fi
done

# Self-heal staled ca.conf `dir =` path from prior PKI relocations.
# (provision_ca.sh expands $INT_DIR at generate-time; if PKI moved later, ca.conf
#  still points at the old location and every openssl ca invocation fails.)
CA_CONF_DIR_VAL=$(grep -E '^\s*dir\s*=' "$INT_DIR/ca.conf" | sed -E 's/.*=\s*//')
if [ "$CA_CONF_DIR_VAL" != "$INT_DIR" ]; then
    echo "==> Fixing stale ca.conf dir: '$CA_CONF_DIR_VAL' → '$INT_DIR'"
    sed -i -E "s|^(\s*dir\s*=).*|\1 $INT_DIR|" "$INT_DIR/ca.conf"
fi

mkdir -p "$FLEET_DIR"
chmod 700 "$FLEET_DIR"

echo "==> [1/5] Generating fleet client key (CN=$FLEET_CN)..."
# Always regenerate — old key is unrecoverable and we need a fresh pair to
# guarantee the firmware's *_der.c match. Force-remove stale artifacts first
# (previous run may have produced them with mode 400 before bailing out).
rm -f "$FLEET_KEY" "$FLEET_CSR" "$FLEET_CERT" \
      "$FLEET_CERT_DER" "$FLEET_KEY_DER" "$INT_CA_DER"
openssl genrsa -out "$FLEET_KEY" 2048
chmod 400 "$FLEET_KEY"

echo "==> [2/5] Generating CSR..."
openssl req -new \
    -key "$FLEET_KEY" \
    -out "$FLEET_CSR" \
    -subj "/CN=$FLEET_CN/O=RobinLab/C=TH"

echo "==> [3/5] Signing with current intermediate CA..."
# Revoke any existing Valid cert for the same CN first — unique_subject=yes in
# index.txt.attr blocks re-issuing otherwise. Prefer openssl ca -revoke when the
# issued file is on disk (filename in column 5 is not "unknown"). Fall back to
# marking the entry Revoked in-place if no file exists (e.g. previous regen
# aborted before copying the cert into new_certs_dir).
PRUNE_LINE="$(awk -F'\t' -v cn="CN=${FLEET_CN}" '$1=="V"&&index($6,cn){print NR;exit}' \
    "$INT_DIR/index.txt")"
if [ -n "$PRUNE_LINE" ]; then
    FIELD5="$(awk -F'\t' -v n="$PRUNE_LINE" 'NR==n{print $5}' "$INT_DIR/index.txt")"
    if [ "$FIELD5" != "unknown" ] && [ -f "$INT_DIR/issued/$FIELD5" ]; then
        echo "    Revoking previous cert (file $FIELD5)..."
        openssl ca -config "$INT_DIR/ca.conf" \
            -revoke "$INT_DIR/issued/$FIELD5" -batch
    else
        echo "    Stripping orphaned index entry (line $PRUNE_LINE, no cert file)..."
        sed -i "${PRUNE_LINE}d" "$INT_DIR/index.txt"
    fi
fi

openssl ca -batch \
    -config "$INT_DIR/ca.conf" \
    -extensions v3_device \
    -in "$FLEET_CSR" \
    -out "$FLEET_CERT"

echo "==> [4/5] Converting PEM → DER (modem uses DER, not PEM)..."
openssl x509 -in "$FLEET_CERT"      -outform DER -out "$FLEET_CERT_DER"
openssl rsa  -in "$FLEET_KEY"       -outform DER -out "$FLEET_KEY_DER"
openssl x509 -in "$INT_DIR/intermediate.crt" -outform DER -out "$INT_CA_DER"

echo "==> [5/5] Regenerating firmware *_der.c arrays..."
"$PEM_TO_C" "$FLEET_CERT_DER"   client_der     "$FW_DIR/client_der.c"
"$PEM_TO_C" "$FLEET_KEY_DER"    client_key_der "$FW_DIR/client_key_der.c"
"$PEM_TO_C" "$INT_CA_DER"       server_der     "$FW_DIR/server_der.c"

echo
echo "========================================================================"
echo "  Regenerated fleet cert + firmware arrays."
echo "========================================================================"
echo
echo "  Fleet cert:  $FLEET_CERT        (PEM, for reference)"
echo "  Fleet key:   $FLEET_KEY         (PEM, KEEP OFF SERVER LONG-TERM)"
echo "  Cert DER:    $FLEET_CERT_DER    (→ client_der.c)"
echo "  Key DER:     $FLEET_KEY_DER     (→ client_key_der.c)"
echo "  CA DER:      $INT_CA_DER        (→ server_der.c)"
echo
echo "  Next steps:"
echo "    1. Ensure pki/private_ca_chain.pem (server-side) contains the SAME"
echo "       intermediate CA used above. If you re-ran provision_ca.sh, rebuild:"
echo "         cat $INT_DIR/intermediate.crt $PKI_DIR/offline/root.crt > $PKI_DIR/private_ca_chain.pem"
echo "    2. Reload nginx: sudo nginx -s reload"
echo "    3. scp lib/A7670/{client,client_key,server}_der.c to the build host"
echo "    4. Rebuild firmware: platformio run && pio run -t upload"
echo "    5. Reflash device. After NTP sync, modem re-injects all three certs."
echo
