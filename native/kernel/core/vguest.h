/* vguest.h -- the VM edition (CK_VM_GUEST=1 images, aienos-run on macOS,
 * Linux and Windows). Under a hypervisor every PCI device it offers is its own
 * emulation, which already reads and writes all guest memory: an SMMU window
 * would confine nothing the hypervisor cannot already touch, and Apple
 * Virtualization.framework has no SMMU and no RNDR at all. So a VM edition
 * image, and only while firmware handed over at EL1 (a hypervisor is below
 * the kernel), lets the hypervisor's devices do DMA without an SMMU window
 * and draws kernel entropy from the hypervisor's virtio entropy device.
 * Hardware images never carry this: the Makefile refuses CK_VM_GUEST with
 * CK_HARDWARE_STAGING, CK_QEMU_UNSAFE_DMA and the Pi images, and every grant
 * is printed ("dma_gate: <dev> granted (HypervisorDevice ...)"). */
#ifndef AIENOS_CK_VGUEST_H
#define AIENOS_CK_VGUEST_H
#include <stdint.h>
/* kmain records the firmware exception level once, before the stages. */
void ck_vguest_set_firmware_el(unsigned el);
/* 1 in a CK_VM_GUEST=1 image booted with firmware at EL1; else 0. */
int ck_vguest(void);
/* 1 when ck_vguest() and vendor:device is a hypervisor-emulated device this
 * image may give DMA without an SMMU window: virtio (0x1af4, any device id)
 * or the Apple Virtualization.framework NVMe controller (0x106b:0x1a09). */
int ck_vguest_dma_device(uint16_t vendor, uint16_t device);
#endif
