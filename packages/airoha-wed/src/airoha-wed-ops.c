// SPDX-License-Identifier: GPL-2.0-only
/*
 * Airoha AN7581 WED -- mtk_soc_wed_ops implementation (STAGE 3b).
 *
 * This is the piece that turns on mainline mt76's existing mt7915 WED
 * support. mt76 never talks to the WED registers directly: it calls
 * mtk_wed_device_attach() and then reaches the hardware purely through the
 * function pointers in `struct mtk_wed_ops`. Whoever fills the global
 * mtk_soc_wed_ops pointer owns the offload path.
 *
 * On a MediaTek SoC that pointer is filled by mtk_wed_add_hw(), called from
 * mtk_eth_soc. On AN7581 mtk_eth_soc never probes, so the pointer stays NULL
 * and mt7915 silently runs software-only. We fill it ourselves.
 *
 * The register sequence is mainline's WED-v1 (mt7622) path nearly verbatim.
 * That is deliberate and it is justified: the AN7581 block reads back
 * WED_REV = 0x76220001, i.e. literally MT7622 rev 1, and every constant
 * mainline writes for v1 matches what mt_whnat's wed_def.h holds for this
 * SoC (see airoha-wed-regs.h). Only three things are genuinely Airoha:
 *
 *   1. WED_PCIE_CFG_BASE must be the AN7581 PCIe controller base
 *      (0x1fc00000 / 0x1fc20000) instead of mt7622's 0x1a143000.
 *   2. There is no "mediatek,pcie-mirror" syscon. mt_whnat sets
 *      CFG_CR_MIRROR_SUPPORT=0 for ECNT and instead sets bit 20 of
 *      WED_PCIE_INT_CTRL. So the regmap_write(hw->mirror, ...) call in
 *      mainline mtk_wed_start()/mtk_wed_dma_disable() is dropped and
 *      replaced by that bit.
 *   3. hifsys HIFSYS_DMA_AG_MAP does not exist here either.
 *
 * Safety: the ops table is only published when the module parameter
 * wed_ops=1, and attach only completes when attach_enable=1. With either at
 * 0, mt7915's mtk_wed_device_attach() fails, mtk_wed_device_active() stays
 * false and Wi-Fi runs exactly as before -- no register is written. That
 * makes a broken build recoverable by editing a module parameter and
 * rebooting, without reflashing.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/pci.h>
#include <linux/of.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/bitfield.h>
#include <net/pkt_cls.h>
#include <net/flow_offload.h>
#include <linux/soc/mediatek/mtk_wed.h>

#include "airoha-wed.h"
#include "airoha-wed-regs.h"
#include "airoha-wdma.h"

/*
 * Without this kernel option struct mtk_wed_device carries none of the
 * fields the ops code touches (ops, dev, tx_buf_ring, ...) and mtk_wed.h
 * declares none of the inline wrappers. Compiling this file anyway would
 * break the build, so the whole thing collapses to stubs instead. That is
 * the correct behaviour: with the option off mt76 never asks for WED.
 */
#ifdef CONFIG_NET_MEDIATEK_SOC_WED

/* Geometry constants copied from mainline mtk_wed.c. They describe the WED
 * IP, not the SoC, so they carry over unchanged. */
#define WED_PKT_SIZE			1920
#define WED_BUF_SIZE			2048
#define WED_TX_RING_SIZE		2048
#define WED_RX_RING_SIZE		1536
#define WED_WDMA_RING_SIZE		1024
#define WED_BUF_PER_PAGE		(PAGE_SIZE / WED_BUF_SIZE)

/* WDMA/WPDMA descriptor, 16 bytes. Matches mainline struct mtk_wdma_desc,
 * which the public header only forward-declares. */
struct airoha_wdma_desc {
	__le32 buf0;
	__le32 buf1;
	__le32 info;
	__le32 ctrl;
};

#define WDMA_DESC_CTRL_DMA_DONE		BIT(31)
#define WDMA_DESC_CTRL_LAST_SEG0	BIT(30)
#define WDMA_DESC_CTRL_LEN0		GENMASK(29, 16)
#define WDMA_DESC_CTRL_LAST_SEG1	BIT(15)
#define WDMA_DESC_CTRL_LEN1		GENMASK(14, 0)

/* External interrupt bits (wed_def.h WED_EX_INT_STA fields). v1 adds
 * TX_DRV_R_RESP_ERR to the common error set, exactly as mainline does. */
#define WED_EXT_INT_ERROR_MASK						\
	(BIT(0) | BIT(1) | BIT(4) | BIT(8) | BIT(9) | BIT(12) |		\
	 BIT(13) | BIT(16) | BIT(17) | BIT(18) | BIT(19) | BIT(20) |	\
	 BIT(21) | BIT(22))
#define WED_EXT_INT_TX_DRV_R_RESP_ERR	BIT(21)

/* Interrupt trigger values for the v1 path. */
#define WED_PCIE_INT_TRIG_STATUS	(BIT(16) | BIT(24))
#define WED_WPDMA_INT_TRIG_RX_DONE	BIT(1)
#define WED_WPDMA_INT_TRIG_TX_DONE	(BIT(4) | BIT(5))
/*
 * WDMA receive-done interrupts live at bit 17:16, not bit 1:0.
 * wdma.h:232-233 defines WDMA_INT_MSK_RX_DONE_INT0/1 as BIT(16)/BIT(17) and
 * wed_def.h:498-499 defines WED_WDMA_INT_TRIG_FLD_RX_DONE0/1 with the same
 * shifts. Masking GENMASK(1, 0) enabled TX_DONE_INT1/2 instead, so the
 * receive path had no interrupt source at all and the RX driver never
 * advanced its DMA index.
 *
 * GENMASK() takes the high bit first: bits 17:16 are GENMASK(17, 16).
 */
#define WED_WDMA_INT_RX_DONE		GENMASK(17, 16)
#define WED_WPDMA_INT_CTRL_SUBRT_ADV	BIT(21)

#define WED_TX_BM_DYN_THR_LO		1
#define WED_TX_BM_DYN_THR_HI		GENMASK(22, 16)

static bool wed_ops;
module_param(wed_ops, bool, 0644);
MODULE_PARM_DESC(wed_ops, "Publish the mtk_soc_wed_ops table (0 keeps Wi-Fi software-only)");

static bool attach_enable;
module_param(attach_enable, bool, 0644);
MODULE_PARM_DESC(attach_enable, "Let attach() succeed (0 = dry run: init then roll back)");

/*
 * AN7581-SPECIFIC: module parameters do not work for this driver.
 *
 * The wed node is status = "okay", so the kernel matches it by modalias and
 * calls request_module() the moment the platform device is registered --
 * measured on-device at t=3.38s, which is *before* preinit (3.75s) and long
 * before procd reads /etc/modules.d (10.4s). By the time procd runs
 * "modprobe airoha-wed wed_ops=1", the module is already resident and the
 * arguments are silently dropped. Verified: /etc/modules.d with explicit
 * arguments still yields wed_ops=N, while
 * "insmod /lib/modules/6.18.52/airoha-wed.ko wed_ops=1" yields wed_ops=Y.
 *
 * So the switches have to come from the device tree, which is readable no
 * matter who loaded us. Properties override module parameters only when the
 * parameter was left at its default, so an explicit module parameter still
 * wins when the module is loaded by hand.
 */
