// SPDX-License-Identifier: GPL-2.0+
/*
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * Copyright (C) 2013 Curt Brune <curt@cumulusnetworks.com>
 * Copyright (C) 2014 Srideep <srideep_devireddy@dell.com>
 * Copyright (C) 2013 Miles Tseng <miles_tseng@accton.com>
 * Copyright (C) 2014,2016 david_yang <david_yang@accton.com>
 */

#include <common.h>
#include <command.h>
#include <env.h>
#include <init.h>
#include <net.h>
#include <asm/global_data.h>
#include <linux/ctype.h>
#include <u-boot/crc.h>

#include "tlv_eeprom.h"

DECLARE_GLOBAL_DATA_PTR;

/* External function declarations */
extern ulong read_boot_storage(void *buff, ulong offset, ulong byte_size);
extern bool write_boot_storage(void *buff, ulong offset, ulong byte_size);
extern bool tlvinfo_find_tlv(u8 *tlv_data, u8 tcode, int *index);
extern bool _is_valid_tlvinfo_header(struct tlvinfo_header *hdr);
extern bool is_checksum_valid(u8 *data);
extern void update_crc(u8 *data);
extern bool tlvinfo_delete_tlv(u8 *data, u8 code);
extern bool tlvinfo_add_tlv(u8 *data, int tcode, char *strval);
extern int write_tlv_to_storage(uint32_t addr, uint8_t* buf, uint32_t size);

/* File scope function prototypes */
static int read_tlv_data(u8 *data);
static void show_tlv_data(u8 *data);
static void decode_tlv(struct tlvinfo_tlv *tlv);
static int prog_tlv_data(u8 *data);
static void show_tlv_code_list(void);

/* Set to 1 if we've read data into memory */
static int has_been_read;
/* The TLV contents after being read into memory */
static u8 tlv_data[TLV_INFO_MAX_LEN] __aligned(64);

#define to_header(p) ((struct tlvinfo_header *)p)
#define to_entry(p) ((struct tlvinfo_tlv *)p)

#define HDR_SIZE sizeof(struct tlvinfo_header)
#define ENT_SIZE sizeof(struct tlvinfo_tlv)

static inline bool is_digit(char c)
{
	return (c >= '0' && c <= '9');
}

/**
 *  is_valid_tlv
 *
 *  Perform basic sanity checks on a TLV field. The TLV is pointed to
 *  by the parameter provided.
 *      1. The type code is not reserved (0x00 or 0xFF)
 */
static inline bool is_valid_tlv(struct tlvinfo_tlv *tlv)
{
	return((tlv->type != 0x00) && (tlv->type != 0xFF));
}

/**
 *  is_hex
 *
 *  Tests if character is an ASCII hex digit
 */
static inline u8 is_hex(char p)
{
	return (((p >= '0') && (p <= '9')) ||
		((p >= 'A') && (p <= 'F')) ||
		((p >= 'a') && (p <= 'f')));
}

/**
 *  read_tlv_data
 *
 *  Read the TLV data into memory, if it hasn't already been read.
 */
static int read_tlv_data(u8 *data)
{
	if (has_been_read)
		return 0;

	bool read_success = false;

#if !defined(CONFIG_SPL_BUILD)
	/* Try to read from EEPROM first */
	if (read_tlv_eeprom(data, 0, TLV_INFO_MAX_LEN, 0) >= 0) {
		read_success = true;
	}
#endif

	/* If EEPROM read fails or in SPL, try boot storage */
	if (!read_success) {
		if (read_boot_storage(data, TLV_DATA_OFFSET, TLV_INFO_MAX_LEN) != TLV_INFO_MAX_LEN) {
			printf("Failed to read TLV data from EEPROM/storage\n");
			return -1;
		}
	}

	struct tlvinfo_header *data_hdr = to_header(data);

	// If the contents are invalid, start over with default contents
	if (!_is_valid_tlvinfo_header(data_hdr) || !is_checksum_valid(data)) {
		strcpy(data_hdr->signature, TLV_INFO_ID_STRING);
		data_hdr->version = TLV_INFO_VERSION;
		data_hdr->totallen = cpu_to_be16(0);
		update_crc(data);
	}

	has_been_read = 1;

#ifdef DEBUG
	show_tlv_data(data);
#endif

	return 0;
}

