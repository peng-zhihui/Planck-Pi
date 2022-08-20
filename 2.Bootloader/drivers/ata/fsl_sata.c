// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2008,2010 Freescale Semiconductor, Inc.
 * Copyright 2019 NXP
 * Author: Dave Liu <daveliu@freescale.com>
 */

#include <common.h>
#include <blk.h>
#include <command.h>
#include <console.h>
#include <cpu_func.h>
#include <log.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/fsl_serdes.h>
#include <malloc.h>
#include <libata.h>
#include <fis.h>
#include <sata.h>
#include <linux/delay.h>
#include "fsl_sata.h"

#if CONFIG_IS_ENABLED(BLK)
#include <dm.h>
#include <ahci.h>
#include <blk.h>
#include <dm/device-internal.h>
#else
#ifndef CONFIG_SYS_SATA1_FLAGS
	#define CONFIG_SYS_SATA1_FLAGS	FLAGS_DMA
#endif
#ifndef CONFIG_SYS_SATA2_FLAGS
	#define CONFIG_SYS_SATA2_FLAGS	FLAGS_DMA
#endif

static struct fsl_sata_info fsl_sata_info[] = {
#ifdef CONFIG_SATA1
	{CONFIG_SYS_SATA1, CONFIG_SYS_SATA1_FLAGS},
#else
	{0, 0},
#endif
#ifdef CONFIG_SATA2
	{CONFIG_SYS_SATA2, CONFIG_SYS_SATA2_FLAGS},
#else
	{0, 0},
#endif
};
#endif

static inline void sdelay(unsigned long sec)
{
	unsigned long i;
	for (i = 0; i < sec; i++)
		mdelay(1000);
}

static void fsl_sata_dump_sfis(struct sata_fis_d2h *s)
{
	printf("Status FIS dump:\n\r");
	printf("fis_type:		%02x\n\r", s->fis_type);
	printf("pm_port_i:		%02x\n\r", s->pm_port_i);
	printf("status:			%02x\n\r", s->status);
	printf("error:			%02x\n\r", s->error);
	printf("lba_low:		%02x\n\r", s->lba_low);
	printf("lba_mid:		%02x\n\r", s->lba_mid);
	printf("lba_high:		%02x\n\r", s->lba_high);
	printf("device:			%02x\n\r", s->device);
	printf("lba_low_exp:		%02x\n\r", s->lba_low_exp);
	printf("lba_mid_exp:		%02x\n\r", s->lba_mid_exp);
	printf("lba_high_exp:		%02x\n\r", s->lba_high_exp);
	printf("res1:			%02x\n\r", s->res1);
	printf("sector_count:		%02x\n\r", s->sector_count);
	printf("sector_count_exp:	%02x\n\r", s->sector_count_exp);
}

static int ata_wait_register(unsigned __iomem *addr, u32 mask,
			 u32 val, u32 timeout_msec)
{
	int i;
	u32 temp;

	for (i = 0; (((temp = in_le32(addr)) & mask) != val)
			 && i < timeout_msec; i++)
		mdelay(1);
	return (i < timeout_msec) ? 0 : -1;
}

