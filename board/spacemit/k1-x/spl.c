// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (c) 2023 Spacemit, Inc
 */

#include <common.h>
#include <dm.h>
#include <init.h>
#include <spl.h>
#include <misc.h>
#include <log.h>
#include <i2c.h>
#include <cpu.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <env.h>
#include <env_internal.h>
#include <mapmem.h>
#include <asm/global_data.h>
#include <fb_spacemit.h>
#include <tlv_eeprom.h>
#include <stdlib.h>
#include <u-boot/crc.h>
#include <cpu_func.h>
#include <dt-bindings/soc/spacemit-k1x.h>
#include <display_options.h>
#include <asm/io.h>
#include <configs/k1-x.h>
#include "../../drivers/gpio/k1x_gpio.h"

#define GEN_CNT			(0xD5001000)
#define STORAGE_API_P_ADDR	(0xC0838498)
#define SDCARD_API_ENTRY	(0xFFE0A548)

/* pin mux */
#define MUX_MODE0       0
#define MUX_MODE1       1
#define MUX_MODE2       2
#define MUX_MODE3       3
#define MUX_MODE4       4
#define MUX_MODE5       5
#define MUX_MODE6       6
#define MUX_MODE7       7

/* edge detect */
#define EDGE_NONE       (1 << 6)
#define EDGE_RISE       (1 << 4)
#define EDGE_FALL       (1 << 5)
#define EDGE_BOTH       (3 << 4)

/* driver strength*/
#define PAD_1V8_DS0     (0 << 11)
#define PAD_1V8_DS1     (1 << 11)
#define PAD_1V8_DS2     (2 << 11)
#define PAD_1V8_DS3     (3 << 11)

/*
 * notice: !!!
 * ds2 ---> bit10, ds1 ----> bit12, ds0 ----> bit11
*/
#define PAD_3V_DS0      (0 << 10)     /* bit[12:10] 000 */
#define PAD_3V_DS1      (2 << 10)     /* bit[12:10] 010 */
#define PAD_3V_DS2      (4 << 10)     /* bit[12:10] 100 */
#define PAD_3V_DS3      (6 << 10)     /* bit[12:10] 110 */
#define PAD_3V_DS4      (1 << 10)     /* bit[12:10] 001 */
#define PAD_3V_DS5      (3 << 10)     /* bit[12:10] 011 */
#define PAD_3V_DS6      (5 << 10)     /* bit[12:10] 101 */
#define PAD_3V_DS7      (7 << 10)     /* bit[12:10] 111 */

/* pull up/down */
#define PULL_DIS        (0 << 13)     /* bit[15:13] 000 */
#define PULL_UP         (6 << 13)     /* bit[15:13] 110 */
#define PULL_DOWN       (5 << 13)     /* bit[15:13] 101 */

#define MFPR_MMC1_BASE     0xD401E1B8
#define MMC1_DATA3_OFFSET  0x00
#define MMC1_DATA2_OFFSET  0x04
#define MMC1_DATA1_OFFSET  0x08
#define MMC1_DATA0_OFFSET  0x0C
#define MMC1_CMD_OFFSET    0x10
#define MMC1_CLK_OFFSET    0x14

extern int __data_start[], __data_end[];
extern int get_tlvinfo(uint8_t id, uint8_t *buffer, int max_size);
extern bool get_mac_address(uint64_t *mac_addr);
extern char *get_product_name(void);
extern void update_ddr_info(void);
extern enum board_boot_mode get_boot_storage(void);
extern ulong read_boot_storage(void *buff, ulong offset, ulong byte_size);
extern int dram_init_banksize(void);
extern void spl_fixup_fdt(void *fdt_blob);

char *product_name;
extern u32 ddr_cs_num, ddr_datarate, ddr_tx_odt;
extern const char *ddr_type;

/* LED configurations for different boards */
#define MAX_PRODUCT_NAMES 1

struct led_config {
	const char *product_names[MAX_PRODUCT_NAMES];
	int gpio;
	unsigned int pad_conf_reg;
	unsigned int pad_conf_val;
};