/**
 *  show_tlv_data
 *
 *  Display the contents of the TLV data
 */
static void show_tlv_data(u8 *data)
{
	int tlv_end;
	int curr_tlv;
#ifdef DEBUG
	int i;
#endif
	struct tlvinfo_header *data_hdr = to_header(data);
	struct tlvinfo_tlv    *data_tlv;

	if (!_is_valid_tlvinfo_header(data_hdr)) {
		printf("TLV data does not contain data in a valid TlvInfo format.\n");
		return;
	}

	printf("TlvInfo Header:\n");
	printf("   Id String:    %s\n", data_hdr->signature);
	printf("   Version:      %d\n", data_hdr->version);
	printf("   Total Length: %d\n", be16_to_cpu(data_hdr->totallen));

	printf("TLV Name             Code Len Value\n");
	printf("-------------------- ---- --- -----\n");
	curr_tlv = HDR_SIZE;
	tlv_end  = HDR_SIZE + be16_to_cpu(data_hdr->totallen);
	while (curr_tlv < tlv_end) {
		data_tlv = to_entry(&data[curr_tlv]);
		if (!is_valid_tlv(data_tlv)) {
			printf("Invalid TLV field starting at TLV offset %d\n",
			       curr_tlv);
			return;
		}
		decode_tlv(data_tlv);
		curr_tlv += ENT_SIZE + data_tlv->length;
	}

	printf("Checksum is %s.\n",
	       is_checksum_valid(data) ? "valid" : "invalid");

#ifdef DEBUG
	printf("TLV data dump: (0x%x bytes)", TLV_INFO_MAX_LEN);
	for (i = 0; i < TLV_INFO_MAX_LEN; i++) {
		if ((i % 16) == 0)
			printf("\n%02X: ", i);
		printf("%02X ", data[i]);
	}
	printf("\n");
#endif
}

/**
 *  Struct for displaying the TLV codes and names.
 */
struct tlv_code_desc {
	u8    m_code;
	char *m_name;
};

/**
 *  List of TLV codes and names.
 */
static struct tlv_code_desc tlv_code_list[] = {
	{ TLV_CODE_PRODUCT_NAME,   "Product Name"},
	{ TLV_CODE_PART_NUMBER,    "Part Number"},
	{ TLV_CODE_SERIAL_NUMBER,  "Serial Number"},
	{ TLV_CODE_MAC_BASE,       "Base MAC Address"},
	{ TLV_CODE_WIFI_MAC_ADDR,  "Wifi MAC Address"},
	{ TLV_CODE_BLUETOOTH_ADDR, "Bluetooth Address"},
	{ TLV_CODE_MANUF_DATE,     "Manufacture Date"},
	{ TLV_CODE_DEVICE_VERSION, "Device Version"},
	{ TLV_CODE_LABEL_REVISION, "Label Revision"},
	{ TLV_CODE_PLATFORM_NAME,  "Platform Name"},
	{ TLV_CODE_ONIE_VERSION,   "ONIE Version"},
	{ TLV_CODE_MAC_SIZE,       "MAC Addresses"},
	{ TLV_CODE_MANUF_NAME,     "Manufacturer"},
	{ TLV_CODE_MANUF_COUNTRY,  "Country Code"},
	{ TLV_CODE_VENDOR_NAME,    "Vendor Name"},
	{ TLV_CODE_DIAG_VERSION,   "Diag Version"},
	{ TLV_CODE_SERVICE_TAG,    "Service Tag"},
	{ TLV_CODE_VENDOR_EXT,     "Vendor Extension"},
	{ TLV_CODE_DDR_TX_ODT,     "DDR tx odt"},
	{ TLV_CODE_CRC_32,         "CRC-32"},
};

/**
 *  Look up a TLV name by its type.
 */
