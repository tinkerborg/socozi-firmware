#include "gd32e23x.h"
#include "handset.h"
#include "debug.h"

extern volatile uint32_t ms_ticks;

/* 8 MHz IRC / 9600. The factory firmware computes this at runtime from the
 * APB2 clock; we run at the reset default and can bake it in.
 */
#define USART_CLK  8000000u
#define BAUD_RATE  9600u

static uint8_t  rx_buf[HS_FRAME_LEN];
static uint8_t  rx_len;
static uint8_t  rx_sum;
static uint8_t  last_button;
static uint32_t last_frame_ms;
static uint8_t  led_bits;

void handset_init(void)
{
    RCU_APB2EN |= RCU_APB2EN_USART0EN;

    /* PA9 / PA10 to alternate function 1. */
    GPIO_CTL(GPIOA_BASE) = (GPIO_CTL(GPIOA_BASE) & ~((3u << 18) | (3u << 20)))
                         | (2u << 18) | (2u << 20);
    GPIO_AFSEL1(GPIOA_BASE) = (GPIO_AFSEL1(GPIOA_BASE) & ~((0xFu << 4) | (0xFu << 8)))
                            | (1u << 4) | (1u << 8);

    USART_CTL0 = 0;                         /* 8 data bits, no parity */
    USART_CTL1 = 0;                         /* 1 stop bit */
    USART_BAUD = (USART_CLK + BAUD_RATE / 2) / BAUD_RATE;
    USART_CTL0 |= USART_CTL0_REN | USART_CTL0_TEN;
    USART_CTL0 |= USART_CTL0_UEN;

    last_button   = HS_NONE;
    last_frame_ms = 0;
}

static void parser_reset(void)
{
    rx_len = 0;
    rx_sum = 0;
}

/* Feed one byte through the frame state machine. Returns 1 on a complete,
 * checksum-valid frame.
 */
static int feed(uint8_t b)
{
    switch (rx_len) {
    case 0:
        if (b != HS_TYPE_IDLE && b != HS_TYPE_PRESSED) {
            parser_reset();
            return 0;
        }
        break;

    case 1:
        if (b != 0x04) {
            parser_reset();
            return 0;
        }
        break;

    case 2:
        break;

    case 3:
        /* Checksum byte, not itself included in the sum. */
        if (b != rx_sum) {
            parser_reset();
            return 0;
        }
        rx_buf[3] = b;
        parser_reset();
        return 1;

    default:
        parser_reset();
        return 0;
    }

    rx_buf[rx_len++] = b;
    rx_sum += b;
    return 0;
}

static void tx_byte(uint8_t b)
{
    uint32_t start = ms_ticks;

    while (!(USART_STAT & USART_STAT_TBE)) {
        if ((ms_ticks - start) >= 5) {
            return;
        }
    }

    USART_TDATA = b;
}

/* The handset does not transmit unsolicited, it answers a poll. The factory
 * firmware sends a 6-byte frame: same framing as receive but with a 3-byte
 * payload carrying handset LED state.
 */
void handset_send(uint8_t type, uint8_t p0, uint8_t p1, uint8_t p2)
{
    uint8_t frame[6];
    uint8_t sum = 0;

    frame[0] = type;
    frame[1] = 0x04;
    frame[2] = p0;
    frame[3] = p1;
    frame[4] = p2;

    for (int i = 0; i < 5; i++) {
        sum += frame[i];
    }
    frame[5] = sum;

    for (int i = 0; i < 6; i++) {
        tx_byte(frame[i]);
    }

    dbg.hs_polls++;
}

int handset_poll(void)
{
    int got = 0;

    /* Clear any error flag first. An uncleared ORERR wedges the receiver
     * permanently: no more bytes are delivered, RBNE never sets again, and
     * because the main loop is otherwise healthy the watchdog never fires. The
     * chair just stops responding to the handset until it is reset.
     *
     * A partial frame is worthless once bytes have been dropped, so drop it
     * too and resynchronise on the next poll.
     */
    uint32_t stat = USART_STAT;

    if (stat & USART_STAT_ERRORS) {
        USART_INTC = USART_INTC_ERRORS;
        parser_reset();
        dbg.hs_errors++;
        dbg.hs_last_error = stat & USART_STAT_ERRORS;
    }

    while (USART_STAT & USART_STAT_RBNE) {
        uint8_t b = (uint8_t)(USART_RDATA & 0xFF);

        dbg.hs_bytes++;
        dbg.hs_last_byte = b;

        if (feed(b)) {
            last_button   = rx_buf[2];
            last_frame_ms = ms_ticks;
            dbg.hs_frames++;
            dbg.hs_button = last_button;
            got = 1;
        }
    }

    return got;
}

void handset_set_leds(uint8_t bits)
{
    led_bits     = bits;
    dbg.leds   = bits;
}

uint8_t handset_leds(void)
{
    return led_bits;
}

uint8_t handset_button(void)
{
    return last_button;
}

uint32_t handset_age_ms(void)
{
    return ms_ticks - last_frame_ms;
}