/* Board specific LED configurations */
static const struct led_config board_leds[] = {
	{
		.product_names = {
			"k1-x_MUSE-Pi-Pro",
		},
		.gpio = STATUS_LED_GPIO0,
		.pad_conf_reg = 0xD401E1E0,  // K1X_PADCONF_DVL0
		.pad_conf_val = MUX_MODE1 | EDGE_NONE | PULL_DOWN | PAD_1V8_DS2,
	},
};

static const struct led_config *current_led;

/* GPIO definitions */
#define GPIO_OUTPUT     1
#define GPIO_HIGH       1
#define GPIO_LOW        0

/* Helper macros for GPIO register access */
#define GPIO_TO_REG(gp)     (gp >> 5)
#define GPIO_TO_BIT(gp)     (1 << (gp & 0x1f))

static inline struct gpio_reg *get_gpio_bank(int gpio)
{
	const unsigned long offset[] = {0, 4, 8, 0x100};
	return (struct gpio_reg *)(K1X_GPIO_BASE + offset[GPIO_TO_REG(gpio)]);
}

static const struct led_config *get_led_config(void)
{
	const char *name = get_product_name();
	int i, j;

	if (!name) {
		name = DEFAULT_PRODUCT_NAME;
	}

	for (i = 0; i < ARRAY_SIZE(board_leds); i++) {
		for (j = 0; j < MAX_PRODUCT_NAMES && board_leds[i].product_names[j]; j++) {
			if (strcmp(name, board_leds[i].product_names[j]) == 0) {
				return &board_leds[i];
			}
		}
	}

	pr_debug("Warning: No LED config found for board %s\n", name);
	return &board_leds[0];
}

static void gpio_led_init(void)
{
	struct gpio_reg *gpio_bank;

	current_led = get_led_config();
	gpio_bank = get_gpio_bank(current_led->gpio);

	/* Configure LED pad */
	writel(current_led->pad_conf_val, (void __iomem *)(ulong)current_led->pad_conf_reg);

	/* Set GPIO as output */
	writel(GPIO_TO_BIT(current_led->gpio), &gpio_bank->gsdr);

	/* Set initial state to HIGH */
	writel(GPIO_TO_BIT(current_led->gpio), &gpio_bank->gpsr);

	pr_debug("GPIO LED initialized for %s on pin %d\n", current_led->product_names[0], current_led->gpio);
}

static void gpio_led_set(int value)
{
	struct gpio_reg *gpio_bank = get_gpio_bank(current_led->gpio);

	if (value)
		writel(GPIO_TO_BIT(current_led->gpio), &gpio_bank->gpsr);  // Set HIGH
	else
		writel(GPIO_TO_BIT(current_led->gpio), &gpio_bank->gpcr);  // Set LOW
}

static void gpio_led_blink_for_nor(void)
{
	while (1) {
		/* One short flash - ON for 100ms */
		gpio_led_set(GPIO_HIGH);
		mdelay(100);
		gpio_led_set(GPIO_LOW);
		mdelay(200);

		/* Long ON for 3 seconds */
		gpio_led_set(GPIO_HIGH);
		mdelay(3000);
	}
}

static void gpio_led_blink_for_ddr(void)
{
	while (1) {
		/* First short flash - ON for 100ms */
		gpio_led_set(GPIO_HIGH);
		mdelay(100);
		gpio_led_set(GPIO_LOW);
		mdelay(200);

		/* Second short flash - ON for 100ms */
		gpio_led_set(GPIO_HIGH);
		mdelay(100);
		gpio_led_set(GPIO_LOW);
		mdelay(200);

		/* Wait for 3 seconds before next cycle */
		gpio_led_set(GPIO_HIGH);
		mdelay(3000);
	}
}

int timer_init(void)
{
	/* enable generic cnt */
	u32 read_data;
	void __iomem *reg;

	reg = ioremap(GEN_CNT, 0x20);
	read_data = readl(reg);
	read_data |= BIT(0);
	writel(read_data, reg);

	return 0;
}

enum board_boot_mode __get_boot_storage(void)
{
	size_t *api = (size_t*)STORAGE_API_P_ADDR;
	size_t address = *api;
	// Did NOT select sdcard boot, but sdcard always has first boot priority
	if (SDCARD_API_ENTRY == address)
		return BOOT_MODE_SD;
	else
		return get_boot_pin_select();
}

