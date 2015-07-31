/*
 * Code borrowed from powerpc/kernel/pci-common.c and arch/ia64/pci/pci.c
 *
 * Copyright (c) 2002, 2005 Hewlett-Packard Development Company, L.P.
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *	Bjorn Helgaas <bjorn.helgaas@hp.com>
 * Copyright (C) 2004 Silicon Graphics, Inc.
 * Copyright (C) 2003 Anton Blanchard <anton@au.ibm.com>, IBM
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iort.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmconfig.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/slab.h>

#include <asm/pci-bridge.h>

int pcibios_root_bridge_prepare(struct pci_host_bridge *bridge)
{
	ACPI_COMPANION_SET(&bridge->dev,
			   PCI_CONTROLLER(bridge->bus)->companion);

	return 0;
}

void pcibios_add_bus(struct pci_bus *bus)
{
	acpi_pci_add_bus(bus);
}

void pcibios_remove_bus(struct pci_bus *bus)
{
	acpi_pci_remove_bus(bus);
}

int
pcibios_enable_device (struct pci_dev *dev, int mask)
{
	int ret;

	ret = pci_enable_resources(dev, mask);
	if (ret < 0)
		return ret;

	if (!dev->msi_enabled)
		return acpi_pci_irq_enable(dev);
	return 0;
}

void
pcibios_disable_device (struct pci_dev *dev)
{
	BUG_ON(atomic_read(&dev->enable_cnt));
	if (!dev->msi_enabled)
		acpi_pci_irq_disable(dev);
}

static int pci_read(struct pci_bus *bus, unsigned int devfn, int where,
		    int size, u32 *value)
{
	return raw_pci_read(pci_domain_nr(bus), bus->number,
			    devfn, where, size, value);
}

static int pci_write(struct pci_bus *bus, unsigned int devfn, int where,
		     int size, u32 value)
{
	return raw_pci_write(pci_domain_nr(bus), bus->number,
			     devfn, where, size, value);
}

struct pci_ops pci_root_ops = {
	.read = pci_read,
	.write = pci_write,
};

static struct pci_controller *alloc_pci_controller(int seg)
{
	struct pci_controller *controller;

	controller = kzalloc(sizeof(*controller), GFP_KERNEL);
	if (!controller)
		return NULL;

	controller->segment = seg;
	return controller;
}

struct pci_root_info {
	struct acpi_device *bridge;
	struct pci_controller *controller;
	struct list_head resources;
	struct resource *res;
	unsigned int res_num;
	char *name;
};

static acpi_status resource_to_window(struct acpi_resource *resource,
				      struct acpi_resource_address64 *addr)
{
	acpi_status status;

	/*
	 * We're only interested in _CRS descriptors that are
	 *	- address space descriptors for memory
	 *	- non-zero size
	 *	- producers, i.e., the address space is routed downstream,
	 *	  not consumed by the bridge itself
	 */
	status = acpi_resource_to_address64(resource, addr);
	if (ACPI_SUCCESS(status) &&
	    (addr->resource_type == ACPI_MEMORY_RANGE ||
	     addr->resource_type == ACPI_IO_RANGE) &&
	    addr->address.address_length &&
	    addr->producer_consumer == ACPI_PRODUCER)
		return AE_OK;

	return AE_ERROR;
}

static acpi_status count_window(struct acpi_resource *resource, void *data)
{
	unsigned int *windows = (unsigned int *) data;
	struct acpi_resource_address64 addr;
	acpi_status status;

	status = resource_to_window(resource, &addr);
	if (ACPI_SUCCESS(status))
		(*windows)++;

	return AE_OK;
}

static acpi_status add_window(struct acpi_resource *res, void *data)
{
	struct pci_root_info *info = data;
	struct resource *resource;
	struct acpi_resource_address64 addr;
	resource_size_t offset;
	acpi_status status;
	unsigned long flags;
	struct resource *root;
	u64 start;

	/* Return AE_OK for non-window resources to keep scanning for more */
	status = resource_to_window(res, &addr);
	if (!ACPI_SUCCESS(status))
		return AE_OK;

	if (addr.resource_type == ACPI_MEMORY_RANGE) {
		flags = IORESOURCE_MEM;
		root = &iomem_resource;
	} else if (addr.resource_type == ACPI_IO_RANGE) {
		flags = IORESOURCE_IO;
		root = &ioport_resource;
	} else
		return AE_OK;

	start = addr.address.minimum + addr.address.translation_offset;

	resource = &info->res[info->res_num];
	resource->name = info->name;
	resource->flags = flags;
	resource->start = start;
	resource->end = resource->start + addr.address.address_length - 1;

	if (flags & IORESOURCE_IO) {
		unsigned long port;
		int err;

		err = pci_register_io_range(start, addr.address.address_length);
		if (err)
			return AE_OK;

		port = pci_address_to_pio(start);
		if (port == (unsigned long)-1)
			return AE_OK;

		resource->start = port;
		resource->end = port + addr.address.address_length - 1;

		if (pci_remap_iospace(resource, start) < 0)
			return AE_OK;

		offset = 0;
	} else
		offset = addr.address.translation_offset;

	if (insert_resource(root, resource)) {
		dev_err(&info->bridge->dev,
			"can't allocate host bridge window %pR\n",
			resource);
	} else {
		if (addr.address.translation_offset)
			dev_info(&info->bridge->dev, "host bridge window %pR "
				 "(PCI address [%#llx-%#llx])\n",
				 resource,
				 resource->start - addr.address.translation_offset,
				 resource->end - addr.address.translation_offset);
		else
			dev_info(&info->bridge->dev,
				 "host bridge window %pR\n", resource);
	}

	pci_add_resource_offset(&info->resources, resource, offset);
	info->res_num++;
	return AE_OK;
}