#if !CONFIG_IS_ENABLED(BLK)
int init_sata(int dev)
#else
static int init_sata(struct fsl_ata_priv *priv, int dev)
#endif
{
	u32 length, align;
	cmd_hdr_tbl_t *cmd_hdr;
	u32 cda;
	u32 val32;
	fsl_sata_reg_t __iomem *reg;
	u32 sig;
	int i;
	fsl_sata_t *sata;

	if (dev < 0 || dev > (CONFIG_SYS_SATA_MAX_DEVICE - 1)) {
		printf("the sata index %d is out of ranges\n\r", dev);
		return -1;
	}

#ifdef CONFIG_MPC85xx
	if ((dev == 0) && (!is_serdes_configured(SATA1))) {
		printf("SATA%d [dev = %d] is not enabled\n", dev+1, dev);
		return -1;
	}
	if ((dev == 1) && (!is_serdes_configured(SATA2))) {
		printf("SATA%d [dev = %d] is not enabled\n", dev+1, dev);
		return -1;
	}
#endif

	/* Allocate SATA device driver struct */
	sata = (fsl_sata_t *)malloc(sizeof(fsl_sata_t));
	if (!sata) {
		printf("alloc the sata device struct failed\n\r");
		return -1;
	}
	/* Zero all of the device driver struct */
	memset((void *)sata, 0, sizeof(fsl_sata_t));

	snprintf(sata->name, 12, "SATA%d:", dev);

	/* Set the controller register base address to device struct */
#if !CONFIG_IS_ENABLED(BLK)
	sata_dev_desc[dev].priv = (void *)sata;
	reg = (fsl_sata_reg_t *)(fsl_sata_info[dev].sata_reg_base);
	sata->dma_flag = fsl_sata_info[dev].flags;
#else
	reg = (fsl_sata_reg_t *)(priv->base + priv->offset * dev);
	sata->dma_flag = priv->flag;
	priv->fsl_sata = sata;
#endif
	sata->reg_base = reg;

	/* Allocate the command header table, 4 bytes aligned */
	length = sizeof(struct cmd_hdr_tbl);
	align = SATA_HC_CMD_HDR_TBL_ALIGN;
	sata->cmd_hdr_tbl_offset = (void *)malloc(length + align);
	if (!sata->cmd_hdr_tbl_offset) {
		printf("alloc the command header failed\n\r");
		return -1;
	}

	cmd_hdr = (cmd_hdr_tbl_t *)(((u32)sata->cmd_hdr_tbl_offset + align)
						& ~(align - 1));
	sata->cmd_hdr = cmd_hdr;

	/* Zero all of the command header table */
	memset((void *)sata->cmd_hdr_tbl_offset, 0, length + align);

	/* Allocate command descriptor for all command */
	length = sizeof(struct cmd_desc) * SATA_HC_MAX_CMD;
	align = SATA_HC_CMD_DESC_ALIGN;
	sata->cmd_desc_offset = (void *)malloc(length + align);
	if (!sata->cmd_desc_offset) {
		printf("alloc the command descriptor failed\n\r");
		return -1;
	}
	sata->cmd_desc = (cmd_desc_t *)(((u32)sata->cmd_desc_offset + align)
						& ~(align - 1));
	/* Zero all of command descriptor */
	memset((void *)sata->cmd_desc_offset, 0, length + align);

	/* Link the command descriptor to command header */
	for (i = 0; i < SATA_HC_MAX_CMD; i++) {
		cda = ((u32)sata->cmd_desc + SATA_HC_CMD_DESC_SIZE * i)
					 & ~(CMD_HDR_CDA_ALIGN - 1);
		cmd_hdr->cmd_slot[i].cda = cpu_to_le32(cda);
	}

	/* To have safe state, force the controller offline */
	val32 = in_le32(&reg->hcontrol);
	val32 &= ~HCONTROL_ONOFF;
	val32 |= HCONTROL_FORCE_OFFLINE;
	out_le32(&reg->hcontrol, val32);

	/* Wait the controller offline */
	ata_wait_register(&reg->hstatus, HSTATUS_ONOFF, 0, 1000);

	/* Set the command header base address to CHBA register to tell DMA */
	out_le32(&reg->chba, (u32)cmd_hdr & ~0x3);

	/* Snoop for the command header */
	val32 = in_le32(&reg->hcontrol);
	val32 |= HCONTROL_HDR_SNOOP;
	out_le32(&reg->hcontrol, val32);

	/* Disable all of interrupts */
	val32 = in_le32(&reg->hcontrol);
	val32 &= ~HCONTROL_INT_EN_ALL;
	out_le32(&reg->hcontrol, val32);

	/* Clear all of interrupts */
	val32 = in_le32(&reg->hstatus);
	out_le32(&reg->hstatus, val32);

	/* Set the ICC, no interrupt coalescing */
	out_le32(&reg->icc, 0x01000000);

	/* No PM attatched, the SATA device direct connect */
	out_le32(&reg->cqpmp, 0);

	/* Clear SError register */
	val32 = in_le32(&reg->serror);
	out_le32(&reg->serror, val32);

	/* Clear CER register */
	val32 = in_le32(&reg->cer);
	out_le32(&reg->cer, val32);

	/* Clear DER register */
	val32 = in_le32(&reg->der);
	out_le32(&reg->der, val32);

	/* No device detection or initialization action requested */
	out_le32(&reg->scontrol, 0x00000300);

	/* Configure the transport layer, default value */
	out_le32(&reg->transcfg, 0x08000016);

	/* Configure the link layer, default value */
	out_le32(&reg->linkcfg, 0x0000ff34);

	/* Bring the controller online */
	val32 = in_le32(&reg->hcontrol);
	val32 |= HCONTROL_ONOFF;
	out_le32(&reg->hcontrol, val32);

	mdelay(100);

	/* print sata device name */
	printf("%s ", sata->name);

	/* Wait PHY RDY signal changed for 500ms */
	ata_wait_register(&reg->hstatus, HSTATUS_PHY_RDY,
			  HSTATUS_PHY_RDY, 500);

	/* Check PHYRDY */
	val32 = in_le32(&reg->hstatus);
	if (val32 & HSTATUS_PHY_RDY) {
		sata->link = 1;
	} else {
		sata->link = 0;
		printf("(No RDY)\n\r");
		return -1;
	}

	/* Wait for signature updated, which is 1st D2H */
	ata_wait_register(&reg->hstatus, HSTATUS_SIGNATURE,
			  HSTATUS_SIGNATURE, 10000);

	if (val32 & HSTATUS_SIGNATURE) {
		sig = in_le32(&reg->sig);
		debug("Signature updated, the sig =%08x\n\r", sig);
		sata->ata_device_type = ata_dev_classify(sig);
	}

	/* Check the speed */
	val32 = in_le32(&reg->sstatus);
	if ((val32 & SSTATUS_SPD_MASK) == SSTATUS_SPD_GEN1)
		printf("(1.5 Gbps)\n\r");
	else if ((val32 & SSTATUS_SPD_MASK) == SSTATUS_SPD_GEN2)
		printf("(3 Gbps)\n\r");

	return 0;
}

