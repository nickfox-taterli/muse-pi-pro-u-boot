// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2023 Spacemit, Inc
 */

#include <common.h>
#include <dm.h>
#include <dm/ofnode.h>
#include <dm/lists.h>
#include <env.h>
#include <fdtdec.h>
#include <image.h>
#include <log.h>
#include <mapmem.h>
#include <spl.h>
#include <init.h>
#include <virtio_types.h>
#include <virtio.h>
#include <asm/io.h>
#include <asm/sections.h>
#include <stdlib.h>
#include <linux/io.h>
#include <asm/global_data.h>
#include <part.h>
#include <env_internal.h>
#include <asm/arch/ddr.h>
#include <power/regulator.h>
#include <net.h>
#include <i2c.h>
#include <linux/delay.h>
#include <tlv_eeprom.h>
#include <u-boot/crc.h>
#include <fb_mtd.h>
#include <power/pmic.h>
#include <dm/device.h>
#include <dm/device-internal.h>
#include <g_dnl.h>
#include <fdt_simplefb.h>
#include <mtd_node.h>
#include <misc.h>
#ifdef CONFIG_ENV_IS_IN_NFS
#include "nfs_env.h"
#endif
#ifdef CONFIG_BUTTON
#include <button.h>
struct fastboot_key_config {
	const char **key_names;
	int key_count;
	u32 press_time;
};
#endif

DECLARE_GLOBAL_DATA_PTR;
static char found_partition[64] = {0};
extern u32 ddr_cs_num;
bool is_video_connected = false;
uint32_t reboot_config;

void set_boot_mode(enum board_boot_mode boot_mode)
{
	writel(boot_mode, (void *)BOOT_DEV_FLAG_REG);
}

enum board_boot_mode get_boot_pin_select(void)
{
	/*if not set boot mode, try to return boot pin select*/
	u32 boot_select = readl((void *)BOOT_PIN_SELECT) & BOOT_STRAP_BIT_STORAGE_MASK;
	boot_select = boot_select >> BOOT_STRAP_BIT_OFFSET;
	pr_debug("boot_select:%x\n", boot_select);

	/*select spl boot device:
		 b'(bit1)(bit0)
	emmc:b'00, //BOOT_STRAP_BIT_EMMC
	nor :b'10, //BOOT_STRAP_BIT_NOR
	nand:b'01, //BOOT_STRAP_BIT_NAND
	sd  :b'11, //BOOT_STRAP_BIT_SD
*/
	switch (boot_select) {
	case BOOT_STRAP_BIT_EMMC:
		return BOOT_MODE_EMMC;
	case BOOT_STRAP_BIT_NAND:
		return BOOT_MODE_NAND;
	case BOOT_STRAP_BIT_NOR:
		return BOOT_MODE_NOR;
	case BOOT_STRAP_BIT_SD:
	default:
		return BOOT_MODE_SD;
	}
}

enum board_boot_mode get_boot_mode(void)
{
	/*if usb boot or has set boot mode, return boot mode*/
	u32 boot_mode = readl((void *)BOOT_DEV_FLAG_REG);
	pr_debug("%s, boot_mode:%x\n", __func__, boot_mode);

	switch (boot_mode) {
	case BOOT_MODE_USB:
		return BOOT_MODE_USB;
	case BOOT_MODE_EMMC:
		return BOOT_MODE_EMMC;
	case BOOT_MODE_NAND:
		return BOOT_MODE_NAND;
	case BOOT_MODE_NOR:
		return BOOT_MODE_NOR;
	case BOOT_MODE_SD:
		return BOOT_MODE_SD;
	case BOOT_MODE_SHELL:
		return BOOT_MODE_SHELL;
	}

	/*else return boot pin select*/
	return get_boot_pin_select();
}

void set_serialnumber_based_on_boot_mode(void)
{
	const char *s = env_get("serial#");
	enum board_boot_mode boot_mode = get_boot_mode();

	if (boot_mode != BOOT_MODE_USB && s) {
		g_dnl_set_serialnumber((char *)s);
	}
}

enum board_boot_mode get_boot_storage(void)
{
	enum board_boot_mode boot_storage = get_boot_mode();

	// save to card only when boot from card
	if (BOOT_MODE_SD != boot_storage)
		boot_storage =  get_boot_pin_select();

	return boot_storage;
}

