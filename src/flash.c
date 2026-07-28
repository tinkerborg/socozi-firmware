#include "flash.h"

#include "gd32e23x.h"
#include "timing.h"

/* Generous against the datasheet's worst flash timing figure of 42 ms. This is
 * a backstop for a controller that never finished, not a duration anything is
 * expected to spend here.
 */
#define FMC_TIMEOUT_MS 200

static void unlock(void)
{
    if (FMC_CTL & FMC_CTL_LK) {
        FMC_KEY = FMC_UNLOCK_KEY0;
        FMC_KEY = FMC_UNLOCK_KEY1;
    }
}

static void lock(void)
{
    FMC_CTL |= FMC_CTL_LK;
}

/* Wait for the controller, then take and clear whatever it is reporting.
 * Returns 1 if the operation finished cleanly.
 */
static int finish(void)
{
    int ok = wait_clear(&FMC_STAT, FMC_STAT_BUSY, FMC_TIMEOUT_MS);

    if (FMC_STAT & FMC_STAT_ERRORS) {
        ok = 0;
    }

    /* Write-one-to-clear, including ENDF, so the next operation starts from a
     * clean status rather than reading this one's leftovers.
     */
    FMC_STAT = FMC_STAT_ERRORS | FMC_STAT_ENDF;

    return ok;
}

/* Through uintptr_t, or the cast warns wherever a pointer is wider than the
 * address. Only the target ever runs this, but it still has to compile clean
 * under the host compiler the tests use.
 */
static volatile uint32_t *at(uint32_t addr)
{
    return (volatile uint32_t *)(uintptr_t)addr;
}

uint32_t flash_read(uint32_t addr)
{
    return *at(addr);
}

int flash_write_word(uint32_t addr, uint32_t value)
{
    int ok;

    unlock();

    FMC_CTL |= FMC_CTL_PG;
    *at(addr) = value;

    ok = finish();

    FMC_CTL &= ~FMC_CTL_PG;
    lock();

    /* Verify rather than trust. The FMC register offsets here are the
     * STM32-alike layout and not confirmed against this silicon, so a readback
     * is what turns a wrong guess into a failed write instead of a corrupt
     * store.
     */
    return ok && flash_read(addr) == value;
}

int flash_erase_page(uint32_t addr)
{
    int ok;

    unlock();

    FMC_CTL |= FMC_CTL_PER;
    FMC_ADDR = addr;
    FMC_CTL |= FMC_CTL_START;

    ok = finish();

    FMC_CTL &= ~FMC_CTL_PER;
    lock();

    return ok && flash_read(addr) == 0xFFFFFFFFu;
}
