/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Register definitions for the MediaTek MT7622-derived WED
 * (Wi-Fi Offload Engine) instances embedded in the Airoha AN7581.
 *
 * Source of truth for these offsets: wed_def.h from the "mt_whnat"
 * WOE/WED driver tree (airoha_wed_wdma), whose banner comment records the
 * original SoC address 1020A000. The same IP was relocated to 0x1fa02000
 * on AN7581, hence WED_REG_BASE is 0: offsets are relative to the DT window.
 *
 * Every offset below was cross-checked against mainline
 * drivers/net/ethernet/mediatek/mtk_wed.c's WED-v1 (mt7622) code path, which
 * is the code that actually drives this IP revision. Two independent
 * confirmations that the two agree:
 *
 *   - WED_WDMA_OFST0/OFST1: wed_def.h gives 0x2a042a20 / 0x29002800 for
 *     instance 0 and 0x2e042e20 / 0x2d002c00 for instance 1. Mainline writes
 *     0x2a042a20 + (index ? 0x04000400 : 0) and 0x29002800 + the same
 *     offset -- identical numbers.
 *   - WED_TX_BM_TKID: wed_def.h 0x088 == mainline mt7622_data
 *     .regmap.tx_bm_tkid = 0x088.
 *
 * The only place the AN7581 genuinely differs from mt7622 is the PCIe
 * address translation, marked AIROHA-specific below.
 */
#ifndef _AIROHA_WED_REGS_H
#define _AIROHA_WED_REGS_H

/* GENMASK comes from here; the macros below are expanded at the call site,
 * but making the dependency explicit keeps the header self-contained. */
#include <linux/bits.h>

#define AIROHA_WED_REG_BASE		0

/* ---------------------------------------------------------------- control */
#define AIROHA_WED_REV			(AIROHA_WED_REG_BASE + 0x00000000)
#define AIROHA_WED_MOD_RST		(AIROHA_WED_REG_BASE + 0x00000008)
#define AIROHA_WED_CTRL			(AIROHA_WED_REG_BASE + 0x0000000c)
#define AIROHA_WED_AXI_CTRL		(AIROHA_WED_REG_BASE + 0x00000010)
#define AIROHA_WED_CTRL2		(AIROHA_WED_REG_BASE + 0x0000001c)
#define AIROHA_WED_EX_INT_STA		(AIROHA_WED_REG_BASE + 0x00000020)
#define AIROHA_WED_EX_INT_MSK		(AIROHA_WED_REG_BASE + 0x00000028)
#define AIROHA_WED_IRQ_MON		(AIROHA_WED_REG_BASE + 0x00000050)
#define AIROHA_WED_ST			(AIROHA_WED_REG_BASE + 0x00000060)
#define AIROHA_WED_WPDMA_ST		(AIROHA_WED_REG_BASE + 0x00000064)
#define AIROHA_WED_WDMA_ST		(AIROHA_WED_REG_BASE + 0x00000068)
#define AIROHA_WED_BM_ST		(AIROHA_WED_REG_BASE + 0x0000006c)

/* ------------------------------------------------------------ buffer mgmt */
#define AIROHA_WED_TX_BM_CTRL		(AIROHA_WED_REG_BASE + 0x00000080)
#define AIROHA_WED_TX_BM_BASE		(AIROHA_WED_REG_BASE + 0x00000084)
#define AIROHA_WED_TX_BM_TKID		(AIROHA_WED_REG_BASE + 0x00000088)
#define AIROHA_WED_TX_BM_BLEN		(AIROHA_WED_REG_BASE + 0x0000008c)
#define AIROHA_WED_TX_BM_STS		(AIROHA_WED_REG_BASE + 0x00000090)
#define AIROHA_WED_TX_BM_INTF		(AIROHA_WED_REG_BASE + 0x0000009c)
#define AIROHA_WED_TX_BM_DYN_TH		(AIROHA_WED_REG_BASE + 0x000000a0)

#define AIROHA_WED_TXDP_CTRL		(AIROHA_WED_REG_BASE + 0x00000130)

#define AIROHA_WED_INT_STA		(AIROHA_WED_REG_BASE + 0x00000200)
#define AIROHA_WED_INT_MSK		(AIROHA_WED_REG_BASE + 0x00000204)
#define AIROHA_WED_GLO_CFG		(AIROHA_WED_REG_BASE + 0x00000208)
#define AIROHA_WED_RST_IDX		(AIROHA_WED_REG_BASE + 0x0000020c)
#define AIROHA_WED_DLY_INT_CFG		(AIROHA_WED_REG_BASE + 0x00000210)
#define AIROHA_WED_SPR			(AIROHA_WED_REG_BASE + 0x0000021c)
#define AIROHA_WED_TX0_MIB		(AIROHA_WED_REG_BASE + 0x000002a0)
#define AIROHA_WED_TX1_MIB		(AIROHA_WED_REG_BASE + 0x000002a4)
#define AIROHA_WED_RX0_MIB		(AIROHA_WED_REG_BASE + 0x000002e0)
#define AIROHA_WED_RX1_MIB		(AIROHA_WED_REG_BASE + 0x000002e4)

