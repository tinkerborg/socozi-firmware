#include "adc.h"
#include "gd32e23x.h"
#include "debug.h"
#include "timing.h"

static uint32_t flags;

uint32_t adc_flags(void)
{
    return flags;
}

/* Ordering transcribed from the factory firmware (0x08000178 and callees).
 *
 * The clock config is the step that is easy to miss; without it the ADC has no
 * usable clock, both calibration waits time out, and conversions hang.
 */
void adc_init(void)
{
    /* ADC clock: CFG0 ADCPSC = /6, CFG2 selects that source. */
    RCU_CFG0 = (RCU_CFG0 & ~RCU_CFG0_ADCPSC_MASK) | RCU_CFG0_ADCPSC_DIV6;
    RCU_CFG2 |= RCU_CFG2_ADCSEL;

    RCU_APB2EN |= RCU_APB2EN_ADCEN;

    /* Analog mode on all three inputs. PA7 is not configured by the factory
     * firmware's pin init at all, but it is sampled, reset state is input,
     * which the ADC can still read.
     */
    GPIO_CTL(GPIOA_BASE) |= (3u << (7 * 2));
    GPIO_CTL(GPIOB_BASE) |= (3u << (0 * 2)) | (3u << (1 * 2));

    ADC_SAMPT0 = 0x00FFFFFF;                   /* max sample time everywhere */
    ADC_SAMPT1 = 0x00FFFFFF;

    ADC_CTL1 |= ADC_CTL1_ETERC | ADC_CTL1_ETSRC_SWRCST;
    ADC_CTL1 |= ADC_CTL1_ADCON;
    delay_ms(1);

    /* Calibration is optional and its exact bit behaviour on this part is
     * unconfirmed, so never block on it, record and move on.
     */
    ADC_CTL1 |= ADC_CTL1_RSTCLB;
    if (!wait_clear(&ADC_CTL1, ADC_CTL1_RSTCLB, 10)) {
        flags |= ADC_FLAG_RSTCLB_TIMEOUT;
    }

    ADC_CTL1 |= ADC_CTL1_CLB;
    if (!wait_clear(&ADC_CTL1, ADC_CTL1_CLB, 10)) {
        flags |= ADC_FLAG_CLB_TIMEOUT;
    }

    dbg.adc_flags = flags;
}

/* Rather than mirror the factory firmware's DMA scan, just point the one-entry
 * regular sequence at whichever channel we want. Simpler, and speed doesn't
 * matter here.
 */
uint32_t adc_read(uint32_t channel)
{
    uint32_t start;

    ADC_RSQ0 = ADC_RSQ0_LENGTH(1);
    ADC_RSQ2 = channel;

    ADC_STAT = 0;
    ADC_CTL1 |= ADC_CTL1_SWRCST;

    start = ms_ticks;
    while (!(ADC_STAT & ADC_STAT_EOC)) {
        if ((ms_ticks - start) >= 5) {
            flags |= ADC_FLAG_EOC_TIMEOUT;
            dbg.adc_flags = flags;
            return 0xFFFFFFFF;
        }
    }

    return ADC_RDATA & 0xFFF;
}