static bool wed_ops_from_dt;
static bool attach_enable_from_dt;

static void airoha_wed_ops_read_dt(struct device_node *np)
{
	if (!of_property_read_bool(np, "airoha,wed-ops"))
		return;

	/*
	 * Only fill in what the module parameter did not set. That keeps a
	 * hand-written "insmod ... attach_enable=1" authoritative while still
	 * giving the auto-loaded path a working default.
	 */
	if (!wed_ops) {
		wed_ops = true;
		wed_ops_from_dt = true;
	}
	if (!attach_enable) {
		attach_enable = of_property_read_bool(np, "airoha,attach-enable");
		attach_enable_from_dt = true;
	}

	pr_info("airoha-wed: switches after device tree: wed_ops=%d%s attach_enable=%d%s\n",
		wed_ops, wed_ops_from_dt ? " (dt)" : "",
		attach_enable, attach_enable_from_dt ? " (dt)" : "");
}

/* One entry per WED instance: which mtk_wed_device is bound to which bank. */
struct airoha_wed_bind {
	struct mtk_wed_device	*dev;
	struct airoha_wed_bank	*bank;
	int			 index;
	bool			 used;
};

static struct airoha_wed_bind wed_bind[AIROHA_WED_MAX_BANKS];
static DEFINE_MUTEX(wed_bind_lock);

/* ------------------------------------------------------------- low level */

static struct airoha_wed_bind *airoha_wed_find(struct mtk_wed_device *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(wed_bind); i++)
		if (wed_bind[i].used && wed_bind[i].dev == dev)
			return &wed_bind[i];

	return NULL;
}

static void airoha_wed_reset(struct airoha_wed_bind *b, u32 mask)
{
	struct airoha_wed_bank *bank = b->bank;
	int i;
	u32 val;

	airoha_wed_write(bank, AIROHA_WED_MOD_RST, mask);
	for (i = 0; i < 1000; i++) {
		val = airoha_wed_read(bank, AIROHA_WED_MOD_RST);
		if (!(val & mask))
			return;
		udelay(1);
	}
	WARN_ONCE(1, "airoha-wed: reset mask 0x%08x did not clear\n", mask);
}

static int airoha_wed_poll_busy(struct airoha_wed_bind *b, u32 reg, u32 busy)
{
	struct airoha_wed_bank *bank = b->bank;
	int i;

	for (i = 0; i < 1000; i++) {
		if (!(airoha_wed_read(bank, reg) & busy))
			return 0;
		udelay(1);
	}

	return -ETIMEDOUT;
}

/* ------------------------------------------------------------ ring alloc */

static int airoha_wed_ring_alloc(struct mtk_wed_device *dev,
				 struct mtk_wed_ring *ring, int size,
				 bool tx)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wdma_desc *desc;
	int i;

	if (!b)
		return -ENODEV;

	ring->desc = dma_alloc_coherent(dev->dev, size * sizeof(*desc),
					&ring->desc_phys, GFP_KERNEL);
	if (!ring->desc)
		return -ENOMEM;

	ring->desc_size = sizeof(*desc);
	ring->size = size;

	/* ring->desc is the opaque struct mtk_wdma_desc *; our local layout
	 * is byte-identical (see the comment on the struct). */
	desc = (struct airoha_wdma_desc *)ring->desc;
	for (i = 0; i < size; i++) {
		desc->buf0 = 0;
		desc->buf1 = 0;
		desc->info = 0;
		desc->ctrl = cpu_to_le32(tx ? WDMA_DESC_CTRL_DMA_DONE : 0);
		desc++;
	}

	return 0;
}

static void airoha_wed_ring_free(struct mtk_wed_device *dev,
				 struct mtk_wed_ring *ring)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);

	if (!b || !ring->desc)
		return;

	dma_free_coherent(dev->dev, ring->size * ring->desc_size, ring->desc,
			  ring->desc_phys);
	ring->desc = NULL;
}

/* ------------------------------------------------------- tx buffer alloc */

/*
 * The WED TX buffer manager hands out 2 KiB buffers in which the WLAN driver
 * lays out its TX descriptors. dev->wlan.init_buf is mt76's
 * mt7915_wed_init_buf; it returns how many bytes of the buffer it consumed so
 * the descriptor can describe the remainder as the second segment.
 */
static int airoha_wed_tx_buffer_alloc(struct mtk_wed_device *dev)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	int token = dev->wlan.token_start;
	int i, page_idx = 0, n_pages, ring_size;
	struct mtk_wed_buf *page_list;
	dma_addr_t desc_phys;
	void *desc_ptr;
	u32 desc_size = sizeof(struct airoha_wdma_desc);

	if (!b || !dev->wlan.init_buf)
		return -ENODEV;

	ring_size = dev->wlan.nbuf & ~(WED_BUF_PER_PAGE - 1);
	dev->tx_buf_ring.size = ring_size;
	n_pages = ring_size / WED_BUF_PER_PAGE;
	if (!n_pages)
		return -EINVAL;

	page_list = kcalloc(n_pages, sizeof(*page_list), GFP_KERNEL);
	if (!page_list)
		return -ENOMEM;
	dev->tx_buf_ring.pages = page_list;

	desc_ptr = dma_alloc_coherent(dev->dev, ring_size * desc_size,
				      &desc_phys, GFP_KERNEL);
	if (!desc_ptr)
		return -ENOMEM;
	dev->tx_buf_ring.desc = desc_ptr;
	dev->tx_buf_ring.desc_phys = desc_phys;

	for (i = 0; i < ring_size; i += WED_BUF_PER_PAGE) {
		dma_addr_t page_phys, buf_phys;
		struct page *page;
		void *buf;
		int s;

		/* mainline uses __dev_alloc_page() here; alloc_page() is the
		 * same thing minus the page_frag reset and, unlike it, needs
		 * no symbol from the net stack in an out-of-tree module. */
		page = alloc_page(GFP_KERNEL | GFP_DMA32);
		if (!page)
			return -ENOMEM;

		page_phys = dma_map_page(dev->dev, page, 0, PAGE_SIZE,
					 DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev->dev, page_phys)) {
			__free_page(page);
			return -ENOMEM;
		}

		page_list[page_idx].p = page;
		page_list[page_idx++].phy_addr = page_phys;

		buf = page_to_virt(page);
		buf_phys = page_phys;

		for (s = 0; s < WED_BUF_PER_PAGE; s++) {
			struct airoha_wdma_desc *desc = desc_ptr;
			u32 ctrl, txd_size;

			desc->buf0 = cpu_to_le32(buf_phys);
			txd_size = dev->wlan.init_buf(buf, buf_phys, token++);
			desc->buf1 = cpu_to_le32(buf_phys + txd_size);
			ctrl = FIELD_PREP(WDMA_DESC_CTRL_LEN0, txd_size) |
			       WDMA_DESC_CTRL_LAST_SEG1 |
			       FIELD_PREP(WDMA_DESC_CTRL_LEN1,
					  WED_BUF_SIZE - txd_size);
			desc->info = 0;
			desc->ctrl = cpu_to_le32(ctrl);

			desc_ptr += desc_size;
			buf += WED_BUF_SIZE;
			buf_phys += WED_BUF_SIZE;
		}
	}

	return 0;
}

