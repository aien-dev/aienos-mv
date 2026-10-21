// SPDX-License-Identifier: GPL-2.0-only
/* Linux host shim only: a module that uses Linux GPL-only symbols must be GPLv2. The driver core in native/kernel/gpu stays AGPL. */
/*
 * aien-gpu: Linux host shim for our own GB10 GPU driver (L332 rung 2, windows 0 and 1).
 *
 * What it does, and nothing more:
 *   - binds the GB10 (10de:2e12) ONLY when loaded with allow_bind=1 (nvidia.ko must be unloaded first:
 *     two drivers on one GPU is the one thing that must never happen);
 *   - maps BAR0 and READS two identity registers (NV_PMC_BOOT_0 at 0x0, NV_PMC_BOOT_42 at 0xA00): no write;
 *   - loads the GSP firmware file on loan from NVIDIA (nvidia/580.173.02/gsp_ga10x.bin) and checks its size;
 *     it does not send it to the GPU yet;
 *   - allocates one DMA-coherent arena (arena_mib, default 2) and offers it to user space by mmap of /dev/aien-gpu,
 *     the shape of the memory interface Omega gets (one big mapping);
 *   - AIEN_GPU_INFO ioctl returns what it read;
 *   - window 1: a second DMA-coherent buffer for the GSP sysmem heap (heap_mib, default 0, never mapped to user space)
 *     and AIEN_GPU_REG_RD / AIEN_GPU_REG_WR for exactly the registers of the GSP start-up contract
 *     (gpu/contract/gb10_gsp.devc). Writes need a second switch, allow_boot=1, and the SEC2 doorbell (QUEUE_HEAD(0))
 *     can be rung once per load: a failed start-up is never retried without a reset.
 * The start-up logic itself (gpu/gsp_mmio.c, dev/gb10_gsp_boot.c) runs in user space (aien_gsp_boot) over these two
 * ioctls, so the driver core stays AGPL and outside this GPL module. GB10B has no FSP: SEC2 takes the start-up packet.
 * What comes next (not here): the RPC subset (docs/numeric/DRIVER_FLOOR.md), the channel from
 * gpu/contract/gb10_chan.devc, Omega's typed handles (L202). Each is its own window.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/firmware.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/mutex.h>

#include "aien_gpu_uapi.h"

#define AIEN_GPU_FW "nvidia/580.173.02/gsp_ga10x.bin"
#define AIEN_GPU_FW_BYTES 75012080u /* /lib/firmware/nvidia/580.173.02/gsp_ga10x.bin, L101 driver floor */
#define NV_PMC_BOOT_0 0x00000000u   /* ampere/ga100/dev_boot.h:27 */
#define NV_PMC_BOOT_42 0x00000a00u

#define AIEN_GPU_DMA_BITS 40         /* nvidia.ko logs "Using 40-bit DMA addresses" for this GPU */

/* The registers user space may reach (BAR0 offsets). Reads: the GSP start-up contract's registers except the
 * write-only data port, plus the two identity registers. Writes: the six the start-up and the notify write.
 * evidence/own-gpu-driver/rung2/window1_sim_test.sh checks these lists against every access the start-up tool
 * makes on the simulator.
 * AIEN-GPU-ALLOW-BEGIN */
static const u32 aien_gpu_rd_ok[] = {
	0x00000000, 0x00000a00,                                       /* PMC_BOOT_0, PMC_BOOT_42 */
	0x00840040, 0x00840044, 0x00840ac0, 0x00840c00, 0x00840c04, /* SEC2 MAILBOX0/1, EMEMC0, QHEAD0, QTAIL0 */
	0x00110040, 0x00110044, 0x00110080, 0x001100f4, 0x00110c00, 0x00111388, /* GSP MAILBOX0/1, OS, HWCFG2, QHEAD0, CPUCTL */
};
static const u32 aien_gpu_wr_ok[] = {
	0x00840ac0, 0x00840ac4, 0x00840c00, 0x00840c04, /* SEC2 EMEMC0, EMEMD0, QHEAD0 (doorbell), QTAIL0 */
	0x00110080, 0x00110c00,                         /* GSP FALCON_OS, GSP QHEAD0 (command-queue notify) */
};
/* AIEN-GPU-ALLOW-END */
#define AIEN_GPU_SEC2_DOORBELL 0x00840c00u

