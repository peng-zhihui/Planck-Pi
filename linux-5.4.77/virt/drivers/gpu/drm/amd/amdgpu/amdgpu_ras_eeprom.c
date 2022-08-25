/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "amdgpu_ras_eeprom.h"
#include "amdgpu.h"
#include "amdgpu_ras.h"
#include <linux/bits.h>
#include "smu_v11_0_i2c.h"

#define EEPROM_I2C_TARGET_ADDR 0xA0

/*
 * The 2 macros bellow represent the actual size in bytes that
 * those entities occupy in the EEPROM memory.
 * EEPROM_TABLE_RECORD_SIZE is different than sizeof(eeprom_table_record) which
 * uses uint64 to store 6b fields such as retired_page.
 */
#define EEPROM_TABLE_HEADER_SIZE 20
#define EEPROM_TABLE_RECORD_SIZE 24

#define EEPROM_ADDRESS_SIZE 0x2

/* Table hdr is 'AMDR' */
#define EEPROM_TABLE_HDR_VAL 0x414d4452
#define EEPROM_TABLE_VER 0x00010000

/* Assume 2 Mbit size */
#define EEPROM_SIZE_BYTES 256000
#define EEPROM_PAGE__SIZE_BYTES 256
#define EEPROM_HDR_START 0
#define EEPROM_RECORD_START (EEPROM_HDR_START + EEPROM_TABLE_HEADER_SIZE)
#define EEPROM_MAX_RECORD_NUM ((EEPROM_SIZE_BYTES - EEPROM_TABLE_HEADER_SIZE) / EEPROM_TABLE_RECORD_SIZE)
#define EEPROM_ADDR_MSB_MASK GENMASK(17, 8)

#define to_amdgpu_device(x) (container_of(x, struct amdgpu_ras, eeprom_control))->adev

static void __encode_table_header_to_buff(struct amdgpu_ras_eeprom_table_header *hdr,
					  unsigned char *buff)
{
	uint32_t *pp = (uint32_t *) buff;

	pp[0] = cpu_to_le32(hdr->header);
	pp[1] = cpu_to_le32(hdr->version);
	pp[2] = cpu_to_le32(hdr->first_rec_offset);
	pp[3] = cpu_to_le32(hdr->tbl_size);
	pp[4] = cpu_to_le32(hdr->checksum);
}

static void __decode_table_header_from_buff(struct amdgpu_ras_eeprom_table_header *hdr,
					  unsigned char *buff)
{
	uint32_t *pp = (uint32_t *)buff;

	hdr->header 	      = le32_to_cpu(pp[0]);
	hdr->version 	      = le32_to_cpu(pp[1]);
	hdr->first_rec_offset = le32_to_cpu(pp[2]);
	hdr->tbl_size 	      = le32_to_cpu(pp[3]);
	hdr->checksum 	      = le32_to_cpu(pp[4]);
}

static int __update_table_header(struct amdgpu_ras_eeprom_control *control,
				 unsigned char *buff)
{
	int ret = 0;
	struct i2c_msg msg = {
			.addr	= EEPROM_I2C_TARGET_ADDR,
			.flags	= 0,
			.len	= EEPROM_ADDRESS_SIZE + EEPROM_TABLE_HEADER_SIZE,
			.buf	= buff,
	};


	*(uint16_t *)buff = EEPROM_HDR_START;
	__encode_table_header_to_buff(&control->tbl_hdr, buff + EEPROM_ADDRESS_SIZE);

	ret = i2c_transfer(&control->eeprom_accessor, &msg, 1);
	if (ret < 1)
		DRM_ERROR("Failed to write EEPROM table header, ret:%d", ret);

	return ret;
}

static uint32_t  __calc_hdr_byte_sum(struct amdgpu_ras_eeprom_control *control);

