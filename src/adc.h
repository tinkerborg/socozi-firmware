/* ADC, three channels, matching the factory firmware.
 *
 * | Channel | Pin | Notes                                         |
 * |---------|-----|-----------------------------------------------|
 * | 7       | PA7 | what the factory over-current cutoff watches  |
 * | 8       | PB0 | steady ~373 at rest                           |
 * | 9       | PB1 | moved during a motor run                      |
 *
 * Which of these is really the current shunt is unresolved, channel 7 reads 0
 * even under load. See docs/custom-firmware.md.
 */

#ifndef ADC_H
#define ADC_H

#include <stdint.h>

#define ADC_CH_CURRENT 7
#define ADC_CH_8       8
#define ADC_CH_9       9

void adc_init(void);

/* Returns 0-4095, or 0xFFFFFFFF if the conversion timed out. */
uint32_t adc_read(uint32_t channel);

/* Bitmask of ADC_FLAG_* recorded during init and conversion. */
uint32_t adc_flags(void);

#endif /* ADC_H */
