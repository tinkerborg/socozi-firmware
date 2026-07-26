/* Handset protocol, SoCozi wired remote on USART0, 9600 8N1.
 *
 * Handset → board frames are 4 bytes:
 *
 *     [0x03 or 0x06] [0x04] [button code] [checksum]
 *
 * Byte 0 is 0x03 when idle and 0x06 when a button is down. Byte 1 is always
 * 0x04. The checksum is the sum of the preceding three bytes, mod 256.
 *
 * Button codes are sequential in the physical order the buttons appear on the
 * handset. See ../docs/firmware-map.md.
 */

#ifndef HANDSET_H
#define HANDSET_H

#include <stdint.h>

#define HS_FRAME_LEN 4

enum {
    HS_NONE          = 0x00,
    HS_POWER         = 0x01,
    HS_MASSAGE       = 0x02,
    HS_HEAT          = 0x03,
    HS_LUMBAR        = 0x04,
    HS_RECLINE_UP    = 0x05,
    HS_RECLINE_DOWN  = 0x06,
    HS_HEADREST_UP   = 0x07,
    HS_HEADREST_DOWN = 0x08,
};

#define HS_TYPE_IDLE    0x03
#define HS_TYPE_PRESSED 0x06

/* Handset LEDs. The outbound frame's first payload byte is a bitmap, one bit
 * per button, in the same order as the button codes above. Derived from
 * FUN_08001810 in the factory image, where bits 4-5 come from the recline axis
 * state and 6-7 from the headrest axis.
 *
 * The factory firmware blinks bit 7 under some conditions rather than holding
 * it solid; that behaviour is not reproduced here.
 */
#define LED_POWER         (1u << 0)
#define LED_MASSAGE       (1u << 1)
#define LED_HEAT          (1u << 2)
#define LED_LUMBAR        (1u << 3)
#define LED_RECLINE_UP    (1u << 4)
#define LED_RECLINE_DOWN  (1u << 5)
#define LED_HEADREST_UP   (1u << 6)
#define LED_HEADREST_DOWN (1u << 7)

/* Set the LED bitmap sent with each poll. */
void handset_set_leds(uint8_t bits);
uint8_t handset_leds(void);

void handset_init(void);

/* Send a poll frame. The handset answers these; it does not talk unprompted. */
void handset_send(uint8_t type, uint8_t p0, uint8_t p1, uint8_t p2);

/* Feed any pending received bytes into the parser. Returns 1 if a valid frame
 * completed during this call.
 */
int handset_poll(void);

/* Button code from the most recent valid frame, or HS_NONE. */
uint8_t handset_button(void);

/* Milliseconds since the last valid frame, for release/timeout detection. */
uint32_t handset_age_ms(void);

#endif /* HANDSET_H */
