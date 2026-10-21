// SPDX-License-Identifier: GPL-2.0-only
/* Linux host shim only: a module that uses Linux GPL-only symbols must be GPLv2. The driver core in native/kernel/gpu stays AGPL. */
/*
 * aien-gpu: Linux host shim for our own GB10 GPU driver (L332 rung 2, step "window 0").
 *
 * What it does today, and nothing more:
 *   - binds the GB10 (10de:2e12) ONLY when loaded with allow_bind=1 (nvidia.ko must be unloaded first:
 *     two drivers on one GPU is the one thing that must never happen);
 *   - maps BAR0 and READS two identity registers (NV_PMC_BOOT_0 at 0x0, NV_PMC_BOOT_42 at 0xA00): no write;
 *   - loads the GSP firmware file on loan from NVIDIA (nvidia/580.173.02/gsp_ga10x.bin) and checks its size;
 *     it does not send it to the GPU yet;
 *   - allocates one DMA-coherent arena (arena_mib, default 2) and offers it to user space by mmap of /dev/aien-gpu,
 *     the shape of the memory interface Omega gets (one big mapping);
 *   - AIEN_GPU_INFO ioctl returns what it read.
 * What comes next (not here): GSP boot through FSP (L192 builder), the RPC subset (docs/numeric/DRIVER_FLOOR.md),
 * the channel from gpu/contract/gb10_chan.devc, and Omega's typed handles (L202). Each is its own window.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/firmware.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/io.h>

#define AIEN_GPU_FW "nvidia/580.173.02/gsp_ga10x.bin"
#define AIEN_GPU_FW_BYTES 75012080u /* /lib/firmware/nvidia/580.173.02/gsp_ga10x.bin, L101 driver floor */
#define NV_PMC_BOOT_0 0x00000000u   /* ampere/ga100/dev_boot.h:27 */
#define NV_PMC_BOOT_42 0x00000a00u

struct aien_gpu_info {
	__u32 boot0, boot42;
	__u64 bar0_bytes;
	__u64 arena_bytes, arena_iova;
	__u64 fw_bytes;
	__u32 fw_ok, pad;
};
#define AIEN_GPU_INFO _IOR('A', 0x01, struct aien_gpu_info)

static bool allow_bind;
module_param(allow_bind, bool, 0444);
MODULE_PARM_DESC(allow_bind, "bind the GB10 (only after nvidia.ko is unloaded; default off)");
static uint arena_mib = 2;
module_param(arena_mib, uint, 0444);
MODULE_PARM_DESC(arena_mib, "DMA-coherent arena size in MiB (default 2)");

static struct {
	struct pci_dev *pdev;
	void __iomem *bar0;
	void *arena;
	dma_addr_t arena_iova;
	size_t arena_bytes;
	struct aien_gpu_info info;
} g;

static long aien_gpu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	if (cmd != AIEN_GPU_INFO)
		return -ENOTTY;
	return copy_to_user((void __user *)arg, &g.info, sizeof(g.info)) ? -EFAULT : 0;
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
	rc = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40)); /* nvidia.ko logs "Using 40-bit DMA addresses" here */
	if (rc)
		return rc;
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
	rc = misc_register(&aien_gpu_misc);
	if (rc) {
		dma_free_coherent(&pdev->dev, g.arena_bytes, g.arena, g.arena_iova);
		g.arena = NULL;
		return rc;
	}
	dev_info(&pdev->dev, "aien-gpu: /dev/aien-gpu ready, arena %zu bytes at iova %pad\n", g.arena_bytes, &g.arena_iova);
	return 0;
}

static void aien_gpu_remove(struct pci_dev *pdev)
{
	misc_deregister(&aien_gpu_misc);
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
