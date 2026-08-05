#include <stddef.h>
#include <stdint.h>

#include "rtt.h"

#if RTT

/* Sizes are a compromise between how much a stalled link can swallow and how
 * much of an 8 KiB part to spend on debugging. The up buffer is what has to
 * ride out an outage; input arrives at typing speed and needs almost nothing.
 */
#define RTT_UP_SIZE   512
#define RTT_DOWN_SIZE 64

static char up_buf[RTT_UP_SIZE];
static char down_buf[RTT_DOWN_SIZE];

/* SEGGER's control block, laid out exactly as the host expects to find it.
 *
 * Each ring has a single writer per index, and that is the whole of its
 * thread-safety: the target owns WrOff on the up buffer and RdOff on the down
 * one, the host owns the other two. Nothing needs locking and a link that dies
 * mid-transaction cannot corrupt what the other side relies on.
 */
struct rtt_ring {
    const char *name;
    char       *buffer;
    uint32_t    size;
    uint32_t    wr;
    uint32_t    rd;
    uint32_t    flags;      /* 0: drop when full, rather than block */
};

struct rtt_cb {
    char            id[16];
    int32_t         max_up;
    int32_t         max_down;
    struct rtt_ring up[1];
    struct rtt_ring down[1];
};

/* Must live in RAM, initialized: the host finds it by scanning target memory
 * for the id string, so a const in flash would never be seen.
 */
volatile struct rtt_cb _SEGGER_RTT = {
    "SEGGER RTT",
    1,
    1,
    { { "Terminal", up_buf, RTT_UP_SIZE, 0, 0, 0 } },
    { { "Terminal", down_buf, RTT_DOWN_SIZE, 0, 0, 0 } },
};

void rtt_putc(char c)
{
    uint32_t wr   = _SEGGER_RTT.up[0].wr;
    uint32_t next = wr + 1;

    if (next >= RTT_UP_SIZE) {
        next = 0;
    }

    /* Full. Drop it: a console is never worth stalling the loop that stops the
     * motors.
     */
    if (next == _SEGGER_RTT.up[0].rd) {
        return;
    }

    up_buf[wr] = c;

    /* Only after the byte is in place, so the host cannot read past the write
     * index into a slot that has not been filled yet.
     */
    _SEGGER_RTT.up[0].wr = next;
}

void rtt_write(const char *s)
{
    while (*s) {
        rtt_putc(*s++);
    }
}

void rtt_write_u32(uint32_t v)
{
    char digits[10];
    int  n = 0;

    if (v == 0) {
        rtt_putc('0');
        return;
    }

    while (v && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    while (n--) {
        rtt_putc(digits[n]);
    }
}

int rtt_getc(void)
{
    uint32_t rd = _SEGGER_RTT.down[0].rd;
    char     c;

    if (rd == _SEGGER_RTT.down[0].wr) {
        return -1;
    }

    c = down_buf[rd];

    if (++rd >= RTT_DOWN_SIZE) {
        rd = 0;
    }

    _SEGGER_RTT.down[0].rd = rd;

    return (int)(unsigned char)c;
}

#endif /* RTT */

uint32_t rtt_control_block(void)
{
#if RTT
    return (uint32_t)(uintptr_t)&_SEGGER_RTT;
#else
    return 0u;
#endif
}
