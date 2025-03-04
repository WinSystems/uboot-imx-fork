// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2017-2019, 2021-2022 NXP
 *
 * Peng Fan <peng.fan@nxp.com>
 */

#include <common.h>
#include <cpu_func.h>
#include <init.h>
#include <log.h>
#include <asm/arch/imx-regs.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/arch/clock.h>
#include <asm/arch/sys_proto.h>
#include <asm/mach-imx/hab.h>
#include <asm/mach-imx/boot_mode.h>
#include <asm/mach-imx/optee.h>
#include <asm/mach-imx/syscounter.h>
#include <asm/ptrace.h>
#include <asm/armv8/mmu.h>
#include <dm/uclass.h>
#include <dm/device.h>
#include <efi_loader.h>
#include <env.h>
#include <env_internal.h>
#include <errno.h>
#include <fdt_support.h>
#include <fsl_wdog.h>
#include <imx_sip.h>
#include <linux/arm-smccc.h>
#include <linux/bitops.h>
#include <asm/setup.h>
#include <asm/bootm.h>
#include <kaslr.h>

DECLARE_GLOBAL_DATA_PTR;

#if defined(CONFIG_IMX_HAB) || defined(CONFIG_AVB_ATX) || defined(CONFIG_IMX_TRUSTY_OS)
struct imx_sec_config_fuse_t const imx_sec_config_fuse = {
	.bank = 1,
	.word = 3,
};
#endif

int timer_init(void)
{
#ifdef CONFIG_SPL_BUILD
	struct sctr_regs *sctr = (struct sctr_regs *)SYSCNT_CTRL_BASE_ADDR;
	unsigned long freq = readl(&sctr->cntfid0);

	/* Update with accurate clock frequency */
	asm volatile("msr cntfrq_el0, %0" : : "r" (freq) : "memory");

	clrsetbits_le32(&sctr->cntcr, SC_CNTCR_FREQ0 | SC_CNTCR_FREQ1,
			SC_CNTCR_FREQ0 | SC_CNTCR_ENABLE | SC_CNTCR_HDBG);
#endif

	gd->arch.tbl = 0;
	gd->arch.tbu = 0;

	return 0;
}

void enable_tzc380(void)
{
	struct iomuxc_gpr_base_regs *gpr =
		(struct iomuxc_gpr_base_regs *)IOMUXC_GPR_BASE_ADDR;

	/* Enable TZASC and lock setting */
	setbits_le32(&gpr->gpr[10], GPR_TZASC_EN);
	setbits_le32(&gpr->gpr[10], GPR_TZASC_EN_LOCK);

	/*
	 * According to TRM, TZASC_ID_SWAP_BYPASS should be set in
	 * order to avoid AXI Bus errors when GPU is in use
	 */
	setbits_le32(&gpr->gpr[10], GPR_TZASC_ID_SWAP_BYPASS);

	/*
	 * imx8m implements the lock bit for
	 * TZASC_ID_SWAP_BYPASS, enable it to lock settings
	 */
	setbits_le32(&gpr->gpr[10], GPR_TZASC_ID_SWAP_BYPASS_LOCK);

	/*
	 * set Region 0 attribute to allow secure and non-secure
	 * read/write permission. Found some masters like usb dwc3
	 * controllers can't work with secure memory.
	 */
	writel(0xf0000000, TZASC_BASE_ADDR + 0x108);
}

void set_wdog_reset(struct wdog_regs *wdog)
{
	/*
	 * Output WDOG_B signal to reset external pmic or POR_B decided by
	 * the board design. Without external reset, the peripherals/DDR/
	 * PMIC are not reset, that may cause system working abnormal.
	 * WDZST bit is write-once only bit. Align this bit in kernel,
	 * otherwise kernel code will have no chance to set this bit.
	 */
	setbits_le16(&wdog->wcr, WDOG_WDT_MASK | WDOG_WDZST_MASK);
}

static struct mm_region imx8m_mem_map[] = {
	{
		/* ROM */
		.virt = 0x0UL,
		.phys = 0x0UL,
		.size = 0x100000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_OUTER_SHARE
	}, {
		/* CAAM */
		.virt = 0x100000UL,
		.phys = 0x100000UL,
		.size = 0x8000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* OCRAM_S */
		.virt = 0x180000UL,
		.phys = 0x180000UL,
		.size = 0x8000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_OUTER_SHARE
	}, {
		/* TCM */
		.virt = 0x7C0000UL,
		.phys = 0x7C0000UL,
		.size = 0x80000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* OCRAM */
		.virt = 0x900000UL,
		.phys = 0x900000UL,
		.size = 0x200000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
			 PTE_BLOCK_OUTER_SHARE
	}, {
		/* AIPS */
		.virt = 0xB00000UL,
		.phys = 0xB00000UL,
		.size = 0x3f500000UL,
		.attrs = PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) |
			 PTE_BLOCK_NON_SHARE |
			 PTE_BLOCK_PXN | PTE_BLOCK_UXN
	}, {
		/* DRAM1 */
		.virt = 0x40000000UL,
		.phys = 0x40000000UL,
		.size = PHYS_SDRAM_SIZE,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
#ifdef CONFIG_IMX_TRUSTY_OS
			 PTE_BLOCK_INNER_SHARE
#else
			 PTE_BLOCK_OUTER_SHARE
#endif
#ifdef PHYS_SDRAM_2_SIZE
	}, {
		/* DRAM2 */
		.virt = 0x100000000UL,
		.phys = 0x100000000UL,
		.size = PHYS_SDRAM_2_SIZE,
		.attrs = PTE_BLOCK_MEMTYPE(MT_NORMAL) |
#ifdef CONFIG_IMX_TRUSTY_OS
			 PTE_BLOCK_INNER_SHARE
#else
			 PTE_BLOCK_OUTER_SHARE
#endif
#endif
	}, {
		/* empty entrie to split table entry 5 if needed when TEEs are used */
		0,
	}, {
		/* List terminator */
		0,
	}
};

struct mm_region *mem_map = imx8m_mem_map;

static unsigned int imx8m_find_dram_entry_in_mem_map(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(imx8m_mem_map); i++)
		if (imx8m_mem_map[i].phys == CONFIG_SYS_SDRAM_BASE)
			return i;

	hang();	/* Entry not found, this must never happen. */
}

void enable_caches(void)
{
	/* If OPTEE runs, remove OPTEE memory from MMU table to avoid speculative prefetch
	 * If OPTEE does not run, still update the MMU table according to dram banks structure
	 * to set correct dram size from board_phys_sdram_size
	 */
	int i = 0;
	/*
	 * please make sure that entry initial value matches
	 * imx8m_mem_map for DRAM1
	 */
	int entry = imx8m_find_dram_entry_in_mem_map();
	u64 attrs = imx8m_mem_map[entry].attrs;

	while (i < CONFIG_NR_DRAM_BANKS &&
	       entry < ARRAY_SIZE(imx8m_mem_map)) {
		if (gd->bd->bi_dram[i].start == 0)
			break;
		imx8m_mem_map[entry].phys = gd->bd->bi_dram[i].start;
		imx8m_mem_map[entry].virt = gd->bd->bi_dram[i].start;
		imx8m_mem_map[entry].size = gd->bd->bi_dram[i].size;
		imx8m_mem_map[entry].attrs = attrs;
		debug("Added memory mapping (%d): %llx %llx\n", entry,
		      imx8m_mem_map[entry].phys, imx8m_mem_map[entry].size);
		i++; entry++;
	}

	icache_enable();
	dcache_enable();
}