static bool allow_bind;
module_param(allow_bind, bool, 0444);
MODULE_PARM_DESC(allow_bind, "bind the GB10 (only after nvidia.ko is unloaded; default off)");
static uint arena_mib = 2;
module_param(arena_mib, uint, 0444);
MODULE_PARM_DESC(arena_mib, "DMA-coherent arena size in MiB (default 2)");
static uint heap_mib;
module_param(heap_mib, uint, 0444);
MODULE_PARM_DESC(heap_mib, "GSP sysmem heap in MiB, chip only (default 0 = none; window 1 uses 512)");
static bool allow_boot;
module_param(allow_boot, bool, 0444);
MODULE_PARM_DESC(allow_boot, "allow the GSP start-up register writes (second switch after allow_bind; default off)");

static struct {
	struct pci_dev *pdev;
	void __iomem *bar0;
	void *arena;
	dma_addr_t arena_iova;
	size_t arena_bytes;
	void *heap;
	dma_addr_t heap_iova;
	size_t heap_bytes;
	struct aien_gpu_info info;
	struct mutex lock;      /* register access and the doorbell latch */
	atomic_t open;          /* one opener at a time */
} g;

static bool aien_gpu_listed(const u32 *list, size_t n, u32 off)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (list[i] == off)
			return true;
	return false;
}

static long aien_gpu_reg(unsigned int cmd, struct aien_gpu_reg __user *u)
{
	struct aien_gpu_reg r;
	long rc = 0;

	if (copy_from_user(&r, u, sizeof(r)))
		return -EFAULT;
	mutex_lock(&g.lock);
	if (cmd == AIEN_GPU_REG_RD) {
		if (!aien_gpu_listed(aien_gpu_rd_ok, ARRAY_SIZE(aien_gpu_rd_ok), r.off))
			rc = -EPERM;
		else
			r.val = ioread32(g.bar0 + r.off);
	} else if (!allow_boot || !aien_gpu_listed(aien_gpu_wr_ok, ARRAY_SIZE(aien_gpu_wr_ok), r.off)) {
		rc = -EPERM;
	} else if (r.off == AIEN_GPU_SEC2_DOORBELL && g.info.doorbells) {
		rc = -EALREADY;         /* one start-up attempt per load: never retried without a reset */
	} else {
		iowrite32(r.val, g.bar0 + r.off);
		if (r.off == AIEN_GPU_SEC2_DOORBELL) {
			g.info.doorbells++;
			dev_info(&g.pdev->dev, "aien-gpu: SEC2 doorbell rung (GSP start-up packet handed over)\n");
		}
	}
	mutex_unlock(&g.lock);
	if (rc)
		return rc;
	return cmd == AIEN_GPU_REG_RD && copy_to_user(u, &r, sizeof(r)) ? -EFAULT : 0;
}

static long aien_gpu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case AIEN_GPU_INFO:
		return copy_to_user((void __user *)arg, &g.info, sizeof(g.info)) ? -EFAULT : 0;
	case AIEN_GPU_REG_RD:
	case AIEN_GPU_REG_WR:
		return aien_gpu_reg(cmd, (struct aien_gpu_reg __user *)arg);
	}
	return -ENOTTY;
}

static int aien_gpu_open(struct inode *i, struct file *f)
{
	return atomic_cmpxchg(&g.open, 0, 1) ? -EBUSY : 0;
}

static int aien_gpu_release(struct inode *i, struct file *f)
{
	atomic_set(&g.open, 0);
	return 0;
}

static int aien_gpu_mmap(struct file *f, struct vm_area_struct *vma)
{
	size_t len = vma->vm_end - vma->vm_start;

	if (!g.arena || vma->vm_pgoff || len > g.arena_bytes)
		return -EINVAL;
	return dma_mmap_coherent(&g.pdev->dev, vma, g.arena, g.arena_iova, len);
}

static const struct file_operations aien_gpu_fops = {
	.owner = THIS_MODULE,
	.open = aien_gpu_open,
	.release = aien_gpu_release,
	.unlocked_ioctl = aien_gpu_ioctl,
	.mmap = aien_gpu_mmap,
};

static struct miscdevice aien_gpu_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "aien-gpu",
	.fops = &aien_gpu_fops,
	.mode = 0600,
};