int reset_sata(int dev)
{
	return 0;
}

static void fsl_sata_dump_regs(fsl_sata_reg_t __iomem *reg)
{
	printf("\n\rSATA:           %08x\n\r", (u32)reg);
	printf("CQR:            %08x\n\r", in_le32(&reg->cqr));
	printf("CAR:            %08x\n\r", in_le32(&reg->car));
	printf("CCR:            %08x\n\r", in_le32(&reg->ccr));
	printf("CER:            %08x\n\r", in_le32(&reg->cer));
	printf("CQR:            %08x\n\r", in_le32(&reg->cqr));
	printf("DER:            %08x\n\r", in_le32(&reg->der));
	printf("CHBA:           %08x\n\r", in_le32(&reg->chba));
	printf("HStatus:        %08x\n\r", in_le32(&reg->hstatus));
	printf("HControl:       %08x\n\r", in_le32(&reg->hcontrol));
	printf("CQPMP:          %08x\n\r", in_le32(&reg->cqpmp));
	printf("SIG:            %08x\n\r", in_le32(&reg->sig));
	printf("ICC:            %08x\n\r", in_le32(&reg->icc));
	printf("SStatus:        %08x\n\r", in_le32(&reg->sstatus));
	printf("SError:         %08x\n\r", in_le32(&reg->serror));
	printf("SControl:       %08x\n\r", in_le32(&reg->scontrol));
	printf("SNotification:  %08x\n\r", in_le32(&reg->snotification));
	printf("TransCfg:       %08x\n\r", in_le32(&reg->transcfg));
	printf("TransStatus:    %08x\n\r", in_le32(&reg->transstatus));
	printf("LinkCfg:        %08x\n\r", in_le32(&reg->linkcfg));
	printf("LinkCfg1:       %08x\n\r", in_le32(&reg->linkcfg1));
	printf("LinkCfg2:       %08x\n\r", in_le32(&reg->linkcfg2));
	printf("LinkStatus:     %08x\n\r", in_le32(&reg->linkstatus));
	printf("LinkStatus1:    %08x\n\r", in_le32(&reg->linkstatus1));
	printf("PhyCtrlCfg:     %08x\n\r", in_le32(&reg->phyctrlcfg));
	printf("SYSPR:          %08x\n\r", in_be32(&reg->syspr));
}

