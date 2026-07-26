#include "gd32e23x.h"
#include "gpio.h"
#include "timing.h"

/* Outputs the factory firmware configures. All driven low at startup and by
 * all_outputs_off(). See docs/firmware-map.md for the full pin map.
 */
static const uint8_t output_pins[] = {
    0x00, 0x01,              /* PA0, PA1  - unidentified */
    0x02, 0x03, 0x04,        /* PA2/3/4   - shift register data/clock/latch */
    0x05, 0x06,              /* PA5, PA6  - recline */
    0x12,                    /* PB2       - recline */
    0x1A,                    /* PB10      - recline */
    0x1B,                    /* PB11      - third motion axis direction */
    0x1D,                    /* PB13      - headrest direction */
    0x1E,                    /* PB14      - third motion axis enable */
    0x1F,                    /* PB15      - headrest enable */
    0x2D,                    /* PC13      - pump */
    0x2E,                    /* PC14      - heater (inferred) */
};

#define OUTPUT_COUNT (sizeof(output_pins) / sizeof(output_pins[0]))

static uint32_t port_base(uint32_t port)
{
    switch (port) {
    case 0:  return GPIOA_BASE;
    case 1:  return GPIOB_BASE;
    case 2:  return GPIOC_BASE;
    default: return GPIOF_BASE;
    }
}

void pin_write(uint8_t id, uint32_t level)
{
    uint32_t base = port_base(PIN_PORT(id));
    uint32_t mask = 1u << PIN_NUM(id);

    GPIO_BOP(base) = level ? mask : (mask << 16);
}

static void pin_mode_output(uint8_t id)
{
    uint32_t base = port_base(PIN_PORT(id));
    uint32_t pin  = PIN_NUM(id);

    GPIO_CTL(base) = (GPIO_CTL(base) & ~(3u << (pin * 2))) | (1u << (pin * 2));
    GPIO_OMODE(base) &= ~(1u << pin);
    GPIO_OSPD(base) |= (3u << (pin * 2));
}

static void pin_mode_input(uint8_t id, uint32_t pull)
{
    uint32_t base = port_base(PIN_PORT(id));
    uint32_t pin  = PIN_NUM(id);

    GPIO_CTL(base) &= ~(3u << (pin * 2));
    GPIO_PUD(base) = (GPIO_PUD(base) & ~(3u << (pin * 2))) | (pull << (pin * 2));
}

void all_outputs_off(void)
{
    for (uint32_t i = 0; i < OUTPUT_COUNT; i++) {
        pin_write(output_pins[i], 0);
    }
}

/* Mirrors FUN_08001CA4 / FUN_08001C78 in the factory image. */
void shift_write(uint32_t value)
{
    for (int i = 3; i >= 0; i--) {
        pin_write(PIN_SHIFT_DATA, (value >> i) & 1);
        pin_write(PIN_SHIFT_CLOCK, 0);
        delay_ms(1);
        pin_write(PIN_SHIFT_CLOCK, 1);
        delay_ms(1);
    }

    pin_write(PIN_SHIFT_LATCH, 0);
    delay_ms(1);
    pin_write(PIN_SHIFT_LATCH, 1);
}

void gpio_init(void)
{
    RCU_AHBEN |= RCU_AHBEN_PAEN | RCU_AHBEN_PBEN | RCU_AHBEN_PCEN | RCU_AHBEN_PFEN;

    for (uint32_t i = 0; i < OUTPUT_COUNT; i++) {
        pin_mode_output(output_pins[i]);
    }
    all_outputs_off();

    /* The three unidentified inputs. The factory firmware leaves PB8/PB9
     * floating and pulls PB12 up; matching that avoids changing how whatever is
     * attached behaves.
     */
    pin_mode_input(0x18, 0);   /* PB8  floating */
    pin_mode_input(0x19, 0);   /* PB9  floating */
    pin_mode_input(0x1C, 1);   /* PB12 pull-up */
}

uint32_t gpio_istat_a(void) { return GPIO_ISTAT(GPIOA_BASE); }
uint32_t gpio_istat_b(void) { return GPIO_ISTAT(GPIOB_BASE); }
uint32_t gpio_istat_c(void) { return GPIO_ISTAT(GPIOC_BASE); }
uint32_t gpio_octl_a(void)  { return GPIO_OCTL(GPIOA_BASE); }
uint32_t gpio_octl_b(void)  { return GPIO_OCTL(GPIOB_BASE); }
uint32_t gpio_octl_c(void)  { return GPIO_OCTL(GPIOC_BASE); }