int amdgpu_ras_eeprom_init(struct amdgpu_ras_eeprom_control *control)
{
	int ret = 0;
	struct amdgpu_device *adev = to_amdgpu_device(control);
	unsigned char buff[EEPROM_ADDRESS_SIZE + EEPROM_TABLE_HEADER_SIZE] = { 0 };
	struct amdgpu_ras_eeprom_table_header *hdr = &control->tbl_hdr;
	struct i2c_msg msg = {
			.addr	= EEPROM_I2C_TARGET_ADDR,
			.flags	= I2C_M_RD,
			.len	= EEPROM_ADDRESS_SIZE + EEPROM_TABLE_HEADER_SIZE,
			.buf	= buff,
	};

	mutex_init(&control->tbl_mutex);

	switch (adev->asic_type) {
	case CHIP_VEGA20:
		ret = smu_v11_0_i2c_eeprom_control_init(&control->eeprom_accessor);
		break;

	default:
		return 0;
	}

	if (ret) {
		DRM_ERROR("Failed to init I2C controller, ret:%d", ret);
		return ret;
	}

	/* Read/Create table header from EEPROM address 0 */
	ret = i2c_transfer(&control->eeprom_accessor, &msg, 1);
	if (ret < 1) {
		DRM_ERROR("Failed to read EEPROM table header, ret:%d", ret);
		return ret;
	}

	__decode_table_header_from_buff(hdr, &buff[2]);

	if (hdr->header == EEPROM_TABLE_HDR_VAL) {
		control->num_recs = (hdr->tbl_size - EEPROM_TABLE_HEADER_SIZE) /
				    EEPROM_TABLE_RECORD_SIZE;
		DRM_DEBUG_DRIVER("Found existing EEPROM table with %d records",
				 control->num_recs);

	} else {
		DRM_INFO("Creating new EEPROM table");

		hdr->header = EEPROM_TABLE_HDR_VAL;
		hdr->version = EEPROM_TABLE_VER;
		hdr->first_rec_offset = EEPROM_RECORD_START;
		hdr->tbl_size = EEPROM_TABLE_HEADER_SIZE;

		adev->psp.ras.ras->eeprom_control.tbl_byte_sum =
				__calc_hdr_byte_sum(&adev->psp.ras.ras->eeprom_control);
		ret = __update_table_header(control, buff);
	}

	/* Start inserting records from here */
	adev->psp.ras.ras->eeprom_control.next_addr = EEPROM_RECORD_START;

	return ret == 1 ? 0 : -EIO;
}

void amdgpu_ras_eeprom_fini(struct amdgpu_ras_eeprom_control *control)
{
	struct amdgpu_device *adev = to_amdgpu_device(control);

	switch (adev->asic_type) {
	case CHIP_VEGA20:
		smu_v11_0_i2c_eeprom_control_fini(&control->eeprom_accessor);
		break;

	default:
		return;
	}
}

static void __encode_table_record_to_buff(struct amdgpu_ras_eeprom_control *control,
					  struct eeprom_table_record *record,
					  unsigned char *buff)
{
	__le64 tmp = 0;
	int i = 0;

	/* Next are all record fields according to EEPROM page spec in LE foramt */
	buff[i++] = record->err_type;

	buff[i++] = record->bank;

	tmp = cpu_to_le64(record->ts);
	memcpy(buff + i, &tmp, 8);
	i += 8;

	tmp = cpu_to_le64((record->offset & 0xffffffffffff));
	memcpy(buff + i, &tmp, 6);
	i += 6;

	buff[i++] = record->mem_channel;
	buff[i++] = record->mcumc_id;

	tmp = cpu_to_le64((record->retired_page & 0xffffffffffff));
	memcpy(buff + i, &tmp, 6);
}

static void __decode_table_record_from_buff(struct amdgpu_ras_eeprom_control *control,
					    struct eeprom_table_record *record,
					    unsigned char *buff)
{
	__le64 tmp = 0;
	int i =  0;

	/* Next are all record fields according to EEPROM page spec in LE foramt */
	record->err_type = buff[i++];

	record->bank = buff[i++];

	memcpy(&tmp, buff + i, 8);
	record->ts = le64_to_cpu(tmp);
	i += 8;

	memcpy(&tmp, buff + i, 6);
	record->offset = (le64_to_cpu(tmp) & 0xffffffffffff);
	i += 6;

	buff[i++] = record->mem_channel;
	buff[i++] = record->mcumc_id;

	memcpy(&tmp, buff + i,  6);
	record->retired_page = (le64_to_cpu(tmp) & 0xffffffffffff);
}

/*
 * When reaching end of EEPROM memory jump back to 0 record address
 * When next record access will go beyond EEPROM page boundary modify bits A17/A8
 * in I2C selector to go to next page
 */