/* ------------------------------------------------------- WED-facing rings */
/* The WLAN driver's own rings get re-pointed here: WED reads descriptor
 * rings from this window instead of letting the WPDMA fetch them. */
#define AIROHA_WED_RING_TX(_n)		(AIROHA_WED_REG_BASE + 0x300 + (_n) * 0x10)
#define AIROHA_WED_RING_RX(_n)		(AIROHA_WED_REG_BASE + 0x400 + (_n) * 0x10)

/* ---------------------------------------------------------- WPDMA mirrors */
#define AIROHA_WED_WPDMA_INT_STA_REC	(AIROHA_WED_REG_BASE + 0x00000500)
#define AIROHA_WED_WPDMA_INT_TRIG	(AIROHA_WED_REG_BASE + 0x00000504)
#define AIROHA_WED_WPDMA_GLO_CFG	(AIROHA_WED_REG_BASE + 0x00000508)
#define AIROHA_WED_WPDMA_RST_IDX	(AIROHA_WED_REG_BASE + 0x0000050c)
#define AIROHA_WED_WPDMA_INT_CTRL	(AIROHA_WED_REG_BASE + 0x00000520)
#define AIROHA_WED_WPDMA_INT_MSK	(AIROHA_WED_REG_BASE + 0x00000524)

#define AIROHA_WED_WPDMA_RING_TX(_n)	(AIROHA_WED_REG_BASE + 0x600 + (_n) * 0x10)
#define AIROHA_WED_WPDMA_RING_RX(_n)	(AIROHA_WED_REG_BASE + 0x700 + (_n) * 0x10)

/* ------------------------------------------------------------ PCIe window */
#define AIROHA_WED_PCIE_CFG_BASE	(AIROHA_WED_REG_BASE + 0x00000560)
#define AIROHA_WED_PCIE_OFST		(AIROHA_WED_REG_BASE + 0x00000564)
#define AIROHA_WED_PCIE_INTS_TRIG	(AIROHA_WED_REG_BASE + 0x00000570)
#define AIROHA_WED_PCIE_INTS_REC	(AIROHA_WED_REG_BASE + 0x00000574)
#define AIROHA_WED_PCIE_INTM_REC	(AIROHA_WED_REG_BASE + 0x00000578)
#define AIROHA_WED_PCIE_INT_CTRL	(AIROHA_WED_REG_BASE + 0x0000057c)

#define AIROHA_WED_WPDMA_CFG_BASE	(AIROHA_WED_REG_BASE + 0x00000580)
#define AIROHA_WED_WPDMA_OFST0		(AIROHA_WED_REG_BASE + 0x00000584)
#define AIROHA_WED_WPDMA_OFST1		(AIROHA_WED_REG_BASE + 0x00000588)

/* ------------------------------------------------------------ WDMA window */
#define AIROHA_WED_WDMA_RING_TX(_n)	(AIROHA_WED_REG_BASE + 0x800 + (_n) * 0x10)
#define AIROHA_WED_WDMA_RING_RX(_n)	(AIROHA_WED_REG_BASE + 0x900 + (_n) * 0x10)
#define AIROHA_WED_WDMA_RX_THRES_CFG(_n) (AIROHA_WED_REG_BASE + 0x940 + (_n) * 4)
#define AIROHA_WED_WDMA_INFO		(AIROHA_WED_REG_BASE + 0x00000a00)
#define AIROHA_WED_WDMA_GLO_CFG		(AIROHA_WED_REG_BASE + 0x00000a04)
#define AIROHA_WED_WDMA_RST_IDX		(AIROHA_WED_REG_BASE + 0x00000a08)
#define AIROHA_WED_WDMA_INT_STA_REC	(AIROHA_WED_REG_BASE + 0x00000a20)
#define AIROHA_WED_WDMA_INT_CLR		(AIROHA_WED_REG_BASE + 0x00000a24)
#define AIROHA_WED_WDMA_INT_TRIG	(AIROHA_WED_REG_BASE + 0x00000a28)
#define AIROHA_WED_WDMA_INT_CTRL	(AIROHA_WED_REG_BASE + 0x00000a2c)
#define AIROHA_WED_WDMA_CFG_BASE	(AIROHA_WED_REG_BASE + 0x00000aa0)
#define AIROHA_WED_WDMA_OFST0		(AIROHA_WED_REG_BASE + 0x00000aa4)
#define AIROHA_WED_WDMA_OFST1		(AIROHA_WED_REG_BASE + 0x00000aa8)
#define AIROHA_WED_WDMA_RX_MIB(_n)	(AIROHA_WED_REG_BASE + 0xae0 + (_n) * 4)