int mmc_get_env_dev(void)
{
	u32 boot_mode = 0;
	boot_mode = get_boot_mode();
	pr_debug("%s, uboot boot_mode:%x\n", __func__, boot_mode);

	if (boot_mode == BOOT_MODE_EMMC)
		return MMC_DEV_EMMC;
	else
		return MMC_DEV_SD;
}

static ulong read_boot_storage_emmc(ulong byte_addr, ulong byte_size, void *buff)
{
	ulong ret;
	struct blk_desc *dev_desc = blk_get_dev("mmc", MMC_DEV_EMMC);
	if (!dev_desc || dev_desc->type == DEV_TYPE_UNKNOWN) {
		pr_err("invalid mmc device\n");
		return 0;
	}

	blk_dselect_hwpart(dev_desc, 0);
	ret = blk_dread(dev_desc,
		byte_addr / dev_desc->blksz,
		byte_size / dev_desc->blksz, buff);
	return dev_desc->blksz * ret;
}

#if !defined(CONFIG_SPL_BUILD)
static ulong read_boot_storage_sdcard(ulong byte_addr, ulong byte_size, void *buff)
{
	ulong ret;
	struct blk_desc *dev_desc = blk_get_dev("mmc", MMC_DEV_SD);
	if (!dev_desc || dev_desc->type == DEV_TYPE_UNKNOWN) {
		pr_err("invalid sdcard device\n");
		return 0;
	}

	ret = blk_dread(dev_desc,
		byte_addr / dev_desc->blksz,
		byte_size / dev_desc->blksz, buff);
	return dev_desc->blksz * ret;
}

static ulong read_boot_storage_spinor(ulong byte_addr, ulong byte_size, void *buff)
{
	struct mtd_info *mtd;
	const char* part = "private";

	mtd_probe_devices();
	mtd = get_mtd_device_nm(part);
	if ((NULL != mtd) && (0 == _fb_mtd_read(mtd, buff, byte_addr, byte_size, NULL))) {
		// print_buffer(0, buff, 1, byte_size, 16);
		return byte_size;
	}
	else
		return 0;
}

static bool write_boot_storage_emmc(ulong byte_addr, ulong byte_size, void *buff)
{
	struct blk_desc *dev_desc = blk_get_dev("mmc", MMC_DEV_EMMC);

	if (!dev_desc || dev_desc->type == DEV_TYPE_UNKNOWN) {
		pr_err("invalid mmc device\n");
		return false;
	}

	blk_dselect_hwpart(dev_desc, 0);
	pr_info("write %ldbyte to emmc address %ld\n", byte_size, byte_addr);
	blk_dwrite(dev_desc,
		byte_addr / dev_desc->blksz,
		byte_size / dev_desc->blksz, buff);
	return true;
}

static bool write_boot_storage_sdcard(ulong byte_addr, ulong byte_size, void *buff)
{
	struct blk_desc *dev_desc = blk_get_dev("mmc", MMC_DEV_SD);

	if (!dev_desc || dev_desc->type == DEV_TYPE_UNKNOWN) {
		pr_err("invalid sd device\n");
		return false;
	}

	pr_info("write %ldbyte to sdcard address %ld\n", byte_size, byte_addr);
	blk_dwrite(dev_desc,
		byte_addr / dev_desc->blksz,
		byte_size / dev_desc->blksz, buff);
	return true;
}

static bool write_boot_storage_spinor(ulong byte_addr, ulong byte_size, void *buff)
{
	struct mtd_info *mtd;
	const char* part = "private";

	mtd_probe_devices();
	mtd = get_mtd_device_nm(part);
	if ((NULL != mtd) && (0 == _fb_mtd_erase(mtd, byte_size))
		&& (0 == _fb_mtd_write(mtd, buff, byte_addr, byte_size, NULL))) {
		pr_info("write %ldbyte to spinor partition %s @offset %ld\n", byte_size, part, byte_addr);
		return true;
	}
	else
		return false;
}

static const struct boot_storage_op storage_op[] = {
	{BOOT_MODE_EMMC, read_boot_storage_emmc, write_boot_storage_emmc},
	{BOOT_MODE_SD, read_boot_storage_sdcard, write_boot_storage_sdcard},
	{BOOT_MODE_NOR, read_boot_storage_spinor, write_boot_storage_spinor},
};
#else
// NOT support write operation in SPL stage
static const struct boot_storage_op storage_op[] = {
	{BOOT_MODE_EMMC, read_boot_storage_emmc, NULL},
};
#endif

