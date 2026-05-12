/* protocol_version.h - wire protocol version numbers.
 *
 * Both firmware and host include this. The current firmware (cdctask.c) does
 * NOT exchange a version handshake at link-up; the host is expected to be
 * compiled against a known firmware version and permissively assume v1.0 on
 * connect.
 *
 * Versioning policy (host side):
 *   - kProtocolMinSupported (v1.0): minimum supported; anything below ⇒ refuse.
 *   - kProtocolPreferred    (v1.0): the version this build of the host targets.
 *   - Permissive on mismatch above the minimum: warn the user, proceed.
 *
 * Bumping rules:
 *   - MINOR: additive, backward-compatible. New opcodes, new fields appended
 *            to existing structs (but NEVER inserted in the middle — that
 *            breaks packed-struct byte offsets on hosts running older builds).
 *   - MAJOR: any breaking change. Field reorder, field removal, opcode
 *            renumbering, magic-byte change, footer-rule change.
 *
 * If/when a handshake opcode is added, the firmware should report its
 * ROBIN_PROTOCOL_VERSION_U16 in the response payload, and the host should
 * compare against kProtocolMinSupported per the rules above.
 */

#ifndef ROBIN_PROTOCOL_VERSION_H
#define ROBIN_PROTOCOL_VERSION_H

#include <stdint.h>

#define ROBIN_PROTOCOL_VERSION_MAJOR  1u
#define ROBIN_PROTOCOL_VERSION_MINOR  0u

/* Encoded as 0xMMmm for compact wire transmission and easy comparison. */
#if defined(__cplusplus)
    #define ROBIN__U16(x) static_cast<uint16_t>(x)
#else
    #define ROBIN__U16(x) ((uint16_t)(x))
#endif

#define ROBIN_PROTOCOL_VERSION_U16    \
    ((ROBIN__U16(ROBIN_PROTOCOL_VERSION_MAJOR) << 8) | \
      ROBIN__U16(ROBIN_PROTOCOL_VERSION_MINOR))

#define ROBIN_PROTOCOL_MIN_SUPPORTED  0x0100u  /* v1.0 */

#endif /* ROBIN_PROTOCOL_VERSION_H */
