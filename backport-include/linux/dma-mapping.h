#ifndef __BACKPORT_LINUX_DMA_MAPPING_H
#define __BACKPORT_LINUX_DMA_MAPPING_H
#include_next <linux/dma-mapping.h>
#include <linux/version.h>

#ifdef BPM_PCI_DMA_COMPAT_H_NOT_PRESENT
#include <linux/pci.h>

#define PCI_DMA_BIDIRECTIONAL  DMA_BIDIRECTIONAL
#define PCI_DMA_TODEVICE       DMA_TO_DEVICE
#define PCI_DMA_FROMDEVICE     DMA_FROM_DEVICE
#define PCI_DMA_NONE           DMA_NONE

#ifdef CONFIG_PCI
static inline int pci_set_dma_mask(struct pci_dev *dev, u64 mask)
{
	return dma_set_mask(&dev->dev, mask);
}

static inline int pci_set_consistent_dma_mask(struct pci_dev *dev, u64 mask)
{
	return dma_set_coherent_mask(&dev->dev, mask);
}
#else
static inline int pci_set_dma_mask(struct pci_dev *dev, u64 mask)
{ return -EIO; }
static inline int pci_set_consistent_dma_mask(struct pci_dev *dev, u64 mask)
{ return -EIO; }
#endif

#endif


#if LINUX_VERSION_IS_LESS(3,2,0)
#define dma_zalloc_coherent LINUX_I915_BACKPORT(dma_zalloc_coherent)
static inline void *dma_zalloc_coherent(struct device *dev, size_t size,
					dma_addr_t *dma_handle, gfp_t flag)
{
	void *ret = dma_alloc_coherent(dev, size, dma_handle, flag);
	if (ret)
		memset(ret, 0, size);
	return ret;
}
#endif

#if LINUX_VERSION_IS_LESS(3,13,0)
/*
 * Set both the DMA mask and the coherent DMA mask to the same thing.
 * Note that we don't check the return value from dma_set_coherent_mask()
 * as the DMA API guarantees that the coherent DMA mask can be set to
 * the same or smaller than the streaming DMA mask.
 */
#define dma_set_mask_and_coherent LINUX_I915_BACKPORT(dma_set_mask_and_coherent)
static inline int dma_set_mask_and_coherent(struct device *dev, u64 mask)
{
	int rc = dma_set_mask(dev, mask);
	if (rc == 0)
		dma_set_coherent_mask(dev, mask);
	return rc;
}
#endif /* LINUX_VERSION_IS_LESS(3,13,0) */

#endif /* __BACKPORT_LINUX_DMA_MAPPING_H */
