#!/usr/bin/env bash
# Convert a PEM/DER file to a C uint8_t array for embedding in firmware.
#
# Usage:
#   pem_to_c_array.sh <INPUT_FILE> <VAR_NAME> <OUTPUT_FILE>
#
# Outputs a .c file with:
#   const uint8_t <VAR_NAME>[]     — raw bytes of INPUT_FILE
#   const size_t  <VAR_NAME>_len   — byte count
#
# The format matches lib/A7670/client_der.c, client_key_der.c, server_der.c.
# Firmware embeds these via ssl_cert_inject() (a7670.c, step 3 of modem init).
#
# Examples (run from project root):
#   # Server CA chain — modem uses to verify the server TLS cert (Arch §2.3)
#   scripts/pem_to_c_array.sh /home/akp/iot_pki/private_ca_chain.pem \
#       server_der lib/A7670/server_der.c
#
#   # Device client cert chain — modem presents as mTLS client cert
#   scripts/pem_to_c_array.sh /home/akp/device_certs/<CN>/<CN>-chain.pem \
#       client_der lib/A7670/client_der.c
#
#   # Device private key — modem uses for mTLS handshake
#   scripts/pem_to_c_array.sh /home/akp/device_certs/<CN>/<CN>.key \
#       client_key_der lib/A7670/client_key_der.c
set -euo pipefail

INPUT_FILE="${1:?Usage: pem_to_c_array.sh <INPUT_FILE> <VAR_NAME> <OUTPUT_FILE>}"
VAR_NAME="${2:?Usage: pem_to_c_array.sh <INPUT_FILE> <VAR_NAME> <OUTPUT_FILE>}"
OUTPUT_FILE="${3:?Usage: pem_to_c_array.sh <INPUT_FILE> <VAR_NAME> <OUTPUT_FILE>}"

if [ ! -f "$INPUT_FILE" ]; then
    echo "ERROR: input file not found: $INPUT_FILE" >&2
    exit 1
fi

python3 - "$INPUT_FILE" "$VAR_NAME" "$OUTPUT_FILE" << 'PYEOF'
import sys, pathlib

input_file = sys.argv[1]
var_name   = sys.argv[2]
output_file = sys.argv[3]

data = pathlib.Path(input_file).read_bytes()
items = [f'0x{b:02x}' for b in data]
lines = ['    ' + ', '.join(items[i:i+12]) for i in range(0, len(items), 12)]

content  = f'/* Automatically generated from {input_file} — do not edit manually */\n'
content += '#include <stddef.h>\n'
content += '#include <stdint.h>\n\n'
content += f'const uint8_t {var_name}[] = {{\n'
content += ',\n'.join(lines) + '\n};\n\n'
content += f'const size_t {var_name}_len = {len(data)};\n'

pathlib.Path(output_file).write_text(content)
print(f'  {output_file}: {len(data)} bytes -> {var_name}[{len(data)}]')
PYEOF

echo "Done: $OUTPUT_FILE"
