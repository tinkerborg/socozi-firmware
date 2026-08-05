/* Build identity, one word.
 *
 * Published in the debug block so the ESP32 bridge can read what the chair is
 * running and compare it against the image it is carrying, without halting
 * anything and without a second link. See docs/esphome-design.md §4.
 *
 * The Makefile computes it:
 *
 *   clean tree   bit 31 clear, low bits the 7-hex-digit git commit. A short
 *                hash is 28 bits, so bit 31 is free by construction.
 *   dirty tree   bit 31 set, low 31 bits the build's Unix time.
 *
 * A hash would be wrong for a dirty tree, because two different working trees
 * at the same commit would compare equal and the bridge would decide the chair
 * was already up to date. A timestamp is unique per link and orders correctly,
 * which is what the comparison actually needs.
 */

#ifndef VERSION_H
#define VERSION_H

#ifndef FW_VERSION
#define FW_VERSION 0u
#endif

#define FW_VERSION_DIRTY (1u << 31)

#endif /* VERSION_H */