static uint32_t __correct_eeprom_dest_address(uint32_t curr_address)
{
	uint32_t next_address = curr_address + EEPROM_TABLE_RECORD_SIZE;

	/* When all EEPROM memory used jump back to 0 address */
	if (next_address > EEPROM_SIZE_BYTES) {
		DRM_INFO("Reached end of EEPROM memory, jumping to 0 "
			 "and overriding old record");
		return EEPROM_RECORD_START;
	}

	/*
	 * To check if we overflow page boundary  compare next address with
	 * current and see if bits 17/8 of the EEPROM address will change
	 * If they do start from the next 256b page
	 *
	 * https://www.st.com/resource/en/datasheet/m24m02-dr.pdf sec. 5.1.2
	 */
	if ((curr_address & EEPROM_ADDR_MSB_MASK) != (next_address & EEPROM_ADDR_MSB_MASK)) {
		DRM_DEBUG_DRIVER("Reached end of EEPROM memory page, jumping to next: %lx",
				(next_address & EEPROM_ADDR_MSB_MASK));

		return  (next_address & EEPROM_ADDR_MSB_MASK);
	}

	return curr_address;
}


static uint32_t  __calc_hdr_byte_sum(struct amdgpu_ras_eeprom_control *control)
{
	int i;
	uint32_t tbl_sum = 0;

	/* Header checksum, skip checksum field in the calculation */
	for (i = 0; i < sizeof(control->tbl_hdr) - sizeof(control->tbl_hdr.checksum); i++)
		tbl_sum += *(((unsigned char *)&control->tbl_hdr) + i);

	return tbl_sum;
}

static uint32_t  __calc_recs_byte_sum(struct eeprom_table_record *records,
				      int num)
{
	int i, j;
	uint32_t tbl_sum = 0;

	/* Records checksum */
	for (i = 0; i < num; i++) {
		struct eeprom_table_record *record = &records[i];

		for (j = 0; j < sizeof(*record); j++) {
			tbl_sum += *(((unsigned char *)record) + j);
		}
	}

	return tbl_sum;
}

static inline uint32_t  __calc_tbl_byte_sum(struct amdgpu_ras_eeprom_control *control,
				  struct eeprom_table_record *records, int num)
{
	return __calc_hdr_byte_sum(control) + __calc_recs_byte_sum(records, num);
}

/* Checksum = 256 -((sum of all table entries) mod 256) */
static void __update_tbl_checksum(struct amdgpu_ras_eeprom_control *control,
				  struct eeprom_table_record *records, int num,
				  uint32_t old_hdr_byte_sum)
{
	/*
	 * This will update the table sum with new records.
	 *
	 * TODO: What happens when the EEPROM table is to be wrapped around
	 * and old records from start will get overridden.
	 */

	/* need to recalculate updated header byte sum */
	control->tbl_byte_sum -= old_hdr_byte_sum;
	control->tbl_byte_sum += __calc_tbl_byte_sum(control, records, num);

	control->tbl_hdr.checksum = 256 - (control->tbl_byte_sum % 256);
}

/* table sum mod 256 + checksum must equals 256 */
static bool __validate_tbl_checksum(struct amdgpu_ras_eeprom_control *control,
			    struct eeprom_table_record *records, int num)
{
	control->tbl_byte_sum = __calc_tbl_byte_sum(control, records, num);

	if (control->tbl_hdr.checksum + (control->tbl_byte_sum % 256) != 256) {
		DRM_WARN("Checksum mismatch, checksum: %u ", control->tbl_hdr.checksum);
		return false;
	}

	return true;
}