void fix_boot_mode(void)
{
	if (0 == readl((void *)BOOT_DEV_FLAG_REG))
		set_boot_mode(__get_boot_storage());
}

void board_pinctrl_setup(void)
{
	//sdcard pinctrl setup
	writel(MUX_MODE0 | EDGE_NONE | PULL_UP | PAD_3V_DS4, (void __iomem *)MFPR_MMC1_BASE + MMC1_DATA3_OFFSET);
	writel(MUX_MODE0 | EDGE_NONE | PULL_UP | PAD_3V_DS4, (void __iomem *)MFPR_MMC1_BASE + MMC1_DATA2_OFFSET);
	writel(MUX_MODE0 | EDGE_NONE | PULL_UP | PAD_3V_DS4, (void __iomem *)MFPR_MMC1_BASE + MMC1_DATA1_OFFSET);
	writel(MUX_MODE0 | EDGE_NONE | PULL_UP | PAD_3V_DS4, (void __iomem *)MFPR_MMC1_BASE + MMC1_DATA0_OFFSET);
	writel(MUX_MODE0 | EDGE_NONE | PULL_UP | PAD_3V_DS4, (void __iomem *)MFPR_MMC1_BASE + MMC1_CMD_OFFSET);
	writel(MUX_MODE0 | EDGE_NONE | PULL_DOWN | PAD_3V_DS4, (void __iomem *)MFPR_MMC1_BASE + MMC1_CLK_OFFSET);
}

static uint32_t adjust_cpu_freq(uint64_t cluster, uint32_t freq)
{
	uint32_t freq_act=freq, val;

	/* switch cpu clock source */
	val = readl((void __iomem *)(K1X_APMU_BASE + 0x38c + cluster*4));
	val &= ~(0x07 | BIT(13));
	switch(freq) {
	case 1600000:
		val |= 0x07;
		break;

	case 1228000:
		val |= 0x04;
		break;

	case 819000:
		val |= 0x01;
		break;

	case 614000:
	default:
		freq_act = 614000;
		val |= 0x00;
		break;
	}
	writel(val, (void __iomem *)(K1X_APMU_BASE + 0x38c + cluster*4));

	/* set cluster frequency change request, and wait done */
	val = readl((void __iomem *)(K1X_APMU_BASE + 0x38c + cluster*4));
	val |= BIT(12);
	writel(val, (void __iomem *)(K1X_APMU_BASE + 0x38c + cluster*4));
	while(readl((void __iomem *)(K1X_APMU_BASE + 0x38c + cluster*4)) & BIT(12));

	return freq_act;
}

void raise_cpu_frequency(void)
{
	uint32_t val, cpu_freq;
	struct udevice *cpu;

	writel(0x2dffff, (void __iomem *)0xd4051024);

	/* enable CLK_1228M */
	val = readl((void __iomem *)(K1X_MPMU_BASE + 0x1024));
	val |= BIT(16) | BIT(15) | BIT(14) | BIT(13);
	writel(val, (void __iomem *)(K1X_MPMU_BASE + 0x1024));

	/* enable PLL3(3200Mhz) */
	val = readl((void __iomem *)(K1X_APB_SPARE_BASE + 0x12C));
	val |= BIT(31);
	writel(val, (void __iomem *)(K1X_APB_SPARE_BASE + 0x12C));
	/* enable PLL3_DIV2 */
	val = readl((void __iomem *)(K1X_APB_SPARE_BASE + 0x128));
	val |= BIT(1);
	writel(val, (void __iomem *)(K1X_APB_SPARE_BASE + 0x128));

	cpu = cpu_get_current_dev();
	if(dev_read_u32u(cpu, "boot_freq_cluster0", &cpu_freq)) {
		pr_info("boot_freq_cluster0 not configured, use 1228000 as default!\n");
		cpu_freq = 1228000;
	}
	cpu_freq = adjust_cpu_freq(0, cpu_freq);
	pr_info("adjust cluster-0 frequency to %u ...	[done]\n", cpu_freq);

	if(dev_read_u32u(cpu, "boot_freq_cluster1", &cpu_freq)) {
		pr_info("boot_freq_cluster1 not configured, use 1228000 as default!\n");
		cpu_freq = 614000;
	}
	cpu_freq = adjust_cpu_freq(1, cpu_freq);
	pr_info("adjust cluster-1 frequency to %u ...	[done]\n", cpu_freq);
}

