/*
 * Simple, generic PCI host controller driver targetting firmware-initialised
 * systems and virtual machines (e.g. the PCI emulation provided by kvmtool).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>

#include "pci-host-common.h"

static void __iomem *gen_pci_map_cfg_bus_cam(struct pci_bus *bus,
					     unsigned int devfn,
					     int where)
{
	struct gen_pci *pci = bus->sysdata;
	resource_size_t idx = bus->number - pci->cfg.bus_range->start;

	return pci->cfg.win[idx] + ((devfn << 8) | where);
}

static struct gen_pci_cfg_bus_ops gen_pci_cfg_cam_bus_ops = {
	.bus_shift	= 16,
	.ops		= {
		.map_bus	= gen_pci_map_cfg_bus_cam,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

static void __iomem *gen_pci_map_cfg_bus_ecam(struct pci_bus *bus,
					      unsigned int devfn,
					      int where)
{
	struct gen_pci *pci = bus->sysdata;
	resource_size_t idx = bus->number - pci->cfg.bus_range->start;

	return pci->cfg.win[idx] + ((devfn << 12) | where);
}

static struct gen_pci_cfg_bus_ops gen_pci_cfg_ecam_bus_ops = {
	.bus_shift	= 20,
	.ops		= {
		.map_bus	= gen_pci_map_cfg_bus_ecam,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

#ifdef CONFIG_PCI_HOST_THUNDER
int thunder_ecam_config_read(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 *val);
int thunder_ecam_config_write(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 val);
static struct gen_pci_cfg_bus_ops gen_pci_cfg_thunder_ecam_bus_ops = {
	.bus_shift	= 20,
	.ops		= {
		.map_bus	= gen_pci_map_cfg_bus_ecam,
		.read		= thunder_ecam_config_read,
		.write		= thunder_ecam_config_write,
	}
};
#endif

static void __iomem *gen_pci_map_cfg_bus_thunder_pem(struct pci_bus *bus,
						     unsigned int devfn,
						     int where)
{
	struct gen_pci *pci = bus->sysdata;
	resource_size_t idx = bus->number - pci->cfg.bus_range->start;

	/*
	 * Thunder PEM is a PCIe RC, but without a root bridge.  On
	 * the primary bus, ignore accesses for devices other than
	 * the first device.
	 */
	if (idx == 0 && (devfn & ~7u))
		return NULL;
	return pci->cfg.win[idx] + ((devfn << 16) | where);
}

static struct gen_pci_cfg_bus_ops gen_pci_cfg_thunder_pem_bus_ops = {
	.bus_shift	= 24,
	.ops		= {
		.map_bus	= gen_pci_map_cfg_bus_thunder_pem,
		.read		= pci_generic_config_read,
		.write		= pci_generic_config_write,
	}
};

static const struct of_device_id gen_pci_of_match[] = {
	{ .compatible = "pci-host-cam-generic",
	  .data = &gen_pci_cfg_cam_bus_ops },

	{ .compatible = "pci-host-ecam-generic",
	  .data = &gen_pci_cfg_ecam_bus_ops },

	{ .compatible = "cavium,pci-host-thunder-pem",
	  .data = &gen_pci_cfg_thunder_pem_bus_ops },
#ifdef CONFIG_PCI_HOST_THUNDER
	{ .compatible = "cavium,pci-host-thunder-ecam",
	  .data = &gen_pci_cfg_thunder_ecam_bus_ops },
#endif
	{ },
};
MODULE_DEVICE_TABLE(of, gen_pci_of_match);

static void gen_pci_release_of_pci_ranges(struct gen_pci *pci)
{
	pci_free_resource_list(&pci->resources);
}

static int gen_pci_parse_request_of_pci_ranges(struct gen_pci *pci)
{
	int err, res_valid = 0;
	struct device *dev = pci->host.dev.parent;
	struct device_node *np = dev->of_node;
	resource_size_t iobase;
	struct resource_entry *win;

	err = of_pci_get_host_bridge_resources(np, 0, 0xff, &pci->resources,
					       &iobase);
	if (err)
		return err;

	resource_list_for_each_entry(win, &pci->resources) {
		struct resource *parent, *res = win->res;

		switch (resource_type(res)) {
		case IORESOURCE_IO:
			parent = &ioport_resource;
			err = pci_remap_iospace(res, iobase);
			if (err) {
				dev_warn(dev, "error %d: failed to map resource %pR\n",
					 err, res);
				continue;
			}
			break;
		case IORESOURCE_MEM:
			parent = &iomem_resource;
			res_valid |= !(res->flags & IORESOURCE_PREFETCH);
			break;
		case IORESOURCE_BUS:
			pci->cfg.bus_range = res;
		default:
			continue;
		}

		err = devm_request_resource(dev, parent, res);
		if (err)
			goto out_release_res;
	}

	if (!res_valid) {
		dev_err(dev, "non-prefetchable memory resource required\n");
		err = -EINVAL;
		goto out_release_res;
	}

	return 0;

out_release_res:
	gen_pci_release_of_pci_ranges(pci);
	return err;
}

