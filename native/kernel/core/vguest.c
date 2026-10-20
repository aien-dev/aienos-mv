/* vguest.c -- see vguest.h. */
#include "vguest.h"

static unsigned fw_el;

void ck_vguest_set_firmware_el(unsigned el)
{
    fw_el = el;
}

int ck_vguest(void)
{
#if defined(CK_VM_GUEST) && CK_VM_GUEST == 1
    return fw_el == 1;
#else
    return 0;
#endif
}

int ck_vguest_dma_device(uint16_t vendor, uint16_t device)
{
    if (!ck_vguest())
        return 0;
    return vendor == 0x1af4 || (vendor == 0x106b && device == 0x1a09) || (vendor == 0x1b36 && device == 0x0010);
}