ulong read_boot_storage(void *buff, ulong offset, ulong byte_size)
{
	int i;
	// read data from boot storage
	enum board_boot_mode boot_storage = get_boot_storage();

	for (i = 0; i < ARRAY_SIZE(storage_op); i++) {
		if (boot_storage == storage_op[i].boot_storage)
			return storage_op[i].read(offset, byte_size, buff);
	}

	return 0;
}

bool write_boot_storage(void *buff, ulong offset, ulong byte_size)
{
	int i;
	// save data to boot storage
	enum board_boot_mode boot_storage = get_boot_storage();

	for (i = 0; i < ARRAY_SIZE(storage_op); i++) {
		if (boot_storage == storage_op[i].boot_storage)
			return storage_op[i].write(offset, byte_size, buff);
	}

	return false;
}

void save_ddr_training_info(void)
{
	struct ddr_training_info_t *info;
	info = (struct ddr_training_info_t*)map_sysmem(DDR_TRAINING_INFO_BUFF, 0);

	if ((DDR_TRAINING_INFO_MAGIC == info->magic) &&
		(info->crc32 == crc32(0, (const uchar *)&info->chipid, sizeof(*info) - 8))) {
		// save DDR training info to boot storage
		write_boot_storage(info, DDR_TRAINING_INFO_OFFSET, sizeof(*info));
	}
}


u32 get_reboot_config(void)
{
	int ret;
	struct udevice *dev;
	u32 flag = 0;
	uint8_t value;

	if (reboot_config)
		return reboot_config;

	/* K1 has non-reset register(BOOT_CIU_DEBUG_REG0) to save boot config
	   before system reboot, but it will be clear when K1 power is down,
	   then boot config will be save in P1.
	*/
	flag = readl((void *)BOOT_CIU_DEBUG_REG0);
	if ((BOOT_MODE_SHELL == flag) || (BOOT_MODE_USB == flag)) {
		/* reset  */
		writel(0, (void *)BOOT_CIU_DEBUG_REG0);
		reboot_config = flag;
	}
	else {
		// select boot mode from boot strap pin
		reboot_config = BOOT_MODE_BOOTSTRAP;
		ret = uclass_get_device_by_driver(UCLASS_PMIC,
				DM_DRIVER_GET(spacemit_pm8xx), &dev);
		if (ret) {
			pr_err("PMIC init failed: %d\n", ret);
			return 0;
		}
		pmic_read(dev, P1_NON_RESET_REG, &value, 1);
		pr_info("Read PMIC reg %x value %x\n", P1_NON_RESET_REG, value);
		if (1 == (value & 7)) {
			reboot_config = BOOT_MODE_USB;
			value &= ~7;
			pmic_write(dev, P1_NON_RESET_REG, &value, 1);
		}
		else if (2 == (value & 7)) {
			reboot_config = BOOT_MODE_SHELL;
			value &= ~7;
			pmic_write(dev, P1_NON_RESET_REG, &value, 1);
		}
	}

	return reboot_config;
}


#ifdef CONFIG_BUTTON
static int button_get_state_by_label(struct udevice *dev, const char *label)
{
	struct udevice *child;
	struct button_uc_plat *plat;
	int state;
	bool invert_state = false;

	pr_debug("Searching for button with label '%s'\n", label);
	for (device_find_first_child(dev, &child);
			child;
			device_find_next_child(&child)) {
		plat = dev_get_uclass_plat(child);
		if (plat->label && !strcmp(plat->label, label)) {
			invert_state = ofnode_read_bool(dev_ofnode(child), "invert-state");

			state = button_get_state(child);
			pr_debug("Button '%s' found, raw state: %d, invert: %d\n", label, state, invert_state);

			if (invert_state) {
				state = (state == BUTTON_ON) ? BUTTON_OFF : BUTTON_ON;
			}
			pr_debug("Button '%s' final state: %d\n", label, state);
			return state;
		}
	}

	pr_err("Button '%s' not found\n", label);
	return -ENOENT;
}

static int get_fastboot_key_config(struct fastboot_key_config *config)
{
	ofnode node;
	int ret;

	node = ofnode_path("/gpio_keys");
	if (!ofnode_valid(node))
		return -ENODEV;

	ret = ofnode_read_string_list(node, "fastboot-key-combo", &config->key_names);
	if (ret < 0)
		return ret;
	config->key_count = ret;

	ret = ofnode_read_u32(node, "fastboot-key-press-time", &config->press_time);
	if (ret < 0)
		return ret;

	return 0;
}

