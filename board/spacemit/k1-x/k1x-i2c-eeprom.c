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

static const struct eeprom_config eeprom_info = {
    /* eeprom @deb1 & deb2: I2C2, pin group(GPIO_84, GPIO_85) */
    .bus          = 2,
    .addr         = 0x50,
    .pin_function = MUX_MODE4,
    .scl_pin_reg  = 0xd401e154,
    .sda_pin_reg  = 0xd401e158,
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

int init_tlv_from_eeprom(uint8_t *tlv_data, uint32_t tlv_size)
{
	const uint32_t pinval = I2C_PIN_CONFIG(eeprom_info.pin_function);

    writel(pinval, (void __iomem *)(uintptr_t)eeprom_info.scl_pin_reg);
    writel(pinval, (void __iomem *)(uintptr_t)eeprom_info.sda_pin_reg);

	init_tlv_data(eeprom_info.addr, tlv_data, tlv_size);
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