__weak int board_phys_sdram_size(phys_size_t *size)
{
	if (!size)
		return -EINVAL;

	*size = PHYS_SDRAM_SIZE;

#ifdef PHYS_SDRAM_2_SIZE
	*size += PHYS_SDRAM_2_SIZE;
#endif
	return 0;
}

int dram_init(void)
{
	phys_size_t sdram_size;
	int ret;

	ret = board_phys_sdram_size(&sdram_size);
	if (ret)
		return ret;

	/* rom_pointer[1] contains the size of TEE occupies */
	if (rom_pointer[1])
		gd->ram_size = sdram_size - rom_pointer[1];
	else
		gd->ram_size = sdram_size;

	return 0;
}

int dram_init_banksize(void)
{
	int bank = 0;
	int ret;
	phys_size_t sdram_size;
	phys_size_t sdram_b1_size, sdram_b2_size;

	ret = board_phys_sdram_size(&sdram_size);
	if (ret)
		return ret;

	/* Bank 1 can't cross over 4GB space */
	if (sdram_size > 0xc0000000) {
		sdram_b1_size = 0xc0000000;
		sdram_b2_size = sdram_size - 0xc0000000;
	} else {
		sdram_b1_size = sdram_size;
		sdram_b2_size = 0;
	}

	gd->bd->bi_dram[bank].start = PHYS_SDRAM;
	/* if optee enabled and optee start overlaps ddr bank address range */
	if (rom_pointer[1] && ((PHYS_SDRAM + sdram_b1_size) > rom_pointer[0])) {
		phys_addr_t optee_start = (phys_addr_t)rom_pointer[0];
		phys_size_t optee_size = (size_t)rom_pointer[1];

		gd->bd->bi_dram[bank].size = optee_start - gd->bd->bi_dram[bank].start;
		if ((optee_start + optee_size) < (PHYS_SDRAM + sdram_b1_size)) {
			if (++bank >= CONFIG_NR_DRAM_BANKS) {
				puts("CONFIG_NR_DRAM_BANKS is not enough\n");
				return -1;
			}

			gd->bd->bi_dram[bank].start = optee_start + optee_size;
			gd->bd->bi_dram[bank].size = PHYS_SDRAM +
				sdram_b1_size - gd->bd->bi_dram[bank].start;
		}
	} else {
		gd->bd->bi_dram[bank].size = sdram_b1_size;
	}

	if (sdram_b2_size) {
		if (++bank >= CONFIG_NR_DRAM_BANKS) {
			puts("CONFIG_NR_DRAM_BANKS is not enough for SDRAM_2\n");
			return -1;
		}
		gd->bd->bi_dram[bank].start = 0x100000000UL;
		gd->bd->bi_dram[bank].size = sdram_b2_size;
	}

	return 0;
}

phys_size_t get_effective_memsize(void)
{
	int ret;
	phys_size_t sdram_size;
	phys_size_t sdram_b1_size;
	ret = board_phys_sdram_size(&sdram_size);
	if (!ret) {
		/* Bank 1 can't cross over 4GB space */
		if (sdram_size > 0xc0000000) {
			sdram_b1_size = 0xc0000000;
		} else {
			sdram_b1_size = sdram_size;
		}

		if (rom_pointer[1]) {
			/* We will relocate u-boot to Top of dram1. Tee position has two cases:
			 * 1. At the top of dram1,  Then return the size removed optee size.
			 * 2. In the middle of dram1, return the size of dram1.
			 */
			if ((rom_pointer[0] + rom_pointer[1]) == (PHYS_SDRAM + sdram_b1_size))
				return ((phys_addr_t)rom_pointer[0] - PHYS_SDRAM);
		}

		return sdram_b1_size;
	} else {
		return PHYS_SDRAM_SIZE;
	}
}

static u32 get_cpu_variant_type(u32 type)
{
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[1];
	struct fuse_bank1_regs *fuse =
		(struct fuse_bank1_regs *)bank->fuse_regs;

	u32 value = readl(&fuse->tester4);

	if (type == MXC_CPU_IMX8MQ) {
		if ((value & 0x3) == 0x2)
			return MXC_CPU_IMX8MD;
		else if (value & 0x200000)
			return MXC_CPU_IMX8MQL;

	} else if (type == MXC_CPU_IMX8MM) {
		switch (value & 0x3) {
		case 2:
			if (value & 0x1c0000)
				return MXC_CPU_IMX8MMDL;
			else
				return MXC_CPU_IMX8MMD;
		case 3:
			if (value & 0x1c0000)
				return MXC_CPU_IMX8MMSL;
			else
				return MXC_CPU_IMX8MMS;
		default:
			if (value & 0x1c0000)
				return MXC_CPU_IMX8MML;
			break;
		}
	} else if (type == MXC_CPU_IMX8MN) {
		switch (value & 0x3) {
		case 2:
			if (value & 0x1000000) {
				if (value & 0x10000000)	 /* MIPI DSI */
					return MXC_CPU_IMX8MNUD;
				else
					return MXC_CPU_IMX8MNDL;
			} else {
				return MXC_CPU_IMX8MND;
			}
		case 3:
			if (value & 0x1000000) {
				if (value & 0x10000000)	 /* MIPI DSI */
					return MXC_CPU_IMX8MNUS;
				else
					return MXC_CPU_IMX8MNSL;
			} else {
				return MXC_CPU_IMX8MNS;
			}
		default:
			if (value & 0x1000000) {
				if (value & 0x10000000)	 /* MIPI DSI */
					return MXC_CPU_IMX8MNUQ;
				else
					return MXC_CPU_IMX8MNL;
			}
			break;
		}
	} else if (type == MXC_CPU_IMX8MP) {
		u32 value0 = readl(&fuse->tester3);
		u32 flag = 0;

		if ((value0 & 0xc0000) == 0x80000)
			return MXC_CPU_IMX8MPD;

			/* vpu disabled */
		if ((value0 & 0x43000000) == 0x43000000)
			flag = 1;

		/* npu disabled*/
		if ((value & 0x8) == 0x8)
			flag |= (1 << 1);

		/* isp disabled */
		if ((value & 0x3) == 0x3)
			flag |= (1 << 2);

		/* gpu disabled */
		if ((value & 0xc0) == 0xc0)
			flag |= (1 << 3);

		/* lvds disabled */
		if ((value & 0x180000) == 0x180000)
			flag |= (1 << 4);

		/* mipi dsi disabled */
		if ((value & 0x60000) == 0x60000)
			flag |= (1 << 5);

		switch (flag) {
		case 0x3f:
			return MXC_CPU_IMX8MPUL;
		case 7:
			return MXC_CPU_IMX8MPL;
		case 2:
			return MXC_CPU_IMX8MP6;
		default:
			break;
		}

	}

	return type;
}