#if CONFIG_IS_ENABLED(SPACEMIT_K1X_EFUSE)
int load_board_config_from_efuse(int *eeprom_i2c_index,
				 int *eeprom_pin_group, int *pmic_type)
{
	struct udevice *dev;
	uint8_t fuses[2];
	int ret;

	/* retrieve the device */
	ret = uclass_get_device_by_driver(UCLASS_MISC,
			DM_DRIVER_GET(spacemit_k1x_efuse), &dev);
	if (ret) {
		return ret;
	}

	// read from efuse, each bank has 32byte efuse data
	ret = misc_read(dev, 9 * 32 + 0, fuses, sizeof(fuses));
	if ((0 == ret) && (0 != fuses[0])) {
		// byte0 bit0~3 is eeprom i2c controller index
		*eeprom_i2c_index = fuses[0] & 0x0F;
		// byte0 bit4~5 is eeprom pin group index
		*eeprom_pin_group = (fuses[0] >> 4) & 0x03;
		// byte1 bit0~3 is pmic type
		*pmic_type = fuses[1] & 0x0F;
	}

	return ret;
}

int load_chipid_from_efuse(uint64_t *chipid)
{
	struct udevice *dev;
	uint8_t fuses[32];
	int ret;

	/* retrieve the device */
	ret = uclass_get_device_by_driver(UCLASS_MISC,
			DM_DRIVER_GET(spacemit_k1x_efuse), &dev);
	if (ret) {
		return ret;
	}

	// read from efuse, each bank has 32byte efuse data
	ret = misc_read(dev, 7 * 32 + 0, fuses, sizeof(fuses));
	if (0 == ret) {
		// bit191~251 is chipid
		// 1. get bit 192~251
		// 2. left shift 1bit, and merge with efuse_bank[7].bit191
		*chipid = 0;
		memcpy(chipid, &fuses[24], 8);
		*chipid <<= 4;
		*chipid >>= 3;
		*chipid |= (fuses[23] & 0x80) >> 7;
		pr_debug("Get chipid %llx\n", *chipid);
	}

	return ret;
}
#endif

static void load_default_board_config(int *eeprom_i2c_index,
		int *eeprom_pin_group, int *pmic_type)
{
	char *temp;

	temp = env_get("eeprom_i2c_index");
	if (NULL != temp)
		*eeprom_i2c_index = dectoul(temp, NULL);
	else
		*eeprom_i2c_index = K1_DEFALT_EEPROM_I2C_INDEX;

	temp = env_get("eeprom_pin_group");
	if (NULL != temp)
		*eeprom_pin_group = dectoul(temp, NULL);
	else
		*eeprom_pin_group = K1_DEFALT_EEPROM_PIN_GROUP;

	temp = env_get("pmic_type");
	if (NULL != temp)
		*pmic_type = dectoul(temp, NULL);
	else
		*pmic_type = K1_DEFALT_PMIC_TYPE;
}

#if CONFIG_IS_ENABLED(SPACEMIT_POWER)
extern int board_pmic_init(void);
#endif

void load_board_config(int *eeprom_i2c_index, int *eeprom_pin_group, int *pmic_type)
{
	load_default_board_config(eeprom_i2c_index, eeprom_pin_group, pmic_type);

#if CONFIG_IS_ENABLED(SPACEMIT_K1X_EFUSE)
	/* update env from efuse data */
	load_board_config_from_efuse(eeprom_i2c_index, eeprom_pin_group, pmic_type);
#endif

	pr_debug("eeprom_i2c_index :%d\n", *eeprom_i2c_index);
	pr_debug("eeprom_pin_group :%d\n", *eeprom_pin_group);
	pr_debug("pmic_type :%d\n", *pmic_type);
}

