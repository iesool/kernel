/*
 * Copyright (C) 2014-2015, Linaro Ltd.
 *	Author: Tomasz Nowicki <tomasz.nowicki@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * This file implements early detection/parsing of I/O mapping
 * reported to OS through BIOS via I/O Remapping Table (IORT) ACPI
 * table.
 *
 * These routines are used by ITS and PCI host bridge drivers.
 */

#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/iort.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#define IORT_PFX	"IORT: "

static LIST_HEAD(iort_node_list);
static DEFINE_MUTEX(iort_tree_roots_mutex);
static struct acpi_table_header *iort_table;

struct iort_its_msi_chip {
	struct list_head	list;
	struct msi_controller	*chip;
	u32			id;
};

typedef acpi_status (*iort_find_node_callback)
	(struct acpi_iort_header *node, void *context);

static LIST_HEAD(iort_pci_msi_chip_list);
static DEFINE_MUTEX(iort_pci_msi_chip_mutex);

int iort_pci_msi_chip_add(struct msi_controller *chip, u32 its_id)
{
	struct iort_its_msi_chip *its_msi_chip;

	its_msi_chip = kzalloc(sizeof(*its_msi_chip), GFP_KERNEL);
	if (!its_msi_chip)
		return -ENOMEM;

	its_msi_chip->chip = chip;
	its_msi_chip->id = its_id;

	mutex_lock(&iort_pci_msi_chip_mutex);
	list_add(&its_msi_chip->list, &iort_pci_msi_chip_list);
	mutex_unlock(&iort_pci_msi_chip_mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(iort_pci_msi_chip_add);

void iort_pci_msi_chip_remove(struct msi_controller *chip)
{
	struct iort_its_msi_chip *its_msi_chip, *tmp;

	mutex_lock(&iort_pci_msi_chip_mutex);
	list_for_each_entry_safe(its_msi_chip, tmp, &iort_pci_msi_chip_list,
				 list) {
		if (its_msi_chip->chip == chip) {
			list_del(&chip->list);
			mutex_unlock(&iort_pci_msi_chip_mutex);
			kfree(its_msi_chip);
		}
	}
	mutex_unlock(&iort_pci_msi_chip_mutex);
}
EXPORT_SYMBOL_GPL(iort_pci_msi_chip_remove);

static struct msi_controller *iort_pci_find_msi_chip_by_id(u32 its_id)
{
	struct iort_its_msi_chip *its_msi_chip;

	mutex_lock(&iort_pci_msi_chip_mutex);
	list_for_each_entry(its_msi_chip, &iort_pci_msi_chip_list, list) {
		if (its_msi_chip->id == its_id) {
			mutex_unlock(&iort_pci_msi_chip_mutex);
			return its_msi_chip->chip;
		}
	}
	mutex_unlock(&iort_pci_msi_chip_mutex);

	return NULL;
}
EXPORT_SYMBOL_GPL(iort_pci_find_msi_chip_by_id);

static struct acpi_iort_header *
iort_find_root_node(struct acpi_iort_header *node)
{
	struct acpi_iort_id *id_map;

	if (!node)
		return NULL;

	/* Root node has no ID map */
	while (node->ref_to_ids) {
		id_map = ACPI_ADD_PTR(struct acpi_iort_id,
				      node, node->ref_to_ids);

		/* Firmware bug! */
		if (!id_map->output_ref) {
			pr_err(IORT_PFX FW_BUG "[node %p type %d] ID map has invalid parent reference\n",
			       node, node->type);
			return NULL;
		}

		node = ACPI_ADD_PTR(struct acpi_iort_header,
					 iort_table, id_map->output_ref);
	}

	return node;
}

static struct acpi_iort_header *
iort_find_node_type(int type, iort_find_node_callback callback, void *context)
{
	struct acpi_iort_header *iort_node, *iort_end;

	/* Skip IORT header */
	iort_node = ACPI_ADD_PTR(struct acpi_iort_header, iort_table,
				 sizeof(struct acpi_table_iort));
	iort_end = ACPI_ADD_PTR(struct acpi_iort_header, iort_table,
				iort_table->length);

	while (iort_node < iort_end) {
		if (iort_node->type == type || type == -1) {
			if (ACPI_SUCCESS(callback(iort_node, context)))
				return iort_node;
		}

		iort_node = ACPI_ADD_PTR(struct acpi_iort_header,
					  iort_node, iort_node->length);
	}

	return NULL;
}

static acpi_status
iort_find_pci_rc_callback(struct acpi_iort_header *node, void *context)
{
	int segment = *(int *)context;
	struct acpi_iort_root_complex *pci_rc;

	pci_rc = ACPI_ADD_PTR(struct acpi_iort_root_complex, node,
			      sizeof(struct acpi_iort_header));

	if (pci_rc->segment == segment)
		return AE_OK;

	return AE_NOT_FOUND;
}

static struct acpi_iort_header *
iort_find_pci_rc(int segment)
{

	if (!iort_table)
		return NULL;

	return iort_find_node_type(ACPI_IORT_TYPE_ROOT_COMPLEX,
				   iort_find_pci_rc_callback, &segment);
}

struct msi_controller *iort_find_pci_msi_chip(int segment, unsigned int idx)
{
	struct acpi_iort_its *its_node;
	struct acpi_iort_header *node;
	struct msi_controller *msi_chip;

	if (!iort_table)
		return NULL;

	node = iort_find_pci_rc(segment);
	if (!node) {
		pr_err(IORT_PFX "can not find node related to PCI host bridge [segment %d]\n",
		       segment);
		return NULL;
	}

	node = iort_find_root_node(node);
	if (!node || node->type != ACPI_IORT_TYPE_ITS_GROUP) {
		pr_err(IORT_PFX "can not find ITS node parent for PCI host bridge [segment %d]\n",
		       segment);
		return NULL;
	}

	/* Move to ITS specific data */
	its_node = ACPI_ADD_PTR(struct acpi_iort_its, node,
				sizeof(struct acpi_iort_header));

	if (idx > its_node->number_of_its) {
		pr_err(IORT_PFX "requested ITS ID index [%d] is greater than available ITS IDs [%d]\n",
		       idx, its_node->number_of_its);
		return NULL;
	}

	msi_chip = iort_pci_find_msi_chip_by_id(its_node->its_id[idx]);
	if (!msi_chip)
		pr_err(IORT_PFX "can not find ITS chip ID:%d, not registered\n",
		       its_node->its_id[idx]);

	return msi_chip;
}
EXPORT_SYMBOL_GPL(iort_find_pci_msi_chip);

static int __init iort_init(void)
{
	struct acpi_table_header *table;
	acpi_status status;

	if (acpi_disabled)
		return -ENODEV;

	status = acpi_get_table(ACPI_SIG_IORT, 0, &table);
	if (status == AE_NOT_FOUND)
		return -ENODEV;
	else if (ACPI_FAILURE(status)) {
		const char *msg = acpi_format_exception(status);
		pr_err(IORT_PFX "Failed to get table, %s\n", msg);
		return -EINVAL;
	}

	if (!table->length) {
		pr_err(IORT_PFX FW_BUG "0 length table\n");
		return -EINVAL;
	}

	iort_table = table;
	return 0;
}

static void __exit iort_exit(void)
{
	iort_table = NULL;
}

arch_initcall(iort_init);
module_exit(iort_exit);

MODULE_DESCRIPTION("IORT (I/O remapping ACPI table) parsing helpers");
MODULE_AUTHOR("Tomasz Nowicki <tomasz.nowicki@linaro.org>");
MODULE_LICENSE("GPL v2");