u32 get_cpu_rev(void)
{
	struct anamix_pll *ana_pll = (struct anamix_pll *)ANATOP_BASE_ADDR;
	u32 reg = readl(&ana_pll->digprog);
	u32 type = (reg >> 16) & 0xff;
	u32 major_low = (reg >> 8) & 0xff;
	u32 rom_version;

	reg &= 0xff;

	/* iMX8MP */
	if (major_low == 0x43) {
		type = get_cpu_variant_type(MXC_CPU_IMX8MP);
	} else if (major_low == 0x42) {
		/* iMX8MN */
		type = get_cpu_variant_type(MXC_CPU_IMX8MN);
	} else if (major_low == 0x41) {
		type = get_cpu_variant_type(MXC_CPU_IMX8MM);
	} else {
		if (reg == CHIP_REV_1_0) {
			/*
			 * For B0 chip, the DIGPROG is not updated,
			 * it is still TO1.0. we have to check ROM
			 * version or OCOTP_READ_FUSE_DATA.
			 * 0xff0055aa is magic number for B1.
			 */
			if (readl((void __iomem *)(OCOTP_BASE_ADDR + 0x40)) == 0xff0055aa) {
				/*
				 * B2 uses same DIGPROG and OCOTP_READ_FUSE_DATA value with B1,
				 * so have to check ROM to distinguish them
				 */
				rom_version = readl((void __iomem *)ROM_VERSION_B0);
				rom_version &= 0xff;
				if (rom_version == CHIP_REV_2_2)
					reg = CHIP_REV_2_2;
				else
					reg = CHIP_REV_2_1;
			} else {
				rom_version =
					readl((void __iomem *)ROM_VERSION_A0);
				if (rom_version != CHIP_REV_1_0) {
					rom_version = readl((void __iomem *)ROM_VERSION_B0);
					rom_version &= 0xff;
					if (rom_version == CHIP_REV_2_0)
						reg = CHIP_REV_2_0;
				}
			}
		}

		type = get_cpu_variant_type(type);
	}

	return (type << 12) | reg;
}

static void imx_set_wdog_powerdown(bool enable)
{
	struct wdog_regs *wdog1 = (struct wdog_regs *)WDOG1_BASE_ADDR;
	struct wdog_regs *wdog2 = (struct wdog_regs *)WDOG2_BASE_ADDR;
	struct wdog_regs *wdog3 = (struct wdog_regs *)WDOG3_BASE_ADDR;

	/* Write to the PDE (Power Down Enable) bit */
	writew(enable, &wdog1->wmcr);
	writew(enable, &wdog2->wmcr);
	writew(enable, &wdog3->wmcr);
}

int arch_cpu_init_dm(void)
{
	struct udevice *dev;
	int ret;

	if (CONFIG_IS_ENABLED(CLK)) {
		ret = uclass_get_device_by_name(UCLASS_CLK,
						"clock-controller@30380000",
						&dev);
		if (ret < 0) {
			printf("Failed to find clock node. Check device tree\n");
			return ret;
		}
	}

	return 0;
}

#if defined(CONFIG_IMX_HAB) && defined(CONFIG_IMX8MQ)
static bool is_hdmi_fused(void) {
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[1];
	struct fuse_bank1_regs *fuse =
		(struct fuse_bank1_regs *)bank->fuse_regs;

	u32 value = readl(&fuse->tester4);

	if (is_imx8mq()) {
		if (value & 0x02000000)
			return true;
	}

	return false;
}

bool is_uid_matched(u64 uid) {
	struct tag_serialnr nr;
	get_board_serial(&nr);

	if (lower_32_bits(uid) == nr.low &&
		upper_32_bits(uid) == nr.high)
		return true;

	return false;
}

static void secure_lockup(void)
{
	if (is_imx8mq() && is_soc_rev(CHIP_REV_2_1) &&
		imx_hab_is_enabled() && !is_hdmi_fused()) {
#ifdef CONFIG_SECURE_STICKY_BITS_LOCKUP
		struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;

		clock_enable(CCGR_OCOTP, 1);
		setbits_le32(&ocotp->sw_sticky, 0x6); /* Lock up field return and SRK revoke */
		writel(0x80000000, &ocotp->scs_set); /* Lock up SCS */
#else
		/* Check the Unique ID, if it is matched with UID config, then allow to leave sticky bits unlocked */
		if (!is_uid_matched(CONFIG_IMX_UNIQUE_ID))
			hang();
#endif
	}
}
#endif

int arch_cpu_init(void)
{
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	/*
	 * ROM might disable clock for SCTR,
	 * enable the clock before timer_init.
	 */
	if (IS_ENABLED(CONFIG_SPL_BUILD))
		clock_enable(CCGR_SCTR, 1);
	/*
	 * Init timer at very early state, because sscg pll setting
	 * will use it
	 */
	timer_init();

	if (IS_ENABLED(CONFIG_SPL_BUILD)) {
		clock_init();
		imx_set_wdog_powerdown(false);

#if defined(CONFIG_IMX_HAB) && defined(CONFIG_IMX8MQ)
		secure_lockup();
#endif
		if (is_imx8md() || is_imx8mmd() || is_imx8mmdl() || is_imx8mms() ||
		    is_imx8mmsl() || is_imx8mnd() || is_imx8mndl() || is_imx8mns() ||
		    is_imx8mnsl() || is_imx8mpd() || is_imx8mnud() || is_imx8mnus()) {
			/* Power down cpu core 1, 2 and 3 for iMX8M Dual core or Single core */
			struct pgc_reg *pgc_core1 = (struct pgc_reg *)(GPC_BASE_ADDR + 0x840);
			struct pgc_reg *pgc_core2 = (struct pgc_reg *)(GPC_BASE_ADDR + 0x880);
			struct pgc_reg *pgc_core3 = (struct pgc_reg *)(GPC_BASE_ADDR + 0x8C0);
			struct gpc_reg *gpc = (struct gpc_reg *)GPC_BASE_ADDR;

			writel(0x1, &pgc_core2->pgcr);
			writel(0x1, &pgc_core3->pgcr);
			if (is_imx8mms() || is_imx8mmsl() || is_imx8mns() || is_imx8mnsl() || is_imx8mnus()) {
				writel(0x1, &pgc_core1->pgcr);
				writel(0xE, &gpc->cpu_pgc_dn_trg);
			} else {
				writel(0xC, &gpc->cpu_pgc_dn_trg);
			}
		}
	}

#if defined(CONFIG_ANDROID_SUPPORT)
	/* Enable RTC */
	writel(0x21, 0x30370038);
#endif

	if (is_imx8mq()) {
		clock_enable(CCGR_OCOTP, 1);
		if (readl(&ocotp->ctrl) & 0x200)
			writel(0x200, &ocotp->ctrl_clr);
	}

	return 0;
}

#if defined(CONFIG_IMX8MN) || defined(CONFIG_IMX8MP)
struct rom_api *g_rom_api = (struct rom_api *)0x980;

enum boot_device get_boot_device(void)
{
	volatile gd_t *pgd = gd;
	int ret;
	u32 boot;
	u16 boot_type;
	u8 boot_instance;
	enum boot_device boot_dev = SD1_BOOT;

	ret = g_rom_api->query_boot_infor(QUERY_BT_DEV, &boot,
					  ((uintptr_t)&boot) ^ QUERY_BT_DEV);
	set_gd(pgd);

	if (ret != ROM_API_OKAY) {
		puts("ROMAPI: failure at query_boot_info\n");
		return -1;
	}

	boot_type = boot >> 16;
	boot_instance = (boot >> 8) & 0xff;

	switch (boot_type) {
	case BT_DEV_TYPE_SD:
		boot_dev = boot_instance + SD1_BOOT;
		break;
	case BT_DEV_TYPE_MMC:
		boot_dev = boot_instance + MMC1_BOOT;
		break;
	case BT_DEV_TYPE_NAND:
		boot_dev = NAND_BOOT;
		break;
	case BT_DEV_TYPE_FLEXSPINOR:
		boot_dev = QSPI_BOOT;
		break;
	case BT_DEV_TYPE_USB:
		boot_dev = USB_BOOT;
		break;
	default:
		break;
	}

