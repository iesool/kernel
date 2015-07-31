/*
 * PCIe host controller driver for Cavium Thunder SOC
 *
 * Copyright (C) 2014, 2015 Cavium Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/msi.h>
#include <linux/irqchip/arm-gic-v3.h>

#define PCI_DEVICE_ID_THUNDER_BRIDGE	0xa002

#define THUNDER_PCIE_BUS_SHIFT		20
#define THUNDER_PCIE_DEV_SHIFT		15
#define THUNDER_PCIE_FUNC_SHIFT		12

#define THUNDER_ECAM0_CFG_BASE		0x848000000000
#define THUNDER_ECAM1_CFG_BASE		0x849000000000
#define THUNDER_ECAM2_CFG_BASE		0x84a000000000
#define THUNDER_ECAM3_CFG_BASE		0x84b000000000
#define THUNDER_ECAM4_CFG_BASE		0x948000000000
#define THUNDER_ECAM5_CFG_BASE		0x949000000000
#define THUNDER_ECAM6_CFG_BASE		0x94a000000000
#define THUNDER_ECAM7_CFG_BASE		0x94b000000000

struct thunder_pcie {
	struct device_node	*node;
	struct device		*dev;
	void __iomem		*cfg_base;
	struct msi_controller	*msi;
	int			ecam;
	bool			valid;
};

int thunder_pem_requester_id(struct pci_dev *dev);

static atomic_t thunder_pcie_ecam_probed;

static u32 pci_requester_id_ecam(struct pci_dev *dev)
{
	return (((pci_domain_nr(dev->bus) >> 2) << 19) |
		((pci_domain_nr(dev->bus) % 4) << 16) |
		(dev->bus->number << 8) | dev->devfn);
}

static u32 thunder_pci_requester_id(struct pci_dev *dev, u16 alias)
{
	int ret;

	ret = thunder_pem_requester_id(dev);
	if (ret >= 0)
		return (u32)ret;

	return pci_requester_id_ecam(dev);
}

/*
 * This bridge is just for the sake of supporting ARI for
 * downstream devices. No resources are attached to it.
 * Copy upstream root bus resources to bridge which aide in
 * resource claiming for downstream devices
 */
static void pci_bridge_resource_fixup(struct pci_dev *dev)
{
	struct pci_bus *bus;
	int resno;

	bus = dev->subordinate;
	for (resno = 0; resno < PCI_BRIDGE_RESOURCE_NUM; resno++) {
		bus->resource[resno] = pci_bus_resource_n(bus->parent,
			PCI_BRIDGE_RESOURCE_NUM + resno);
	}

	for (resno = PCI_BRIDGE_RESOURCES;
		resno <= PCI_BRIDGE_RESOURCE_END; resno++) {
		dev->resource[resno].start = dev->resource[resno].end = 0;
		dev->resource[resno].flags = 0;
	}
}
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_THUNDER_BRIDGE,
			pci_bridge_resource_fixup);

/*
 * All PCIe devices in Thunder have fixed resources, shouldn't be reassigned.
 * Also claim the device's valid resources to set 'res->parent' hierarchy.
 */
static void pci_dev_resource_fixup(struct pci_dev *dev)
{
	struct resource *res;
	int resno;

	/*
	 * If the ECAM is not yet probed, we must be in a virtual
	 * machine.  In that case, don't mark things as
	 * IORESOURCE_PCI_FIXED
	 */
	if (!atomic_read(&thunder_pcie_ecam_probed))
		return;

	for (resno = 0; resno < PCI_NUM_RESOURCES; resno++)
		dev->resource[resno].flags |= IORESOURCE_PCI_FIXED;

	for (resno = 0; resno < PCI_BRIDGE_RESOURCES; resno++) {
		res = &dev->resource[resno];
		if (res->parent || !(res->flags & IORESOURCE_MEM))
			continue;
		pci_claim_resource(dev, resno);
	}
}
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_CAVIUM, PCI_ANY_ID,
			pci_dev_resource_fixup);

static void __iomem *thunder_pcie_get_cfg_addr(struct thunder_pcie *pcie,
					       unsigned int busnr,
					       unsigned int devfn, int reg)
{
	return  pcie->cfg_base +
		((busnr << THUNDER_PCIE_BUS_SHIFT)
		 | (PCI_SLOT(devfn) << THUNDER_PCIE_DEV_SHIFT)
		 | (PCI_FUNC(devfn) << THUNDER_PCIE_FUNC_SHIFT)) + reg;
}

