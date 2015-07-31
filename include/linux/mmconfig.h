#ifndef __MMCONFIG_H
#define __MMCONFIG_H
#ifdef __KERNEL__

#include <linux/types.h>
#include <linux/acpi.h>

#ifdef CONFIG_PCI_MMCONFIG
/* "PCI MMCONFIG %04x [bus %02x-%02x]" */
#define PCI_MMCFG_RESOURCE_NAME_LEN (22 + 4 + 2 + 2)

struct pci_mmcfg_region {
	struct list_head list;
	struct resource res;
	u64 address;
	char __iomem *virt;
	u16 segment;
	u8 start_bus;
	u8 end_bus;
	char name[PCI_MMCFG_RESOURCE_NAME_LEN];
};

void pci_mmcfg_early_init(void);
void pci_mmcfg_late_init(void);
struct pci_mmcfg_region *pci_mmconfig_lookup(int segment, int bus);

int pci_parse_mcfg(struct acpi_table_header *header);
struct pci_mmcfg_region *pci_mmconfig_alloc(int segment, int start,
						   int end, u64 addr);
int pci_mmconfig_inject(struct pci_mmcfg_region *cfg);
struct pci_mmcfg_region *pci_mmconfig_add(int segment, int start,
						 int end, u64 addr);
void list_add_sorted(struct pci_mmcfg_region *new);
int acpi_mcfg_check_entry(struct acpi_table_mcfg *mcfg,
			  struct acpi_mcfg_allocation *cfg);
void free_all_mmcfg(void);
int pci_mmconfig_insert(struct device *dev, u16 seg, u8 start, u8 end,
		       phys_addr_t addr);
int pci_mmconfig_delete(u16 seg, u8 start, u8 end);

/* Arch specific calls */
int pci_mmcfg_arch_init(void);
void pci_mmcfg_arch_free(void);
int pci_mmcfg_arch_map(struct pci_mmcfg_region *cfg);
void pci_mmcfg_arch_unmap(struct pci_mmcfg_region *cfg);
int pci_mmcfg_read(unsigned int seg, unsigned int bus,
		   unsigned int devfn, int reg, int len, u32 *value);
int pci_mmcfg_write(unsigned int seg, unsigned int bus,
		    unsigned int devfn, int reg, int len, u32 value);

extern struct list_head pci_mmcfg_list;

#define PCI_MMCFG_BUS_OFFSET(bus)      ((bus) << 20)
#else /* CONFIG_PCI_MMCONFIG */
static inline void pci_mmcfg_late_init(void) { }
static inline void pci_mmcfg_early_init(void) { }
static inline void *pci_mmconfig_lookup(int segment, int bus)
{ return NULL; }
#endif /* CONFIG_PCI_MMCONFIG */

#endif  /* __KERNEL__ */
#endif  /* __MMCONFIG_H */