	return boot_dev;
}
#endif

#if defined(CONFIG_IMX8M)
#include <spl.h>
int spl_mmc_emmc_boot_partition(struct mmc *mmc)
{
	u32 *rom_log_addr = (u32 *)0x9e0;
	u32 *rom_log;
	u8 event_id;
	int i, part;

	part = default_spl_mmc_emmc_boot_partition(mmc);

	/* If the ROM event log pointer is not valid. */
	if (*rom_log_addr < 0x900000 || *rom_log_addr >= 0xb00000 ||
	    *rom_log_addr & 0x3)
		return part;

	/* Parse the ROM event ID version 2 log */
	rom_log = (u32 *)(uintptr_t)(*rom_log_addr);
	for (i = 0; i < 128; i++) {
		event_id = rom_log[i] >> 24;
		switch (event_id) {
		case 0x00: /* End of list */
			return part;
		/* Log entries with 1 parameter, skip 1 */
		case 0x80: /* Start to perform the device initialization */
		case 0x81: /* The boot device initialization completes */
		case 0x8f: /* The boot device initialization fails */
		case 0x90: /* Start to read data from boot device */
		case 0x91: /* Reading data from boot device completes */
		case 0x9f: /* Reading data from boot device fails */
			i += 1;
			continue;
		/* Log entries with 2 parameters, skip 2 */
		case 0xa0: /* Image authentication result */
		case 0xc0: /* Jump to the boot image soon */
			i += 2;
			continue;
		/* Boot from the secondary boot image */
		case 0x51:
			/*
			 * Swap the eMMC boot partitions in case there was a
			 * fallback event (i.e. primary image was corrupted
			 * and that corruption was recognized by the BootROM),
			 * so the SPL loads the rest of the U-Boot from the
			 * correct eMMC boot partition, since the BootROM
			 * leaves the boot partition set to the corrupted one.
			 */
			if (part == 1)
				part = 2;
			else if (part == 2)
				part = 1;
			continue;
		default:
			continue;
		}
	}

	return part;
}
#endif

bool is_usb_boot(void)
{
	return get_boot_device() == USB_BOOT;
}
#ifdef CONFIG_SERIAL_TAG
void get_board_serial(struct tag_serialnr *serialnr)
{
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[0];
	struct fuse_bank0_regs *fuse =
		(struct fuse_bank0_regs *)bank->fuse_regs;

	serialnr->low = fuse->uid_low;
	serialnr->high = fuse->uid_high;
}
#endif

#ifdef CONFIG_OF_SYSTEM_SETUP
bool check_fdt_new_path(void *blob)
{
	const char *soc_path = "/soc@0";
	int nodeoff;

	nodeoff = fdt_path_offset(blob, soc_path);
	if (nodeoff < 0)
		return false;

	return true;
}

static int disable_fdt_nodes(void *blob, const char *const nodes_path[], int size_array)
{
	int i = 0;
	int rc;
	int nodeoff;
	const char *status = "disabled";

	for (i = 0; i < size_array; i++) {
		nodeoff = fdt_path_offset(blob, nodes_path[i]);
		if (nodeoff < 0)
			continue; /* Not found, skip it */

		debug("Found %s node\n", nodes_path[i]);

add_status:
		rc = fdt_setprop(blob, nodeoff, "status", status, strlen(status) + 1);
		if (rc) {
			if (rc == -FDT_ERR_NOSPACE) {
				rc = fdt_increase_size(blob, 512);
				if (!rc)
					goto add_status;
			}
			printf("Unable to update property %s:%s, err=%s\n",
			       nodes_path[i], "status", fdt_strerror(rc));
		} else {
			printf("Modify %s:%s disabled\n",
			       nodes_path[i], "status");
		}
	}

	return 0;
}

#ifdef CONFIG_IMX8MQ
bool check_dcss_fused(void)
{
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[1];
	struct fuse_bank1_regs *fuse =
		(struct fuse_bank1_regs *)bank->fuse_regs;
	u32 value = readl(&fuse->tester4);

	if (value & 0x4000000)
		return true;

	return false;
}

static int disable_mipi_dsi_nodes(void *blob)
{
	static const char * const nodes_path[] = {
		"/mipi_dsi@30A00000",
		"/mipi_dsi_bridge@30A00000",
		"/dsi_phy@30A00300",
		"/soc@0/bus@30800000/mipi_dsi@30a00000",
		"/soc@0/bus@30800000/dphy@30a00300",
		"/soc@0/bus@30800000/mipi-dsi@30a00000",
	};

	return disable_fdt_nodes(blob, nodes_path, ARRAY_SIZE(nodes_path));
}

static int disable_dcss_nodes(void *blob)
{
	static const char * const nodes_path[] = {
		"/dcss@0x32e00000",
		"/dcss@32e00000",
		"/hdmi@32c00000",
		"/hdmi_cec@32c33800",
		"/hdmi_drm@32c00000",
		"/display-subsystem",
		"/sound-hdmi",
		"/sound-hdmi-arc",
		"/soc@0/bus@32c00000/display-controller@32e00000",
		"/soc@0/bus@32c00000/hdmi@32c00000",
	};

	return disable_fdt_nodes(blob, nodes_path, ARRAY_SIZE(nodes_path));
}

static int check_mipi_dsi_nodes(void *blob)
{
	static const char * const lcdif_path[] = {
		"/lcdif@30320000",
		"/soc@0/bus@30000000/lcdif@30320000",
		"/soc@0/bus@30000000/lcd-controller@30320000"
	};
	static const char * const mipi_dsi_path[] = {
		"/mipi_dsi@30A00000",
		"/soc@0/bus@30800000/mipi_dsi@30a00000"
	};
	static const char * const lcdif_ep_path[] = {
		"/lcdif@30320000/port@0/mipi-dsi-endpoint",
		"/soc@0/bus@30000000/lcdif@30320000/port@0/endpoint",
		"/soc@0/bus@30000000/lcd-controller@30320000/port@0/endpoint"
	};
	static const char * const mipi_dsi_ep_path[] = {
		"/mipi_dsi@30A00000/port@1/endpoint",
		"/soc@0/bus@30800000/mipi_dsi@30a00000/ports/port@0/endpoint",
		"/soc@0/bus@30800000/mipi-dsi@30a00000/ports/port@0/endpoint@0"
	};

	int lookup_node;
	int nodeoff;
	bool new_path = check_fdt_new_path(blob);
	int i = new_path ? 1 : 0;

	nodeoff = fdt_path_offset(blob, lcdif_path[i]);
	if (nodeoff < 0 || !fdtdec_get_is_enabled(blob, nodeoff)) {
		/*
		 * If can't find lcdif node or lcdif node is disabled,
		 * then disable all mipi dsi, since they only can input
		 * from DCSS
		 */
		return disable_mipi_dsi_nodes(blob);
	}

	nodeoff = fdt_path_offset(blob, mipi_dsi_path[i]);
	if (nodeoff < 0 || !fdtdec_get_is_enabled(blob, nodeoff))
		return 0;

	nodeoff = fdt_path_offset(blob, lcdif_ep_path[i]);
	if (nodeoff < 0) {
		/*
		 * If can't find lcdif endpoint, then disable all mipi dsi,
		 * since they only can input from DCSS
		 */
		return disable_mipi_dsi_nodes(blob);
	}

	lookup_node = fdtdec_lookup_phandle(blob, nodeoff, "remote-endpoint");
	nodeoff = fdt_path_offset(blob, mipi_dsi_ep_path[i]);

	if (nodeoff > 0 && nodeoff == lookup_node)
		return 0;

	return disable_mipi_dsi_nodes(blob);
}
#endif