static int thunder_pcie_read_config(struct pci_bus *bus, unsigned int devfn,
				int reg, int size, u32 *val)
{
	struct thunder_pcie *pcie = bus->sysdata;
	void __iomem *addr;
	unsigned int busnr = bus->number;

	if (busnr > 255 || devfn > 255 || reg > 4095)
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = thunder_pcie_get_cfg_addr(pcie, busnr, devfn, reg);

	switch (size) {
	case 1:
		*val = readb(addr);
		break;
	case 2:
		*val = readw(addr);
		break;
	case 4:
		*val = readl(addr);
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	return PCIBIOS_SUCCESSFUL;
}

static int thunder_pcie_write_config(struct pci_bus *bus, unsigned int devfn,
				  int reg, int size, u32 val)
{
	struct thunder_pcie *pcie = bus->sysdata;
	void __iomem *addr;
	unsigned int busnr = bus->number;

	if (busnr > 255 || devfn > 255 || reg > 4095)
		return PCIBIOS_DEVICE_NOT_FOUND;

	addr = thunder_pcie_get_cfg_addr(pcie, busnr, devfn, reg);

	switch (size) {
	case 1:
		writeb(val, addr);
		break;
	case 2:
		writew(val, addr);
		break;
	case 4:
		writel(val, addr);
		break;
	default:
		return PCIBIOS_BAD_REGISTER_NUMBER;
	}

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops thunder_pcie_ops = {
	.read	= thunder_pcie_read_config,
	.write	= thunder_pcie_write_config,
};

static int thunder_pcie_msi_enable(struct thunder_pcie *pcie,
					struct pci_bus *bus)
{
	struct device_node *msi_node;

	msi_node = of_parse_phandle(pcie->node, "msi-parent", 0);
	if (!msi_node)
		return -ENODEV;

	pcie->msi = of_pci_find_msi_chip_by_node(msi_node);
	if (!pcie->msi)
		return -ENODEV;

	pcie->msi->dev = pcie->dev;
	bus->msi = pcie->msi;

	return 0;
}

static void thunder_pcie_config(struct thunder_pcie *pcie, u64 addr)
{
	atomic_set(&thunder_pcie_ecam_probed, 1);
	set_its_pci_requester_id(thunder_pci_requester_id);

	pcie->valid = true;

	switch (addr) {
	case THUNDER_ECAM0_CFG_BASE:
		pcie->ecam = 0;
		break;
	case THUNDER_ECAM1_CFG_BASE:
		pcie->ecam = 1;
		break;
	case THUNDER_ECAM2_CFG_BASE:
		pcie->ecam = 2;
		break;
	case THUNDER_ECAM3_CFG_BASE:
		pcie->ecam = 3;
		break;
	case THUNDER_ECAM4_CFG_BASE:
		pcie->ecam = 4;
		break;
	case THUNDER_ECAM5_CFG_BASE:
		pcie->ecam = 5;
		break;
	case THUNDER_ECAM6_CFG_BASE:
		pcie->ecam = 6;
		break;
	case THUNDER_ECAM7_CFG_BASE:
		pcie->ecam = 7;
		break;
	default:
		pcie->valid = false;
		break;
	}
}

static int thunder_pcie_probe(struct platform_device *pdev)
{
	struct thunder_pcie *pcie;
	struct resource *cfg_base;
	struct pci_bus *bus;
	int ret = 0;
	LIST_HEAD(res);

	pcie = devm_kzalloc(&pdev->dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->node = of_node_get(pdev->dev.of_node);
	pcie->dev = &pdev->dev;

	/* Get controller's configuration space range */
	cfg_base = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	thunder_pcie_config(pcie, cfg_base->start);

	pcie->cfg_base = devm_ioremap_resource(&pdev->dev, cfg_base);
	if (IS_ERR(pcie->cfg_base)) {
		ret = PTR_ERR(pcie->cfg_base);
		goto err_ioremap;
	}

	dev_info(&pdev->dev, "ECAM%d CFG BASE 0x%llx\n",
		 pcie->ecam, (u64)cfg_base->start);

	ret = of_pci_get_host_bridge_resources(pdev->dev.of_node,
					0, 255, &res, NULL);
	if (ret)
		goto err_root_bus;

	bus = pci_create_root_bus(&pdev->dev, 0, &thunder_pcie_ops, pcie, &res);
	if (!bus) {
		ret = -ENODEV;
		goto err_root_bus;
	}

	/* Set reference to MSI chip */
	ret = thunder_pcie_msi_enable(pcie, bus);
	if (ret) {
		dev_err(&pdev->dev,
			"Unable to set reference to MSI chip: ret=%d\n", ret);
		goto err_msi;
	}

	platform_set_drvdata(pdev, pcie);

	pci_scan_child_bus(bus);
	pci_bus_add_devices(bus);

	return 0;
err_msi:
	pci_remove_root_bus(bus);
err_root_bus:
	pci_free_resource_list(&res);
err_ioremap:
	of_node_put(pcie->node);
	return ret;
}

static const struct of_device_id thunder_pcie_of_match[] = {
	{ .compatible = "cavium,thunder-pcie", },
	{},
};
MODULE_DEVICE_TABLE(of, thunder_pcie_of_match);

static struct platform_driver thunder_pcie_driver = {
	.driver = {
		.name = "thunder-pcie",
		.owner = THIS_MODULE,
		.of_match_table = thunder_pcie_of_match,
	},
	.probe = thunder_pcie_probe,
};
module_platform_driver(thunder_pcie_driver);

MODULE_AUTHOR("Sunil Goutham");
MODULE_DESCRIPTION("Cavium Thunder ECAM host controller driver");
MODULE_LICENSE("GPL v2");

