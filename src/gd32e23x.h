/* Minimal register definitions for the GD32E23x.
 *
 * Offsets follow the STM32F0-alike map that GD32 clones. Verified so far
 * against the running chair: GPIO bases, GPIOA_CTL, GPIOA_AFSEL1, RCU_RSTSCK,
 * FWDGT_CTL, DBG_ID. Anything else should be treated as probable, not certain.
 */

#ifndef GD32E23X_H
#define GD32E23X_H

#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))

/* ---- Reset and clock unit ---- */

#define RCU_BASE        0x40021000u
#define RCU_CFG0        REG32(RCU_BASE + 0x04)
#define RCU_AHBEN       REG32(RCU_BASE + 0x14)
#define RCU_APB2EN      REG32(RCU_BASE + 0x18)
#define RCU_RSTSCK      REG32(RCU_BASE + 0x24)
#define RCU_CFG2        REG32(RCU_BASE + 0x30)

/* ADC clock select/prescale. Transcribed from the factory firmware's
 * rcu_adc_clock_config(6) at 0x080037D4, CFG0 ADCPSC = 0b10, CFG2 bit 8 set.
 * Getting this wrong leaves the ADC unclocked and calibration never completes.
 */
#define RCU_CFG0_ADCPSC_MASK  (3u << 14)
#define RCU_CFG0_ADCPSC_DIV6  (2u << 14)
#define RCU_CFG2_ADCSEL       (1u << 8)

#define RCU_AHBEN_PAEN  (1u << 17)
#define RCU_AHBEN_PBEN  (1u << 18)
#define RCU_AHBEN_PCEN  (1u << 19)
#define RCU_AHBEN_PFEN  (1u << 22)
#define RCU_APB2EN_ADCEN (1u << 9)

/* ---- GPIO ---- */

#define GPIOA_BASE      0x48000000u
#define GPIOB_BASE      0x48000400u
#define GPIOC_BASE      0x48000800u
#define GPIOF_BASE      0x48001400u

#define GPIO_CTL(base)     REG32((base) + 0x00)  /* 2 bits/pin: 0 in, 1 out, 2 AF, 3 analog */
#define GPIO_OMODE(base)   REG32((base) + 0x04)  /* 1 bit/pin: 0 push-pull, 1 open-drain */
#define GPIO_OSPD(base)    REG32((base) + 0x08)
#define GPIO_PUD(base)     REG32((base) + 0x0C)  /* 2 bits/pin: 0 none, 1 pull-up, 2 pull-down */
#define GPIO_ISTAT(base)   REG32((base) + 0x10)
#define GPIO_OCTL(base)    REG32((base) + 0x14)
#define GPIO_BOP(base)     REG32((base) + 0x18)  /* [15:0] set, [31:16] clear */

/* ---- ADC ---- */

#define ADC_BASE        0x40012400u
#define ADC_STAT        REG32(ADC_BASE + 0x00)
#define ADC_CTL0        REG32(ADC_BASE + 0x04)
#define ADC_CTL1        REG32(ADC_BASE + 0x08)
#define ADC_SAMPT0      REG32(ADC_BASE + 0x0C)  /* channels 10-18 */
#define ADC_SAMPT1      REG32(ADC_BASE + 0x10)  /* channels 0-9 */
#define ADC_RSQ0        REG32(ADC_BASE + 0x2C)  /* length field + ranks 12-15 */
#define ADC_RSQ1        REG32(ADC_BASE + 0x30)  /* ranks 6-11 */
#define ADC_RSQ2        REG32(ADC_BASE + 0x34)  /* ranks 0-5 */
#define ADC_RDATA       REG32(ADC_BASE + 0x4C)

/* Regular sequence length, n-1 in bits 23:20 of RSQ0. */
#define ADC_RSQ0_LENGTH(n) (((n) - 1u) << 20)

#define ADC_STAT_EOC    (1u << 1)
#define ADC_CTL1_ADCON  (1u << 0)
#define ADC_CTL1_RSTCLB (1u << 3)
#define ADC_CTL1_CLB    (1u << 2)
#define ADC_CTL1_ETERC  (1u << 20)
#define ADC_CTL1_ETSRC_SWRCST (7u << 17)
#define ADC_CTL1_SWRCST (1u << 22)