static int fsl_ata_exec_ata_cmd(struct fsl_sata *sata, struct sata_fis_h2d *cfis,
				int is_ncq, int tag, u8 *buffer, u32 len)
{
	cmd_hdr_entry_t *cmd_hdr;
	cmd_desc_t *cmd_desc;
	sata_fis_h2d_t *h2d;
	prd_entry_t *prde;
	u32 ext_c_ddc;
	u32 prde_count;
	u32 val32;
	u32 ttl;
	fsl_sata_reg_t __iomem *reg = sata->reg_base;
	int i;

	/* Check xfer length */
	if (len > SATA_HC_MAX_XFER_LEN) {
		printf("max transfer length is 64MB\n\r");
		return 0;
	}

	/* Setup the command descriptor */
	cmd_desc = sata->cmd_desc + tag;

	/* Get the pointer cfis of command descriptor */
	h2d = (sata_fis_h2d_t *)cmd_desc->cfis;

	/* Zero the cfis of command descriptor */
	memset((void *)h2d, 0, SATA_HC_CMD_DESC_CFIS_SIZE);

	/* Copy the cfis from user to command descriptor */
	h2d->fis_type = cfis->fis_type;
	h2d->pm_port_c = cfis->pm_port_c;
	h2d->command = cfis->command;

	h2d->features = cfis->features;
	h2d->features_exp = cfis->features_exp;

	h2d->lba_low = cfis->lba_low;
	h2d->lba_mid = cfis->lba_mid;
	h2d->lba_high = cfis->lba_high;
	h2d->lba_low_exp = cfis->lba_low_exp;
	h2d->lba_mid_exp = cfis->lba_mid_exp;
	h2d->lba_high_exp = cfis->lba_high_exp;

	if (!is_ncq) {
		h2d->sector_count = cfis->sector_count;
		h2d->sector_count_exp = cfis->sector_count_exp;
	} else { /* NCQ */
		h2d->sector_count = (u8)(tag << 3);
	}

	h2d->device = cfis->device;
	h2d->control = cfis->control;

	/* Setup the PRD table */
	prde = (prd_entry_t *)cmd_desc->prdt;
	memset((void *)prde, 0, sizeof(struct prdt));

	prde_count = 0;
	ttl = len;
	for (i = 0; i < SATA_HC_MAX_PRD_DIRECT; i++) {
		if (!len)
			break;
		prde->dba = cpu_to_le32((u32)buffer & ~0x3);
		debug("dba = %08x\n\r", (u32)buffer);

		if (len < PRD_ENTRY_MAX_XFER_SZ) {
			ext_c_ddc = PRD_ENTRY_DATA_SNOOP | len;
			debug("ext_c_ddc1 = %08x, len = %08x\n\r", ext_c_ddc, len);
			prde->ext_c_ddc = cpu_to_le32(ext_c_ddc);
			prde_count++;
			prde++;
			break;
		} else {
			ext_c_ddc = PRD_ENTRY_DATA_SNOOP; /* 4M bytes */
			debug("ext_c_ddc2 = %08x, len = %08x\n\r", ext_c_ddc, len);
			prde->ext_c_ddc = cpu_to_le32(ext_c_ddc);
			buffer += PRD_ENTRY_MAX_XFER_SZ;
			len -= PRD_ENTRY_MAX_XFER_SZ;
			prde_count++;
			prde++;
		}
	}

	/* Setup the command slot of cmd hdr */
	cmd_hdr = (cmd_hdr_entry_t *)&sata->cmd_hdr->cmd_slot[tag];

	cmd_hdr->cda = cpu_to_le32((u32)cmd_desc & ~0x3);

	val32 = prde_count << CMD_HDR_PRD_ENTRY_SHIFT;
	val32 |= sizeof(sata_fis_h2d_t);
	cmd_hdr->prde_fis_len = cpu_to_le32(val32);

	cmd_hdr->ttl = cpu_to_le32(ttl);

	if (!is_ncq) {
		val32 = CMD_HDR_ATTR_RES | CMD_HDR_ATTR_SNOOP;
	} else {
		val32 = CMD_HDR_ATTR_RES | CMD_HDR_ATTR_SNOOP | CMD_HDR_ATTR_FPDMA;
	}

	tag &= CMD_HDR_ATTR_TAG;
	val32 |= tag;

	debug("attribute = %08x\n\r", val32);
	cmd_hdr->attribute = cpu_to_le32(val32);

	/* Make sure cmd desc and cmd slot valid before command issue */
	sync();

	/* PMP*/
	val32 = (u32)(h2d->pm_port_c & 0x0f);
	out_le32(&reg->cqpmp, val32);

	/* Wait no active */
	if (ata_wait_register(&reg->car, (1 << tag), 0, 10000))
		printf("Wait no active time out\n\r");

	/* Issue command */
	if (!(in_le32(&reg->cqr) & (1 << tag))) {
		val32 = 1 << tag;
		out_le32(&reg->cqr, val32);
	}

	/* Wait command completed for 10s */
	if (ata_wait_register(&reg->ccr, (1 << tag), (1 << tag), 10000)) {
		if (!is_ncq)
			printf("Non-NCQ command time out\n\r");
		else
			printf("NCQ command time out\n\r");
	}

	val32 = in_le32(&reg->cer);

	if (val32) {
		u32 der;
		fsl_sata_dump_sfis((struct sata_fis_d2h *)cmd_desc->sfis);
		printf("CE at device\n\r");
		fsl_sata_dump_regs(reg);
		der = in_le32(&reg->der);
		out_le32(&reg->cer, val32);
		out_le32(&reg->der, der);
	}

	/* Clear complete flags */
	val32 = in_le32(&reg->ccr);
	out_le32(&reg->ccr, val32);

	return len;
}

static int fsl_ata_exec_reset_cmd(struct fsl_sata *sata, struct sata_fis_h2d *cfis,
				 int tag, u8 *buffer, u32 len)
{
	return 0;
}

static int fsl_sata_exec_cmd(struct fsl_sata *sata, struct sata_fis_h2d *cfis,
		 enum cmd_type command_type, int tag, u8 *buffer, u32 len)
{
	int rc;

	if (tag > SATA_HC_MAX_CMD || tag < 0) {
		printf("tag is out of range, tag=%d\n\r", tag);
		return -1;
	}

