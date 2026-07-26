/* Pin access and the board's output/input configuration.
 *
 * Pin ids use the factory firmware's packing, high nibble port (0=A, 1=B,
 * 2=C, 3=F), low nibble pin, so ids in docs/firmware-map.md carry over
 * directly.
 */

#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>

/* Named pins, all verified on the chair. See docs/firmware-map.md. */
#define PIN_SHIFT_DATA   0x02   /* PA2  */
#define PIN_SHIFT_CLOCK  0x03   /* PA3  */
#define PIN_SHIFT_LATCH  0x04   /* PA4  */
#define PIN_RECLINE_A    0x06   /* PA6, pairs with PIN_RECLINE_B  */
#define PIN_RECLINE_B    0x12   /* PB2  */
#define PIN_RECLINE_C    0x05   /* PA5, pairs with PIN_RECLINE_D  */
#define PIN_RECLINE_D    0x1A   /* PB10 */
#define PIN_HEADREST_DIR 0x1D   /* PB13, 0 = up, 1 = down */
#define PIN_HEADREST_EN  0x1F   /* PB15 */
#define PIN_PUMP         0x2D   /* PC13 */
#define PIN_HEATER       0x2E   /* PC14, inferred, untested */

void gpio_init(void);

void pin_write(uint8_t id, uint32_t level);
void all_outputs_off(void);

/* Clock 4 bits MSB-first into the valve shift register, then latch. */
void shift_write(uint32_t value);

/* Raw port input states, for the debug block. */
uint32_t gpio_istat_a(void);
uint32_t gpio_istat_b(void);
uint32_t gpio_istat_c(void);
uint32_t gpio_octl_a(void);
uint32_t gpio_octl_b(void);
uint32_t gpio_octl_c(void);

#endif /* GPIO_H */