static int gen_pci_parse_map_cfg_windows(struct gen_pci *pci)
{
	int err;
	u8 bus_max;
	resource_size_t busn;
	struct resource *bus_range;
	struct device *dev = pci->host.dev.parent;
	struct device_node *np = dev->of_node;
	u32 sz = 1 << pci->cfg.ops->bus_shift;

	err = of_address_to_resource(np, 0, &pci->cfg.res);
	if (err) {
		dev_err(dev, "missing \"reg\" property\n");
		return err;
	}

	/* Limit the bus-range to fit within reg */
	bus_max = pci->cfg.bus_range->start +
		  (resource_size(&pci->cfg.res) >> pci->cfg.ops->bus_shift) - 1;
	pci->cfg.bus_range->end = min_t(resource_size_t,
					pci->cfg.bus_range->end, bus_max);

	pci->cfg.win = devm_kcalloc(dev, resource_size(pci->cfg.bus_range),
				    sizeof(*pci->cfg.win), GFP_KERNEL);
	if (!pci->cfg.win)
		return -ENOMEM;

	/* Map our Configuration Space windows */
	if (!devm_request_mem_region(dev, pci->cfg.res.start,
				     resource_size(&pci->cfg.res),
				     "Configuration Space"))
		return -ENOMEM;

	bus_range = pci->cfg.bus_range;
	for (busn = bus_range->start; busn <= bus_range->end; ++busn) {
		u32 idx = busn - bus_range->start;

		pci->cfg.win[idx] = devm_ioremap(dev,
						 pci->cfg.res.start + idx * sz,
						 sz);
		if (!pci->cfg.win[idx])
			return -ENOMEM;
	}

	return 0;
}

#ifdef CONFIG_KVM_ARM_VGIC
struct msi_chip *vgic_its_get_msi_node(struct pci_bus *bus, struct msi_chip *msi);
#endif

static int pcie_msi_enable_cb(struct pci_dev *dev, void *arg)
{
	if (dev->subordinate) {
		/* it is a bridge, propagate the msi */
		dev->subordinate->msi = dev->bus->msi;
	}
	return 0;
}

static int pcie_msi_enable(struct device_node *np, struct pci_bus *bus)
{
	struct device_node *msi_node;
#ifdef CONFIG_KVM_ARM_VGIC
	struct msi_chip *vits_msi;
#endif
	struct msi_controller *msi;

	msi_node = of_parse_phandle(np, "msi-parent", 0);
	if (!msi_node)
		return -ENODEV;

	msi = of_pci_find_msi_chip_by_node(msi_node);
	if (!msi)
		return -ENODEV;

#ifdef CONFIG_KVM_ARM_VGIC
	vits_msi = vgic_its_get_msi_node(bus, msi);

	msi->dev = bus->bridge->parent;
	bus->msi = vits_msi;
#else
	bus->msi = msi;
#endif
	pci_walk_bus(bus, pcie_msi_enable_cb, NULL);
	return 0;
}

int pci_host_common_probe(struct platform_device *pdev,
				 struct gen_pci *pci)
{
	int err;
	const char *type;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct pci_bus *bus, *child;

	type = of_get_property(np, "device_type", NULL);
	if (!type || strcmp(type, "pci")) {
		dev_err(dev, "invalid \"device_type\" %s\n", type);
		return -EINVAL;
	}

	of_pci_check_probe_only();

	pci->host.dev.parent = dev;
	INIT_LIST_HEAD(&pci->host.windows);
	INIT_LIST_HEAD(&pci->resources);

	/* Parse our PCI ranges and request their resources */
	err = gen_pci_parse_request_of_pci_ranges(pci);
	if (err)
		return err;

	/* Parse and map our Configuration Space windows */
	err = gen_pci_parse_map_cfg_windows(pci);
	if (err) {
		gen_pci_release_of_pci_ranges(pci);
		return err;
	}

	/* Do not reassign resource if probe only */
	if (!pci_has_flag(PCI_PROBE_ONLY))
		pci_add_flags(PCI_REASSIGN_ALL_RSRC | PCI_REASSIGN_ALL_BUS);


	bus = pci_scan_root_bus(dev, pci->cfg.bus_range->start,
				&pci->cfg.ops->ops, pci, &pci->resources);
	if (!bus) {
		dev_err(dev, "Scanning rootbus failed");
		return -ENODEV;
	}
	pcie_msi_enable(np, bus);

	pci_fixup_irqs(pci_common_swizzle, of_irq_parse_and_map_pci);

	if (!pci_has_flag(PCI_PROBE_ONLY)) {
		pci_bus_size_bridges(bus);
		pci_bus_assign_resources(bus);

		list_for_each_entry(child, &bus->children, node)
			pcie_bus_configure_settings(child);
	}

	pci_bus_add_devices(bus);
	return 0;
}

static int gen_pci_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct of_device_id *of_id;
	struct gen_pci *pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);

	if (!pci)
		return -ENOMEM;

	of_id = of_match_node(gen_pci_of_match, dev->of_node);
	set_dev_node(dev, of_node_to_nid(dev->of_node));
	pci->cfg.ops = (struct gen_pci_cfg_bus_ops *)of_id->data;

	return pci_host_common_probe(pdev, pci);
}

static struct platform_driver gen_pci_driver = {
	.driver = {
		.name = "pci-host-generic",
		.of_match_table = gen_pci_of_match,
	},
	.probe = gen_pci_probe,
};
module_platform_driver(gen_pci_driver);

MODULE_DESCRIPTION("Generic PCI host driver");
MODULE_AUTHOR("Will Deacon <will.deacon@arm.com>");
MODULE_LICENSE("GPL v2");