static bool check_fastboot_keys(void)
{
	struct udevice *dev;
	struct fastboot_key_config config;
	int *key_states;
	ulong press_start = 0;
	int i, ret;
	bool all_pressed = true;

	ret = get_fastboot_key_config(&config);
	if (ret < 0) {
		pr_err("Failed to get fastboot key config: %d\n", ret);
		return false;
	}

	pr_debug("Fastboot key config: count=%d, press_time=%u\n", config.key_count, config.press_time);

	ret = uclass_get_device_by_name(UCLASS_BUTTON, "gpio_keys", &dev);
	if (ret) {
		pr_err("Failed to get device for gpio_keys\n");
		return false;
	}

	key_states = calloc(config.key_count, sizeof(int));
	if (!key_states) {
		pr_err("Failed to allocate memory for key_states\n");
		return false;
	}

	press_start = get_timer(0);

	while (get_timer(press_start) < config.press_time) {
		all_pressed = true;
		for (i = 0; i < config.key_count; i++) {
			key_states[i] = button_get_state_by_label(dev, config.key_names[i]);
			if (key_states[i] < 0 || key_states[i] != BUTTON_ON) {
				all_pressed = false;
				break;
			}
		}

		if (!all_pressed) {
			/* Key released within the specified time, normal boot */
			free(key_states);
			return false;
		}

		mdelay(10);
	}

	/* Keys held down longer than specified time, enter Fastboot mode */
	free(key_states);
	pr_info("Fastboot key combination detected! Duration: %u ms\n", config.press_time);
	return true;
}
#endif


void run_fastboot_command(void)
{
	u32 boot_mode = get_boot_mode();

	if (boot_mode == BOOT_MODE_USB || BOOT_MODE_USB == get_reboot_config()
#ifdef CONFIG_BUTTON
		|| check_fastboot_keys()
#endif
	) {

		char *cmd_para = "fastboot 0";
		run_command(cmd_para, 0);
	}
}


int run_uboot_shell(void)
{
	u32 boot_mode = get_boot_mode();

	if (boot_mode == BOOT_MODE_SHELL || BOOT_MODE_SHELL == get_reboot_config()) {
		return 0;
	}
	return 1;
}

char* parse_mtdparts_and_find_bootfs(void) {
	const char *mtdparts = env_get("mtdparts");
	char cmd_buf[256];

	if (!mtdparts) {
		pr_debug("mtdparts not set\n");
		return NULL;
	}

	/* First try to find bootfs as an independent partition */
	if (strstr(mtdparts, "(bootfs)")) {
		strcpy(found_partition, "bootfs");
		snprintf(cmd_buf, sizeof(cmd_buf), "ubi part %s", found_partition);
		if (run_command(cmd_buf, 0) == 0) {
			/* Check if the bootfs volume exists in bootfs partition */
			snprintf(cmd_buf, sizeof(cmd_buf), "ubi check %s", BOOTFS_NAME);
			if (run_command(cmd_buf, 0) == 0) {
				pr_info("Found bootfs volume in independent bootfs partition\n");
				return found_partition;
			}
		}
	}

	/* Fallback: Find the last partition (for backward compatibility) */
	const char *last_part_start = strrchr(mtdparts, '(');
	if (last_part_start) {
		last_part_start++; /* Skip the left parenthesis */
		const char *end = strchr(last_part_start, ')');
		if (end && (end - last_part_start < sizeof(found_partition))) {
			int len = end - last_part_start;
			strncpy(found_partition, last_part_start, len);
			found_partition[len] = '\0';

			snprintf(cmd_buf, sizeof(cmd_buf), "ubi part %s", found_partition);
			if (run_command(cmd_buf, 0) == 0) {
				/* Check if the bootfs volume exists */
				snprintf(cmd_buf, sizeof(cmd_buf), "ubi check %s", BOOTFS_NAME);
				if (run_command(cmd_buf, 0) == 0) {
					pr_info("Found bootfs in partition: %s\n", found_partition);
					return found_partition;
				}
			}
		}
	}

	pr_debug("bootfs not found in any partition\n");
	return NULL;
}

struct code_desc_info {
	u8	m_code;
	char	*m_name;
};