static inline const char *tlv_type2name(u8 type)
{
	char *name = "Unknown";
	int   i;

	for (i = 0; i < ARRAY_SIZE(tlv_code_list); i++) {
		if (tlv_code_list[i].m_code == type) {
			name = tlv_code_list[i].m_name;
			break;
		}
	}

	return name;
}

/*
 *  decode_tlv
 *
 *  Print a string representing the contents of the TLV field. The format of
 *  the string is:
 *      1. The name of the field left justified in 20 characters
 *      2. The type code in hex right justified in 5 characters
 *      3. The length in decimal right justified in 4 characters
 *      4. The value, left justified in however many characters it takes
 *  The validity of EEPROM contents and the TLV field have been verified
 *  prior to calling this function.
 */
#define DECODE_NAME_MAX     20

/*
 * The max decode value is currently for the 'raw' type or the 'vendor
 * extension' type, both of which have the same decode format.  The
 * max decode string size is computed as follows:
 *
 *   strlen(" 0xFF") * TLV_VALUE_MAX_LEN + 1
 *
 */
#define DECODE_VALUE_MAX    ((5 * TLV_VALUE_MAX_LEN) + 1)

static void decode_tlv(struct tlvinfo_tlv *tlv)
{
	char name[DECODE_NAME_MAX];
	char value[DECODE_VALUE_MAX];
	int i;

	strncpy(name, tlv_type2name(tlv->type), DECODE_NAME_MAX);

	switch (tlv->type) {
	case TLV_CODE_PRODUCT_NAME:
	case TLV_CODE_PART_NUMBER:
	case TLV_CODE_SERIAL_NUMBER:
	case TLV_CODE_MANUF_DATE:
	case TLV_CODE_LABEL_REVISION:
	case TLV_CODE_PLATFORM_NAME:
	case TLV_CODE_ONIE_VERSION:
	case TLV_CODE_MANUF_NAME:
	case TLV_CODE_MANUF_COUNTRY:
	case TLV_CODE_VENDOR_NAME:
	case TLV_CODE_DIAG_VERSION:
	case TLV_CODE_SERVICE_TAG:
		memcpy(value, tlv->value, tlv->length);
		value[tlv->length] = 0;
		break;
	case TLV_CODE_MAC_BASE:
	case TLV_CODE_WIFI_MAC_ADDR:
	case TLV_CODE_BLUETOOTH_ADDR:
		sprintf(value, "%02X:%02X:%02X:%02X:%02X:%02X",
			tlv->value[0], tlv->value[1], tlv->value[2],
			tlv->value[3], tlv->value[4], tlv->value[5]);
		break;
	case TLV_CODE_DEVICE_VERSION:
		sprintf(value, "%u", tlv->value[0]);
		break;
	case TLV_CODE_MAC_SIZE:
	case TLV_CODE_DDR_DATARATE:
		sprintf(value, "%u", (tlv->value[0] << 8) | tlv->value[1]);
		break;
	case TLV_CODE_VENDOR_EXT:
		value[0] = 0;
		for (i = 0; (i < (DECODE_VALUE_MAX / 5)) && (i < tlv->length);
				i++) {
			sprintf(value, "%s 0x%02X", value, tlv->value[i]);
		}
		break;
	case TLV_CODE_CRC_32:
		sprintf(value, "0x%02X%02X%02X%02X",
			tlv->value[0], tlv->value[1],
			tlv->value[2], tlv->value[3]);
		break;
	default:
		value[0] = 0;
		for (i = 0; (i < (DECODE_VALUE_MAX / 5)) && (i < tlv->length);
				i++) {
			sprintf(value, "%s 0x%02X", value, tlv->value[i]);
		}
		break;
	}

	name[DECODE_NAME_MAX - 1] = 0;
	printf("%-20s 0x%02X %3d %s\n", name, tlv->type, tlv->length, value);
}

/**
 *  prog_tlv_data
 *
 *  Write the TLV data from CPU memory to EEPROM or boot storage.
 */