static void airoha_wed_free_tx_buffer(struct mtk_wed_device *dev)
{
	struct mtk_wed_buf *page_list = dev->tx_buf_ring.pages;
	int i, page_idx = 0;

	if (!page_list)
		return;

	if (dev->tx_buf_ring.desc) {
		for (i = 0; i < dev->tx_buf_ring.size; i += WED_BUF_PER_PAGE) {
			dma_addr_t page_phy = page_list[page_idx].phy_addr;
			void *page = page_list[page_idx++].p;

			if (!page)
				break;

			dma_unmap_page(dev->dev, page_phy, PAGE_SIZE,
				       DMA_BIDIRECTIONAL);
			__free_page(page);
		}

		dma_free_coherent(dev->dev,
				  dev->tx_buf_ring.size *
				  sizeof(struct airoha_wdma_desc),
				  dev->tx_buf_ring.desc,
				  dev->tx_buf_ring.desc_phys);
		dev->tx_buf_ring.desc = NULL;
	}

	kfree(page_list);
	dev->tx_buf_ring.pages = NULL;
}

/* --------------------------------------------------------- hw init paths */

static void airoha_wed_set_wpdma(struct airoha_wed_bind *b,
				 struct mtk_wed_device *dev)
{
	/* v1: a single base pointer, already computed by mt7915 as
	 * BAR0 + MT_WFDMA_EXT_CSR_BASE. mt_whnat writes the same thing
	 * (pci_resource_start | WPDMA_OFFSET) into the same register. */
	airoha_wed_write(b->bank, AIROHA_WED_WPDMA_CFG_BASE,
			 dev->wlan.wpdma_phys);
}

/*
 * The Airoha replacement for mainline's "mediatek,pcie-mirror" syscon write.
 * mt7622 tells WED where the WLAN WPDMA lives by programming a mirror
 * register; AN7581 instead points WED at the PCIe controller that carries
 * the MT7916 and lets WED translate. mt_whnat does the same in
 * whnat_hal_pcie_map(), plus the EN7581-only bit 20 of WED_PCIE_INT_CTRL.
 *
 * Called from both attach() and start() -- start() also runs after a reset,
 * and the mapping is part of what a reset clears.
 */
static void airoha_wed_pcie_map(struct airoha_wed_bind *b,
				struct mtk_wed_device *dev)
{
	struct airoha_wed_bank *bank = b->bank;
	struct pci_dev *pdev = dev->wlan.pci_dev;
	u8 bus;

	/*
	 * Pick the controller base from the PCI bus the radio is actually
	 * behind, not from the WED bank index. On this board MT7916D shows up
	 * as 0001:01:00.0 (pcie@1fc20000, bus 1) while the HIF is 0000:01:00.0
	 * (pcie@1fc00000, bus 0); bank 0 is the first free one and would
	 * otherwise program the wrong controller.
	 *
	 * dev->bus->number rather than the pci_bus_nr() helper: that inline
	 * disappeared from <linux/pci.h> during the 6.x cleanups.
	 */
	bus = pdev ? pdev->bus->number : b->index;

	airoha_wed_write(bank, AIROHA_WED_PCIE_CFG_BASE,
			 AIROHA_WED_PCIE_BASE_FOR_BUS(bus));
	airoha_wed_write(bank, AIROHA_WED_WPDMA_CFG_BASE,
			 dev->wlan.wpdma_phys);

	/* No CR mirror exists on AN7581, so WED has to poll the PCIe
	 * interrupt status register instead of waiting for a write-back. */
	airoha_wed_write(bank, AIROHA_WED_PCIE_INT_CTRL,
			 AIROHA_WED_PCIE_INT_CTRL_POLL_ALWAYS |
			 AIROHA_WED_PCIE_INT_CTRL_MSK_EN_POLA);
	airoha_wed_write(bank, AIROHA_WED_PCIE_INTS_TRIG,
			 AIROHA_WED_PCIE_INTS_TRIG_EN7581);
	airoha_wed_write(bank, AIROHA_WED_PCIE_OFST,
			 AIROHA_WED_PCIE_OFST_EN7581);

	/*
	 * Log through the platform device. struct airoha_wed_bank carries only
	 * the register windows, no struct device *, and dev->dev is already
	 * pointed at the WED platform device by attach().
	 */
	dev_info(dev->dev,
		 "airoha-wed: WED%d PCIe map: bus=%u cfg_base=0x%08x wpdma_phys=0x%08x int_ctrl=0x%08x\n",
		 b->index, bus,
		 airoha_wed_read(bank, AIROHA_WED_PCIE_CFG_BASE),
		 airoha_wed_read(bank, AIROHA_WED_WPDMA_CFG_BASE),
		 airoha_wed_read(bank, AIROHA_WED_PCIE_INT_CTRL));
}

static void airoha_wed_hw_init_early(struct airoha_wed_bind *b,
				     struct mtk_wed_device *dev)
{
	struct airoha_wed_bank *bank = b->bank;
	u32 set = FIELD_PREP(AIROHA_WED_WDMA_BT_SIZE, 2);
	u32 mask = AIROHA_WED_WDMA_BT_SIZE;
	u32 offset = b->index ? 0x04000400 : 0;

	airoha_wed_reset(b, AIROHA_WED_RST_WED);
	airoha_wed_set_wpdma(b, dev);

	/*
	 * AN7581 needs the ECNT branch of whnat_hal_wed_init(), not the
	 * mt7622 one that mainline carries. The vendor clears [25][24][23]
	 * and sets [21] AXI_W_AFTER_AW_EN plus [16] WCOMPLETE_SEL with the
	 * comment "for fix WDMA stress fail issue on ECNT platform"; the
	 * mainline/mt7622 path instead *sets* [24] and [23]. Setting them
	 * here leaves the RX driver fed but never completing AXI writes, so
	 * it polls the ring without ever consuming PSE port 3.
	 */
	mask |= AIROHA_WED_WDMA_DYNAMIC_DMAD_RECYCLE |
		AIROHA_WED_WDMA_RX_DIS_FSM_AUTO_IDLE |
		AIROHA_WED_WDMA_SKIP_DMAD_PREPARE |
		AIROHA_WED_WDMA_IDLE_DMAD_SUPPLY;
	set |= AIROHA_WED_WDMA_AXI_W_AFTER_AW_EN |
	       AIROHA_WED_WDMA_WCOMPLETE_SEL;

	airoha_wed_write(bank, AIROHA_WED_WDMA_GLO_CFG,
			 (airoha_wed_read(bank, AIROHA_WED_WDMA_GLO_CFG) &
			  ~mask) | set);
	dev_info(dev->dev,
		 "airoha-wed: WED%d WDMA glo_cfg 0x%08x\n", bank->slot,
		 airoha_wed_read(bank, AIROHA_WED_WDMA_GLO_CFG));

