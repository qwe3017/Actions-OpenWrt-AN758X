/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared declarations between the WED register probe (airoha-wed.c) and the
 * mtk_soc_wed_ops implementation (airoha-wed-ops.c). Both live in the same
 * module because the ops table needs the ioremap'ed register banks that the
 * platform probe owns.
 */
#ifndef _AIROHA_WED_H
#define _AIROHA_WED_H

#include <linux/io.h>
#include <linux/types.h>

struct device;
struct device_node;

#define AIROHA_WED_MAX_BANKS		2

struct airoha_wed_bank {
	void __iomem		*base;
	unsigned long long	 phys;
	unsigned long long	 size;
	int			 irq;
	u32			 rev;
	u32			 slot;
	/* companion WDMA instance window (0x1fa06000 / 0x1fa06400) */
	void __iomem		*wdma;
	unsigned long long	 wdma_phys;
};

struct airoha_wed {
	struct device		*dev;
	u8			 nbank;
	struct airoha_wed_bank	 bank[AIROHA_WED_MAX_BANKS];
	struct dentry		*dbgfs;
};

/* Set by probe, cleared by remove. NULL until the platform device binds. */
struct airoha_wed *airoha_wed_get(void);

static inline u32 airoha_wed_read(const struct airoha_wed_bank *b, u32 reg)
{
	return readl(b->base + reg);
}

static inline void airoha_wed_write(const struct airoha_wed_bank *b, u32 reg,
				    u32 val)
{
	writel(val, b->base + reg);
}

static inline u32 airoha_wdma_read(const struct airoha_wed_bank *b, u32 reg)
{
	return readl(b->wdma + reg);
}

static inline void airoha_wdma_write(const struct airoha_wed_bank *b, u32 reg,
				     u32 val)
{
	writel(val, b->wdma + reg);
}

/* airoha-wed-ops.c */
int airoha_wed_ops_register(struct device_node *np);
void airoha_wed_ops_unregister(void);

#endif /* _AIROHA_WED_H */
