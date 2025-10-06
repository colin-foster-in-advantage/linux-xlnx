// SPDX-License-Identifier: GPL-2.0
/*
 * Xilinx-specific SMMU CSR irqchip driver
 *
 * Copyright (C) 2025 Advanced Micro Devices, Inc.
 */

#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>

/**
 * struct xilinx_smmu_csr - SMMU CSR interrupt controller context
 * @base: MMIO base address of CSR registers
 * @parent_irq: Parent IRQ number
 * @root_domain: IRQ domain for child interrupts
 */
struct xilinx_smmu_csr {
	void __iomem *base;
	int parent_irq;
	struct irq_domain *root_domain;
};

static struct xilinx_smmu_csr *smmu_csr;

/* Interrupt types */
#define SMMU_INTR_EVENT		BIT(0)
#define SMMU_INTR_CMD_SYNC	BIT(1)
#define SMMU_INTR_GLOBAL	BIT(2)
#define SMMU_INTR_PRI		BIT(3)

/* Mask for all SMMU interrupts */
#define SMMU_INTR_ALL		(SMMU_INTR_EVENT | SMMU_INTR_CMD_SYNC | \
				 SMMU_INTR_GLOBAL | SMMU_INTR_PRI)

static int smmu_intr_mask = SMMU_INTR_EVENT; /* default: event interrupt enabled */

module_param_named(interrupt_mask, smmu_intr_mask, hexint, 0644);
MODULE_PARM_DESC(interrupt_mask, "Bitmask of interrupts to enable: 1=EVENT, 2=CMD_SYNC, 4=GLOBAL, 8=PRI");

#define ISR	0x24 /* Interrupt Status Register */
#define IMR	0x28 /* Interrupt Mask Register */
#define IER	0x2C /* Interrupt Enable Register */
#define IDR	0x30 /* Interrupt Disable Register */

static void smmu_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct xilinx_smmu_csr *smmu_intr = smmu_csr;
	int status, ret;

	chained_irq_enter(chip, desc);
	status = readl(smmu_intr->base + ISR);

	if (status) {
		/* Forward to SMMU interrupt handler */
		ret = generic_handle_domain_irq(smmu_intr->root_domain, 0);
		if (ret)
			pr_err("xilinx-smmu-csr: Failed to handle domain IRQ: %d\n", ret);

		/* Ack/clear interrupt */
		writel(status, smmu_intr->base + ISR);
	}

	chained_irq_exit(chip, desc);
}

static int xilinx_smmu_irq_map(struct irq_domain *d,
			       unsigned int irq,
			       irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops smmu_domain_ops = {
	.map = xilinx_smmu_irq_map,
	.xlate = irq_domain_xlate_onecell,
};

static int __init xilinx_smmu_csr_of_init(struct device_node *np,
					  struct device_node *parent)
{
	struct xilinx_smmu_csr *smmu_intr;
	int ret;

	if (smmu_csr)
		return -ENODEV;

	if (WARN_ON_ONCE(!parent))
		return -EINVAL;

	/* Allocate context */
	smmu_intr = kzalloc(sizeof(*smmu_intr), GFP_KERNEL);
	if (!smmu_intr)
		return -ENOMEM;

	smmu_intr->base = of_iomap(np, 0);
	if (!smmu_intr->base) {
		ret = -EINVAL;
		goto free_smmu;
	}

	/* Disable all interrupts to start with */
	writel(SMMU_INTR_ALL, smmu_intr->base + IDR);

	/* Ack all pending interrupts if any */
	writel(SMMU_INTR_ALL, smmu_intr->base + ISR);

	/* Create domain with one child IRQ (for SMMU interrupt handling) */
	smmu_intr->root_domain = irq_domain_add_linear(np, 1,
						       &smmu_domain_ops,
						       smmu_intr);
	if (!smmu_intr->root_domain) {
		pr_err("xilinx-smmu-csr: failed to create irqdomain\n");
		ret = -ENODEV;
		goto unmap;
	}

	/* Get parent IRQ */
	smmu_intr->parent_irq = irq_of_parse_and_map(np, 0);
	if (!smmu_intr->parent_irq) {
		pr_err("xilinx-smmu-csr: failed to get parent irq\n");
		ret = -EINVAL;
		goto remove_domain;
	}

	/* Chain handler into parent IRQ */
	irq_set_chained_handler_and_data(smmu_intr->parent_irq,
					 smmu_irq_handler,
					 smmu_intr);

	if (smmu_intr_mask & SMMU_INTR_EVENT)
		pr_debug("xilinx-smmu-csr: Enabling EVENT interrupt\n");
	if (smmu_intr_mask & SMMU_INTR_CMD_SYNC)
		pr_debug("xilinx-smmu-csr: Enabling CMD_SYNC interrupt\n");
	if (smmu_intr_mask & SMMU_INTR_GLOBAL)
		pr_debug("xilinx-smmu-csr: Enabling GLOBAL interrupt\n");
	if (smmu_intr_mask & SMMU_INTR_PRI)
		pr_debug("xilinx-smmu-csr: Enabling PRI interrupt\n");

	smmu_csr = smmu_intr;

	writel(smmu_intr_mask, smmu_csr->base + IER);

	pr_debug("xilinx-smmu-csr: registered\n");
	return 0;

remove_domain:
	irq_domain_remove(smmu_intr->root_domain);
unmap:
	iounmap(smmu_intr->base);
free_smmu:
	kfree(smmu_intr);
	return ret;
}

IRQCHIP_DECLARE(xilinx_smmu_csr, "xlnx,versal-net-smmu-csr",
		xilinx_smmu_csr_of_init);
