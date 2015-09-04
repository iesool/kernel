/*
 * Copyright (C) 2013 - 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/efi.h>
#include <linux/of.h>
#include <linux/of_fdt.h>

#define UEFI_PARAM(name, prop, field)			   \
	{						   \
		{ name },				   \
		{ prop },				   \
		offsetof(struct efi_fdt_params, field),    \
		FIELD_SIZEOF(struct efi_fdt_params, field) \
	}

static __initdata struct {
	const char name[32];
	const char propname[32];
	int offset;
	int size;
} dt_params[] = {
	UEFI_PARAM("System Table", "linux,uefi-system-table", system_table),
	UEFI_PARAM("MemMap Address", "linux,uefi-mmap-start", mmap),
	UEFI_PARAM("MemMap Size", "linux,uefi-mmap-size", mmap_size),
	UEFI_PARAM("MemMap Desc. Size", "linux,uefi-mmap-desc-size", desc_size),
	UEFI_PARAM("MemMap Desc. Version", "linux,uefi-mmap-desc-ver", desc_ver)
};

struct param_info {
	int verbose;
	int found;
	void *params;
};

static int __init fdt_find_uefi_params(unsigned long node, const char *uname,
				       int depth, void *data)
{
	struct param_info *info = data;
	const void *prop;
	void *dest;
	u64 val;
	int i, len;

	if (depth != 1 || strcmp(uname, "chosen") != 0)
		return 0;

	for (i = 0; i < ARRAY_SIZE(dt_params); i++) {
		prop = of_get_flat_dt_prop(node, dt_params[i].propname, &len);
		if (!prop)
			return 0;
		dest = info->params + dt_params[i].offset;
		info->found++;

		val = of_read_number(prop, len / sizeof(u32));

		if (dt_params[i].size == sizeof(u32))
			*(u32 *)dest = val;
		else
			*(u64 *)dest = val;

		if (info->verbose)
			pr_info("  %s: 0x%0*llx\n", dt_params[i].name,
				dt_params[i].size * 2, val);
	}
	return 1;
}

int __init efi_get_fdt_params(struct efi_fdt_params *params, int verbose)
{
	struct param_info info;
	int ret;

	pr_info("Getting EFI parameters from FDT:\n");

	info.verbose = verbose;
	info.found = 0;
	info.params = params;

	ret = of_scan_flat_dt(fdt_find_uefi_params, &info);
	if (!info.found)
		pr_info("UEFI not found.\n");
	else if (!ret)
		pr_err("Can't find '%s' in device tree!\n",
		       dt_params[info.found].name);

	return ret;
}
