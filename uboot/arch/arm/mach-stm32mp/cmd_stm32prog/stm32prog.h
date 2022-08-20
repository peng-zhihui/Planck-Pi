/* SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause */
/*
 * Copyright (C) 2020, STMicroelectronics - All Rights Reserved
 */

#ifndef _STM32PROG_H_
#define _STM32PROG_H_

/* - phase defines ------------------------------------------------*/
#define PHASE_FLASHLAYOUT	0x00
#define PHASE_FIRST_USER	0x10
#define PHASE_LAST_USER		0xF0
#define PHASE_CMD		0xF1
#define PHASE_OTP		0xF2
#define PHASE_PMIC		0xF4
#define PHASE_END		0xFE
#define PHASE_RESET		0xFF
#define PHASE_DO_RESET		0x1FF

#define DEFAULT_ADDRESS		0xFFFFFFFF

#define OTP_SIZE		1024
#define PMIC_SIZE		8

enum stm32prog_target {
	STM32PROG_NONE,
	STM32PROG_MMC,
	STM32PROG_NAND,
	STM32PROG_NOR,
	STM32PROG_SPI_NAND,
	STM32PROG_RAM
};

enum stm32prog_link_t {
	LINK_SERIAL,
	LINK_USB,
	LINK_UNDEFINED,
};

struct image_header_s {
	bool	present;
	u32	image_checksum;
	u32	image_length;
};

struct raw_header_s {
	u32 magic_number;
	u32 image_signature[64 / 4];
	u32 image_checksum;
	u32 header_version;
	u32 image_length;
	u32 image_entry_point;
	u32 reserved1;
	u32 load_address;
	u32 reserved2;
	u32 version_number;
	u32 option_flags;
	u32 ecdsa_algorithm;
	u32 ecdsa_public_key[64 / 4];
	u32 padding[83 / 4];
	u32 binary_type;
};

#define BL_HEADER_SIZE	sizeof(struct raw_header_s)

/* partition type in flashlayout file */
enum stm32prog_part_type {
	PART_BINARY,
	PART_SYSTEM,
	PART_FILESYSTEM,
	RAW_IMAGE
};

/* device information */
struct stm32prog_dev_t {
	enum stm32prog_target	target;
	char			dev_id;
	u32			erase_size;
	struct mmc		*mmc;
	struct mtd_info		*mtd;
	/* list of partition for this device / ordered in offset */
	struct list_head	part_list;
	bool			full_update;
};

/* partition information build from FlashLayout and device */
struct stm32prog_part_t {
	/* FlashLayout information */
	int			option;
	int			id;
	enum stm32prog_part_type part_type;
	enum stm32prog_target	target;
	char			dev_id;

	/* partition name
	 * (16 char in gpt, + 1 for null terminated string
	 */
	char			name[16 + 1];
	u64			addr;
	u64			size;
	enum stm32prog_part_type bin_nb;	/* SSBL repeatition */

	/* information on associated device */
	struct stm32prog_dev_t	*dev;		/* pointer to device */
	s16			part_id;	/* partition id in device */
	int			alt_id;		/* alt id in usb/dfu */

	struct list_head	list;
};

#define STM32PROG_MAX_DEV 5
struct stm32prog_data {
	/* Layout information */
	int			dev_nb;		/* device number*/
	struct stm32prog_dev_t	dev[STM32PROG_MAX_DEV];	/* array of device */
	int			part_nb;	/* nb of partition */
	struct stm32prog_part_t	*part_array;	/* array of partition */
	bool			tee_detected;
	bool			fsbl_nor_detected;

	/* command internal information */
	unsigned int		phase;
	u32			offset;
	char			error[255];
	struct stm32prog_part_t	*cur_part;
	u32			*otp_part;
	u8			pmic_part[PMIC_SIZE];

	/* STM32 header information */
	struct raw_header_s	*header_data;
	struct image_header_s	header;

	/* SERIAL information */
	u32	cursor;
	u32	packet_number;
	u32	checksum;
	u8	*buffer; /* size = USART_RAM_BUFFER_SIZE*/
	int	dfu_seq;
	u8	read_phase;

	/* bootm information */
	u32	uimage;
	u32	dtb;
};

extern struct stm32prog_data *stm32prog_data;

/* OTP access */
int stm32prog_otp_write(struct stm32prog_data *data, u32 offset,
			u8 *buffer, long *size);
int stm32prog_otp_read(struct stm32prog_data *data, u32 offset,
		       u8 *buffer, long *size);
int stm32prog_otp_start(struct stm32prog_data *data);

/* PMIC access */
int stm32prog_pmic_write(struct stm32prog_data *data, u32 offset,
			 u8 *buffer, long *size);
int stm32prog_pmic_read(struct stm32prog_data *data, u32 offset,
			u8 *buffer, long *size);
int stm32prog_pmic_start(struct stm32prog_data *data);

/* generic part*/
u8 stm32prog_header_check(struct raw_header_s *raw_header,
			  struct image_header_s *header);
int stm32prog_dfu_init(struct stm32prog_data *data);
void stm32prog_next_phase(struct stm32prog_data *data);
void stm32prog_do_reset(struct stm32prog_data *data);

char *stm32prog_get_error(struct stm32prog_data *data);

#define stm32prog_err(args...) {\
	if (data->phase != PHASE_RESET) { \
		sprintf(data->error, args); \
		data->phase = PHASE_RESET; \
		pr_err("Error: %s\n", data->error); } \
	}

/* Main function */
int stm32prog_init(struct stm32prog_data *data, ulong addr, ulong size);
int stm32prog_serial_init(struct stm32prog_data *data, int link_dev);
bool stm32prog_serial_loop(struct stm32prog_data *data);
bool stm32prog_usb_loop(struct stm32prog_data *data, int dev);
void stm32prog_clean(struct stm32prog_data *data);

#endif