	/* WDMA must reserve the three RX info words before WED reads it. The
	 * airoha-wdma driver already does this; do not clear it here. */
	airoha_wdma_write(bank, AIROHA_WDMA_GLO_CFG,
			  airoha_wdma_read(bank, AIROHA_WDMA_GLO_CFG) |
			  AIROHA_WDMA_GLO_RX_INFO1_PRERES |
			  AIROHA_WDMA_GLO_RX_INFO2_PRERES |
			  AIROHA_WDMA_GLO_RX_INFO3_PRERES);

	/*
	 * The WED engine reaches its companion WDMA through WED_WDMA_CFG_BASE
	 * plus the bank selectors in OFST0/OFST1, and the two must describe the
	 * same hardware. The mt7622 pair is CFG_BASE 0x1b100000 with
	 * OFST1 0x29002800, resolving to ring banks at 0x1b102800 -- an address
	 * that does not exist here, so the RX driver quietly polled nothing and
	 * every packet queued on PSE port 3 stayed there.
	 *
	 * On the AN7581 the window is at 0x1fa06000 / 0x1fa06400, so CFG_BASE
	 * comes from the real window address and OFST0/OFST1 from the matching
	 * ECNT constants: 0x1fa00000 + 0x6000 = 0x1fa06000 for instance 0 and
	 * + 0x6400 = 0x1fa06400 for instance 1.
	 */
	if (bank->wdma_phys) {
		u32 cfg_base = (u32)(bank->wdma_phys -
				     (b->index ? 0x6400 : 0x6000));

		airoha_wed_write(bank, AIROHA_WED_WDMA_CFG_BASE, cfg_base);
		dev_info(dev->dev,
			 "airoha-wed: WED%d WDMA cfg_base 0x%08x for window 0x%llx\n",
			 bank->slot, cfg_base, bank->wdma_phys);
	}

	airoha_wed_write(bank, AIROHA_WED_WDMA_OFST0, 0x62046220 + offset);
	airoha_wed_write(bank, AIROHA_WED_WDMA_OFST1, 0x61006000 + offset);

	airoha_wed_pcie_map(b, dev);
}

static void airoha_wed_set_ext_int(struct airoha_wed_bind *b, bool en)
{
	struct airoha_wed_bank *bank = b->bank;
	u32 mask = WED_EXT_INT_ERROR_MASK | WED_EXT_INT_TX_DRV_R_RESP_ERR;

	airoha_wed_write(bank, AIROHA_WED_EX_INT_MSK, en ? mask : 0);
	airoha_wed_read(bank, AIROHA_WED_EX_INT_MSK);
}

/*
 * WED_WDMA_RX{0,1}_THRES_CFG gate the WDMA receive driver.
 *
 * mainline mtk_wed_mainline.c never writes them: on MT7622 the bootloader
 * pre-loads the values. AN7581's bootloader does not, so the registers keep
 * their reset value 0x00040020, whose DRX_CRX_DISTANCE_THRES field is 0. With a
 * zero distance RX_DRV concludes that no descriptor is pending and leaves
 * RX0PROC at zero forever, so the packets we hand to PSE port 3 are never
 * drained. Vendor writes this unconditionally in whnat_hal_wed_init()
 * (woe_hw.c:1149-1152) with WAIT_BM_CNT_MAX = 0xffff and distance = len - 3.
 */
static void airoha_wed_configure_rx_drv(struct airoha_wed_bind *b,
					struct mtk_wed_device *dev)
{
	struct airoha_wed_bank *bank = b->bank;
	unsigned int ring_len = WED_WDMA_RING_SIZE;
	unsigned int thres;
	int i;

	/*
	 * Fetch the depth the rings were actually programmed with rather than
	 * assuming: airoha_wed_tx_ring_setup() may have used a different size,
	 * and the vendor distance threshold is derived from it.
	 */
	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++) {
		if (dev->rx_wdma[i].size) {
			ring_len = dev->rx_wdma[i].size;
			break;
		}
	}

	thres = AIROHA_WED_RX_THRES_WAIT_BM_CNT_MAX |
		FIELD_PREP(AIROHA_WED_RX_THRES_DRX_CRX_DISTANCE,
			   (ring_len - 3) & GENMASK(11, 0));

	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++) {
		airoha_wed_write(bank, AIROHA_WED_WDMA_RX_THRES_CFG(i),
				 thres);

		/*
		 * Vendor's whnat_hal_wdma_ring_init() only programs BASE and
		 * COUNT for these rings, so a stale CPU index survives from
		 * power-on. A WED-mirror index that disagrees with the WDMA
		 * window looks like a desynchronised ring to RX_DRV; clear both
		 * copies so the driver starts from a known state.
		 */
		airoha_wed_write(bank, AIROHA_WED_WDMA_RING_RX(i) +
				 AIROHA_WED_RING_OFS_CPU_IDX, 0);
		airoha_wed_write(bank, AIROHA_WED_WDMA_RING_RX(i) +
				 AIROHA_WED_RING_OFS_DMA_IDX, 0);
		airoha_wdma_write(bank, AIROHA_WDMA_RING_RX(i) +
				  AIROHA_WDMA_RING_OFS_CPU_IDX, 0);
		airoha_wdma_write(bank, AIROHA_WDMA_RING_RX(i) +
				  AIROHA_WDMA_RING_OFS_DMA_IDX, 0);
	}

	dev_info(dev->dev,
		 "airoha-wed: WED%d RX driver thresholds: ring_len=%u thres=0x%08x\n",
		 bank->slot, ring_len, thres);
}

static void airoha_wed_hw_init(struct airoha_wed_bind *b,
			       struct mtk_wed_device *dev)
{
	struct airoha_wed_bank *bank = b->bank;

	if (dev->init_done)
		return;

	dev->init_done = true;
	airoha_wed_set_ext_int(b, false);

	/* Must precede the TXBM reset below: RX_DRV latches these on reset. */
	airoha_wed_configure_rx_drv(b, dev);

	airoha_wed_write(bank, AIROHA_WED_TX_BM_BASE,
			 dev->tx_buf_ring.desc_phys);
	airoha_wed_write(bank, AIROHA_WED_TX_BM_BLEN, WED_PKT_SIZE);

	airoha_wed_write(bank, AIROHA_WED_TX_BM_CTRL,
			 AIROHA_WED_TX_BM_CTRL_PAUSE |
			 FIELD_PREP(AIROHA_WED_TX_BM_CTRL_VLD_GRP,
				    dev->tx_buf_ring.size / 128) |
			 FIELD_PREP(AIROHA_WED_TX_BM_CTRL_RSV_GRP,
				    WED_TX_RING_SIZE / 256));
	airoha_wed_write(bank, AIROHA_WED_TX_BM_DYN_TH,
			 FIELD_PREP(AIROHA_WED_TX_BM_DYN_TH_LO,
				    WED_TX_BM_DYN_THR_LO) |
			 WED_TX_BM_DYN_THR_HI);

	airoha_wed_write(bank, AIROHA_WED_TX_BM_TKID,
			 FIELD_PREP(AIROHA_WED_TX_BM_TKID_START,
				    dev->wlan.token_start) |
			 FIELD_PREP(AIROHA_WED_TX_BM_TKID_END,
				    dev->wlan.token_start +
				    dev->wlan.nbuf - 1));

	airoha_wed_reset(b, AIROHA_WED_RST_TX_BM);