	switch (command_type) {
	case CMD_ATA:
		rc = fsl_ata_exec_ata_cmd(sata, cfis, 0, tag, buffer, len);
		return rc;
	case CMD_RESET:
		rc = fsl_ata_exec_reset_cmd(sata, cfis, tag, buffer, len);
		return rc;
	case CMD_NCQ:
		rc = fsl_ata_exec_ata_cmd(sata, cfis, 1, tag, buffer, len);
		return rc;
	case CMD_ATAPI:
	case CMD_VENDOR_BIST:
	case CMD_BIST:
		printf("not support now\n\r");
		return -1;
	default:
		break;
	}

	return -1;
}

static void fsl_sata_xfer_mode(fsl_sata_t *sata, u16 *id)
{
	sata->pio = id[ATA_ID_PIO_MODES];
	sata->mwdma = id[ATA_ID_MWDMA_MODES];
	sata->udma = id[ATA_ID_UDMA_MODES];
	debug("pio %04x, mwdma %04x, udma %04x\n\r", sata->pio, sata->mwdma, sata->udma);
}

static void fsl_sata_set_features(fsl_sata_t *sata)
{
	struct sata_fis_h2d h2d, *cfis = &h2d;
	u8 udma_cap;

	memset(cfis, 0, sizeof(struct sata_fis_h2d));

	cfis->fis_type = SATA_FIS_TYPE_REGISTER_H2D;
	cfis->pm_port_c = 0x80; /* is command */
	cfis->command = ATA_CMD_SET_FEATURES;
	cfis->features = SETFEATURES_XFER;

	/* First check the device capablity */
	udma_cap = (u8)(sata->udma & 0xff);
	debug("udma_cap %02x\n\r", udma_cap);

	if (udma_cap == ATA_UDMA6)
		cfis->sector_count = XFER_UDMA_6;
	if (udma_cap == ATA_UDMA5)
		cfis->sector_count = XFER_UDMA_5;
	if (udma_cap == ATA_UDMA4)
		cfis->sector_count = XFER_UDMA_4;
	if (udma_cap == ATA_UDMA3)
		cfis->sector_count = XFER_UDMA_3;

	fsl_sata_exec_cmd(sata, cfis, CMD_ATA, 0, NULL, 0);
}

static u32 fsl_sata_rw_cmd(fsl_sata_t *sata, u32 start, u32 blkcnt, u8 *buffer,
			   int is_write)
{
	struct sata_fis_h2d h2d, *cfis = &h2d;
	u32 block;

	block = start;

	memset(cfis, 0, sizeof(struct sata_fis_h2d));

	cfis->fis_type = SATA_FIS_TYPE_REGISTER_H2D;
	cfis->pm_port_c = 0x80; /* is command */
	cfis->command = (is_write) ? ATA_CMD_WRITE : ATA_CMD_READ;
	cfis->device = ATA_LBA;

	cfis->device |= (block >> 24) & 0xf;
	cfis->lba_high = (block >> 16) & 0xff;
	cfis->lba_mid = (block >> 8) & 0xff;
	cfis->lba_low = block & 0xff;
	cfis->sector_count = (u8)(blkcnt & 0xff);

	fsl_sata_exec_cmd(sata, cfis, CMD_ATA, 0, buffer, ATA_SECT_SIZE * blkcnt);
	return blkcnt;
}

static void fsl_sata_flush_cache(fsl_sata_t *sata)
{
	struct sata_fis_h2d h2d, *cfis = &h2d;

	memset(cfis, 0, sizeof(struct sata_fis_h2d));

	cfis->fis_type = SATA_FIS_TYPE_REGISTER_H2D;
	cfis->pm_port_c = 0x80; /* is command */
	cfis->command = ATA_CMD_FLUSH;

	fsl_sata_exec_cmd(sata, cfis, CMD_ATA, 0, NULL, 0);
}

static u32 fsl_sata_rw_cmd_ext(fsl_sata_t *sata, u32 start, u32 blkcnt,
			       u8 *buffer, int is_write)
{
	struct sata_fis_h2d h2d, *cfis = &h2d;
	u64 block;

	block = (u64)start;

	memset(cfis, 0, sizeof(struct sata_fis_h2d));

	cfis->fis_type = SATA_FIS_TYPE_REGISTER_H2D;
	cfis->pm_port_c = 0x80; /* is command */

	cfis->command = (is_write) ? ATA_CMD_WRITE_EXT
				 : ATA_CMD_READ_EXT;

	cfis->lba_high_exp = (block >> 40) & 0xff;
	cfis->lba_mid_exp = (block >> 32) & 0xff;
	cfis->lba_low_exp = (block >> 24) & 0xff;
	cfis->lba_high = (block >> 16) & 0xff;
	cfis->lba_mid = (block >> 8) & 0xff;
	cfis->lba_low = block & 0xff;
	cfis->device = ATA_LBA;
	cfis->sector_count_exp = (blkcnt >> 8) & 0xff;
	cfis->sector_count = blkcnt & 0xff;

	fsl_sata_exec_cmd(sata, cfis, CMD_ATA, 0, buffer, ATA_SECT_SIZE * blkcnt);
	return blkcnt;
}

