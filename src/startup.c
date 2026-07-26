/* Minimal startup for GD32E23x (Cortex-M23).
 *
 * Vector table layout matches what we read out of the factory image: offsets
 * 0x10-0x28 are reserved on ARMv8-M baseline, so they stay zero.
 */

#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;

int  main(void);
void SysTick_Handler(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    while (dst < &_edata) {
        *dst++ = *src++;
    }

    for (dst = &_sbss; dst < &_ebss; dst++) {
        *dst = 0;
    }

    main();

    for (;;) {
        /* main() never returns */
    }
}

static void Default_Handler(void)
{
    for (;;) {
        /* park here; the debugger can see where we ended up */
    }
}

void NMI_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void SVC_Handler(void)       __attribute__((weak, alias("Default_Handler")));
void PendSV_Handler(void)    __attribute__((weak, alias("Default_Handler")));

__attribute__((section(".isr_vector"), used))
void (* const vector_table[])(void) = {
    (void (*)(void))&_estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    0, 0, 0, 0, 0, 0, 0,   /* reserved on ARMv8-M baseline */
    SVC_Handler,
    0, 0,
    PendSV_Handler,
    SysTick_Handler,
};