static int aien_gpu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	const struct firmware *fw;
	int rc;

	if (!allow_bind) {
		dev_info(&pdev->dev, "aien-gpu: allow_bind=0, not binding\n");
		return -ENODEV;
	}
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;
	pci_set_master(pdev);
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(AIEN_GPU_DMA_BITS));
	if (rc)
		return rc;
	g.info.dma_bits = AIEN_GPU_DMA_BITS;
	g.info.allow_boot = allow_boot;
	g.info.bar0_pa = pci_resource_start(pdev, 0);
	mutex_init(&g.lock);
	g.bar0 = pcim_iomap(pdev, 0, 0);
	if (!g.bar0)
		return -ENOMEM;
	g.pdev = pdev;
	g.info.bar0_bytes = pci_resource_len(pdev, 0);
	g.info.boot0 = ioread32(g.bar0 + NV_PMC_BOOT_0);
	g.info.boot42 = ioread32(g.bar0 + NV_PMC_BOOT_42);
	dev_info(&pdev->dev, "aien-gpu: BAR0 %llu bytes, PMC_BOOT_0 %08x PMC_BOOT_42 %08x\n",
		 g.info.bar0_bytes, g.info.boot0, g.info.boot42);

	rc = request_firmware(&fw, AIEN_GPU_FW, &pdev->dev);
	if (!rc) {
		g.info.fw_bytes = fw->size;
		g.info.fw_ok = fw->size == AIEN_GPU_FW_BYTES;
		release_firmware(fw);
	}
	dev_info(&pdev->dev, "aien-gpu: firmware %s rc %d bytes %llu ok %u\n", AIEN_GPU_FW, rc, g.info.fw_bytes, g.info.fw_ok);

	g.arena_bytes = (size_t)arena_mib << 20;
	g.arena = dma_alloc_coherent(&pdev->dev, g.arena_bytes, &g.arena_iova, GFP_KERNEL);
	if (!g.arena)
		return -ENOMEM;
	g.info.arena_bytes = g.arena_bytes;
	g.info.arena_iova = g.arena_iova;
	g.heap_bytes = (size_t)heap_mib << 20;
	if (g.heap_bytes) {
		g.heap = dma_alloc_coherent(&pdev->dev, g.heap_bytes, &g.heap_iova, GFP_KERNEL);
		if (!g.heap) {
			rc = -ENOMEM;
			goto free_arena;
		}
		g.info.heap_bytes = g.heap_bytes;
		g.info.heap_iova = g.heap_iova;
	}
	rc = misc_register(&aien_gpu_misc);
	if (rc)
		goto free_heap;
	dev_info(&pdev->dev, "aien-gpu: /dev/aien-gpu ready, arena %zu bytes at iova %pad, heap %zu bytes at iova %pad, allow_boot %d\n",
		 g.arena_bytes, &g.arena_iova, g.heap_bytes, &g.heap_iova, allow_boot);
	return 0;
free_heap:
	if (g.heap)
		dma_free_coherent(&pdev->dev, g.heap_bytes, g.heap, g.heap_iova);
	g.heap = NULL;
free_arena:
	dma_free_coherent(&pdev->dev, g.arena_bytes, g.arena, g.arena_iova);
	g.arena = NULL;
	return rc;
}

static void aien_gpu_remove(struct pci_dev *pdev)
{
	misc_deregister(&aien_gpu_misc);
	if (g.info.doorbells) {
		/* GSP may still read and write the arena and the heap: keep both (leaked until reboot) rather than hand
		 * memory back to Linux under a running DMA master. The window-1 runbook reboots afterwards. */
		dev_warn(&pdev->dev, "aien-gpu: GSP start-up was attempted; arena and heap kept until reboot\n");
		g.heap = NULL;
		g.arena = NULL;
	}
	if (g.heap)
		dma_free_coherent(&pdev->dev, g.heap_bytes, g.heap, g.heap_iova);
	g.heap = NULL;
	if (g.arena)
		dma_free_coherent(&pdev->dev, g.arena_bytes, g.arena, g.arena_iova);
	g.arena = NULL;
	g.pdev = NULL;
}

static const struct pci_device_id aien_gpu_ids[] = {
	{ PCI_DEVICE(0x10de, 0x2e12) }, /* GB10, lspci -nn on the Spark 2026-10-21 */
	{ }
};
MODULE_DEVICE_TABLE(pci, aien_gpu_ids);

static struct pci_driver aien_gpu_driver = {
	.name = "aien-gpu",
	.id_table = aien_gpu_ids,
	.probe = aien_gpu_probe,
	.remove = aien_gpu_remove,
};
module_pci_driver(aien_gpu_driver);

MODULE_DESCRIPTION("AIEN own GB10 GPU driver, Linux host shim (L332)");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(AIEN_GPU_FW);