void board_quiesce_devices(void)
{
#ifdef CONFIG_USB_DWC3
	if (is_usb_boot())
		disconnect_from_pc();
#endif
}

int disable_vpu_nodes(void *blob)
{
	static const char * const nodes_path_8mq[] = {
		"/vpu@38300000",
		"/soc@0/vpu@38300000"
	};

	static const char * const nodes_path_8mm[] = {
		"/vpu_g1@38300000",
		"/vpu_g2@38310000",
		"/vpu_h1@38320000"
	};

	static const char * const nodes_path_8mp[] = {
		"/vpu_g1@38300000",
		"/vpu_g2@38310000",
		"/vpu_vc8000e@38320000"
	};

	if (is_imx8mq())
		return disable_fdt_nodes(blob, nodes_path_8mq, ARRAY_SIZE(nodes_path_8mq));
	else if (is_imx8mm())
		return disable_fdt_nodes(blob, nodes_path_8mm, ARRAY_SIZE(nodes_path_8mm));
	else if (is_imx8mp())
		return disable_fdt_nodes(blob, nodes_path_8mp, ARRAY_SIZE(nodes_path_8mp));
	else
		return -EPERM;
}

#ifdef CONFIG_IMX8MN_LOW_DRIVE_MODE
static int low_drive_gpu_freq(void *blob)
{
	static const char *nodes_path_8mn[] = {
		"/gpu@38000000",
		"/soc@0/gpu@38000000"
	};

	int nodeoff, cnt, i, j;
	u32 assignedclks[7];

	for (i = 0; i < ARRAY_SIZE(nodes_path_8mn); i++) {
		nodeoff = fdt_path_offset(blob, nodes_path_8mn[i]);
		if (nodeoff < 0)
			continue;

		cnt = fdtdec_get_int_array_count(blob, nodeoff, "assigned-clock-rates", assignedclks, 7);
		if (cnt < 0)
			return cnt;

		if (cnt != 7)
			printf("Warning: %s, assigned-clock-rates count %d\n", nodes_path_8mn[i], cnt);

		assignedclks[cnt - 1] = 200000000;
		assignedclks[cnt - 2] = 200000000;

		for (j = 0; j < cnt; j++) {
			debug("<%u>, ", assignedclks[j]);
			assignedclks[j] = cpu_to_fdt32(assignedclks[j]);
		}
		debug("\n");

		return fdt_setprop(blob, nodeoff, "assigned-clock-rates", &assignedclks, sizeof(assignedclks));
	}

	return -ENOENT;
}
#endif

static bool check_remote_endpoint(void *blob, const char *ep1, const char *ep2)
{
	int lookup_node;
	int nodeoff;

	nodeoff = fdt_path_offset(blob, ep1);
	if (nodeoff) {
		lookup_node = fdtdec_lookup_phandle(blob, nodeoff, "remote-endpoint");
		nodeoff = fdt_path_offset(blob, ep2);

		if (nodeoff > 0 && nodeoff == lookup_node) {
			return true;
		}
	}

	return false;
}

int disable_dsi_lcdif_nodes(void *blob)
{
	int ret;

	static const char * const dsi_path_8mp[] = {
		"/soc@0/bus@32c00000/mipi_dsi@32e60000"
	};

	static const char * const lcdif_path_8mp[] = {
		"/soc@0/bus@32c00000/lcd-controller@32e80000"
	};

	static const char * const lcdif_ep_path_8mp[] = {
		"/soc@0/bus@32c00000/lcd-controller@32e80000/port@0/endpoint"
	};
	static const char * const dsi_ep_path_8mp[] = {
		"/soc@0/bus@32c00000/mipi_dsi@32e60000/port@0/endpoint"
	};

	ret = disable_fdt_nodes(blob, dsi_path_8mp, ARRAY_SIZE(dsi_path_8mp));
	if (ret)
		return ret;

	if (check_remote_endpoint(blob, dsi_ep_path_8mp[0], lcdif_ep_path_8mp[0])) {
		/* Disable lcdif node */
		return disable_fdt_nodes(blob, lcdif_path_8mp, ARRAY_SIZE(lcdif_path_8mp));
	}

	return 0;
}

int disable_lvds_lcdif_nodes(void *blob)
{
	int ret, i;

	static const char * const ldb_path_8mp[] = {
		"/soc@0/bus@32c00000/ldb@32ec005c",
		"/soc@0/bus@32c00000/phy@32ec0128"
	};

	static const char * const lcdif_path_8mp[] = {
		"/soc@0/bus@32c00000/lcd-controller@32e90000"
	};

	static const char * const lcdif_ep_path_8mp[] = {
		"/soc@0/bus@32c00000/lcd-controller@32e90000/port@0/endpoint@0",
		"/soc@0/bus@32c00000/lcd-controller@32e90000/port@0/endpoint@1"
	};
	static const char * const ldb_ep_path_8mp[] = {
		"/soc@0/bus@32c00000/ldb@32ec005c/lvds-channel@0/port@0/endpoint",
		"/soc@0/bus@32c00000/ldb@32ec005c/lvds-channel@1/port@0/endpoint"
	};

	ret = disable_fdt_nodes(blob, ldb_path_8mp, ARRAY_SIZE(ldb_path_8mp));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(ldb_ep_path_8mp); i++) {
		if (check_remote_endpoint(blob, ldb_ep_path_8mp[i], lcdif_ep_path_8mp[i])) {
			/* Disable lcdif node */
			return disable_fdt_nodes(blob, lcdif_path_8mp, ARRAY_SIZE(lcdif_path_8mp));
		}
	}

	return 0;
}

int disable_gpu_nodes(void *blob)
{
	static const char * const nodes_path_8mn[] = {
		"/gpu@38000000",
		"/soc@/gpu@38000000"
	};

	static const char * const nodes_path_8mp[] = {
		"/gpu3d@38000000",
		"/gpu2d@38008000"
	};

	if (is_imx8mp())
		return disable_fdt_nodes(blob, nodes_path_8mp, ARRAY_SIZE(nodes_path_8mp));
	else
		return disable_fdt_nodes(blob, nodes_path_8mn, ARRAY_SIZE(nodes_path_8mn));
}

int disable_npu_nodes(void *blob)
{
	static const char * const nodes_path_8mp[] = {
		"/vipsi@38500000"
	};

	return disable_fdt_nodes(blob, nodes_path_8mp, ARRAY_SIZE(nodes_path_8mp));
}

int disable_isp_nodes(void *blob)
{
	static const char * const nodes_path_8mp[] = {
		"/soc@0/bus@32c00000/camera/isp@32e10000",
		"/soc@0/bus@32c00000/camera/isp@32e20000"
	};

	return disable_fdt_nodes(blob, nodes_path_8mp, ARRAY_SIZE(nodes_path_8mp));
}

int disable_dsp_nodes(void *blob)
{
	static const char * const nodes_path_8mp[] = {
		"/dsp@3b6e8000"
	};

	return disable_fdt_nodes(blob, nodes_path_8mp, ARRAY_SIZE(nodes_path_8mp));
}