bool restore_ddr_training_info(uint64_t chipid, uint64_t mac_addr)
{
	bool success = true;
	struct ddr_training_info_t *info;
	ulong flush_start, flush_lenth;

	pr_debug("chipid %llx\n", chipid);
	pr_debug("mac_addr %llx\n", mac_addr);

	info = (struct ddr_training_info_t*)map_sysmem(DDR_TRAINING_INFO_BUFF, 0);
	// Force to do DDR software training while in USB download mode or info is invalid
	if ((BOOT_MODE_USB == get_boot_mode()) ||
		(sizeof(*info) != read_boot_storage(info, DDR_TRAINING_INFO_OFFSET, sizeof(*info))) ||
		(DDR_TRAINING_INFO_MAGIC != info->magic) ||
		(chipid != info->chipid) ||
		(mac_addr != info->mac_addr) ||
		(DDR_TRAINING_INFO_VER != info->version) ||
		(info->crc32 != crc32(0, (const uchar *)&info->chipid, sizeof(*info) - 8))) {
		// clear magic, set invalid
		memset(info, 0, sizeof(*info));
		success = false;
	}

	flush_start = round_down((size_t)info, CONFIG_RISCV_CBOM_BLOCK_SIZE);
	flush_lenth = round_up(sizeof(*info), CONFIG_RISCV_CBOM_BLOCK_SIZE);
	flush_dcache_range(flush_start, flush_start + flush_lenth);
	return success;
}

void update_ddr_training_info(uint64_t chipid, uint64_t mac_addr)
{
	struct ddr_training_info_t *info;
	// ulong flush_start, flush_lenth;

	info = (struct ddr_training_info_t*)map_sysmem(DDR_TRAINING_INFO_BUFF, 0);
	if ((DDR_TRAINING_INFO_MAGIC == info->magic) &&
		(info->crc32 == crc32(0, (const uchar *)&info->chipid, sizeof(*info) - 8))) {
		// NO need to save ddr trainig info
		info->magic = 0;
		}
	else {
		// first time to do ddr training or ddr training info is update
		info->magic = DDR_TRAINING_INFO_MAGIC;
		info->chipid = chipid;
		info->mac_addr = mac_addr;
		info->version = DDR_TRAINING_INFO_VER;
		info->crc32 = crc32(0, (const uchar *)&info->chipid, sizeof(*info) - 8);
	}

	// flush_start = round_down((size_t)info, CONFIG_RISCV_CBOM_BLOCK_SIZE);
	// flush_lenth = round_up(sizeof(*info), CONFIG_RISCV_CBOM_BLOCK_SIZE);
	// flush_dcache_range(flush_start, flush_start + flush_lenth);
}

void update_ddr_config_info(uint32_t cs_num)
{
	struct ddr_training_info_t *info;

	info = (struct ddr_training_info_t*)map_sysmem(DDR_TRAINING_INFO_BUFF, 0);

	info->magic = DDR_TRAINING_INFO_MAGIC;
	info->version = DDR_TRAINING_INFO_VER;
	info->cs_num = cs_num;
	info->crc32 = crc32(0, (const uchar *)&info->chipid, sizeof(*info) - 8);
}

int spl_board_init_f(void)
{
	int ret;
	struct udevice *dev;
	bool flag;
	// uint64_t chipid = 0, mac_addr = 0;

#if CONFIG_IS_ENABLED(SYS_I2C_LEGACY)
	/* init i2c */
	i2c_init_board();
#endif

#if CONFIG_IS_ENABLED(SPACEMIT_POWER)
	board_pmic_init();
#endif

	raise_cpu_frequency();
#if CONFIG_IS_ENABLED(SPACEMIT_K1X_EFUSE)
	// load_chipid_from_efuse(&chipid);
#endif
	// get_mac_address(&mac_addr);

	update_ddr_info();

	// restore prevous saved ddr training info data
	// flag = restore_ddr_training_info(chipid, mac_addr);
	flag = true;
	if (!flag) {
		// flush data and stack
		flush_dcache_range(CONFIG_SPL_BSS_START_ADDR, CONFIG_SPL_STACK);
		flush_dcache_range(round_down((size_t)__data_start, CONFIG_RISCV_CBOM_BLOCK_SIZE),
		 	round_up((size_t)__data_end, CONFIG_RISCV_CBOM_BLOCK_SIZE));
		icache_disable();
		dcache_disable();
		invalidate_dcache_range(CONFIG_SPL_BSS_START_ADDR, CONFIG_SPL_STACK);
	}

	/* DDR init */
	ret = uclass_get_device(UCLASS_RAM, 0, &dev);
	if (ret) {
		pr_err("DRAM init failed: %d\n", ret);
		gpio_led_blink_for_ddr();
		return ret;
	}

	if (!flag) {
		icache_enable();
		dcache_enable();
	}

	// update_ddr_training_info(chipid, mac_addr);
	update_ddr_config_info(ddr_cs_num);
	timer_init();

	return 0;
}