static u32 fsl_sata_rw_ncq_cmd(fsl_sata_t *sata, u32 start, u32 blkcnt,
			       u8 *buffer, int is_write)
{
	struct sata_fis_h2d h2d, *cfis = &h2d;
	int ncq_channel;
	u64 block;

	if (sata->lba48 != 1) {
		printf("execute FPDMA command on non-LBA48 hard disk\n\r");
		return -1;
	}

	block = (u64)start;

	memset(cfis, 0, sizeof(struct sata_fis_h2d));

	cfis->fis_type = SATA_FIS_TYPE_REGISTER_H2D;
	cfis->pm_port_c = 0x80; /* is command */

	cfis->command = (is_write) ? ATA_CMD_FPDMA_WRITE
				 : ATA_CMD_FPDMA_READ;

	cfis->lba_high_exp = (block >> 40) & 0xff;
	cfis->lba_mid_exp = (block >> 32) & 0xff;
	cfis->lba_low_exp = (block >> 24) & 0xff;
	cfis->lba_high = (block >> 16) & 0xff;
	cfis->lba_mid = (block >> 8) & 0xff;
	cfis->lba_low = block & 0xff;

	cfis->device = ATA_LBA;
	cfis->features_exp = (blkcnt >> 8) & 0xff;
	cfis->features = blkcnt & 0xff;

	if (sata->queue_depth >= SATA_HC_MAX_CMD)
		ncq_channel = SATA_HC_MAX_CMD - 1;
	else
		ncq_channel = sata->queue_depth - 1;

	/* Use the latest queue */
	fsl_sata_exec_cmd(sata, cfis, CMD_NCQ, ncq_channel, buffer, ATA_SECT_SIZE * blkcnt);
	return blkcnt;
}

static void fsl_sata_flush_cache_ext(fsl_sata_t *sata)
{
	struct sata_fis_h2d h2d, *cfis = &h2d;

	memset(cfis, 0, sizeof(struct sata_fis_h2d));

	cfis->fis_type = SATA_FIS_TYPE_REGISTER_H2D;
	cfis->pm_port_c = 0x80; /* is command */
	cfis->command = ATA_CMD_FLUSH_EXT;

	fsl_sata_exec_cmd(sata, cfis, CMD_ATA, 0, NULL, 0);
}

static void fsl_sata_init_wcache(fsl_sata_t *sata, u16 *id)
{
	if (ata_id_has_wcache(id) && ata_id_wcache_enabled(id))
		sata->wcache = 1;
	if (ata_id_has_flush(id))
		sata->flush = 1;
	if (ata_id_has_flush_ext(id))
		sata->flush_ext = 1;
}

static u32 ata_low_level_rw_lba48(fsl_sata_t *sata, u32 blknr, lbaint_t blkcnt,
				  const void *buffer, int is_write)
{
	u32 start, blks;
	u8 *addr;
	int max_blks;

	start = blknr;
	blks = blkcnt;
	addr = (u8 *)buffer;

	max_blks = ATA_MAX_SECTORS_LBA48;
	do {
		if (blks > max_blks) {
			if (sata->dma_flag != FLAGS_FPDMA)
				fsl_sata_rw_cmd_ext(sata, start, max_blks, addr,
						    is_write);
			else
				fsl_sata_rw_ncq_cmd(sata, start, max_blks, addr,
						    is_write);
			start += max_blks;
			blks -= max_blks;
			addr += ATA_SECT_SIZE * max_blks;
		} else {
			if (sata->dma_flag != FLAGS_FPDMA)
				fsl_sata_rw_cmd_ext(sata, start, blks, addr,
						    is_write);
			else
				fsl_sata_rw_ncq_cmd(sata, start, blks, addr,
						    is_write);
			start += blks;
			blks = 0;
			addr += ATA_SECT_SIZE * blks;
		}
	} while (blks != 0);

	return blkcnt;
}

static u32 ata_low_level_rw_lba28(fsl_sata_t *sata, u32 blknr, u32 blkcnt,
				  const void *buffer, int is_write)
{
	u32 start, blks;
	u8 *addr;
	int max_blks;

	start = blknr;
	blks = blkcnt;
	addr = (u8 *)buffer;

	max_blks = ATA_MAX_SECTORS;
	do {
		if (blks > max_blks) {
			fsl_sata_rw_cmd(sata, start, max_blks, addr, is_write);
			start += max_blks;
			blks -= max_blks;
			addr += ATA_SECT_SIZE * max_blks;
		} else {
			fsl_sata_rw_cmd(sata, start, blks, addr, is_write);
			start += blks;
			blks = 0;
			addr += ATA_SECT_SIZE * blks;
		}
	} while (blks != 0);

	return blkcnt;
}