int amdgpu_ras_eeprom_process_recods(struct amdgpu_ras_eeprom_control *control,
					    struct eeprom_table_record *records,
					    bool write,
					    int num)
{
	int i, ret = 0;
	struct i2c_msg *msgs;
	unsigned char *buffs;
	struct amdgpu_device *adev = to_amdgpu_device(control);

	if (adev->asic_type != CHIP_VEGA20)
		return 0;

	buffs = kcalloc(num, EEPROM_ADDRESS_SIZE + EEPROM_TABLE_RECORD_SIZE,
			 GFP_KERNEL);
	if (!buffs)
		return -ENOMEM;

	mutex_lock(&control->tbl_mutex);

	msgs = kcalloc(num, sizeof(*msgs), GFP_KERNEL);
	if (!msgs) {
		ret = -ENOMEM;
		goto free_buff;
	}

	/* In case of overflow just start from beginning to not lose newest records */
	if (write && (control->next_addr + EEPROM_TABLE_RECORD_SIZE * num > EEPROM_SIZE_BYTES))
		control->next_addr = EEPROM_RECORD_START;


	/*
	 * TODO Currently makes EEPROM writes for each record, this creates
	 * internal fragmentation. Optimized the code to do full page write of
	 * 256b
	 */
	for (i = 0; i < num; i++) {
		unsigned char *buff = &buffs[i * (EEPROM_ADDRESS_SIZE + EEPROM_TABLE_RECORD_SIZE)];
		struct eeprom_table_record *record = &records[i];
		struct i2c_msg *msg = &msgs[i];

		control->next_addr = __correct_eeprom_dest_address(control->next_addr);

		/*
		 * Update bits 16,17 of EEPROM address in I2C address by setting them
		 * to bits 1,2 of Device address byte
		 */
		msg->addr = EEPROM_I2C_TARGET_ADDR |
			       ((control->next_addr & EEPROM_ADDR_MSB_MASK) >> 15);
		msg->flags	= write ? 0 : I2C_M_RD;
		msg->len	= EEPROM_ADDRESS_SIZE + EEPROM_TABLE_RECORD_SIZE;
		msg->buf	= buff;

		/* Insert the EEPROM dest addess, bits 0-15 */
		buff[0] = ((control->next_addr >> 8) & 0xff);
		buff[1] = (control->next_addr & 0xff);

		/* EEPROM table content is stored in LE format */
		if (write)
			__encode_table_record_to_buff(control, record, buff + EEPROM_ADDRESS_SIZE);

		/*
		 * The destination EEPROM address might need to be corrected to account
		 * for page or entire memory wrapping
		 */
		control->next_addr += EEPROM_TABLE_RECORD_SIZE;
	}

	ret = i2c_transfer(&control->eeprom_accessor, msgs, num);
	if (ret < 1) {
		DRM_ERROR("Failed to process EEPROM table records, ret:%d", ret);

		/* TODO Restore prev next EEPROM address ? */
		goto free_msgs;
	}


	if (!write) {
		for (i = 0; i < num; i++) {
			unsigned char *buff = &buffs[i*(EEPROM_ADDRESS_SIZE + EEPROM_TABLE_RECORD_SIZE)];
			struct eeprom_table_record *record = &records[i];

			__decode_table_record_from_buff(control, record, buff + EEPROM_ADDRESS_SIZE);
		}
	}

	if (write) {
		uint32_t old_hdr_byte_sum = __calc_hdr_byte_sum(control);

		/*
		 * Update table header with size and CRC and account for table
		 * wrap around where the assumption is that we treat it as empty
		 * table
		 *
		 * TODO - Check the assumption is correct
		 */
		control->num_recs += num;
		control->num_recs %= EEPROM_MAX_RECORD_NUM;
		control->tbl_hdr.tbl_size += EEPROM_TABLE_RECORD_SIZE * num;
		if (control->tbl_hdr.tbl_size > EEPROM_SIZE_BYTES)
			control->tbl_hdr.tbl_size = EEPROM_TABLE_HEADER_SIZE +
			control->num_recs * EEPROM_TABLE_RECORD_SIZE;

		__update_tbl_checksum(control, records, num, old_hdr_byte_sum);

		__update_table_header(control, buffs);
	} else if (!__validate_tbl_checksum(control, records, num)) {
		DRM_WARN("EEPROM Table checksum mismatch!");
		/* TODO Uncomment when EEPROM read/write is relliable */
		/* ret = -EIO; */
	}

free_msgs:
	kfree(msgs);

free_buff:
	kfree(buffs);

	mutex_unlock(&control->tbl_mutex);

	return ret == num ? 0 : -EIO;
}

/* Used for testing if bugs encountered */
#if 0
void amdgpu_ras_eeprom_test(struct amdgpu_ras_eeprom_control *control)
{
	int i;
	struct eeprom_table_record *recs = kcalloc(1, sizeof(*recs), GFP_KERNEL);

	if (!recs)
		return;

	for (i = 0; i < 1 ; i++) {
		recs[i].address = 0xdeadbeef;
		recs[i].retired_page = i;
	}

	if (!amdgpu_ras_eeprom_process_recods(control, recs, true, 1)) {

		memset(recs, 0, sizeof(*recs) * 1);

		control->next_addr = EEPROM_RECORD_START;

		if (!amdgpu_ras_eeprom_process_recods(control, recs, false, 1)) {
			for (i = 0; i < 1; i++)
				DRM_INFO("rec.address :0x%llx, rec.retired_page :%llu",
					 recs[i].address, recs[i].retired_page);
		} else
			DRM_ERROR("Failed in reading from table");

	} else
		DRM_ERROR("Failed in writing to table");
}
#endif