	airoha_wed_write(bank, AIROHA_WED_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_CTRL) |
			 AIROHA_WED_CTRL_WED_TX_BM_EN |
			 AIROHA_WED_CTRL_WED_TX_FREE_AGT_EN);

	airoha_wed_write(bank, AIROHA_WED_TX_BM_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_TX_BM_CTRL) &
			 ~AIROHA_WED_TX_BM_CTRL_PAUSE);
}

static void airoha_wed_configure_irq(struct airoha_wed_bind *b, u32 irq_mask)
{
	struct airoha_wed_bank *bank = b->bank;
	u32 wdma_mask = WED_WDMA_INT_RX_DONE;

	airoha_wed_write(bank, AIROHA_WED_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_CTRL) |
			 AIROHA_WED_CTRL_WDMA_INT_AGT_EN |
			 AIROHA_WED_CTRL_WPDMA_INT_AGT_EN |
			 AIROHA_WED_CTRL_WED_TX_BM_EN |
			 AIROHA_WED_CTRL_WED_TX_FREE_AGT_EN);

	airoha_wed_write(bank, AIROHA_WED_PCIE_INTS_TRIG,
			 WED_PCIE_INT_TRIG_STATUS);
	airoha_wed_write(bank, AIROHA_WED_WPDMA_INT_TRIG,
			 WED_WPDMA_INT_TRIG_RX_DONE |
			 WED_WPDMA_INT_TRIG_TX_DONE);
	airoha_wed_write(bank, AIROHA_WED_WDMA_INT_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_WDMA_INT_CTRL) &
			 ~wdma_mask);

	airoha_wed_write(bank, AIROHA_WED_WDMA_INT_TRIG, wdma_mask);
	airoha_wdma_write(bank, AIROHA_WDMA_INT_MSK, wdma_mask);
	airoha_wdma_write(bank, AIROHA_WDMA_INT_GRP2, wdma_mask);
	airoha_wed_write(bank, AIROHA_WED_WPDMA_INT_MSK, irq_mask);
	airoha_wed_write(bank, AIROHA_WED_INT_MSK, irq_mask);
}

static void airoha_wed_dma_enable(struct airoha_wed_bind *b)
{
	struct airoha_wed_bank *bank = b->bank;

	airoha_wed_write(bank, AIROHA_WED_WPDMA_INT_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_WPDMA_INT_CTRL) |
			 WED_WPDMA_INT_CTRL_SUBRT_ADV);
	airoha_wed_write(bank, AIROHA_WED_WPDMA_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_WPDMA_GLO_CFG) |
			 AIROHA_WED_WPDMA_TX_DRV_EN |
			 AIROHA_WED_WPDMA_RX_DRV_EN);
	airoha_wdma_write(bank, AIROHA_WDMA_GLO_CFG,
			 airoha_wdma_read(bank, AIROHA_WDMA_GLO_CFG) |
			 AIROHA_WDMA_GLO_TX_DMA_EN |
			 AIROHA_WDMA_GLO_RX_INFO1_PRERES |
			 AIROHA_WDMA_GLO_RX_INFO2_PRERES);

	airoha_wed_write(bank, AIROHA_WED_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_GLO_CFG) |
			 AIROHA_WED_GLO_TX_DMA_EN | AIROHA_WED_GLO_RX_DMA_EN);
	airoha_wed_write(bank, AIROHA_WED_WDMA_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_WDMA_GLO_CFG) |
			 AIROHA_WED_WDMA_RX_DRV_EN);

	/* v1 only: the third RX info word is reserved by WED itself. */
	airoha_wdma_write(bank, AIROHA_WDMA_GLO_CFG,
			 airoha_wdma_read(bank, AIROHA_WDMA_GLO_CFG) |
			 AIROHA_WDMA_GLO_RX_INFO3_PRERES);
}

static void airoha_wed_dma_disable(struct airoha_wed_bind *b)
{
	struct airoha_wed_bank *bank = b->bank;

	airoha_wed_write(bank, AIROHA_WED_WPDMA_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_WPDMA_GLO_CFG) &
			 ~(AIROHA_WED_WPDMA_TX_DRV_EN |
			   AIROHA_WED_WPDMA_RX_DRV_EN));
	airoha_wed_write(bank, AIROHA_WED_WDMA_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_WDMA_GLO_CFG) &
			 ~AIROHA_WED_WDMA_RX_DRV_EN);
	airoha_wed_write(bank, AIROHA_WED_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_GLO_CFG) &
			 ~(AIROHA_WED_GLO_TX_DMA_EN |
			   AIROHA_WED_GLO_RX_DMA_EN));
	airoha_wdma_write(bank, AIROHA_WDMA_GLO_CFG,
			  airoha_wdma_read(bank, AIROHA_WDMA_GLO_CFG) &
			  ~(AIROHA_WDMA_GLO_TX_DMA_EN |
			    AIROHA_WDMA_GLO_RX_INFO1_PRERES |
			    AIROHA_WDMA_GLO_RX_INFO2_PRERES |
			    AIROHA_WDMA_GLO_RX_INFO3_PRERES));
}

/* ------------------------------------------------------------- the ops */

/*
 * Called under rcu_read_lock() by mtk_wed_device_attach(). Mainline releases
 * the lock as its very first action; we must do the same or the RCU read-side
 * critical section never ends and the CPU stalls.
 */
static int airoha_wed_attach(struct mtk_wed_device *dev)
	__releases(RCU)
{
	struct airoha_wed *wed = airoha_wed_get();
	struct airoha_wed_bind *b = NULL;
	struct device *wlan_dev;
	int i, ret = -ENODEV;

	rcu_read_unlock();

	if (!wed) {
		pr_info("airoha-wed: attach without a bound WED device\n");
		return -ENODEV;
	}

	wlan_dev = dev->wlan.bus_type == MTK_WED_BUS_PCIE
		 ? &dev->wlan.pci_dev->dev
		 : &dev->wlan.platform_dev->dev;

	mutex_lock(&wed_bind_lock);

	for (i = 0; i < ARRAY_SIZE(wed_bind); i++) {
		if (!wed_bind[i].used) {
			b = &wed_bind[i];
			break;
		}
	}
	if (!b || b->index >= wed->nbank || !wed->bank[b->index].base ||
	    !wed->bank[b->index].wdma) {
		dev_info(wlan_dev, "airoha-wed: no free WED instance\n");
		goto unlock;
	}

	b->dev = dev;
	b->bank = &wed->bank[b->index];
	b->used = true;

	dev_info(wlan_dev,
		 "airoha-wed: attaching WED%d (rev 0x%08x, irq %d) wpdma_phys=0x%08x nbuf=%u token_start=%u\n",
		 b->index, b->bank->rev, b->bank->irq,
		 dev->wlan.wpdma_phys, dev->wlan.nbuf, dev->wlan.token_start);

	dev->dev = wed->dev;
	dev->irq = b->bank->irq;
	dev->wdma_idx = b->index;
	dev->version = 1;

	ret = dma_set_mask_and_coherent(wed->dev, DMA_BIT_MASK(32));
	if (ret)
		goto release;

	ret = airoha_wed_tx_buffer_alloc(dev);
	if (ret)
		goto release;

	airoha_wed_hw_init_early(b, dev);

	if (!attach_enable) {
		/*
		 * Dry run: hardware is now programmed exactly as a real
		 * attach would leave it, but we roll back and refuse. mt76
		 * clears dev->ops and never calls us again, so Wi-Fi keeps
		 * running in software. Use this to read back the register
		 * window after a real programming attempt.
		 */
		dev_info(wed->dev,
			 "airoha-wed: dry run -- WED_CTRL=0x%08x WDMA_GLO_CFG=0x%08x PCIE_CFG_BASE=0x%08x WPDMA_CFG_BASE=0x%08x\n",
			 airoha_wed_read(b->bank, AIROHA_WED_CTRL),
			 airoha_wed_read(b->bank, AIROHA_WED_WDMA_GLO_CFG),
			 airoha_wed_read(b->bank, AIROHA_WED_PCIE_CFG_BASE),
			 airoha_wed_read(b->bank, AIROHA_WED_WPDMA_CFG_BASE));
		airoha_wed_free_tx_buffer(dev);
		ret = -ENODEV;
		goto release;
	}

	dev_info(wed->dev, "airoha-wed: WED%d attached\n", b->index);
	goto unlock;

release:
	b->used = false;
	b->dev = NULL;
	b->bank = NULL;
unlock:
	mutex_unlock(&wed_bind_lock);

	return ret;
}

