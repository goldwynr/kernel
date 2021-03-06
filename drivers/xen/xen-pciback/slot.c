/*
 * PCI Backend - Provides a Virtual PCI bus (with real devices)
 *               to the frontend
 *
 *   Author: Ryan Wilson <hap9@epoch.ncsc.mil> (vpci.c)
 *   Author: Tristan Gingold <tristan.gingold@bull.net>, from vpci.c
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/spinlock.h>
#include "pciback.h"

/* There are at most 32 slots in a pci bus.  */
#define PCI_SLOT_MAX 32

#define PCI_BUS_NBR 2

struct slot_dev_data {
	/* Access to dev_list must be protected by lock */
	struct pci_dev *slots[PCI_BUS_NBR][PCI_SLOT_MAX];
	spinlock_t lock;
};

static struct pci_dev *_xen_pcibk_get_pci_dev(struct xen_pcibk_device *pdev,
					      unsigned int domain,
					      unsigned int bus,
					      unsigned int devfn)
{
	struct pci_dev *dev = NULL;
	struct slot_dev_data *slot_dev = pdev->pci_dev_data;
	unsigned long flags;

	if (domain != 0 || PCI_FUNC(devfn) != 0)
		return NULL;

	if (PCI_SLOT(devfn) >= PCI_SLOT_MAX || bus >= PCI_BUS_NBR)
		return NULL;

	spin_lock_irqsave(&slot_dev->lock, flags);
	dev = slot_dev->slots[bus][PCI_SLOT(devfn)];
	spin_unlock_irqrestore(&slot_dev->lock, flags);

	return dev;
}

static int _xen_pcibk_add_pci_dev(struct xen_pcibk_device *pdev,
				  struct pci_dev *dev, int devid,
				  publish_pci_dev_cb publish_cb)
{
	int err = 0, slot, bus;
	struct slot_dev_data *slot_dev = pdev->pci_dev_data;
	unsigned long flags;

	if ((dev->class >> 24) == PCI_BASE_CLASS_BRIDGE) {
		err = -EFAULT;
		xenbus_dev_fatal(pdev->xdev, err,
				 "Can't export bridges on the virtual PCI bus");
		goto out;
	}

	spin_lock_irqsave(&slot_dev->lock, flags);

	/* Assign to a new slot on the virtual PCI bus */
	for (bus = 0; bus < PCI_BUS_NBR; bus++)
		for (slot = 0; slot < PCI_SLOT_MAX; slot++) {
			if (slot_dev->slots[bus][slot] == NULL) {
				pr_info("slot: %s: assign to virtual slot %d, bus %d\n",
					pci_name(dev), slot, bus);
				slot_dev->slots[bus][slot] = dev;
				goto unlock;
			}
		}

	err = -ENOMEM;
	xenbus_dev_fatal(pdev->xdev, err,
			 "No more space on root virtual PCI bus");

      unlock:
	spin_unlock_irqrestore(&slot_dev->lock, flags);

	/* Publish this device. */
	if(!err)
		err = publish_cb(pdev, 0, 0, PCI_DEVFN(slot, 0), devid);

      out:
	return err;
}

static void _xen_pcibk_release_pci_dev(struct xen_pcibk_device *pdev,
				       struct pci_dev *dev, bool lock)
{
	int slot, bus;
	struct slot_dev_data *slot_dev = pdev->pci_dev_data;
	struct pci_dev *found_dev = NULL;
	unsigned long flags;

	spin_lock_irqsave(&slot_dev->lock, flags);

	for (bus = 0; bus < PCI_BUS_NBR; bus++)
		for (slot = 0; slot < PCI_SLOT_MAX; slot++) {
			if (slot_dev->slots[bus][slot] == dev) {
				slot_dev->slots[bus][slot] = NULL;
				found_dev = dev;
				goto out;
			}
		}

      out:
	spin_unlock_irqrestore(&slot_dev->lock, flags);

	if (found_dev) {
		if (lock)
			device_lock(&found_dev->dev);
		pcistub_put_pci_dev(found_dev);
		if (lock)
			device_unlock(&found_dev->dev);
	}
}

static int _xen_pcibk_init_devices(struct xen_pcibk_device *pdev)
{
	int slot, bus;
	struct slot_dev_data *slot_dev;

	slot_dev = kmalloc(sizeof(*slot_dev), GFP_KERNEL);
	if (!slot_dev)
		return -ENOMEM;

	spin_lock_init(&slot_dev->lock);

	for (bus = 0; bus < PCI_BUS_NBR; bus++)
		for (slot = 0; slot < PCI_SLOT_MAX; slot++)
			slot_dev->slots[bus][slot] = NULL;

	pdev->pci_dev_data = slot_dev;

	return 0;
}

static int _xen_pcibk_publish_pci_roots(struct xen_pcibk_device *pdev,
					publish_pci_root_cb publish_cb)
{
	/* The Virtual PCI bus has only one root */
	return publish_cb(pdev, 0, 0);
}

static void _xen_pcibk_release_devices(struct xen_pcibk_device *pdev)
{
	int slot, bus;
	struct slot_dev_data *slot_dev = pdev->pci_dev_data;
	struct pci_dev *dev;

	for (bus = 0; bus < PCI_BUS_NBR; bus++)
		for (slot = 0; slot < PCI_SLOT_MAX; slot++) {
			dev = slot_dev->slots[bus][slot];
			if (dev != NULL) {
				device_lock(&dev->dev);
				pcistub_put_pci_dev(dev);
				device_unlock(&dev->dev);
			}
		}

	kfree(slot_dev);
	pdev->pci_dev_data = NULL;
}

static int _xen_pcibk_get_pcifront_dev(struct pci_dev *pcidev,
				       struct xen_pcibk_device *pdev,
				       unsigned int *domain,
				       unsigned int *bus, unsigned int *devfn)
{
	int slot, busnr;
	struct slot_dev_data *slot_dev = pdev->pci_dev_data;
	struct pci_dev *dev;
	int found = 0;
	unsigned long flags;

	spin_lock_irqsave(&slot_dev->lock, flags);

	for (busnr = 0; busnr < PCI_BUS_NBR; bus++)
		for (slot = 0; slot < PCI_SLOT_MAX; slot++) {
			dev = slot_dev->slots[busnr][slot];
			if (dev && dev->bus->number == pcidev->bus->number
				&& dev->devfn == pcidev->devfn
				&& pci_domain_nr(dev->bus) == pci_domain_nr(pcidev->bus)) {
				found = 1;
				*domain = 0;
				*bus = busnr;
				*devfn = PCI_DEVFN(slot,0);
				goto out;
			}
		}
out:
	spin_unlock_irqrestore(&slot_dev->lock, flags);
	return found;

}

const struct xen_pcibk_backend xen_pcibk_slot_backend = {
	.name		= "slot",
	.init		= _xen_pcibk_init_devices,
	.free		= _xen_pcibk_release_devices,
	.find		= _xen_pcibk_get_pcifront_dev,
	.publish	= _xen_pcibk_publish_pci_roots,
	.release	= _xen_pcibk_release_pci_dev,
	.add		= _xen_pcibk_add_pci_dev,
	.get		= _xen_pcibk_get_pci_dev,
};