static int probe_shutdown_charge(void)
{
#ifdef CONFIG_SPACEMIT_SHUTDOWN_CHARGE
	struct udevice *udev;
	int ret;

#ifdef CONFIG_TYPEC_HUSB239
	ret = uclass_get_device_by_driver(UCLASS_I2C_GENERIC, DM_DRIVER_GET(husb239), &udev);
	if (ret) {
		pr_err("Failed to probe HUSB239: %d\n", ret);
	}
#endif

	ret = uclass_get_device_by_driver(UCLASS_MISC, DM_DRIVER_GET(shutdown_charge), &udev);
	if (ret) {
		pr_info("Continue to boot\n");
	}
	return ret;
#else
	return 0;
#endif
}

int board_init(void)
{
#ifdef CONFIG_DM_REGULATOR_SPM8XX
	int ret;

	ret = regulators_enable_boot_on(true);
	if (ret)
		pr_debug("%s: Cannot enable boot on regulator\n", __func__);
#endif

	return 0;
}

int board_late_init(void)
{
	ulong kernel_start;
	ofnode chosen_node;
	char ram_size_str[16] = {"\0"};
	int ret;

	// set_serialnumber_based_on_boot_mode();

	run_fastboot_command();

	probe_shutdown_charge();

	ret = run_uboot_shell();
	if (!ret) {
		pr_info("reboot into uboot shell\n");
		return 0;
	}

	if (!is_video_connected) {
		env_set("stdout", "serial");
	}

	/*save ram size to env, transfer to MB*/
	sprintf(ram_size_str, "mem=%dMB", (int)(gd->ram_size / SZ_1MB));
	env_set("ram_size", ram_size_str);

	chosen_node = ofnode_path("/chosen");
	if (!ofnode_valid(chosen_node)) {
		pr_debug("No chosen node found, can't get kernel start address\n");
		return 0;
	}

	ret = ofnode_read_u64(chosen_node, "riscv,kernel-start",
				  (u64 *)&kernel_start);
	if (ret) {
		pr_debug("Can't find kernel start address in device tree\n");
		return 0;
	}

	env_set_hex("kernel_start", kernel_start);

	return 0;
}

void *board_fdt_blob_setup(int *err)
{
	*err = 0;

	/* Stored the DTB address there during our init */
	if (IS_ENABLED(CONFIG_OF_SEPARATE) || IS_ENABLED(CONFIG_OF_BOARD)) {
		if (gd->arch.firmware_fdt_addr){
			if (!fdt_check_header((void *)(ulong)gd->arch.firmware_fdt_addr)){
				return (void *)(ulong)gd->arch.firmware_fdt_addr;
			}
		}
	}
	return (ulong *)&_end;
}

enum env_location env_get_location(enum env_operation op, int prio)
{
	if (prio >= 1)
		return ENVL_UNKNOWN;

	u32 boot_mode = get_boot_mode();
	switch (boot_mode) {
#ifdef CONFIG_ENV_IS_IN_MTD
	case BOOT_MODE_NAND:
		return ENVL_MTD;
#endif
#ifdef CONFIG_ENV_IS_IN_NAND
	case BOOT_MODE_NAND:
		return ENVL_NAND;
#endif
#ifdef CONFIG_ENV_IS_IN_SPI_FLASH
	case BOOT_MODE_NOR:
		return ENVL_SPI_FLASH;
#endif
#ifdef CONFIG_ENV_IS_IN_MMC
	case BOOT_MODE_EMMC:
	case BOOT_MODE_SD:
		return ENVL_MMC;
#endif
	default:
#ifdef CONFIG_ENV_IS_NOWHERE
		return ENVL_NOWHERE;
#else
		return ENVL_UNKNOWN;
#endif
	}
}

int misc_init_r(void)
{
	return 0;
}

int dram_init(void)
{
	u64 dram_size = (u64)ddr_get_density() * SZ_1MB;

	gd->ram_base = CONFIG_SYS_SDRAM_BASE;
	gd->ram_size = dram_size;

	return 0;
}

int dram_init_banksize(void)
{
	u64 dram_size = (u64)ddr_get_density() * SZ_1MB;

	memset(gd->bd->bi_dram, 0, sizeof(gd->bd->bi_dram));
	gd->bd->bi_dram[0].start = CONFIG_SYS_SDRAM_BASE;
	if(dram_size > SZ_2GB) {
		gd->bd->bi_dram[0].size = SZ_2G;
		if (CONFIG_NR_DRAM_BANKS > 1) {
			gd->bd->bi_dram[1].start = 0x100000000;
			gd->bd->bi_dram[1].size = dram_size - SZ_2G;
		}
	} else {
		gd->bd->bi_dram[0].size = dram_size;
	}

	return 0;
}

