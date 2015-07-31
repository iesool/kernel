/*
 * Copyright (C) 2014, Linaro Ltd.
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
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 */

#ifndef __IORT_H__
#define __IORT_H__

#ifdef CONFIG_ACPI

#ifdef CONFIG_IORT_TABLE

#include <linux/msi.h>

int iort_pci_msi_chip_add(struct msi_controller *chip, u32 its_id);
void iort_pci_msi_chip_remove(struct msi_controller *chip);
struct msi_controller *iort_find_pci_msi_chip(int segment, unsigned int idx);

#endif /* CONFIG_IORT_TABLE */

#endif /* CONFIG_ACPI */

#endif /* __IORT_H__ */
