// SPDX-License-Identifier: GPL-2.0+

#include <env.h>
#include <i2c.h>
#include <asm/io.h>
#include <common.h>
#include <asm/global_data.h>
#include <stdlib.h>
#include <linux/delay.h>
#include <tlv_eeprom.h>

DECLARE_GLOBAL_DATA_PTR;

#define MUX_MODE0	0					/* func 0 */
#define MUX_MODE1	BIT(0)				/* func 1 */
#define MUX_MODE2	BIT(1)				/* func 2 */
#define MUX_MODE3	BIT(0) | BIT(1)		/* func 3 */
#define MUX_MODE4	BIT(2)				/* func 4 */
#define MUX_MODE5	BIT(0) | BIT(2)		/* func 5 */
#define EDGE_NONE	BIT(6)				/* edge-detection is unabled */
#define PAD_1V8_DS2	BIT(12)				/* voltage:1.8v, driver strength: 2 */
#define PULL_UP		BIT(14) | BIT(15)	/* pull-up */

#define I2C_PIN_CONFIG(x)	((x) | EDGE_NONE | PULL_UP | PAD_1V8_DS2)
#define READ_I2C_LINE_LEN (16)

int _read_from_i2c(int chip, u32 addr, u32 size, uchar *buf);
bool _is_valid_tlvinfo_header(struct tlvinfo_header *hdr);

char *spacemit_i2c_eeprom[] = {
	"atmel,24c02",
};

struct tlv_eeprom {
	uint8_t type;
	uint8_t length;
};

struct eeprom_config {
	uint8_t bus;
	uint16_t addr;
	uint8_t pin_function;
	uint32_t scl_pin_reg;
	uint32_t sda_pin_reg;
};

const struct eeprom_config eeprom_info[] = {
	// eeprom @deb1 & deb2: I2C2, pin group(GPIO_84, GPIO_85)
	{2, 0x50, MUX_MODE4, 0xd401e154, 0xd401e158},
	// eeprom @evb: I2C6, pin group(GPIO_118, GPIO_119)
	{6, 0x50, MUX_MODE2, 0xd401e228, 0xd401e22c},
};

static void init_tlv_data(uint8_t chip, uint8_t *buffer, uint32_t size)
{
	uint32_t offset;
	struct tlvinfo_header *hdr = (struct tlvinfo_header*)buffer;

	offset = sizeof(struct tlvinfo_header);
	_read_from_i2c(chip, 0, offset, buffer);
	if (!_is_valid_tlvinfo_header(hdr) || ((be16_to_cpu(hdr->totallen) + offset) > size)) {
		memset(buffer, 0, size);
		return;
	}

	_read_from_i2c(chip, offset, be16_to_cpu(hdr->totallen), buffer + offset);
}

static void i2c_set_pinctrl(uint32_t value, uint32_t reg_addr)
{
	writel(value, (void __iomem *)(size_t)reg_addr);
}

static uint32_t i2c_get_pinctrl(uint32_t reg_addr)
{
	return readl((void __iomem *)(size_t)reg_addr);
}

int init_tlv_from_eeprom(uint8_t *tlv_data, uint32_t tlv_size)
{
	int saddr, i;
	uint8_t bus;
	uint32_t scl_pin_backup, sda_pin_backup;

	for (i = 0; i < ARRAY_SIZE(eeprom_info); i++) {
		bus = eeprom_info[i].bus;
		saddr = eeprom_info[i].addr;

		scl_pin_backup = i2c_get_pinctrl(eeprom_info[i].scl_pin_reg);;
		sda_pin_backup = i2c_get_pinctrl(eeprom_info[i].sda_pin_reg);;
		i2c_set_pinctrl(I2C_PIN_CONFIG(eeprom_info[i].pin_function), eeprom_info[i].scl_pin_reg);
		i2c_set_pinctrl(I2C_PIN_CONFIG(eeprom_info[i].pin_function), eeprom_info[i].sda_pin_reg);

		if ((i2c_set_bus_num(bus) < 0) || (i2c_probe(saddr) < 0)) {
			pr_err("%s: probe i2c(%d) @eeprom %d failed\n", __func__, bus, saddr);
			i2c_set_pinctrl(scl_pin_backup, eeprom_info[i].scl_pin_reg);
			i2c_set_pinctrl(sda_pin_backup, eeprom_info[i].sda_pin_reg);
		}
		else {
			pr_info("find eeprom in bus %d, address %d\n", bus, saddr);
			init_tlv_data(saddr, tlv_data, tlv_size);
			return tlv_size;
		}
	}

	return -EINVAL;
}

int _read_from_i2c(int chip, u32 addr, u32 size, uchar *buf)
{
	u32 nbytes = size;
	u32 linebytes = 0;
	int ret;

	do {
		linebytes = (nbytes > READ_I2C_LINE_LEN) ? READ_I2C_LINE_LEN : nbytes;
		ret = i2c_read(chip, addr, 1, buf, linebytes);
		if (ret){
			pr_err("read from i2c error:%d\n", ret);
			return -1;
		}

		buf += linebytes;
		nbytes -= linebytes;
		addr += linebytes;
	} while (nbytes > 0);

	return 0;
}