/*
 * WDMA RX ring idx is the PPE->WiFi feed paired with WED TX ring idx. It is
 * programmed twice: once in the WDMA's own window, once in the WED's mirror
 * of it. mainline does exactly the same in mtk_wed_wdma_rx_ring_setup().
 */
static int airoha_wed_wdma_rx_ring_setup(struct mtk_wed_device *dev, int idx,
					 int size, bool reset)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wed_bank *bank;
	struct mtk_wed_ring *wdma;
	u32 base;

	if (!b || idx >= ARRAY_SIZE(dev->rx_wdma))
		return -EINVAL;

	bank = b->bank;
	wdma = &dev->rx_wdma[idx];

	if (!reset && airoha_wed_ring_alloc(dev, wdma, size, true))
		return -ENOMEM;

	base = AIROHA_WDMA_RING_RX(idx);
	airoha_wdma_write(bank, base + AIROHA_WDMA_RING_OFS_BASE,
			  wdma->desc_phys);
	airoha_wdma_write(bank, base + AIROHA_WDMA_RING_OFS_COUNT, size);
	airoha_wdma_write(bank, base + AIROHA_WDMA_RING_OFS_CPU_IDX, 0);

	airoha_wed_write(bank, AIROHA_WED_WDMA_RING_RX(idx) +
			 AIROHA_WED_RING_OFS_BASE, wdma->desc_phys);
	airoha_wed_write(bank, AIROHA_WED_WDMA_RING_RX(idx) +
			 AIROHA_WED_RING_OFS_COUNT, size);

	return 0;
}

static int airoha_wed_tx_ring_setup(struct mtk_wed_device *dev, int idx,
				    void __iomem *regs, bool reset)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wed_bank *bank;
	struct mtk_wed_ring *ring;

	if (!b || idx >= ARRAY_SIZE(dev->tx_ring))
		return -EINVAL;

	bank = b->bank;
	ring = &dev->tx_ring[idx];

	if (!reset && airoha_wed_ring_alloc(dev, ring, WED_TX_RING_SIZE, true))
		return -ENOMEM;

	if (airoha_wed_wdma_rx_ring_setup(dev, idx, WED_WDMA_RING_SIZE, reset))
		return -ENOMEM;

	/* WED -> WPDMA: mt76's ring goes into WED's TX window, WED's own
	 * ring is published to the WLAN WPDMA. */
	ring->reg_base = AIROHA_WED_RING_TX(idx);
	ring->wpdma = regs;
	ring->flags |= MTK_WED_RING_CONFIGURED;

	writel(ring->desc_phys, regs + AIROHA_WED_RING_OFS_BASE);
	writel(WED_TX_RING_SIZE, regs + AIROHA_WED_RING_OFS_COUNT);
	writel(0, regs + AIROHA_WED_RING_OFS_CPU_IDX);

	airoha_wed_write(bank, AIROHA_WED_WPDMA_RING_TX(idx) +
			 AIROHA_WED_RING_OFS_BASE, ring->desc_phys);
	airoha_wed_write(bank, AIROHA_WED_WPDMA_RING_TX(idx) +
			 AIROHA_WED_RING_OFS_COUNT, WED_TX_RING_SIZE);
	airoha_wed_write(bank, AIROHA_WED_WPDMA_RING_TX(idx) +
			 AIROHA_WED_RING_OFS_CPU_IDX, 0);

	return 0;
}

static int airoha_wed_rx_ring_setup(struct mtk_wed_device *dev, int idx,
				    void __iomem *regs, bool reset)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wed_bank *bank;
	struct mtk_wed_ring *ring;

	if (!b || idx >= ARRAY_SIZE(dev->rx_ring))
		return -EINVAL;

	bank = b->bank;
	ring = &dev->rx_ring[idx];

	if (!reset && airoha_wed_ring_alloc(dev, ring, WED_RX_RING_SIZE, false))
		return -ENOMEM;

	ring->reg_base = AIROHA_WED_RING_RX(idx);
	ring->wpdma = regs;
	ring->flags |= MTK_WED_RING_CONFIGURED;

	writel(ring->desc_phys, regs + AIROHA_WED_RING_OFS_BASE);
	writel(WED_RX_RING_SIZE, regs + AIROHA_WED_RING_OFS_COUNT);

	airoha_wed_write(bank, AIROHA_WED_WPDMA_RING_RX(idx) +
			 AIROHA_WED_RING_OFS_BASE, ring->desc_phys);
	airoha_wed_write(bank, AIROHA_WED_WPDMA_RING_RX(idx) +
			 AIROHA_WED_RING_OFS_COUNT, WED_RX_RING_SIZE);

	return 0;
}

static int airoha_wed_txfree_ring_setup(struct mtk_wed_device *dev,
					void __iomem *regs)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wed_bank *bank;
	struct mtk_wed_ring *ring = &dev->txfree_ring;
	int i;

	if (!b)
		return -ENODEV;

	bank = b->bank;
	ring->reg_base = AIROHA_WED_RING_RX(1);
	ring->wpdma = regs;

	for (i = 0; i < 12; i += 4) {
		u32 val = readl(regs + i);

		airoha_wed_write(bank, AIROHA_WED_RING_RX(1) + i, val);
		airoha_wed_write(bank, AIROHA_WED_WPDMA_RING_RX(1) + i, val);
	}

	return 0;
}

static void airoha_wed_start(struct mtk_wed_device *dev, u32 irq_mask)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	int i;

	if (!b)
		return;

	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++)
		if (!dev->rx_wdma[i].desc)
			airoha_wed_wdma_rx_ring_setup(dev, i, 16, false);

	airoha_wed_hw_init(b, dev);
	airoha_wed_configure_irq(b, irq_mask);
	airoha_wed_set_ext_int(b, true);

	/* AIROHA: replaces mainline's regmap_write(hw->mirror, ...). */
	airoha_wed_pcie_map(b, dev);

	airoha_wed_dma_enable(b);
	dev->running = true;

	dev_info(dev->dev, "airoha-wed: WED%d started (irq_mask=0x%08x)\n",
		 b->index, irq_mask);
}

