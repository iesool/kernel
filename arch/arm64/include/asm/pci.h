#ifndef __ASM_PCI_H
#define __ASM_PCI_H
#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>

#include <asm/io.h>
#include <asm-generic/pci-bridge.h>
#include <asm-generic/pci-dma-compat.h>

#define PCIBIOS_MIN_IO		0x1000
#define PCIBIOS_MIN_MEM		0

/*
 * Set to 1 if the kernel should re-assign all PCI bus numbers
 */
#define pcibios_assign_all_busses() \
	(pci_has_flag(PCI_REASSIGN_ALL_BUS))

/*
 * PCI address space differs from physical memory address space
 */
#define PCI_DMA_BUS_IS_PHYS	(0)

extern int isa_dma_bridge_buggy;

#ifdef CONFIG_PCI

#ifdef CONFIG_ACPI
struct pci_controller {
	struct acpi_device *companion;
	int segment;
	int node;		/* nearest node with memory or NUMA_NO_NODE for global allocation */
};

#define PCI_CONTROLLER(busdev) ((struct pci_controller *) busdev->sysdata)

/*
 * ARM64 PCI config space access primitives.
 */
static inline unsigned char mmio_config_readb(void __iomem *pos)
{
	return readb(pos);
}

static inline unsigned short mmio_config_readw(void __iomem *pos)
{
	return readw(pos);
}

static inline unsigned int mmio_config_readl(void __iomem *pos)
{
	return readl(pos);
}

static inline void mmio_config_writeb(void __iomem *pos, u8 val)
{
	writeb(val, pos);
}

static inline void mmio_config_writew(void __iomem *pos, u16 val)
{
	writew(val, pos);
}

static inline void mmio_config_writel(void __iomem *pos, u32 val)
{
	writel(val, pos);
}
#endif  /* CONFIG_ACPI */

static inline int pci_get_legacy_ide_irq(struct pci_dev *dev, int channel)
{
	/* no legacy IRQ on arm64 */
	return -ENODEV;
}

static inline int pci_proc_domain(struct pci_bus *bus)
{
	return 1;
}

void set_pcibios_add_device(int (*arg)(struct pci_dev *));

#endif  /* CONFIG_PCI */

#endif  /* __KERNEL__ */
#endif  /* __ASM_PCI_H */