/*
 * SATA interface between low level driver and command layer
 */
#if !CONFIG_IS_ENABLED(BLK)
ulong sata_read(int dev, ulong blknr, lbaint_t blkcnt, void *buffer)
{
	fsl_sata_t *sata = (fsl_sata_t *)sata_dev_desc[dev].priv;
#else
static ulong sata_read(struct udevice *dev, lbaint_t blknr, lbaint_t blkcnt,
		       void *buffer)
{
	struct fsl_ata_priv *priv = dev_get_platdata(dev);
	fsl_sata_t *sata = priv->fsl_sata;
#endif
	u32 rc;

	if (sata->lba48)
		rc = ata_low_level_rw_lba48(sata, blknr, blkcnt, buffer,
					    READ_CMD);
	else
		rc = ata_low_level_rw_lba28(sata, blknr, blkcnt, buffer,
					    READ_CMD);
	return rc;
}

#if !CONFIG_IS_ENABLED(BLK)
ulong sata_write(int dev, ulong blknr, lbaint_t blkcnt, const void *buffer)
{
	fsl_sata_t *sata = (fsl_sata_t *)sata_dev_desc[dev].priv;
#else
static ulong sata_write(struct udevice *dev, lbaint_t blknr, lbaint_t blkcnt,
			const void *buffer)
{
	struct fsl_ata_priv *priv = dev_get_platdata(dev);
	fsl_sata_t *sata = priv->fsl_sata;
#endif
	u32 rc;

	if (sata->lba48) {
		rc = ata_low_level_rw_lba48(sata, blknr, blkcnt, buffer,
					    WRITE_CMD);
		if (sata->wcache && sata->flush_ext)
			fsl_sata_flush_cache_ext(sata);
	} else {
		rc = ata_low_level_rw_lba28(sata, blknr, blkcnt, buffer,
					    WRITE_CMD);
		if (sata->wcache && sata->flush)
			fsl_sata_flush_cache(sata);
	}
	return rc;
}

static void fsl_sata_identify(fsl_sata_t *sata, u16 *id)
{
	struct sata_fis_h2d h2d, *cfis = &h2d;

	memset(cfis, 0, sizeof(struct sata_fis_h2d));

	cfis->fis_type = SATA_FIS_TYPE_REGISTER_H2D;
	cfis->pm_port_c = 0x80; /* is command */
	cfis->command = ATA_CMD_ID_ATA;

	fsl_sata_exec_cmd(sata, cfis, CMD_ATA, 0, (u8 *)id, ATA_ID_WORDS * 2);
	ata_swap_buf_le16(id, ATA_ID_WORDS);
}

#if !CONFIG_IS_ENABLED(BLK)
int scan_sata(int dev)
{
	fsl_sata_t *sata = (fsl_sata_t *)sata_dev_desc[dev].priv;
#else
static int scan_sata(struct udevice *dev)
{
	struct blk_desc *desc = dev_get_uclass_platdata(dev);
	struct fsl_ata_priv *priv = dev_get_platdata(dev);
	fsl_sata_t *sata = priv->fsl_sata;
#endif

	unsigned char serial[ATA_ID_SERNO_LEN + 1];
	unsigned char firmware[ATA_ID_FW_REV_LEN + 1];
	unsigned char product[ATA_ID_PROD_LEN + 1];
	u16 *id;
	u64 n_sectors;

	/* if no detected link */
	if (!sata->link)
		return -1;

	id = (u16 *)malloc(ATA_ID_WORDS * 2);
	if (!id) {
		printf("id malloc failed\n\r");
		return -1;
	}

	/* Identify device to get information */
	fsl_sata_identify(sata, id);

	/* Serial number */
	ata_id_c_string(id, serial, ATA_ID_SERNO, sizeof(serial));

	/* Firmware version */
	ata_id_c_string(id, firmware, ATA_ID_FW_REV, sizeof(firmware));

	/* Product model */
	ata_id_c_string(id, product, ATA_ID_PROD, sizeof(product));

	/* Totoal sectors */
	n_sectors = ata_id_n_sectors(id);

#ifdef CONFIG_LBA48
	/* Check if support LBA48 */
	if (ata_id_has_lba48(id)) {
		sata->lba48 = 1;
		debug("Device support LBA48\n\r");
	} else
		debug("Device supports LBA28\n\r");
#endif

#if !CONFIG_IS_ENABLED(BLK)
	memcpy(sata_dev_desc[dev].product, serial, sizeof(serial));
	memcpy(sata_dev_desc[dev].revision, firmware, sizeof(firmware));
	memcpy(sata_dev_desc[dev].vendor, product, sizeof(product));
	sata_dev_desc[dev].lba = (u32)n_sectors;
#ifdef CONFIG_LBA48
	sata_dev_desc[dev].lba48 = sata->lba48;
#endif
#else
	memcpy(desc->product, serial, sizeof(serial));
	memcpy(desc->revision, firmware, sizeof(firmware));
	memcpy(desc->vendor, product, sizeof(product));
	desc->lba = n_sectors;
#ifdef CONFIG_LBA48
	desc->lba48 = sata->lba48;
#endif
#endif

	/* Get the NCQ queue depth from device */
	sata->queue_depth = ata_id_queue_depth(id);

	/* Get the xfer mode from device */
	fsl_sata_xfer_mode(sata, id);

	/* Get the write cache status from device */
	fsl_sata_init_wcache(sata, id);

	/* Set the xfer mode to highest speed */
	fsl_sata_set_features(sata);

#ifdef DEBUG
	ata_dump_id(id);
#endif
	free((void *)id);
	return 0;
}

#if CONFIG_IS_ENABLED(BLK)
static const struct blk_ops sata_fsl_blk_ops = {
	.read	= sata_read,
	.write	= sata_write,
};

U_BOOT_DRIVER(sata_fsl_driver) = {
	.name = "sata_fsl_blk",
	.id = UCLASS_BLK,
	.ops = &sata_fsl_blk_ops,
	.platdata_auto_alloc_size = sizeof(struct fsl_ata_priv),
};

static int fsl_ata_ofdata_to_platdata(struct udevice *dev)
{
	struct fsl_ata_priv *priv = dev_get_priv(dev);

	priv->number = dev_read_u32_default(dev, "sata-number", -1);
	priv->flag = dev_read_u32_default(dev, "sata-fpdma", -1);
	priv->offset = dev_read_u32_default(dev, "sata-offset", -1);

	priv->base = dev_read_addr(dev);
	if (priv->base == FDT_ADDR_T_NONE)
		return -EINVAL;

	return 0;
}

static int fsl_unbind_device(struct udevice *dev)
{
	int ret;

	ret = device_remove(dev, DM_REMOVE_NORMAL);
	if (ret)
		return ret;

	ret = device_unbind(dev);
	if (ret)
		return ret;

	return 0;
}

static int fsl_ata_probe(struct udevice *dev)
{
	struct fsl_ata_priv *blk_priv, *priv;
	struct udevice *blk;
	int failed_number;
	char sata_name[10];
	int nr_ports;
	int ret;
	int i;

	failed_number = 0;
	priv = dev_get_priv(dev);
	nr_ports = priv->number;
	nr_ports = min(nr_ports, CONFIG_SYS_SATA_MAX_DEVICE);

	for (i = 0; i < nr_ports; i++) {
		snprintf(sata_name, sizeof(sata_name), "fsl_sata%d", i);
		ret = blk_create_devicef(dev, "sata_fsl_blk", sata_name,
					 IF_TYPE_SATA, -1, 512, 0, &blk);
		if (ret) {
			debug("Can't create device\n");
			return ret;
		}

		/* Init SATA port */
		ret = init_sata(priv, i);
		if (ret) {
			debug("%s: Failed to init sata\n", __func__);
			ret = fsl_unbind_device(blk);
			if (ret)
				return ret;

			failed_number++;
			continue;
		}

		blk_priv = dev_get_platdata(blk);
		blk_priv->fsl_sata = priv->fsl_sata;
		/* Scan SATA port */
		ret = scan_sata(blk);
		if (ret) {
			debug("%s: Failed to scan bus\n", __func__);
			ret = fsl_unbind_device(blk);
			if (ret)
				return ret;

			failed_number++;
			continue;
		}
	}

	if (failed_number == nr_ports)
		return -ENODEV;
	else
		return 0;
}

static int fsl_ata_remove(struct udevice *dev)
{
	fsl_sata_t *sata;
	struct fsl_ata_priv *priv;

	priv = dev_get_priv(dev);
	sata = priv->fsl_sata;

	free(sata->cmd_hdr_tbl_offset);
	free(sata->cmd_desc_offset);
	free(sata);

	return 0;
}

static int sata_fsl_scan(struct udevice *dev)
{
	/* Nothing to do here */

	return 0;
}

struct ahci_ops sata_fsl_ahci_ops = {
	.scan = sata_fsl_scan,
};

static const struct udevice_id fsl_ata_ids[] = {
	{ .compatible = "fsl,pq-sata-v2" },
	{ }
};

U_BOOT_DRIVER(fsl_ahci) = {
	.name	= "fsl_ahci",
	.id = UCLASS_AHCI,
	.of_match = fsl_ata_ids,
	.ops = &sata_fsl_ahci_ops,
	.ofdata_to_platdata = fsl_ata_ofdata_to_platdata,
	.probe	= fsl_ata_probe,
	.remove = fsl_ata_remove,
	.priv_auto_alloc_size = sizeof(struct fsl_ata_priv),
};
#endif