static void airoha_wed_stop(struct mtk_wed_device *dev)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wed_bank *bank;

	if (!b)
		return;

	bank = b->bank;
	airoha_wed_dma_disable(b);
	airoha_wed_set_ext_int(b, false);

	airoha_wed_write(bank, AIROHA_WED_WPDMA_INT_TRIG, 0);
	airoha_wed_write(bank, AIROHA_WED_WDMA_INT_TRIG, 0);
	airoha_wdma_write(bank, AIROHA_WDMA_INT_MSK, 0);
	airoha_wdma_write(bank, AIROHA_WDMA_INT_GRP2, 0);
	dev->running = false;
}

/*
 * Drain the WDMA RX path so the frames it already pulled from the FE come
 * back to the PSE shared buffer.
 *
 * Stopping the RX driver is not enough: whnat's woe_hw.c reset sequence
 * (airoha_wed_wdma/woe_hw.c:515-548) does three more things that we used to
 * skip, and without them the received frames stay pinned in hardware.
 *
 * 1. Wait for WED_WDMA_RX_DRV_BUSY to fall, i.e. the driver is handed back
 *    the ring and is no longer fetching. Resetting the index underneath a
 *    running driver is what leaves the descriptors half-owned.
 * 2. Reset WDMA_RST_IDX on the *WDMA* window (DRX0/DRX1), then zero
 *    WDMA_RX_CRX_IDX0/1. Resetting only the WED-side mirror rewinds the
 *    producer while the consumer counters still point into the old frames.
 * 3. Pulse WED_WDMA_GLO_CFG_RST_INIT_COMPLETE. The reset above is only a
 *    request; this is the hardware's acknowledgement handshake, and skipping
 *    it leaves the block stalled mid-recovery.
 *
 * This matters because every attach/detach cycle leaks otherwise: measured on
 * the real device, one WED activation followed by a rollback left ~11880
 * frames in FE port 3, matching PSE_SHARE_BUF_STA share_use 11833 of 12064
 * with share_free down to 231 pages.
 */
static void airoha_wed_drain_rx(struct airoha_wed_bind *b)
{
	struct airoha_wed *wed = airoha_wed_get();
	struct airoha_wed_bank *bank = b->bank;
	u32 val;
	int ret;

	ret = airoha_wed_poll_busy(b, AIROHA_WED_WDMA_GLO_CFG,
				    AIROHA_WED_WDMA_RX_DRV_BUSY);
	if (ret && wed)
		dev_warn(wed->dev,
			 "WED%d: RX driver still busy after stop, draining anyway\n",
			 b->index);

	/* Producer index first, then the consumer counters that referenced
	 * the frames it had already handed over.
	 */
	airoha_wdma_write(bank, AIROHA_WDMA_RST_IDX,
			  AIROHA_WDMA_RST_DRX_IDX(0) |
			  AIROHA_WDMA_RST_DRX_IDX(1));
	airoha_wdma_write(bank, AIROHA_WDMA_RST_IDX, 0);
	airoha_wdma_write(bank, AIROHA_WDMA_RX_CRX_IDX0, 0);
	airoha_wdma_write(bank, AIROHA_WDMA_RX_CRX_IDX1, 0);

	/* Tell WED the WDMA reset it asked for has completed. */
	val = airoha_wed_read(bank, AIROHA_WED_WDMA_GLO_CFG) |
	      AIROHA_WED_WDMA_RST_INIT_COMPLETE;
	airoha_wed_write(bank, AIROHA_WED_WDMA_GLO_CFG, val);
	airoha_wed_write(bank, AIROHA_WED_WDMA_GLO_CFG,
			 val & ~AIROHA_WED_WDMA_RST_INIT_COMPLETE);
}

static void airoha_wed_detach(struct mtk_wed_device *dev)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wed_bank *bank;
	int i;

	if (!b)
		return;

	bank = b->bank;
	airoha_wed_stop(dev);

	airoha_wed_write(bank, AIROHA_WED_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_CTRL) &
			 ~(AIROHA_WED_CTRL_WDMA_INT_AGT_EN |
			   AIROHA_WED_CTRL_WPDMA_INT_AGT_EN |
			   AIROHA_WED_CTRL_WED_TX_BM_EN |
			   AIROHA_WED_CTRL_WED_TX_FREE_AGT_EN));

	airoha_wed_drain_rx(b);

	airoha_wed_write(bank, AIROHA_WED_WDMA_RST_IDX,
			 AIROHA_WED_WDMA_RST_IDX_RX |
			 AIROHA_WED_WDMA_RST_IDX_DRV);
	airoha_wed_write(bank, AIROHA_WED_WDMA_RST_IDX, 0);
	airoha_wed_reset(b, AIROHA_WED_RST_WED);

	airoha_wed_free_tx_buffer(dev);
	for (i = 0; i < ARRAY_SIZE(dev->tx_ring); i++)
		airoha_wed_ring_free(dev, &dev->tx_ring[i]);
	for (i = 0; i < ARRAY_SIZE(dev->rx_wdma); i++)
		airoha_wed_ring_free(dev, &dev->rx_wdma[i]);
	airoha_wed_ring_free(dev, &dev->txfree_ring);

	for (i = 0; i < ARRAY_SIZE(dev->rx_ring); i++)
		airoha_wed_ring_free(dev, &dev->rx_ring[i]);

	dev_info(dev->dev, "airoha-wed: WED%d detached\n", b->index);

	mutex_lock(&wed_bind_lock);
	b->used = false;
	b->dev = NULL;
	b->bank = NULL;
	mutex_unlock(&wed_bind_lock);
}