static int prog_tlv_data(u8 *data)
{
	struct tlvinfo_header *data_hdr = to_header(data);
	int data_len;
	bool success = false;

	update_crc(data);

	data_len = HDR_SIZE + be16_to_cpu(data_hdr->totallen);

#if !defined(CONFIG_SPL_BUILD)
	/* Try to write to EEPROM first */
	if (write_tlv_to_storage(0, data, data_len) == 0) {
		success = true;
	}
#endif

	/* If EEPROM write fails or in SPL, try boot storage */
	if (!success) {
		// Round up to block size
		int aligned_len = (data_len + 511) & ~511;
		if (!write_boot_storage(data, TLV_DATA_OFFSET, aligned_len)) {
			printf("Programming to storage failed.\n");
			return -1;
		}
	}

	printf("Programming to EEPROM/storage passed.\n");
	return 0;
}

/**
 *  show_tlv_code_list - Display the list of TLV codes and names
 */
void show_tlv_code_list(void)
{
	int i;

	printf("TLV Code    TLV Name\n");
	printf("========    =================\n");
	for (i = 0; i < ARRAY_SIZE(tlv_code_list); i++) {
		printf("0x%02X        %s\n",
		       tlv_code_list[i].m_code,
		       tlv_code_list[i].m_name);
	}
}

/**
 *  do_tlv_custom
 *
 *  This function implements the tlv_custom command.
 */
int do_tlv_custom(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	char cmd;
	struct tlvinfo_header *data_hdr = to_header(tlv_data);

	// If no arguments, read the data and display its contents
	if (argc == 1) {
		read_tlv_data(tlv_data);
		show_tlv_data(tlv_data);
		return 0;
	}

	cmd = argv[1][0];

	// Read the TLV contents
	if (cmd == 'r') {
		has_been_read = 0;
		if (!read_tlv_data(tlv_data))
			printf("TLV data loaded from EEPROM or boot storage to memory.\n");
		return 0;
	}

	// Subsequent commands require that the TLV data has already been read.
	if (!has_been_read) {
		printf("Please read the TLV data first, using the 'tlv_custom read' command.\n");
		return 0;
	}

	// Handle the commands that don't take parameters
	if (argc == 2) {
		switch (cmd) {
		case 'w':   /* write */
			prog_tlv_data(tlv_data);
			break;
		case 'e':   /* erase */
			strcpy(data_hdr->signature, TLV_INFO_ID_STRING);
			data_hdr->version = TLV_INFO_VERSION;
			data_hdr->totallen = cpu_to_be16(0);
			update_crc(tlv_data);
			printf("TLV data in memory reset.\n");
			break;
		case 'l':   /* list */
			show_tlv_code_list();
			break;
		default:
			cmd_usage(cmdtp);
			break;
		}
		return 0;
	}

	// The set command takes one or two args.
	if (argc > 4) {
		cmd_usage(cmdtp);
		return 0;
	}

	// Set command
	if (cmd == 's') {
		int tcode;

		tcode = simple_strtoul(argv[2], NULL, 0);
		tlvinfo_delete_tlv(tlv_data, tcode);
		if (argc == 4)
			tlvinfo_add_tlv(tlv_data, tcode, argv[3]);
	} else {
		cmd_usage(cmdtp);
	}

	return 0;
}

/**
 *  This macro defines the tlv_custom command line command.
 */
U_BOOT_CMD(tlv_custom, 4, 1, do_tlv_custom,
	"Display and program the TLV data block in EEPROM or boot storage.",
	"[read|write|set <type_code> <string_value>|erase|list]\n"
	"tlv_custom\n"
	"    - With no arguments display the current contents.\n"
	"tlv_custom read\n"
	"    - Load TLV data from EEPROM or boot storage to memory.\n"
	"tlv_custom write\n"
	"    - Write the TLV data to EEPROM or boot storage.\n"
	"tlv_custom set <type_code> <string_value>\n"
	"    - Set a field to a value.\n"
	"    - If no string_value, field is deleted.\n"
	"    - Use 'tlv_custom write' to make changes permanent.\n"
	"tlv_custom erase\n"
	"    - Reset the in memory TLV data.\n"
	"    - Use 'tlv_custom read' to refresh the in memory TLV data.\n"
	"    - Use 'tlv_custom write' to make changes permanent.\n"
	"tlv_custom list\n"
	"    - List the understood TLV codes and names.\n"
);