static void disable_thermal_cpu_nodes(void *blob, u32 disabled_cores)
{
	static const char * const thermal_path[] = {
		"/thermal-zones/cpu-thermal/cooling-maps/map0"
	};

	int nodeoff, cnt, i, ret, j;
	u32 cooling_dev[12];

	for (i = 0; i < ARRAY_SIZE(thermal_path); i++) {
		nodeoff = fdt_path_offset(blob, thermal_path[i]);
		if (nodeoff < 0)
			continue; /* Not found, skip it */

		cnt = fdtdec_get_int_array_count(blob, nodeoff, "cooling-device", cooling_dev, 12);
		if (cnt < 0)
			continue;

		if (cnt != 12)
			printf("Warning: %s, cooling-device count %d\n", thermal_path[i], cnt);

		for (j = 0; j < cnt; j++)
			cooling_dev[j] = cpu_to_fdt32(cooling_dev[j]);

		ret = fdt_setprop(blob, nodeoff, "cooling-device", &cooling_dev,
				  sizeof(u32) * (12 - disabled_cores * 3));
		if (ret < 0) {
			printf("Warning: %s, cooling-device setprop failed %d\n",
			       thermal_path[i], ret);
			continue;
		}

		printf("Update node %s, cooling-device prop\n", thermal_path[i]);
	}
}

static void disable_pmu_cpu_nodes(void *blob, u32 disabled_cores)
{
	static const char * const pmu_path[] = {
		"/pmu"
	};

	int nodeoff, cnt, i, ret, j;
	u32 irq_affinity[4];

	for (i = 0; i < ARRAY_SIZE(pmu_path); i++) {
		nodeoff = fdt_path_offset(blob, pmu_path[i]);
		if (nodeoff < 0)
			continue; /* Not found, skip it */

		cnt = fdtdec_get_int_array_count(blob, nodeoff, "interrupt-affinity",
						 irq_affinity, 4);
		if (cnt < 0)
			continue;

		if (cnt != 4)
			printf("Warning: %s, interrupt-affinity count %d\n", pmu_path[i], cnt);

		for (j = 0; j < cnt; j++)
			irq_affinity[j] = cpu_to_fdt32(irq_affinity[j]);

		ret = fdt_setprop(blob, nodeoff, "interrupt-affinity", &irq_affinity,
				 sizeof(u32) * (4 - disabled_cores));
		if (ret < 0) {
			printf("Warning: %s, interrupt-affinity setprop failed %d\n",
			       pmu_path[i], ret);
			continue;
		}

		printf("Update node %s, interrupt-affinity prop\n", pmu_path[i]);
	}
}

static int disable_cpu_nodes(void *blob, u32 disabled_cores)
{
	static const char * const nodes_path[] = {
		"/cpus/cpu@1",
		"/cpus/cpu@2",
		"/cpus/cpu@3",
	};
	u32 i = 0;
	int rc;
	int nodeoff;

	if (disabled_cores > 3)
		return -EINVAL;

	i = 3 - disabled_cores;

	for (; i < 3; i++) {
		nodeoff = fdt_path_offset(blob, nodes_path[i]);
		if (nodeoff < 0)
			continue; /* Not found, skip it */

		debug("Found %s node\n", nodes_path[i]);

		rc = fdt_del_node(blob, nodeoff);
		if (rc < 0) {
			printf("Unable to delete node %s, err=%s\n",
			       nodes_path[i], fdt_strerror(rc));
		} else {
			printf("Delete node %s\n", nodes_path[i]);
		}
	}

	disable_thermal_cpu_nodes(blob, disabled_cores);
	disable_pmu_cpu_nodes(blob, disabled_cores);

	return 0;
}

static int delete_u_boot_nodes(void *blob)
{
	static const char * const nodes_path = "/u-boot-node";
	int nodeoff;
	int rc;

	nodeoff = fdt_path_offset(blob, nodes_path);
	if (nodeoff < 0)
		return 0;

	debug("Found %s node\n", nodes_path);

	rc = fdt_del_node(blob, nodeoff);
	if (rc < 0) {
		printf("Unable to delete node %s, err=%s\n",
		       nodes_path, fdt_strerror(rc));
	} else {
		printf("Delete node %s\n", nodes_path);
	}

	return 0;
}

static int cleanup_nodes_for_efi(void *blob)
{
	static const char * const path[][2] = {
		{ "/soc@0/bus@32c00000/usb@32e40000", "extcon" },
		{ "/soc@0/bus@32c00000/usb@32e50000", "extcon" },
		{ "/soc@0/bus@30800000/ethernet@30be0000", "phy-reset-gpios" },
		{ "/soc@0/bus@30800000/ethernet@30bf0000", "phy-reset-gpios" }
	};
	int nodeoff, i, rc;

	for (i = 0; i < ARRAY_SIZE(path); i++) {
		nodeoff = fdt_path_offset(blob, path[i][0]);
		if (nodeoff < 0)
			continue; /* Not found, skip it */
		debug("Found %s node\n", path[i][0]);

		rc = fdt_delprop(blob, nodeoff, path[i][1]);
		if (rc == -FDT_ERR_NOTFOUND)
			continue;
		if (rc) {
			printf("Unable to update property %s:%s, err=%s\n",
			       path[i][0], path[i][1], fdt_strerror(rc));
			return rc;
		}

		printf("Remove %s:%s\n", path[i][0], path[i][1]);
	}

	return 0;
}