static void free_pci_root_info_res(struct pci_root_info *info)
{
	kfree(info->name);
	kfree(info->res);
	info->res = NULL;
	info->res_num = 0;
	kfree(info->controller);
	info->controller = NULL;
}

static void __release_pci_root_info(struct pci_root_info *info)
{
	int i;
	struct resource *res;

	for (i = 0; i < info->res_num; i++) {
		res = &info->res[i];

		if (!res->parent)
			continue;

		if (!(res->flags & (IORESOURCE_MEM | IORESOURCE_IO)))
			continue;

		release_resource(res);
	}

	free_pci_root_info_res(info);
	kfree(info);
}

static void release_pci_root_info(struct pci_host_bridge *bridge)
{
	struct pci_root_info *info = bridge->release_data;

	__release_pci_root_info(info);
}

static int
probe_pci_root_info(struct pci_root_info *info, struct acpi_device *device,
		int busnum, int domain)
{
	char *name;

	name = kmalloc(16, GFP_KERNEL);
	if (!name)
		return -ENOMEM;

	sprintf(name, "PCI Bus %04x:%02x", domain, busnum);
	info->bridge = device;
	info->name = name;

	acpi_walk_resources(device->handle, METHOD_NAME__CRS, count_window,
			&info->res_num);
	if (info->res_num) {
		info->res =
			kzalloc_node(sizeof(*info->res) * info->res_num,
				     GFP_KERNEL, info->controller->node);
		if (!info->res) {
			kfree(name);
			return -ENOMEM;
		}

		info->res_num = 0;
		acpi_walk_resources(device->handle, METHOD_NAME__CRS,
			add_window, info);
	} else
		kfree(name);

	return 0;
}

/* Root bridge scanning */
struct pci_bus *pci_acpi_scan_root(struct acpi_pci_root *root)
{
	struct acpi_device *device = root->device;
	struct pci_mmcfg_region *mcfg;
	int domain = root->segment;
	int bus = root->secondary.start;
	struct pci_controller *controller;
	struct pci_root_info *info = NULL;
	int busnum = root->secondary.start;
	struct pci_bus *pbus;
	int ret;

	/* we need mmconfig */
	mcfg = pci_mmconfig_lookup(domain, busnum);
	if (!mcfg) {
		pr_err("pci_bus %04x:%02x has no MCFG table\n",
		       domain, busnum);
		return NULL;
	}

	if (mcfg->fixup)
		(*mcfg->fixup)(root, mcfg);

	controller = alloc_pci_controller(domain);
	if (!controller)
		return NULL;

	controller->companion = device;
	controller->node = acpi_get_node(device->handle);

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		dev_err(&device->dev,
				"pci_bus %04x:%02x: ignored (out of memory)\n",
				domain, busnum);
		kfree(controller);
		return NULL;
	}

	info->controller = controller;
	INIT_LIST_HEAD(&info->resources);

	ret = probe_pci_root_info(info, device, busnum, domain);
	if (ret) {
		kfree(info->controller);
		kfree(info);
		return NULL;
	}
	/* insert busn resource at first */
	pci_add_resource(&info->resources, &root->secondary);

	pbus = pci_create_root_bus(NULL, bus, &pci_root_ops, controller,
				   &info->resources);
	if (!pbus) {
		pci_free_resource_list(&info->resources);
		__release_pci_root_info(info);
		return NULL;
	}
	pbus->msi = iort_find_pci_msi_chip(domain, 0);

	pci_set_host_bridge_release(to_pci_host_bridge(pbus->bridge),
			release_pci_root_info, info);
	pci_scan_child_bus(pbus);
	return pbus;
}
