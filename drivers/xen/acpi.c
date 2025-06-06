/******************************************************************************
 * acpi.c
 * acpi file for domain 0 kernel
 *
 * Copyright (c) 2011 Konrad Rzeszutek Wilk <konrad.wilk@oracle.com>
 * Copyright (c) 2011 Yu Ke ke.yu@intel.com
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <linux/pci.h>
#include <xen/acpi.h>
#include <xen/interface/platform.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/hypervisor.h>

static int xen_acpi_notify_hypervisor_state(u8 sleep_state,
					    u32 val_a, u32 val_b,
					    bool extended)
{
	unsigned int bits = extended ? 8 : 16;

	struct xen_platform_op op = {
		.cmd = XENPF_enter_acpi_sleep,
		.interface_version = XENPF_INTERFACE_VERSION,
		.u.enter_acpi_sleep = {
			.val_a = (u16)val_a,
			.val_b = (u16)val_b,
			.sleep_state = sleep_state,
			.flags = extended ? XENPF_ACPI_SLEEP_EXTENDED : 0,
		},
	};

	if (WARN((val_a & (~0 << bits)) || (val_b & (~0 << bits)),
		 "Using more than %u bits of sleep control values %#x/%#x!"
		 "Email xen-devel@lists.xen.org - Thank you.\n", \
		 bits, val_a, val_b))
		return -1;

	HYPERVISOR_platform_op(&op);
	return 1;
}

int xen_acpi_notify_hypervisor_sleep(u8 sleep_state,
				     u32 pm1a_cnt, u32 pm1b_cnt)
{
	return xen_acpi_notify_hypervisor_state(sleep_state, pm1a_cnt,
						pm1b_cnt, false);
}

int xen_acpi_notify_hypervisor_extended_sleep(u8 sleep_state,
				     u32 val_a, u32 val_b)
{
	return xen_acpi_notify_hypervisor_state(sleep_state, val_a,
						val_b, true);
}

struct acpi_prt_entry {
	struct acpi_pci_id      id;
	u8                      pin;
	acpi_handle             link;
	u32                     index;
};

int xen_acpi_get_gsi_info(struct pci_dev *dev,
						  int *gsi_out,
						  int *trigger_out,
						  int *polarity_out)
{
	int gsi;
	u8 pin;
	struct acpi_prt_entry *entry;
	int trigger = ACPI_LEVEL_SENSITIVE;
	int polarity = acpi_irq_model == ACPI_IRQ_MODEL_GIC ?
				      ACPI_ACTIVE_HIGH : ACPI_ACTIVE_LOW;

	if (!dev || !gsi_out || !trigger_out || !polarity_out)
		return -EINVAL;

	pin = dev->pin;
	if (!pin)
		return -EINVAL;

	entry = acpi_pci_irq_lookup(dev, pin);
	if (entry) {
		if (entry->link)
			gsi = acpi_pci_link_allocate_irq(entry->link,
							 entry->index,
							 &trigger, &polarity,
							 NULL);
		else
			gsi = entry->index;
	} else
		gsi = -1;

	if (gsi < 0)
		return -EINVAL;

	*gsi_out = gsi;
	*trigger_out = trigger;
	*polarity_out = polarity;

	return 0;
}
EXPORT_SYMBOL_GPL(xen_acpi_get_gsi_info);

static get_gsi_from_sbdf_t get_gsi_from_sbdf;
static DEFINE_RWLOCK(get_gsi_from_sbdf_lock);

void xen_acpi_register_get_gsi_func(get_gsi_from_sbdf_t func)
{
	write_lock(&get_gsi_from_sbdf_lock);
	get_gsi_from_sbdf = func;
	write_unlock(&get_gsi_from_sbdf_lock);
}
EXPORT_SYMBOL_GPL(xen_acpi_register_get_gsi_func);

int xen_acpi_get_gsi_from_sbdf(u32 sbdf)
{
	int ret = -EOPNOTSUPP;

	read_lock(&get_gsi_from_sbdf_lock);
	if (get_gsi_from_sbdf)
		ret = get_gsi_from_sbdf(sbdf);
	read_unlock(&get_gsi_from_sbdf_lock);

	return ret;
}
EXPORT_SYMBOL_GPL(xen_acpi_get_gsi_from_sbdf);