int ft_system_setup(void *blob, struct bd_info *bd)
{
#ifdef CONFIG_IMX8MQ
	int i = 0;
	int rc;
	int nodeoff;

	if (get_boot_device() == USB_BOOT) {
		disable_dcss_nodes(blob);

		bool new_path = check_fdt_new_path(blob);
		int v = new_path ? 1 : 0;
		static const char * const usb_dwc3_path[] = {
			"/usb@38100000/dwc3",
			"/soc@0/usb@38100000"
		};

		nodeoff = fdt_path_offset(blob, usb_dwc3_path[v]);
		if (nodeoff >= 0) {
			const char *speed = "high-speed";

			printf("Found %s node\n", usb_dwc3_path[v]);

usb_modify_speed:

			rc = fdt_setprop(blob, nodeoff, "maximum-speed", speed, strlen(speed) + 1);
			if (rc) {
				if (rc == -FDT_ERR_NOSPACE) {
					rc = fdt_increase_size(blob, 512);
					if (!rc)
						goto usb_modify_speed;
				}
				printf("Unable to set property %s:%s, err=%s\n",
				       usb_dwc3_path[v], "maximum-speed", fdt_strerror(rc));
			} else {
				printf("Modify %s:%s = %s\n",
				       usb_dwc3_path[v], "maximum-speed", speed);
			}
		} else {
			printf("Can't found %s node\n", usb_dwc3_path[v]);
		}
	}

	/* Disable the CPU idle for A0 chip since the HW does not support it */
	if (is_soc_rev(CHIP_REV_1_0)) {
		static const char * const nodes_path[] = {
			"/cpus/cpu@0",
			"/cpus/cpu@1",
			"/cpus/cpu@2",
			"/cpus/cpu@3",
		};

		for (i = 0; i < ARRAY_SIZE(nodes_path); i++) {
			nodeoff = fdt_path_offset(blob, nodes_path[i]);
			if (nodeoff < 0)
				continue; /* Not found, skip it */

			debug("Found %s node\n", nodes_path[i]);

			rc = fdt_delprop(blob, nodeoff, "cpu-idle-states");
			if (rc == -FDT_ERR_NOTFOUND)
				continue;
			if (rc) {
				printf("Unable to update property %s:%s, err=%s\n",
				       nodes_path[i], "status", fdt_strerror(rc));
				return rc;
			}

			debug("Remove %s:%s\n", nodes_path[i],
			       "cpu-idle-states");
		}
	}

	if (is_imx8mql()) {
		disable_vpu_nodes(blob);
		if (check_dcss_fused()) {
			printf("DCSS is fused\n");
			disable_dcss_nodes(blob);
			check_mipi_dsi_nodes(blob);
		}
	}

	if (is_imx8md())
		disable_cpu_nodes(blob, 2);

#elif defined(CONFIG_IMX8MM)
	if (is_imx8mml() || is_imx8mmdl() ||  is_imx8mmsl())
		disable_vpu_nodes(blob);

	if (is_imx8mmd() || is_imx8mmdl())
		disable_cpu_nodes(blob, 2);
	else if (is_imx8mms() || is_imx8mmsl())
		disable_cpu_nodes(blob, 3);

#elif defined(CONFIG_IMX8MN)
	if (is_imx8mnl() || is_imx8mndl() ||  is_imx8mnsl())
		disable_gpu_nodes(blob);
#ifdef CONFIG_IMX8MN_LOW_DRIVE_MODE
	else {
		int ldm_gpu = low_drive_gpu_freq(blob);

		if (ldm_gpu < 0)
			printf("Update GPU node assigned-clock-rates failed\n");
		else
			printf("Update GPU node assigned-clock-rates ok\n");
	}
#endif

	if (is_imx8mnd() || is_imx8mndl() || is_imx8mnud())
		disable_cpu_nodes(blob, 2);
	else if (is_imx8mns() || is_imx8mnsl() || is_imx8mnus())
		disable_cpu_nodes(blob, 3);

#elif defined(CONFIG_IMX8MP)
	if (is_imx8mpul()) {
		/* Disable GPU */
		disable_gpu_nodes(blob);

		/* Disable DSI */
		disable_dsi_lcdif_nodes(blob);

		/* Disable LVDS */
		disable_lvds_lcdif_nodes(blob);
	}

	if (is_imx8mpul() || is_imx8mpl())
		disable_vpu_nodes(blob);

	if (is_imx8mpul() || is_imx8mpl() || is_imx8mp6())
		disable_npu_nodes(blob);

	if (is_imx8mpul() || is_imx8mpl())
		disable_isp_nodes(blob);

	if (is_imx8mpul() || is_imx8mpl() || is_imx8mp6())
		disable_dsp_nodes(blob);

	if (is_imx8mpd())
		disable_cpu_nodes(blob, 2);
#endif

	cleanup_nodes_for_efi(blob);

	delete_u_boot_nodes(blob);

if (IS_ENABLED(CONFIG_KASLR)) {
       int ret = do_generate_kaslr(blob);
       if (ret)
           printf("Unable to set property %s, err=%s\n",
                       "kaslr-seed", fdt_strerror(ret));
       }

#if defined(CONFIG_ANDROID_SUPPORT) || defined(CONFIG_ANDROID_AUTO_SUPPORT)
	return 0;
#else
	return ft_add_optee_node(blob, bd);
#endif
}
#endif

#ifdef CONFIG_OF_BOARD_FIXUP
#ifndef CONFIG_SPL_BUILD
int board_fix_fdt(void *fdt)
{
	if (is_imx8mpul()) {
		int i = 0;
		int nodeoff, ret;
		const char *status = "disabled";
		static const char * dsi_nodes[] = {
			"/soc@0/bus@32c00000/mipi_dsi@32e60000",
			"/soc@0/bus@32c00000/lcd-controller@32e80000",
			"/dsi-host"
		};

		for (i = 0; i < ARRAY_SIZE(dsi_nodes); i++) {
			nodeoff = fdt_path_offset(fdt, dsi_nodes[i]);
			if (nodeoff > 0) {
set_status:
				ret = fdt_setprop(fdt, nodeoff, "status", status,
					strlen(status) + 1);
				if (ret == -FDT_ERR_NOSPACE) {
					ret = fdt_increase_size(fdt, 512);
					if (!ret)
						goto set_status;
				}
			}
		}
	}

	return 0;
}
#endif
#endif

#if !CONFIG_IS_ENABLED(SYSRESET)
void reset_cpu(void)
{
	struct watchdog_regs *wdog = (struct watchdog_regs *)WDOG1_BASE_ADDR;

	/* Clear WDA to trigger WDOG_B immediately */
	writew((SET_WCR_WT(1) | WCR_WDT | WCR_WDE | WCR_SRS), &wdog->wcr);

	while (1) {
		/*
		 * spin for .5 seconds before reset
		 */
	}
}
#endif

#if defined(CONFIG_ARCH_MISC_INIT)
static __maybe_unused void acquire_buildinfo(void)
{
	u64 atf_commit = 0;
	struct arm_smccc_res res;

	/* Get ARM Trusted Firmware commit id */
	arm_smccc_smc(IMX_SIP_BUILDINFO, IMX_SIP_BUILDINFO_GET_COMMITHASH,
		      0, 0, 0, 0, 0, 0, &res);
	atf_commit = res.a0;
	if (atf_commit == 0xffffffff) {
		debug("ATF does not support build info\n");
		atf_commit = 0x30; /* Display 0, 0 ascii is 0x30 */
	}

	printf("\n BuildInfo:\n  - ATF %s\n\n", (char *)&atf_commit);
}

int arch_misc_init(void)
{
#ifndef CONFIG_ANDROID_SUPPORT
	if (IS_ENABLED(CONFIG_FSL_CAAM)) {
		struct udevice *dev;
		int ret;

		ret = uclass_get_device_by_driver(UCLASS_MISC, DM_DRIVER_GET(caam_jr), &dev);
		if (ret)
			printf("Failed to initialize caam_jr: %d\n", ret);
	}
#endif
	acquire_buildinfo();

	return 0;
}
#endif

#ifdef CONFIG_SPL_BUILD
#ifdef CONFIG_IMX8MP
#define HSIO_GPR_BASE                               (0x32F10000U)
#define HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN_SHIFT    (1)
#define HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN          (0x1U << HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN_SHIFT)
#endif

static uint32_t gpc_pu_m_core_offset[11] = {
	0xc00, 0xc40, 0xc80, 0xcc0,
	0xdc0, 0xe00, 0xe40, 0xe80,
	0xec0, 0xf00, 0xf40,
};

#define PGC_PCR				0

void imx_gpc_set_m_core_pgc(unsigned int offset, bool pdn)
{
	uint32_t val;
	uintptr_t reg = GPC_BASE_ADDR + offset;

	val = readl(reg);
	val &= ~(0x1 << PGC_PCR);

	if(pdn)
		val |= 0x1 << PGC_PCR;
	writel(val, reg);
}

void imx8m_usb_power_domain(uint32_t domain_id, bool on)
{
	uint32_t val;
	uintptr_t reg;

#ifdef CONFIG_IMX8MP
	if (on) {
		/* enable usb clock via hsio gpr */
		reg = readl(HSIO_GPR_BASE);
		reg |= HSIO_GPR_REG_0_USB_CLOCK_MODULE_EN;
		writel(reg, HSIO_GPR_BASE);
	}
#endif

	imx_gpc_set_m_core_pgc(gpc_pu_m_core_offset[domain_id], true);

	reg = GPC_BASE_ADDR + (on ? 0xf8 : 0x104);
	val = 1 << (domain_id > 3 ? (domain_id + 3) : domain_id);
	writel(val, reg);
	while (readl(reg) & val)
		;
	imx_gpc_set_m_core_pgc(gpc_pu_m_core_offset[domain_id], false);
}
#endif