static void airoha_wed_reset_dma(struct mtk_wed_device *dev)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	struct airoha_wed_bank *bank;
	bool busy;
	int i;

	if (!b)
		return;

	bank = b->bank;

	airoha_wed_write(bank, AIROHA_WED_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_GLO_CFG) &
			 ~AIROHA_WED_GLO_TX_DMA_EN);
	busy = !!airoha_wed_poll_busy(b, AIROHA_WED_GLO_CFG,
				      AIROHA_WED_GLO_TX_DMA_BUSY);
	if (busy) {
		airoha_wed_reset(b, AIROHA_WED_RST_WED_TX_DMA);
	} else {
		airoha_wed_write(bank, AIROHA_WED_RST_IDX,
				 AIROHA_WED_RST_IDX_TX_MASK);
		airoha_wed_write(bank, AIROHA_WED_RST_IDX, 0);
	}

	airoha_wed_write(bank, AIROHA_WED_WDMA_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_WDMA_GLO_CFG) &
			 ~AIROHA_WED_WDMA_RX_DRV_EN);
	busy = !!airoha_wed_poll_busy(b, AIROHA_WED_WDMA_GLO_CFG,
				      AIROHA_WED_WDMA_RX_DRV_BUSY);
	if (busy) {
		airoha_wed_reset(b, AIROHA_WED_RST_WDMA_INT_AGT);
		airoha_wed_reset(b, AIROHA_WED_RST_WDMA_RX_DRV);
	} else {
		airoha_wed_drain_rx(b);
		airoha_wed_write(bank, AIROHA_WED_WDMA_RST_IDX,
				 AIROHA_WED_WDMA_RST_IDX_RX |
				 AIROHA_WED_WDMA_RST_IDX_DRV);
		airoha_wed_write(bank, AIROHA_WED_WDMA_RST_IDX, 0);
	}

	airoha_wed_write(bank, AIROHA_WED_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_CTRL) &
			 ~AIROHA_WED_CTRL_WED_TX_FREE_AGT_EN);

	for (i = 0; i < 100; i++) {
		u32 val = FIELD_GET(AIROHA_WED_TX_BM_INTF_TKFIFO_FDEP,
				    airoha_wed_read(bank,
						    AIROHA_WED_TX_BM_INTF));
		if (val == 0x40)
			break;
		udelay(1);
	}

	airoha_wed_reset(b, AIROHA_WED_RST_TX_FREE_AGT);
	airoha_wed_write(bank, AIROHA_WED_CTRL,
			 airoha_wed_read(bank, AIROHA_WED_CTRL) &
			 ~AIROHA_WED_CTRL_WED_TX_BM_EN);
	airoha_wed_reset(b, AIROHA_WED_RST_TX_BM);

	busy = !!airoha_wed_poll_busy(b, AIROHA_WED_WPDMA_GLO_CFG,
				      AIROHA_WED_WPDMA_TX_DRV_BUSY);
	airoha_wed_write(bank, AIROHA_WED_WPDMA_GLO_CFG,
			 airoha_wed_read(bank, AIROHA_WED_WPDMA_GLO_CFG) &
			 ~(AIROHA_WED_WPDMA_TX_DRV_EN |
			   AIROHA_WED_WPDMA_RX_DRV_EN));
	if (!busy)
		busy = !!airoha_wed_poll_busy(b, AIROHA_WED_WPDMA_GLO_CFG,
					      AIROHA_WED_WPDMA_RX_DRV_BUSY);

	if (busy) {
		airoha_wed_reset(b, AIROHA_WED_RST_WPDMA_INT_AGT);
		airoha_wed_reset(b, AIROHA_WED_RST_WPDMA_TX_DRV);
		airoha_wed_reset(b, AIROHA_WED_RST_WPDMA_RX_DRV);
	} else {
		airoha_wed_write(bank, AIROHA_WED_WPDMA_RST_IDX,
				 AIROHA_WED_WPDMA_RST_IDX_TX |
				 AIROHA_WED_WPDMA_RST_IDX_RX);
		airoha_wed_write(bank, AIROHA_WED_WPDMA_RST_IDX, 0);
	}

	dev->init_done = false;
}

static u32 airoha_wed_reg_read(struct mtk_wed_device *dev, u32 reg)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);

	if (!b)
		return 0;

	return airoha_wed_read(b->bank, reg);
}

static void airoha_wed_reg_write(struct mtk_wed_device *dev, u32 reg, u32 val)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);

	if (!b)
		return;

	airoha_wed_write(b->bank, reg, val);
}

static u32 airoha_wed_irq_get(struct mtk_wed_device *dev, u32 mask)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);
	u32 val;

	if (!b)
		return 0;

	val = airoha_wed_read(b->bank, AIROHA_WED_EX_INT_STA);
	airoha_wed_write(b->bank, AIROHA_WED_EX_INT_STA, val);
	val &= WED_EXT_INT_ERROR_MASK | WED_EXT_INT_TX_DRV_R_RESP_ERR;
	if (val)
		pr_err_ratelimited("airoha-wed%d: error status=%08x\n",
				   b->index, val);

	val = airoha_wed_read(b->bank, AIROHA_WED_INT_STA);
	val &= mask;
	airoha_wed_write(b->bank, AIROHA_WED_INT_STA, val);

	return val;
}

static void airoha_wed_irq_set_mask(struct mtk_wed_device *dev, u32 mask)
{
	struct airoha_wed_bind *b = airoha_wed_find(dev);

	if (!b)
		return;

	airoha_wed_set_ext_int(b, !!mask);
	airoha_wed_write(b->bank, AIROHA_WED_INT_MSK, mask);
}

static int airoha_wed_msg_update(struct mtk_wed_device *dev, int cmd_id,
				 void *data, int len)
{
	/* WED v1 has no WO firmware to talk to; mt76 only needs this for the
	 * v2/v3 MCU channels. */
	return 0;
}

static void airoha_wed_ppe_check(struct mtk_wed_device *dev,
				 struct sk_buff *skb, u32 reason, u32 hash)
{
	/* On AN7581 the PPE is airoha_ppe's, not mtk_ppe's. Nothing to do. */
}

static int airoha_wed_setup_tc(struct mtk_wed_device *wed,
			       struct net_device *dev,
			       enum tc_setup_type type, void *type_data)
{
	return -EOPNOTSUPP;
}

static const struct mtk_wed_ops airoha_wed_ops = {
	.attach			= airoha_wed_attach,
	.tx_ring_setup		= airoha_wed_tx_ring_setup,
	.rx_ring_setup		= airoha_wed_rx_ring_setup,
	.txfree_ring_setup	= airoha_wed_txfree_ring_setup,
	.msg_update		= airoha_wed_msg_update,
	.detach			= airoha_wed_detach,
	.ppe_check		= airoha_wed_ppe_check,
	.stop			= airoha_wed_stop,
	.start			= airoha_wed_start,
	.reset_dma		= airoha_wed_reset_dma,
	.reg_read		= airoha_wed_reg_read,
	.reg_write		= airoha_wed_reg_write,
	.irq_get		= airoha_wed_irq_get,
	.irq_set_mask		= airoha_wed_irq_set_mask,
	.setup_tc		= airoha_wed_setup_tc,
};

int airoha_wed_ops_register(struct device_node *np)
{
	int i;

	airoha_wed_ops_read_dt(np);

	if (!wed_ops)
		return 0;

	if (rcu_access_pointer(mtk_soc_wed_ops)) {
		pr_warn("airoha-wed: mtk_soc_wed_ops already set, not taking over\n");
		return -EBUSY;
	}

	for (i = 0; i < ARRAY_SIZE(wed_bind); i++)
		wed_bind[i].index = i;

	rcu_assign_pointer(mtk_soc_wed_ops, &airoha_wed_ops);
	pr_info("airoha-wed: mtk_soc_wed_ops published (attach_enable=%d)\n",
		attach_enable);

	return 0;
}

void airoha_wed_ops_unregister(void)
{
	if (rcu_access_pointer(mtk_soc_wed_ops) != &airoha_wed_ops)
		return;

	rcu_assign_pointer(mtk_soc_wed_ops, NULL);
	synchronize_rcu();
	pr_info("airoha-wed: mtk_soc_wed_ops withdrawn\n");
}

#else /* !CONFIG_NET_MEDIATEK_SOC_WED */

int airoha_wed_ops_register(struct device_node *np)
{
	pr_info("airoha-wed: CONFIG_NET_MEDIATEK_SOC_WED is off, no ops table\n");
	return 0;
}

void airoha_wed_ops_unregister(void)
{
}

#endif /* CONFIG_NET_MEDIATEK_SOC_WED */