void board_init_f(ulong dummy)
{
	int ret;

	// fix boot mode after boot rom
	fix_boot_mode();

	// setup pinctrl
	board_pinctrl_setup();

	ret = spl_early_init();
	if (ret)
		panic("spl_early_init() failed: %d\n", ret);

	riscv_cpu_setup(NULL, NULL);

	preloader_console_init();
	pr_debug("boot_mode: %x\n", get_boot_mode());

	ret = spl_board_init_f();
	if (ret)
		panic("spl_board_init_f() failed: %d\n", ret);
}

#ifdef CONFIG_SPL_LOAD_FIT
int board_fit_config_name_match(const char *name)
{
	char *buildin_name;
	char tmp_name[64];

	buildin_name = product_name;
	if (NULL == buildin_name)
		buildin_name = DEFAULT_PRODUCT_NAME;

	/*
		be compatible to previous format name,
		such as: k1_deb1 -> k1-x_deb1
	*/
	if (!strncmp(buildin_name, "k1_", 3)){
		sprintf(tmp_name, "%s_%s", "k1-x", &buildin_name[3]);
		buildin_name = tmp_name;
	}

	if ((NULL != buildin_name) && (0 == strcmp(buildin_name, name))) {
		log_emerg("Boot from fit configuration %s\n", name);
		return 0;
	}
	else
		return -1;
}
#endif

static struct env_driver *_spl_env_driver_lookup(enum env_location loc)
{
	struct env_driver *drv;
	const int n_ents = ll_entry_count(struct env_driver, env_driver);
	struct env_driver *entry;

	drv = ll_entry_start(struct env_driver, env_driver);
	for (entry = drv; entry != drv + n_ents; entry++) {
		if (loc == entry->location)
			return entry;
	}

	/* Not found */
	return NULL;
}

static struct env_driver *spl_env_driver_lookup(enum env_operation op, enum env_location loc)
{
	struct env_driver *drv;

	if (loc == ENVL_UNKNOWN)
		return NULL;

	drv = _spl_env_driver_lookup(loc);
	if (!drv) {
		pr_debug("%s: No environment driver for location %d\n", __func__, loc);
		return NULL;
	}

	return drv;
}

static void spl_load_env(void)
{
	struct env_driver *drv;
	int ret = -1;
	u32 boot_mode = get_boot_mode();

	/*if boot from usb, spl should not find env*/
	if (boot_mode == BOOT_MODE_USB){
		return;
	}

	/*
	only load env from mtd dev, because only mtd dev need
	env mtdparts info to load image.
	*/
	enum env_location loc = ENVL_UNKNOWN;
	switch (boot_mode) {
#ifdef CONFIG_ENV_IS_IN_NAND
	case BOOT_MODE_NAND:
		loc = ENVL_NAND;
		break;
#endif
#ifdef CONFIG_ENV_IS_IN_SPI_FLASH
	case BOOT_MODE_NOR:
		loc = ENVL_SPI_FLASH;
		break;
#endif
#ifdef CONFIG_ENV_IS_IN_MTD
	case BOOT_MODE_NAND:
	case BOOT_MODE_NOR:
		loc = ENVL_MTD;
		break;
#endif
	default:
		return;
	}

	drv = spl_env_driver_lookup(ENVOP_INIT, loc);
	if (!drv){
		pr_err("%s, can not load env from storage\n", __func__);
		return;
	}

	ret = drv->load();
	if (!ret){
		pr_info("has init env successful\n");
	}else{
		pr_err("load env from storage fail, would use default env\n");
		/*if load env from storage fail, it should not write bootmode to reg*/
		boot_mode = BOOT_MODE_NONE;
	}
}

