/*
 * Cavium ThunderX memory controller kernel module
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright Cavium, Inc. (C) 2015. All rights reserved.
 *
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/edac.h>
#include <linux/interrupt.h>

#include "edac_core.h"
#include "edac_module.h"

#define PCI_DEVICE_ID_THUNDER_OCX 0xa013

#define OCX_INTS		4

#define OCX_COM_INT		0x100
#define OCX_COM_INT_W1S		0x108
#define OCX_COM_INT_ENA_W1S	0x110
#define OCX_COM_INT_ENA_W1C	0x118

#define OCX_COM_LINKX_INT(x)		(0x120 + (x) * 8)
#define OCX_COM_LINKX_INT_W1S(x)	(0x140 + (x) * 8)
#define OCX_COM_LINKX_INT_ENA_W1S(x)	(0x160 + (x) * 8)
#define OCX_COM_LINKX_INT_ENA_W1C(x)	(0x180 + (x) * 8)

#define OCX_COM_INT_ENA_ALL	((0x1fULL << 50) | (0xffffffULL))
#define OCX_COM_LINKX_INT_ENA_ALL	((3 << 12) | (7 << 7) | (0x3f))

struct thunderx_ocx {
	void __iomem *regs;
	int com_link;
	struct pci_dev *pdev;
	struct edac_device_ctl_info *edac_dev;

	struct msix_entry msix_ent[OCX_INTS];
};

static irqreturn_t thunderx_ocx_com_isr(int irq, void *irq_id)
{
	struct msix_entry *msix = irq_id;
	struct thunderx_ocx *ocx = container_of(msix, struct thunderx_ocx,
						msix_ent[msix->entry]);

	u64 ocx_com_int = readq(ocx->regs + OCX_COM_INT);

	dev_info(&ocx->pdev->dev, "OCX_COM_INT: %016llx\n", ocx_com_int);

	writeq(ocx_com_int, ocx->regs + OCX_COM_INT);

	edac_device_handle_ue(ocx->edac_dev, 0, 0, ocx->edac_dev->ctl_name);

	return IRQ_HANDLED;
}

static irqreturn_t thunderx_ocx_lnk_isr(int irq, void *irq_id)
{
	struct msix_entry *msix = irq_id;
	struct thunderx_ocx *ocx = container_of(msix, struct thunderx_ocx,
						msix_ent[msix->entry]);

	u64 ocx_com_link_int = readq(ocx->regs +
				     OCX_COM_LINKX_INT(msix->entry - 1));

	dev_info(&ocx->pdev->dev, "OCX_COM_LINK_INT[%d]: %016llx\n",
		 msix->entry - 1, ocx_com_link_int);

	writeq(ocx_com_link_int, ocx->regs +
	       OCX_COM_LINKX_INT(msix->entry - 1));

	edac_device_handle_ue(ocx->edac_dev, 0, 0, ocx->edac_dev->ctl_name);

	return IRQ_HANDLED;
}

static ssize_t thunderx_ocx_com_int_show(struct device *dev,
					 struct device_attribute *mattr,
					 char *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);

	return sprintf(data, "0x%016llx",
		       readq(ocx->regs + OCX_COM_INT));
}

static ssize_t thunderx_ocx_com_int_store(struct device *dev,
					  struct device_attribute *mattr,
					  const char *data, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);
	u64 val;
	int res;

	res = kstrtoull(data, 0, &val);

	if (!res) {
		writeq(val, ocx->regs + OCX_COM_INT);
		res = count;
	}

	return res;
}

DEVICE_ATTR(inject_com_int, S_IRUGO | S_IWUSR,
	    thunderx_ocx_com_int_show, thunderx_ocx_com_int_store);

static ssize_t thunderx_ocx_com_link_int_show(struct device *dev,
					      struct device_attribute *mattr,
					      char *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);

	return sprintf(data, "0x%016llx",
		       readq(ocx->regs + OCX_COM_LINKX_INT(ocx->com_link)));
}

static ssize_t thunderx_ocx_com_link_int_store(struct device *dev,
					       struct device_attribute *mattr,
					       const char *data, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);
	u64 val;
	int res;

	res = kstrtoull(data, 0, &val);

	if (!res) {
		writeq(val, ocx->regs + OCX_COM_LINKX_INT(ocx->com_link));
		res = count;
	}

	return res;
}

DEVICE_ATTR(inject_com_link_int, S_IRUGO | S_IWUSR,
	    thunderx_ocx_com_link_int_show, thunderx_ocx_com_link_int_store);

static ssize_t thunderx_ocx_com_link_show(struct device *dev,
					    struct device_attribute *mattr,
					    char *data)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);

	return sprintf(data, "%d", ocx->com_link);
}

static ssize_t thunderx_ocx_com_link_store(struct device *dev,
					   struct device_attribute *mattr,
					   const char *data, size_t count)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	struct thunderx_ocx *ocx = pci_get_drvdata(pdev);
	unsigned long val;
	int res;

	res = kstrtoul(data, 0, &val);

	if (val > 2)
		res = -EINVAL;

	if (!res) {
		ocx->com_link = val;
		res = count;
	}

	return res;
}

DEVICE_ATTR(com_link, S_IRUGO | S_IWUSR,
	    thunderx_ocx_com_link_show, thunderx_ocx_com_link_store);

static int thunderx_create_sysfs_attrs(struct pci_dev *pdev)
{
	int rc;

	rc = device_create_file(&pdev->dev, &dev_attr_inject_com_int);
	if (rc < 0)
		return rc;
	rc = device_create_file(&pdev->dev, &dev_attr_inject_com_link_int);
	if (rc < 0)
		return rc;
	rc = device_create_file(&pdev->dev, &dev_attr_com_link);
	if (rc < 0)
		return rc;

	return 0;
}

static void thunderx_remove_sysfs_attrs(struct pci_dev *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_com_link);
	device_remove_file(&pdev->dev, &dev_attr_inject_com_link_int);
	device_remove_file(&pdev->dev, &dev_attr_inject_com_int);
}



static const struct pci_device_id thunderx_ocx_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_CAVIUM, PCI_DEVICE_ID_THUNDER_OCX) },
	{ 0, },
};


static int thunderx_ocx_probe(struct pci_dev *pdev,
			      const struct pci_device_id *id)
{
	struct thunderx_ocx *ocx;
	struct edac_device_ctl_info *edac_dev;
	char name[6];
	int idx;
	int i;
	int err = -ENOMEM;

	idx = edac_device_alloc_index();
	snprintf(name, sizeof(name), "OCX%d", idx);
	edac_dev = edac_device_alloc_ctl_info(sizeof(struct thunderx_ocx),
					name, 1, "CCPI", 1, 0, NULL, 0, idx);
	if (!edac_dev) {
		dev_err(&pdev->dev, "Cannot allocate EDAC device: %d\n", err);
		return err;
	}

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable PCI device: %d\n", err);
		goto err_kfree;
	}

	err = pcim_iomap_regions(pdev, 1 << 0, "thunderx_ocx");
	if (err) {
		dev_err(&pdev->dev, "Cannot map PCI resources: %d\n", err);
		goto err_kfree;
	}

	ocx = edac_dev->pvt_info;
	ocx->edac_dev = edac_dev;

	ocx->regs = pcim_iomap_table(pdev)[0];

	if (!ocx->regs) {
		dev_err(&pdev->dev, "Cannot map PCI resources: %d\n", err);
		err = -ENODEV;
		goto err_kfree;
	}

	ocx->pdev = pdev;

	for (i = 0; i < OCX_INTS; i++) {
		ocx->msix_ent[i].entry = i;
		ocx->msix_ent[i].vector = 0;
	}

	err = pci_enable_msix_exact(pdev, ocx->msix_ent, OCX_INTS);
	if (err) {
		dev_err(&pdev->dev, "Cannot enable interrupt: %d\n", err);
		goto err_kfree;
	}

	for (i = 0; i < OCX_INTS; i++) {
		err = devm_request_irq(&pdev->dev, ocx->msix_ent[i].vector,
				       (i == 0) ? thunderx_ocx_com_isr :
						  thunderx_ocx_lnk_isr,
				       0, "[EDAC] ThunderX OCX",
				       &ocx->msix_ent[i]);
		if (err)
			goto err_kfree;
	}

	edac_dev->dev = &pdev->dev;
	edac_dev->dev_name = dev_name(&pdev->dev);
	edac_dev->mod_name = "thunderx-ocx";
	edac_dev->ctl_name = "thunderx-ocx-err";

	err = edac_device_add_device(edac_dev);
	if (err) {
		dev_err(&pdev->dev, "Cannot add EDAC device: %d\n", err);
		goto err_kfree;
	}

	err = thunderx_create_sysfs_attrs(pdev);
	if (err) {
		dev_err(&pdev->dev, "Cannot add device attrs: %d\n", err);
		goto err_del_dev;
	}

	pci_set_drvdata(pdev, edac_dev);

	writeq(OCX_COM_INT_ENA_ALL, ocx->regs + OCX_COM_INT_ENA_W1S);

	for (i = 0; i < OCX_INTS; i++) {
		writeq(OCX_COM_LINKX_INT_ENA_ALL,
		       ocx->regs + OCX_COM_LINKX_INT_ENA_W1S(i));
	}

	return 0;

err_del_dev:
	edac_device_del_device(&pdev->dev);
err_kfree:
	edac_device_free_ctl_info(edac_dev);

	return err;
}


static void thunderx_ocx_remove(struct pci_dev *pdev)
{
	struct edac_device_ctl_info *edac_dev = pci_get_drvdata(pdev);
	struct thunderx_ocx *ocx = edac_dev->pvt_info;
	int i;

	writeq(OCX_COM_INT_ENA_ALL, ocx->regs + OCX_COM_INT_ENA_W1C);

	for (i = 0; i < OCX_INTS; i++) {
		writeq(OCX_COM_LINKX_INT_ENA_ALL,
		       ocx->regs + OCX_COM_LINKX_INT_ENA_W1C(i));
	}

	edac_device_del_device(&pdev->dev);
	thunderx_remove_sysfs_attrs(pdev);

	edac_device_free_ctl_info(edac_dev);
}

MODULE_DEVICE_TABLE(pci, thunderx_ocx_pci_tbl);

static struct pci_driver thunderx_ocx_driver = {
	.name     = "thunderx_ocx_edac",
	.probe    = thunderx_ocx_probe,
	.remove   = thunderx_ocx_remove,
	.id_table = thunderx_ocx_pci_tbl,
};

module_pci_driver(thunderx_ocx_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Cavium, Inc.");
MODULE_DESCRIPTION("EDAC Driver for Cavium ThunderX");