ulong board_get_usable_ram_top(ulong total_size)
{
	u64 dram_size = (u64)ddr_get_density() * SZ_1MB;

		/* Some devices (like the EMAC) have a 32-bit DMA limit. */
	if(dram_size > SZ_2GB) {
		return 0x80000000;
	} else {
		return dram_size;
	}
}

static uint32_t get_dro_from_efuse(void)
{
	struct udevice *dev;
	uint8_t fuses[2];
	uint32_t dro = SVT_DRO_DEFAULT_VALUE;
	int ret;

	/* retrieve the device */
	ret = uclass_get_device_by_driver(UCLASS_MISC,
			DM_DRIVER_GET(spacemit_k1x_efuse), &dev);
	if (ret) {
		return dro;
	}

	// read from efuse, each bank has 32byte efuse data
	// SVT-DRO in bank7 bit173~bit181
	ret = misc_read(dev, 7 * 32 + 21, fuses, sizeof(fuses));
	if (0 == ret) {
		// (byte1 bit0~bit5) << 3 | (byte0 bit5~7) >> 5
		dro = (fuses[0] >> 5) & 0x07;
		dro |= (fuses[1] & 0x3F) << 3;
	}

	if (0 == dro)
		dro = SVT_DRO_DEFAULT_VALUE;
	return dro;
}

static int get_chipinfo_from_efuse(uint32_t *product_id, uint32_t *wafer_tid)
{
	struct udevice *dev;
	uint8_t fuses[3];
	int ret;

	*product_id = 0;
	*wafer_tid = 0;
	/* retrieve the device */
	ret = uclass_get_device_by_driver(UCLASS_MISC,
			DM_DRIVER_GET(spacemit_k1x_efuse), &dev);
	if (ret) {
		return ENODEV;
	}

	// read from efuse, each bank has 32byte efuse data
	// product id in bank7 bit182~bit190
	ret = misc_read(dev, 7 * 32 + 22, fuses, sizeof(fuses));
	if (0 == ret) {
		// (byte1 bit0~bit6) << 2 | (byte0 bit6~7) >> 6
		*product_id = (fuses[0] >> 6) & 0x03;
		*product_id |= (fuses[1] & 0x7F) << 2;
	}

	// read from efuse, each bank has 32byte efuse data
	// product id in bank7 bit139~bit154
	ret = misc_read(dev, 7 * 32 + 17, fuses, sizeof(fuses));
	if (0 == ret) {
		// (byte1 bit0~bit6) << 2 | (byte0 bit3~7) >> 3
		*wafer_tid = (fuses[0] >> 3) & 0x1F;
		*wafer_tid |= fuses[1] << 5;
		*wafer_tid |= (fuses[2] & 0x07) << 13;
	}

	return 0;
}

static int ft_board_cpu_fixup(void *blob, struct bd_info *bd)
{
	int node, ret;
	uint32_t dro, product_id, wafer_tid;

	node = fdt_path_offset(blob, "/");
	if (node < 0) {
		pr_err("Can't find root node!\n");
		return -EINVAL;
	}

	get_chipinfo_from_efuse(&product_id, &wafer_tid);
	product_id = cpu_to_fdt32(product_id);
	wafer_tid = cpu_to_fdt32(wafer_tid);
	fdt_setprop(blob, node, "product-id", &product_id, sizeof(product_id));
	fdt_setprop(blob, node, "wafer-id", &wafer_tid, sizeof(wafer_tid));

	node = fdt_path_offset(blob, "/cpus");
	if (node < 0) {
		pr_err("Can't find cpus node!\n");
		return -EINVAL;
	}

	dro = cpu_to_fdt32(get_dro_from_efuse());
	ret = fdt_setprop(blob, node, "svt-dro", &dro, sizeof(dro));
	if (ret < 0)
		return ret;
	return 0;
}

static int ft_board_info_fixup(void *blob, struct bd_info *bd)
{
	int node;
	const char *part_number;

	node = fdt_path_offset(blob, "/");
	if (node < 0) {
		pr_err("Can't find root node!\n");
		return -EINVAL;
	}

	part_number = env_get("part#");
	if (NULL != part_number)
		fdt_setprop(blob, node, "part-number", part_number, strlen(part_number));

	return 0;
}