/* ---- USART ----
 *
 * Offsets taken from the factory firmware, not from recall: it reads received
 * bytes from base+0x24 (0x080047B0), writes the baud divider to base+0x0C
 * (0x08004700), and enables the peripheral with bit 0 of base+0x00
 * (0x0800482C).
 */

#define USART0_BASE     0x40013800u
#define USART_CTL0      REG32(USART0_BASE + 0x00)
#define USART_CTL1      REG32(USART0_BASE + 0x04)
#define USART_CTL2      REG32(USART0_BASE + 0x08)
#define USART_BAUD      REG32(USART0_BASE + 0x0C)
#define USART_STAT      REG32(USART0_BASE + 0x1C)
#define USART_INTC      REG32(USART0_BASE + 0x20)
#define USART_RDATA     REG32(USART0_BASE + 0x24)
#define USART_TDATA     REG32(USART0_BASE + 0x28)

#define USART_CTL0_UEN  (1u << 0)
#define USART_CTL0_REN  (1u << 2)
#define USART_CTL0_TEN  (1u << 3)

/* This is the newer USART block, the one with STAT/INTC rather than an SR that
 * self-clears on a data read. Error flags are cleared ONLY by writing a 1 to
 * the matching INTC bit.
 *
 * That matters a lot. While ORERR is set the receiver discards everything and
 * RBNE never sets again, so a single overrun kills the handset link for good.
 * The main loop keeps running and keeps kicking the watchdog, so nothing
 * recovers it. Always clear the error flags.
 */
#define USART_STAT_PERR  (1u << 0)
#define USART_STAT_FERR  (1u << 1)
#define USART_STAT_NERR  (1u << 2)
#define USART_STAT_ORERR (1u << 3)
#define USART_STAT_RBNE  (1u << 5)
#define USART_STAT_TBE   (1u << 7)

#define USART_STAT_ERRORS \
    (USART_STAT_PERR | USART_STAT_FERR | USART_STAT_NERR | USART_STAT_ORERR)

/* INTC clear bits sit at the same positions as the STAT flags. */
#define USART_INTC_ERRORS USART_STAT_ERRORS

#define RCU_APB2EN_USART0EN (1u << 14)

/* GPIO alternate function select, 4 bits per pin. AFSEL0 covers pins 0-7,
 * AFSEL1 pins 8-15. AF1 on PA9/PA10 is USART0 TX/RX, confirmed by reading
 * GPIOA_AFSEL1 = 0x00000110 off the running chair.
 */
#define GPIO_AFSEL0(base)  REG32((base) + 0x20)
#define GPIO_AFSEL1(base)  REG32((base) + 0x24)

/* ---- Free watchdog (FWDGT) ----
 *
 * Clocked from the ~40 kHz internal RC, independent of the core clock, so it
 * keeps counting even if the main clock or firmware is wedged.
 */

#define FWDGT_BASE      0x40003000u
#define FWDGT_CTL       REG32(FWDGT_BASE + 0x00)
#define FWDGT_PSC       REG32(FWDGT_BASE + 0x04)
#define FWDGT_RLD       REG32(FWDGT_BASE + 0x08)
#define FWDGT_STAT      REG32(FWDGT_BASE + 0x0C)

#define FWDGT_KEY_RELOAD 0x0000AAAAu
#define FWDGT_KEY_UNLOCK 0x00005555u
#define FWDGT_KEY_ENABLE 0x0000CCCCu

#define FWDGT_STAT_PUD  (1u << 0)   /* prescaler update in progress */
#define FWDGT_STAT_RUD  (1u << 1)   /* reload update in progress */

/* Debug control. Bit 8 freezes the free watchdog while the core is halted,
 * which is what makes a watchdog and a debugger coexist.
 */
#define DBG_CTL         REG32(0x40015804u)
#define DBG_CTL_FWDGT_HOLD (1u << 8)

/* ---- SysTick ---- */

#define SYSTICK_CTL     REG32(0xE000E010)
#define SYSTICK_RELOAD  REG32(0xE000E014)
#define SYSTICK_VAL     REG32(0xE000E018)

/* ---- Pin ids ----
 *
 * Same packing the factory firmware uses: high nibble = port index
 * (0=A, 1=B, 2=C, 3=F), low nibble = pin number. Keeping the encoding
 * identical means the ids in docs/firmware-map.md carry over unchanged.
 */

#define PIN_PORT(id) (((id) >> 4) & 0xF)
#define PIN_NUM(id)  ((id) & 0xF)

#endif /* GD32E23X_H */