/* Ring register layout, shared by every ring block above. */
#define AIROHA_WED_RING_OFS_BASE	0x00
#define AIROHA_WED_RING_OFS_COUNT	0x04
#define AIROHA_WED_RING_OFS_CPU_IDX	0x08
#define AIROHA_WED_RING_OFS_DMA_IDX	0x0c

/*
 * WED_WDMA_RX{0,1}_THRES_CFG -- vendor woe_hw.c:1149-1150.
 *
 * These two registers are NOT written by mainline mtk_wed_mainline.c; MT7622
 * expects the bootloader/firmware to have pre-loaded them. The AN7581
 * bootloader does not, so they keep their power-on reset values, where
 * DRX_CRX_DISTANCE_THRES reads back as 0. RX_DRV then computes a descriptor
 * distance of zero, decides there is nothing to move, and never fetches a
 * single packet: downlink traffic piles up in PSE port 3 until the shared
 * buffer is drained and the link stalls.
 *
 * Measured on hardware: 0x00040020 (WAIT_BM_CNT_MAX = 0x020, DISTANCE = 0).
 * Correct value for a 1024-entry ring: 0x03fdffff.
 */
#define AIROHA_WED_RX_THRES_WAIT_BM_CNT_MAX	GENMASK(12, 0)
#define AIROHA_WED_RX_THRES_DRX_CRX_DISTANCE	GENMASK(28, 16)

/* ------------------------------------------------------------- field defs */
/* WED_REV */
#define AIROHA_WED_REV_ID		GENMASK(31, 16)
#define AIROHA_WED_REV_SUB_ID		GENMASK(15, 0)

/*
 * Must match WED_REV's 31:16 as read from real hardware so that a broken
 * DT window (wrong base address) is reported loudly instead of silently
 * looking like an uninitialised block.
 */
#define AIROHA_WED_EXPECTED_REV_ID	0x7622

/* WED_MOD_RST -- one bit per sub-module, write 1 to reset. */
#define AIROHA_WED_RST_WED		BIT(31)
#define AIROHA_WED_RST_TX_BM		BIT(0)
#define AIROHA_WED_RST_TX_FREE_AGT	BIT(4)
#define AIROHA_WED_RST_WPDMA_TX_DRV	BIT(8)
#define AIROHA_WED_RST_WPDMA_RX_DRV	BIT(9)
#define AIROHA_WED_RST_WPDMA_INT_AGT	BIT(11)
#define AIROHA_WED_RST_WED_TX_DMA	BIT(12)
#define AIROHA_WED_RST_WDMA_RX_DRV	BIT(17)
#define AIROHA_WED_RST_WDMA_INT_AGT	BIT(19)

/* WED_CTRL */
#define AIROHA_WED_CTRL_WPDMA_INT_AGT_EN	BIT(0)
#define AIROHA_WED_CTRL_WDMA_INT_AGT_EN		BIT(2)
#define AIROHA_WED_CTRL_WED_TX_BM_EN		BIT(8)
#define AIROHA_WED_CTRL_WED_TX_FREE_AGT_EN	BIT(10)

/* WED_GLO_CFG */
#define AIROHA_WED_GLO_TX_DMA_EN	BIT(0)
#define AIROHA_WED_GLO_TX_DMA_BUSY	BIT(1)
#define AIROHA_WED_GLO_RX_DMA_EN	BIT(2)
#define AIROHA_WED_GLO_RX_DMA_BUSY	BIT(3)
#define AIROHA_WED_GLO_RX_BT_SIZE	GENMASK(5, 4)
#define AIROHA_WED_GLO_TX_WB_DDONE	BIT(6)

/* WED_WPDMA_GLO_CFG */
#define AIROHA_WED_WPDMA_TX_DRV_EN	BIT(0)
#define AIROHA_WED_WPDMA_TX_DRV_BUSY	BIT(1)
#define AIROHA_WED_WPDMA_RX_DRV_EN	BIT(2)
#define AIROHA_WED_WPDMA_RX_DRV_BUSY	BIT(3)
#define AIROHA_WED_WPDMA_RX_BT_SIZE	GENMASK(5, 4)
#define AIROHA_WED_WPDMA_TX_WB_DDONE	BIT(6)