bool get_mac_address(uint64_t *mac_addr)
{
	if ((NULL != mac_addr) &&
		(get_tlvinfo(TLV_CODE_MAC_BASE, (uint8_t*)mac_addr, 6)) > 0) {
		pr_info("Get mac address %llx from eeprom\n", *mac_addr);
		return true;
	}

	return false;
}

char *get_product_name(void)
{
	char *name = NULL;

	name = calloc(1, 64);
	if ((NULL != name) &&
		(get_tlvinfo(TLV_CODE_PRODUCT_NAME, name, 64)) > 0) {
		pr_info("Get product name from eeprom %s\n", name);
		return name;
	}

	if (NULL != name)
		free(name);

	pr_debug("Use default product name %s\n", DEFAULT_PRODUCT_NAME);
	return NULL;
}

void update_ddr_info(void)
{
	uint8_t *info;

	ddr_cs_num = 0;
	ddr_datarate = 0;
	ddr_tx_odt = 0;
	ddr_type = NULL;

	// read ddr type from eeprom
	info = malloc(32);
	memset(info, 0, 32);
	if (get_tlvinfo(TLV_CODE_DDR_TYPE, info, 32) > 0)
		ddr_type = info;
	else
		free(info);

	// if fail to get ddr cs number from eeprom, update it from dts node
	if (get_tlvinfo(TLV_CODE_DDR_CSNUM, (uint8_t*)&ddr_cs_num, 1) > 0)
		pr_info("Get ddr cs num %d from eeprom\n", ddr_cs_num);

	// if fail to get ddr cs number from eeprom, update it from dts node
	if (get_tlvinfo(TLV_CODE_DDR_DATARATE, (uint8_t*)&ddr_datarate, 2) > 0) {
		// convert it from big endian to little endian
		ddr_datarate = be16_to_cpu(ddr_datarate);
		pr_info("Get ddr datarate %d from eeprom\n", ddr_datarate);
	}

	// if fail to get ddr tx odt from eeprom, update it from dts node
	if (get_tlvinfo(TLV_CODE_DDR_TX_ODT, (uint8_t*)&ddr_tx_odt, 1) > 0) {
		pr_info("Get ddr tx odt(%dohm) from eeprom\n", ddr_tx_odt);
	}
}

void spl_board_init(void)
{
	/*load env*/
	spl_load_env();
	product_name = get_product_name();
	/* Initialize LED */
	gpio_led_init();
}

struct image_header *spl_get_load_buffer(ssize_t offset, size_t size)
{
	return map_sysmem(CONFIG_SPL_LOAD_FIT_ADDRESS, 0);
}

void board_boot_order(u32 *spl_boot_list)
{
	u32 boot_mode = get_boot_mode();
	pr_debug("boot_mode:%x\n", boot_mode);
	if (boot_mode == BOOT_MODE_USB){
		spl_boot_list[0] = BOOT_DEVICE_BOARD;
	}else{
		switch (boot_mode) {
		case BOOT_MODE_EMMC:
			spl_boot_list[0] = BOOT_DEVICE_MMC2;
			break;
		case BOOT_MODE_NAND:
			spl_boot_list[0] = BOOT_DEVICE_NAND;
			break;
		case BOOT_MODE_NOR:
			spl_boot_list[0] = BOOT_DEVICE_NOR;
			break;
		case BOOT_MODE_SD:
			spl_boot_list[0] = BOOT_DEVICE_MMC1;
			break;
		default:
			spl_boot_list[0] = BOOT_DEVICE_RAM;
			break;
		}

		//reserve for debug/test to load/run uboot from ram.
		spl_boot_list[1] = BOOT_DEVICE_RAM;
	}
}

void spl_load_error_handler(int bootdev, const char *loader_name)
{
	printf("SPL load error: bootdev=%d, loader=%s\n", bootdev, loader_name);
	switch (bootdev) {
	case BOOT_DEVICE_NOR:
		printf("SPI Flash load failed\n");
		gpio_led_blink_for_nor();
		break;
	case BOOT_DEVICE_MMC1:
	case BOOT_DEVICE_MMC2:
	case BOOT_DEVICE_NAND:
	default:
		printf("%s load failed\n", loader_name);
		break;
	}
}

void spl_perform_fixups(struct spl_image_info *spl_image)
{
	dram_init_banksize();
	spl_fixup_fdt(spl_image->fdt_addr);
}