static int ft_board_mac_addr_fixup(void *blob, struct bd_info *bd)
{
	int node, i;
	const char *addr_value;
	// char addr_str[ARP_HLEN_ASCII + 1];
	const char *mac_item[] = {"wifi_addr", "bt_addr"};

	node = fdt_path_offset(blob, "/soc");
	if (node < 0) {
		pr_err("Can't find soc node!\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(mac_item); i++) {
		addr_value = env_get(mac_item[i]);
		if (NULL != addr_value) {
			// memset(addr_str, 0, sizeof(addr_str));
			// sprintf(addr_str, "%pM", addr_value);
			fdt_setprop(blob, node, mac_item[i], addr_value, strlen(addr_value));
		}
	}

	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{

	static const struct node_info nodes[] = {
		{ "spacemit,k1x-qspi", MTD_DEV_TYPE_NOR, },  /* SPI flash */
	};

	/* update MTD partition info for nor boot */
	if (CONFIG_IS_ENABLED(FDT_FIXUP_PARTITIONS) &&
		BOOT_MODE_NOR == get_boot_mode())
		fdt_fixup_mtdparts(blob, nodes, ARRAY_SIZE(nodes));

#if CONFIG_IS_ENABLED(FDT_SIMPLEFB)
	struct fdt_memory mem;
	/* reserved with no-map tag the video buffer */
	mem.start = gd->video_bottom;
	mem.end = gd->video_top - 1;

	fdtdec_add_reserved_memory(blob, "framebuffer", &mem, NULL, 0, NULL, 0);
#endif

	ft_board_cpu_fixup(blob, bd);
	ft_board_info_fixup(blob, bd);
	ft_board_mac_addr_fixup(blob, bd);
	return 0;
}

static bool has_bootarg(const char *args, const char *param, size_t param_len)
{
	const char *p = args;

	if (!args || !param || !param_len)
		return false;

	// Iterate through all parameters in args
	while (*p) {
		// Skip spaces
		while (*p == ' ')
			p++;
		if (!*p)
			break;

		// Check if current parameter matches
		if (strncmp(p, param, param_len) == 0 &&
			(p[param_len] == '\0' || p[param_len] == ' ' || p[param_len] == '=')) {
			return true;
		}

		// Move to next parameter
		while (*p && *p != ' ')
			p++;
	}
	return false;
}

char *board_fdt_chosen_bootargs(void)
{
	const void *fdt;
	const char *env_args = env_get("bootargs");
	const char *dts_args = NULL;
	char *merged = NULL;
	int nodeoffset;

	fdt = (void *)env_get_hex("fdt_addr", 0);
	if (!fdt) {
		return (char *)env_args;
	}

	if (fdt_check_header(fdt)) {
		pr_err("Invalid kernel DTB\n");
		return (char *)env_args;
	}

	nodeoffset = fdt_path_offset(fdt, "/chosen");
	if (nodeoffset >= 0)
		dts_args = fdt_getprop(fdt, nodeoffset, "bootargs", NULL);

	// Print env bootargs
	pr_debug("Env bootargs:\n    %s\n", env_args ? env_args : "NULL");
	// Print DTS bootargs
	pr_debug("DTS bootargs:\n    %s\n", dts_args ? dts_args : "NULL");

	if (!dts_args)
		return (char *)env_args;

	size_t total_len = 1;
	if (env_args)
		total_len += strlen(env_args);
	if (dts_args)
		total_len += strlen(dts_args) + 1;

	merged = calloc(1, total_len);
	if (!merged) {
		pr_err("Memory allocation failed\n");
		return NULL;
	}

	if (env_args)
		strcpy(merged, env_args);

	const char *p = dts_args;
	bool need_space = (merged[0] != '\0');

	while (p && *p) {
		while (*p && *p == ' ')
			p++;
		if (!*p)
			break;

		const char *end = p;
		while (*end && *end != ' ')
			end++;

		size_t param_len;
		const char *eq = memchr(p, '=', end - p);
		param_len = eq ? (size_t)(eq - p) : (size_t)(end - p);

		if (!has_bootarg(env_args, p, param_len)) {
			if (need_space)
				strcat(merged, " ");
			strncat(merged, p, end - p);
			need_space = true;
		}

		p = end;
	}

	if (!merged[0]) {
		free(merged);
		merged = NULL;
	}

	return merged;
}