int imx8m_usb_power(int usb_id, bool on)
{
	if (usb_id > 1)
		return -EINVAL;

#ifdef CONFIG_SPL_BUILD
	imx8m_usb_power_domain(2 + usb_id, on);
#else
	struct arm_smccc_res res;
	arm_smccc_smc(IMX_SIP_GPC, IMX_SIP_GPC_PM_DOMAIN,
			2 + usb_id, on, 0, 0, 0, 0, &res);
	if (res.a0)
		return -EPERM;
#endif

	return 0;
}

void imx_tmu_arch_init(void *reg_base)
{
	if (is_imx8mm() || is_imx8mn()) {
		/* Load TCALIV and TASR from fuses */
		struct ocotp_regs *ocotp =
			(struct ocotp_regs *)OCOTP_BASE_ADDR;
		struct fuse_bank *bank = &ocotp->bank[3];
		struct fuse_bank3_regs *fuse =
			(struct fuse_bank3_regs *)bank->fuse_regs;

		u32 tca_rt, tca_hr, tca_en;
		u32 buf_vref, buf_slope;

		tca_rt = fuse->ana0 & 0xFF;
		tca_hr = (fuse->ana0 & 0xFF00) >> 8;
		tca_en = (fuse->ana0 & 0x2000000) >> 25;

		buf_vref = (fuse->ana0 & 0x1F00000) >> 20;
		buf_slope = (fuse->ana0 & 0xF0000) >> 16;

		writel(buf_vref | (buf_slope << 16), (ulong)reg_base + 0x28);
		writel((tca_en << 31) | (tca_hr << 16) | tca_rt,
		       (ulong)reg_base + 0x30);
	}
#ifdef CONFIG_IMX8MP
	/* Load TCALIV0/1/m40 and TRIM from fuses */
	struct ocotp_regs *ocotp = (struct ocotp_regs *)OCOTP_BASE_ADDR;
	struct fuse_bank *bank = &ocotp->bank[38];
	struct fuse_bank38_regs *fuse =
		(struct fuse_bank38_regs *)bank->fuse_regs;
	struct fuse_bank *bank2 = &ocotp->bank[39];
	struct fuse_bank39_regs *fuse2 =
		(struct fuse_bank39_regs *)bank2->fuse_regs;
	u32 buf_vref, buf_slope, bjt_cur, vlsb, bgr;
	u32 reg;
	u32 tca40[2], tca25[2], tca105[2];

	/* For blank sample */
	if (!fuse->ana_trim2 && !fuse->ana_trim3 &&
	    !fuse->ana_trim4 && !fuse2->ana_trim5) {
		/* Use a default 25C binary codes */
		tca25[0] = 1596;
		tca25[1] = 1596;
		writel(tca25[0], (ulong)reg_base + 0x30);
		writel(tca25[1], (ulong)reg_base + 0x34);
		return;
	}

	buf_vref = (fuse->ana_trim2 & 0xc0) >> 6;
	buf_slope = (fuse->ana_trim2 & 0xF00) >> 8;
	bjt_cur = (fuse->ana_trim2 & 0xF000) >> 12;
	bgr = (fuse->ana_trim2 & 0xF0000) >> 16;
	vlsb = (fuse->ana_trim2 & 0xF00000) >> 20;
	writel(buf_vref | (buf_slope << 16), (ulong)reg_base + 0x28);

	reg = (bgr << 28) | (bjt_cur << 20) | (vlsb << 12) | (1 << 7);
	writel(reg, (ulong)reg_base + 0x3c);

	tca40[0] = (fuse->ana_trim3 & 0xFFF0000) >> 16;
	tca25[0] = (fuse->ana_trim3 & 0xF0000000) >> 28;
	tca25[0] |= ((fuse->ana_trim4 & 0xFF) << 4);
	tca105[0] = (fuse->ana_trim4 & 0xFFF00) >> 8;
	tca40[1] = (fuse->ana_trim4 & 0xFFF00000) >> 20;
	tca25[1] = fuse2->ana_trim5 & 0xFFF;
	tca105[1] = (fuse2->ana_trim5 & 0xFFF000) >> 12;

	/* use 25c for 1p calibration */
	writel(tca25[0] | (tca105[0] << 16), (ulong)reg_base + 0x30);
	writel(tca25[1] | (tca105[1] << 16), (ulong)reg_base + 0x34);
	writel(tca40[0] | (tca40[1] << 16), (ulong)reg_base + 0x38);
#endif
}

#if defined(CONFIG_SPL_BUILD)
#if defined(CONFIG_IMX8MQ) || defined(CONFIG_IMX8MM) || defined(CONFIG_IMX8MN)
bool serror_need_skip = true;

void do_error(struct pt_regs *pt_regs, unsigned int esr)
{
	/*
	 * If stack is still in ROM reserved OCRAM not switch to SPL,
	 * it is the ROM SError
	 */
	ulong sp;

	asm volatile("mov %0, sp" : "=r"(sp) : );

	if (serror_need_skip && sp < 0x910000 && sp >= 0x900000) {
		/* Check for ERR050342, imx8mq HDCP enabled parts */
		if (is_imx8mq() && !(readl(OCOTP_BASE_ADDR + 0x450) & 0x08000000)) {
			serror_need_skip = false;
			return; /* Do nothing skip the SError in ROM */
		}

		/* Check for ERR050350, field return mode for imx8mq, mm and mn */
		if (readl(OCOTP_BASE_ADDR + 0x630) & 0x1) {
			serror_need_skip = false;
			return; /* Do nothing skip the SError in ROM */
		}
	}

	efi_restore_gd();
	printf("\"Error\" handler, esr 0x%08x\n", esr);
	show_regs(pt_regs);
	panic("Resetting CPU ...\n");
}
#endif
#endif

#if defined(CONFIG_IMX8MN) || defined(CONFIG_IMX8MP)
enum env_location env_get_location(enum env_operation op, int prio)
{
	enum boot_device dev = get_boot_device();

	if (prio)
		return ENVL_UNKNOWN;

	switch (dev) {
	case QSPI_BOOT:
		if (IS_ENABLED(CONFIG_ENV_IS_IN_SPI_FLASH))
			return ENVL_SPI_FLASH;
		return ENVL_NOWHERE;
	case NAND_BOOT:
		if (IS_ENABLED(CONFIG_ENV_IS_IN_NAND))
			return ENVL_NAND;
		return ENVL_NOWHERE;
	case SD1_BOOT:
	case SD2_BOOT:
	case SD3_BOOT:
	case MMC1_BOOT:
	case MMC2_BOOT:
	case MMC3_BOOT:
		if (IS_ENABLED(CONFIG_ENV_IS_IN_MMC))
			return ENVL_MMC;
		else if (IS_ENABLED(CONFIG_ENV_IS_IN_EXT4))
			return ENVL_EXT4;
		else if (IS_ENABLED(CONFIG_ENV_IS_IN_FAT))
			return ENVL_FAT;
		return ENVL_NOWHERE;
	default:
		return ENVL_NOWHERE;
	}
}

#endif

#ifdef CONFIG_IMX8MQ
int imx8m_dcss_power_init(void)
{
	/* Enable the display CCGR before power on */
	clock_enable(CCGR_DISPLAY, 1);

	writel(0x0000ffff, 0x303A00EC); /*PGC_CPU_MAPPING */
	setbits_le32(0x303A00F8, 0x1 << 10); /*PU_PGC_SW_PUP_REQ : disp was 10 */

	return 0;
}
#endif