/* WED_WDMA_GLO_CFG (the WED's view of the WDMA block) */
#define AIROHA_WED_WDMA_TX_DRV_EN	BIT(0)
#define AIROHA_WED_WDMA_RX_DRV_EN	BIT(2)
#define AIROHA_WED_WDMA_RX_DRV_BUSY	BIT(3)
#define AIROHA_WED_WDMA_BT_SIZE		GENMASK(5, 4)
#define AIROHA_WED_WDMA_RST_INIT_COMPLETE BIT(26)
#define AIROHA_WED_WDMA_DYNAMIC_DMAD_RECYCLE BIT(25)
#define AIROHA_WED_WDMA_SKIP_DMAD_PREPARE BIT(24)
#define AIROHA_WED_WDMA_IDLE_DMAD_SUPPLY	BIT(23)
#define AIROHA_WED_WDMA_RX_DIS_FSM_AUTO_IDLE BIT(13)

/* reset index registers */
#define AIROHA_WED_RST_IDX_TX_MASK	GENMASK(3, 0)
#define AIROHA_WED_RST_IDX_RX_MASK	GENMASK(17, 16)
#define AIROHA_WED_WPDMA_RST_IDX_TX	(BIT(0) | BIT(1))
#define AIROHA_WED_WPDMA_RST_IDX_RX	BIT(17)
#define AIROHA_WED_WDMA_RST_IDX_RX	(BIT(16) | BIT(17))
#define AIROHA_WED_WDMA_RST_IDX_DRV	(BIT(24) | BIT(25))

/* TX buffer manager */
#define AIROHA_WED_TX_BM_CTRL_PAUSE	BIT(28)
#define AIROHA_WED_TX_BM_CTRL_RSV_GRP	GENMASK(22, 16)
#define AIROHA_WED_TX_BM_CTRL_VLD_GRP	GENMASK(6, 0)
#define AIROHA_WED_TX_BM_TKID_END	GENMASK(31, 16)
#define AIROHA_WED_TX_BM_TKID_START	GENMASK(15, 0)
/* NOTE: AIROHA_WED_TX_BM_BLEN above is the register offset; this is the
 * buffer-length field inside it. Distinct name on purpose -- a second
 * #define of the same identifier is a -Werror=macro-redefined trap. */
#define AIROHA_WED_TX_BM_BLEN_MASK	GENMASK(13, 0)
#define AIROHA_WED_TX_BM_INTF_TKFIFO_FDEP GENMASK(22, 16)
#define AIROHA_WED_TX_BM_DYN_TH_HI	GENMASK(22, 16)
#define AIROHA_WED_TX_BM_DYN_TH_LO	GENMASK(6, 0)

/* WED_PCIE_INT_CTRL */
#define AIROHA_WED_PCIE_INT_CTRL_MSK_EN_POLA BIT(20)

/*
 * AIROHA-SPECIFIC: mt7622 reaches the WLAN WPDMA through a syscon
 * "mediatek,pcie-mirror" window (mainline mtk_wed_start() writes
 * wpdma_phys | MTK_PCIE_MIRROR_MAP_EN there). AN7581 has no such syscon --
 * mt_whnat's woe_hif.h sets CFG_CR_MIRROR_SUPPORT=0 for ECNT -- and instead
 * programs the PCIe controller base straight into WED_PCIE_CFG_BASE, then
 * sets bit 20 of WED_PCIE_INT_CTRL. These are the EN7581 branches of
 * whnat_hal_pcie_map() / woe_hif.h.
 *
 * The base must match the PCIe controller the MT7916D actually sits on, not
 * the WED index. On this board the radio enumerates as 0001:01:00.0, i.e.
 * behind pcie@1fc20000 (bus 0001), while 0000:01:00.0 (the HIF) is on
 * pcie@1fc00000. The two WED banks are wired 1:1 to those two controllers,
 * so the mapping is taken from the PCI bus number rather than assumed.
 */
#define AIROHA_WED_PCIE_BASE_FOR_BUS(_bus)	(0x1fc00000 + (_bus) * 0x20000)

/* WED_PCIE_INTS_TRIG: EN7581 uses bit 24, not the mt7622 bit 16
 * (woe_hw.h: PCIE_INT_STA_OFFSET_EN7581 = 1 << 24). */
#define AIROHA_WED_PCIE_INTS_TRIG_EN7581	BIT(24)

/* WED_PCIE_OFST: EN7581 fixed value, woe_hw.c whnat_hal_int_ctrl(). */
#define AIROHA_WED_PCIE_OFST_EN7581		0x01800184

/* WED_PCIE_INT_CTRL polling mode, needed because there is no CR mirror to
 * deliver the WED <-> PCIe interrupt. woe_hw.h: PCIE_POLL_MODE_ALWAYS. */
#define AIROHA_WED_PCIE_INT_CTRL_POLL_EN	BIT(14)	/* REG_FLD(2, 12) */
#define AIROHA_WED_PCIE_INT_CTRL_POLL_ALWAYS	(2 << 12)

#endif /* _AIROHA_WED_REGS_H */
