// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include "main.h"
#include "mac.h"
#include "reg.h"
#include "fw.h"
#include "debug.h"

void rtw_set_channel_mac(struct rtw_dev *rtwdev, u8 channel, u8 bw,
			 u8 primary_ch_idx)
{
	u8 txsc40 = 0, txsc20 = 0;
	u32 value32;
	u8 value8;

	txsc20 = primary_ch_idx;
	if (txsc20 == 1 || txsc20 == 3)
		txsc40 = 9;
	else
		txsc40 = 10;
	rtw_write8(rtwdev, REG_DATA_SC,
		   BIT_TXSC_20M(txsc20) | BIT_TXSC_40M(txsc40));

	value32 = rtw_read32(rtwdev, REG_WMAC_TRXPTCL_CTL);
	value32 &= ~BIT_RFMOD;
	switch (bw) {
	case RTW_CHANNEL_WIDTH_80:
		value32 |= BIT_RFMOD_80M;
		break;
	case RTW_CHANNEL_WIDTH_40:
		value32 |= BIT_RFMOD_40M;
		break;
	case RTW_CHANNEL_WIDTH_20:
	default:
		break;
	}
	rtw_write32(rtwdev, REG_WMAC_TRXPTCL_CTL, value32);

	value32 = rtw_read32(rtwdev, REG_AFE_CTRL1) & ~(BIT_MAC_CLK_SEL);
	value32 |= (MAC_CLK_HW_DEF_80M << BIT_SHIFT_MAC_CLK_SEL);
	rtw_write32(rtwdev, REG_AFE_CTRL1, value32);

	rtw_write8(rtwdev, REG_USTIME_TSF, MAC_CLK_SPEED);
	rtw_write8(rtwdev, REG_USTIME_EDCA, MAC_CLK_SPEED);

	value8 = rtw_read8(rtwdev, REG_CCK_CHECK);
	value8 = value8 & ~BIT_CHECK_CCK_EN;
	if (channel > 35)
		value8 |= BIT_CHECK_CCK_EN;
	rtw_write8(rtwdev, REG_CCK_CHECK, value8);
}

static int rtw_mac_pre_system_cfg(struct rtw_dev *rtwdev)
{
	u32 value32;
	u8 value8;

	rtw_write8(rtwdev, REG_RSV_CTRL, 0);

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		rtw_write32_set(rtwdev, REG_HCI_OPT_CTRL, BIT_BT_DIG_CLK_EN);
		break;
	case RTW_HCI_TYPE_USB:
		break;
	default:
		return -EINVAL;
	}

	/* config PIN Mux */
	value32 = rtw_read32(rtwdev, REG_PAD_CTRL1);
	value32 |= BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL;
	rtw_write32(rtwdev, REG_PAD_CTRL1, value32);

	value32 = rtw_read32(rtwdev, REG_LED_CFG);
	value32 &= ~(BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN);
	rtw_write32(rtwdev, REG_LED_CFG, value32);

	value32 = rtw_read32(rtwdev, REG_GPIO_MUXCFG);
	value32 |= BIT_WLRFE_4_5_EN;
	rtw_write32(rtwdev, REG_GPIO_MUXCFG, value32);

	/* disable BB/RF */
	value8 = rtw_read8(rtwdev, REG_SYS_FUNC_EN);
	value8 &= ~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
	rtw_write8(rtwdev, REG_SYS_FUNC_EN, value8);

	value8 = rtw_read8(rtwdev, REG_RF_CTRL);
	value8 &= ~(BIT_RF_SDM_RSTB | BIT_RF_RSTB | BIT_RF_EN);
	rtw_write8(rtwdev, REG_RF_CTRL, value8);

	value32 = rtw_read32(rtwdev, REG_WLRF1);
	value32 &= ~BIT_WLRF1_BBRF_EN;
	rtw_write32(rtwdev, REG_WLRF1, value32);

	return 0;
}

static int rtw_pwr_cmd_polling(struct rtw_dev *rtwdev,
			       struct rtw_pwr_seq_cmd *cmd)
{
	u8 value;
	u8 flag = 0;
	u32 offset;
	u32 cnt = RTW_PWR_POLLING_CNT;

	if (cmd->base == RTW_PWR_ADDR_SDIO)
		offset = cmd->offset | SDIO_LOCAL_OFFSET;
	else
		offset = cmd->offset;

	do {
		cnt--;
		value = rtw_read8(rtwdev, offset);
		value &= cmd->mask;
		if (value == (cmd->value & cmd->mask))
			return 0;
		if (cnt == 0) {
			if (rtw_hci_type(rtwdev) == RTW_HCI_TYPE_PCIE &&
			    flag == 0) {
				value = rtw_read8(rtwdev, REG_SYS_PW_CTRL);
				value |= BIT(3);
				rtw_write8(rtwdev, REG_SYS_PW_CTRL, value);
				value &= ~BIT(3);
				rtw_write8(rtwdev, REG_SYS_PW_CTRL, value);
				cnt = RTW_PWR_POLLING_CNT;
				flag = 1;
			} else {
				return -EBUSY;
			}
		} else {
			udelay(50);
		}
	} while (1);
}

static int rtw_sub_pwr_seq_parser(struct rtw_dev *rtwdev, u8 intf_mask,
				  u8 cut_mask, struct rtw_pwr_seq_cmd *cmd)
{
	struct rtw_pwr_seq_cmd *cur_cmd;
	u32 offset;
	u8 value;

	for (cur_cmd = cmd; cur_cmd->cmd != RTW_PWR_CMD_END; cur_cmd++) {
		if (!(cur_cmd->intf_mask & intf_mask) ||
		    !(cur_cmd->cut_mask & cut_mask))
			continue;

		switch (cur_cmd->cmd) {
		case RTW_PWR_CMD_WRITE:
			offset = cur_cmd->offset;

			if (cur_cmd->base == RTW_PWR_ADDR_SDIO)
				offset |= SDIO_LOCAL_OFFSET;

			value = rtw_read8(rtwdev, offset);
			value &= ~cur_cmd->mask;
			value |= (cur_cmd->value & cur_cmd->mask);
			rtw_write8(rtwdev, offset, value);
			break;
		case RTW_PWR_CMD_POLLING:
			if (rtw_pwr_cmd_polling(rtwdev, cur_cmd))
				return -EBUSY;
			break;
		case RTW_PWR_CMD_DELAY:
			if (cur_cmd->value == RTW_PWR_DELAY_US)
				udelay(cur_cmd->offset);
			else
				mdelay(cur_cmd->offset);
			break;
		case RTW_PWR_CMD_READ:
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int rtw_pwr_seq_parser(struct rtw_dev *rtwdev,
			      struct rtw_pwr_seq_cmd **cmd_seq)
{
	u8 cut_mask;
	u8 intf_mask;
	u8 cut;
	u32 idx = 0;
	struct rtw_pwr_seq_cmd *cmd;
	int ret;

	cut = rtwdev->hal.cut_version;
	cut_mask = cut_version_to_mask(cut);
	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		intf_mask = BIT(2);
		break;
	case RTW_HCI_TYPE_USB:
		intf_mask = BIT(1);
		break;
	default:
		return -EINVAL;
	}

	do {
		cmd = cmd_seq[idx];
		if (!cmd)
			break;

		ret = rtw_sub_pwr_seq_parser(rtwdev, intf_mask, cut_mask, cmd);
		if (ret)
			return -EBUSY;

		idx++;
	} while (1);

	return 0;
}

static int rtw_mac_power_switch(struct rtw_dev *rtwdev, bool pwr_on)
{
	struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_pwr_seq_cmd **pwr_seq;
	u8 rpwm;
	bool cur_pwr;

	rpwm = rtw_read8(rtwdev, rtwdev->hci.rpwm_addr);

	/* Check FW still exist or not */
	if (rtw_read16(rtwdev, REG_MCUFW_CTRL) == 0xC078) {
		rpwm = (rpwm ^ BIT_RPWM_TOGGLE) & BIT_RPWM_TOGGLE;
		rtw_write8(rtwdev, rtwdev->hci.rpwm_addr, rpwm);
	}

	if (rtw_read8(rtwdev, REG_CR) == 0xea)
		cur_pwr = false;
	else if (rtw_hci_type(rtwdev) == RTW_HCI_TYPE_USB &&
		 (rtw_read8(rtwdev, REG_SYS_STATUS1 + 1) & BIT(0)))
		cur_pwr = false;
	else
		cur_pwr = true;

	if (pwr_on && cur_pwr)
		return -EALREADY;

	pwr_seq = pwr_on ? chip->pwr_on_seq : chip->pwr_off_seq;
	if (rtw_pwr_seq_parser(rtwdev, pwr_seq))
		return -EINVAL;

	return 0;
}

static int rtw_mac_init_system_cfg(struct rtw_dev *rtwdev)
{
	u8 sys_func_en = rtwdev->chip->sys_func_en;
	u8 value8;
	u32 value, tmp;

	value = rtw_read32(rtwdev, REG_CPU_DMEM_CON);
	value |= BIT_WL_PLATFORM_RST | BIT_DDMA_EN;
	rtw_write32(rtwdev, REG_CPU_DMEM_CON, value);

	rtw_write8(rtwdev, REG_SYS_FUNC_EN + 1, sys_func_en);
	value8 = (rtw_read8(rtwdev, REG_CR_EXT + 3) & 0xF0) | 0x0C;
	rtw_write8(rtwdev, REG_CR_EXT + 3, value8);

	/* disable boot-from-flash for driver's DL FW */
	tmp = rtw_read32(rtwdev, REG_MCUFW_CTRL);
	if (tmp & BIT_BOOT_FSPI_EN) {
		rtw_write32(rtwdev, REG_MCUFW_CTRL, tmp & (~BIT_BOOT_FSPI_EN));
		value = rtw_read32(rtwdev, REG_GPIO_MUXCFG) & (~BIT_FSPI_EN);
		rtw_write32(rtwdev, REG_GPIO_MUXCFG, value);
	}

	return 0;
}

int rtw_mac_power_on(struct rtw_dev *rtwdev)
{
	int ret = 0;

	ret = rtw_mac_pre_system_cfg(rtwdev);
	if (ret)
		goto err;

	ret = rtw_mac_power_switch(rtwdev, true);
	if (ret == -EALREADY) {
		rtw_mac_power_switch(rtwdev, false);
		ret = rtw_mac_power_switch(rtwdev, true);
		if (ret)
			goto err;
	} else if (ret) {
		goto err;
	}

	ret = rtw_mac_init_system_cfg(rtwdev);
	if (ret)
		goto err;

	return 0;

err:
	rtw_err(rtwdev, "mac power on failed");
	return ret;
}

void rtw_mac_power_off(struct rtw_dev *rtwdev)
{
	rtw_mac_power_switch(rtwdev, false);
}

static bool check_firmware_size(const u8 *data, u32 size)
{
	u32 dmem_size;
	u32 imem_size;
	u32 emem_size;
	u32 real_size;

	dmem_size = le32_to_cpu(*((__le32 *)(data + FW_HDR_DMEM_SIZE)));
	imem_size = le32_to_cpu(*((__le32 *)(data + FW_HDR_IMEM_SIZE)));
	emem_size = ((*(data + FW_HDR_MEM_USAGE)) & BIT(4)) ?
		    le32_to_cpu(*((__le32 *)(data + FW_HDR_EMEM_SIZE))) : 0;

	dmem_size += FW_HDR_CHKSUM_SIZE;
	imem_size += FW_HDR_CHKSUM_SIZE;
	emem_size += emem_size ? FW_HDR_CHKSUM_SIZE : 0;
	real_size = FW_HDR_SIZE + dmem_size + imem_size + emem_size;
	if (real_size != size)
		return false;

	return true;
}

static void wlan_cpu_enable(struct rtw_dev *rtwdev, bool enable)
{
	if (enable) {
		/* cpu io interface enable */
		rtw_write8_set(rtwdev, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);

		/* cpu enable */
		rtw_write8_set(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
	} else {
		/* cpu io interface disable */
		rtw_write8_clr(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);

		/* cpu disable */
		rtw_write8_clr(rtwdev, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
	}
}

#define DLFW_RESTORE_REG_NUM 6

static void download_firmware_reg_backup(struct rtw_dev *rtwdev,
					 struct rtw_backup_info *bckp)
{
	u8 tmp;
	u8 bckp_idx = 0;

	/* set HIQ to hi priority */
	bckp[bckp_idx].len = 1;
	bckp[bckp_idx].reg = REG_TXDMA_PQ_MAP + 1;
	bckp[bckp_idx].val = rtw_read8(rtwdev, REG_TXDMA_PQ_MAP + 1);
	bckp_idx++;
	tmp = RTW_DMA_MAPPING_HIGH << 6;
	rtw_write8(rtwdev, REG_TXDMA_PQ_MAP + 1, tmp);

	/* DLFW only use HIQ, map HIQ to hi priority */
	bckp[bckp_idx].len = 1;
	bckp[bckp_idx].reg = REG_CR;
	bckp[bckp_idx].val = rtw_read8(rtwdev, REG_CR);
	bckp_idx++;
	bckp[bckp_idx].len = 4;
	bckp[bckp_idx].reg = REG_H2CQ_CSR;
	bckp[bckp_idx].val = BIT_H2CQ_FULL;
	bckp_idx++;
	tmp = BIT_HCI_TXDMA_EN | BIT_TXDMA_EN;
	rtw_write8(rtwdev, REG_CR, tmp);
	rtw_write32(rtwdev, REG_H2CQ_CSR, BIT_H2CQ_FULL);

	/* Config hi priority queue and public priority queue page number */
	bckp[bckp_idx].len = 2;
	bckp[bckp_idx].reg = REG_FIFOPAGE_INFO_1;
	bckp[bckp_idx].val = rtw_read16(rtwdev, REG_FIFOPAGE_INFO_1);
	bckp_idx++;
	bckp[bckp_idx].len = 4;
	bckp[bckp_idx].reg = REG_RQPN_CTRL_2;
	bckp[bckp_idx].val = rtw_read32(rtwdev, REG_RQPN_CTRL_2) | BIT_LD_RQPN;
	bckp_idx++;
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_1, 0x200);
	rtw_write32(rtwdev, REG_RQPN_CTRL_2, bckp[bckp_idx - 1].val);

	/* Disable beacon related functions */
	tmp = rtw_read8(rtwdev, REG_BCN_CTRL);
	bckp[bckp_idx].len = 1;
	bckp[bckp_idx].reg = REG_BCN_CTRL;
	bckp[bckp_idx].val = tmp;
	bckp_idx++;
	tmp = (u8)((tmp & (~BIT_EN_BCN_FUNCTION)) | BIT_DIS_TSF_UDT);
	rtw_write8(rtwdev, REG_BCN_CTRL, tmp);

	WARN(bckp_idx != DLFW_RESTORE_REG_NUM, "wrong backup number\n");
}

static void download_firmware_reset_platform(struct rtw_dev *rtwdev)
{
	rtw_write8_clr(rtwdev, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
	rtw_write8_clr(rtwdev, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
	rtw_write8_set(rtwdev, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
	rtw_write8_set(rtwdev, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
}

static void download_firmware_reg_restore(struct rtw_dev *rtwdev,
					  struct rtw_backup_info *bckp,
					  u8 bckp_num)
{
	rtw_restore_reg(rtwdev, bckp, bckp_num);
}

#define TX_DESC_SIZE 48

static int send_firmware_pkt_rsvd_page(struct rtw_dev *rtwdev, u16 pg_addr,
				       const u8 *data, u32 size)
{
	u8 *buf;
	int ret;

	buf = kmemdup(data, size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = rtw_fw_write_data_rsvd_page(rtwdev, pg_addr, buf, size);
	kfree(buf);
	return ret;
}

static int
send_firmware_pkt(struct rtw_dev *rtwdev, u16 pg_addr, const u8 *data, u32 size)
{
	int ret;

	if (rtw_hci_type(rtwdev) == RTW_HCI_TYPE_USB &&
	    !((size + TX_DESC_SIZE) & (512 - 1)))
		size += 1;

	ret = send_firmware_pkt_rsvd_page(rtwdev, pg_addr, data, size);
	if (ret)
		rtw_err(rtwdev, "failed to download rsvd page\n");

	return ret;
}

static int
iddma_enable(struct rtw_dev *rtwdev, u32 src, u32 dst, u32 ctrl)
{
	rtw_write32(rtwdev, REG_DDMA_CH0SA, src);
	rtw_write32(rtwdev, REG_DDMA_CH0DA, dst);
	rtw_write32(rtwdev, REG_DDMA_CH0CTRL, ctrl);

	if (!check_hw_ready(rtwdev, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
		return -EBUSY;

	return 0;
}

static int iddma_download_firmware(struct rtw_dev *rtwdev, u32 src, u32 dst,
				   u32 len, u8 first)
{
	u32 ch0_ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN;

	if (!check_hw_ready(rtwdev, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
		return -EBUSY;

	ch0_ctrl |= len & BIT_MASK_DDMACH0_DLEN;
	if (!first)
		ch0_ctrl |= BIT_DDMACH0_CHKSUM_CONT;

	if (iddma_enable(rtwdev, src, dst, ch0_ctrl))
		return -EBUSY;

	return 0;
}

static bool
check_fw_checksum(struct rtw_dev *rtwdev, u32 addr)
{
	u8 fw_ctrl;

	fw_ctrl = rtw_read8(rtwdev, REG_MCUFW_CTRL);

	if (rtw_read32(rtwdev, REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) {
		if (addr < OCPBASE_DMEM_88XX) {
			fw_ctrl |= BIT_IMEM_DW_OK;
			fw_ctrl &= ~BIT_IMEM_CHKSUM_OK;
			rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
		} else {
			fw_ctrl |= BIT_DMEM_DW_OK;
			fw_ctrl &= ~BIT_DMEM_CHKSUM_OK;
			rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
		}

		rtw_err(rtwdev, "invalid fw checksum\n");

		return false;
	}

	if (addr < OCPBASE_DMEM_88XX) {
		fw_ctrl |= (BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK);
		rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
	} else {
		fw_ctrl |= (BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
		rtw_write8(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
	}

	return true;
}

static int
download_firmware_to_mem(struct rtw_dev *rtwdev, const u8 *data,
			 u32 src, u32 dst, u32 size)
{
	struct rtw_chip_info *chip = rtwdev->chip;
	u32 desc_size = chip->tx_pkt_desc_sz;
	u8 first_part;
	u32 mem_offset;
	u32 residue_size;
	u32 pkt_size;
	u32 max_size = 0x1000;
	u32 val;
	int ret;

	mem_offset = 0;
	first_part = 1;
	residue_size = size;

	val = rtw_read32(rtwdev, REG_DDMA_CH0CTRL);
	val |= BIT_DDMACH0_RESET_CHKSUM_STS;
	rtw_write32(rtwdev, REG_DDMA_CH0CTRL, val);

	while (residue_size) {
		if (residue_size >= max_size)
			pkt_size = max_size;
		else
			pkt_size = residue_size;

		ret = send_firmware_pkt(rtwdev, (u16)(src >> 7),
					data + mem_offset, pkt_size);
		if (ret)
			return ret;

		ret = iddma_download_firmware(rtwdev, OCPBASE_TXBUF_88XX +
					      src + desc_size,
					      dst + mem_offset, pkt_size,
					      first_part);
		if (ret)
			return ret;

		first_part = 0;
		mem_offset += pkt_size;
		residue_size -= pkt_size;
	}

	if (!check_fw_checksum(rtwdev, dst))
		return -EINVAL;

	return 0;
}

static void update_firmware_info(struct rtw_dev *rtwdev,
				 struct rtw_fw_state *fw)
{
	const u8 *data = fw->firmware->data;

	fw->h2c_version =
		le16_to_cpu(*((__le16 *)(data + FW_HDR_H2C_FMT_VER)));
	fw->version =
		le16_to_cpu(*((__le16 *)(data + FW_HDR_VERSION)));
	fw->sub_version = *(data + FW_HDR_SUBVERSION);
	fw->sub_index = *(data + FW_HDR_SUBINDEX);

	rtw_dbg(rtwdev, RTW_DBG_FW, "fw h2c version: %x\n", fw->h2c_version);
	rtw_dbg(rtwdev, RTW_DBG_FW, "fw version:     %x\n", fw->version);
	rtw_dbg(rtwdev, RTW_DBG_FW, "fw sub version: %x\n", fw->sub_version);
	rtw_dbg(rtwdev, RTW_DBG_FW, "fw sub index:   %x\n", fw->sub_index);
}

static int
start_download_firmware(struct rtw_dev *rtwdev, const u8 *data, u32 size)
{
	const u8 *cur_fw;
	u16 val;
	u32 imem_size;
	u32 dmem_size;
	u32 emem_size;
	u32 addr;
	int ret;

	dmem_size = le32_to_cpu(*((__le32 *)(data + FW_HDR_DMEM_SIZE)));
	imem_size = le32_to_cpu(*((__le32 *)(data + FW_HDR_IMEM_SIZE)));
	emem_size = ((*(data + FW_HDR_MEM_USAGE)) & BIT(4)) ?
		    le32_to_cpu(*((__le32 *)(data + FW_HDR_EMEM_SIZE))) : 0;
	dmem_size += FW_HDR_CHKSUM_SIZE;
	imem_size += FW_HDR_CHKSUM_SIZE;
	emem_size += emem_size ? FW_HDR_CHKSUM_SIZE : 0;

	val = (u16)(rtw_read16(rtwdev, REG_MCUFW_CTRL) & 0x3800);
	val |= BIT_MCUFWDL_EN;
	rtw_write16(rtwdev, REG_MCUFW_CTRL, val);

	cur_fw = data + FW_HDR_SIZE;
	addr = le32_to_cpu(*((__le32 *)(data + FW_HDR_DMEM_ADDR)));
	addr &= ~BIT(31);
	ret = download_firmware_to_mem(rtwdev, cur_fw, 0, addr, dmem_size);
	if (ret)
		return ret;

	cur_fw = data + FW_HDR_SIZE + dmem_size;
	addr = le32_to_cpu(*((__le32 *)(data + FW_HDR_IMEM_ADDR)));
	addr &= ~BIT(31);
	ret = download_firmware_to_mem(rtwdev, cur_fw, 0, addr, imem_size);
	if (ret)
		return ret;

	if (emem_size) {
		cur_fw = data + FW_HDR_SIZE + dmem_size + imem_size;
		addr = le32_to_cpu(*((__le32 *)(data + FW_HDR_EMEM_ADDR)));
		addr &= ~BIT(31);
		ret = download_firmware_to_mem(rtwdev, cur_fw, 0, addr,
					       emem_size);
		if (ret)
			return ret;
	}

	return 0;
}

static int download_firmware_validate(struct rtw_dev *rtwdev)
{
	u32 fw_key;

	if (!check_hw_ready(rtwdev, REG_MCUFW_CTRL, FW_READY_MASK, FW_READY)) {
		fw_key = rtw_read32(rtwdev, REG_FW_DBG7) & FW_KEY_MASK;
		if (fw_key == ILLEGAL_KEY_GROUP)
			rtw_err(rtwdev, "invalid fw key\n");
		return -EINVAL;
	}

	return 0;
}

static void download_firmware_end_flow(struct rtw_dev *rtwdev)
{
	u16 fw_ctrl;

	rtw_write32(rtwdev, REG_TXDMA_STATUS, BTI_PAGE_OVF);

	/* Check IMEM & DMEM checksum is OK or not */
	fw_ctrl = rtw_read16(rtwdev, REG_MCUFW_CTRL);
	if ((fw_ctrl & BIT_CHECK_SUM_OK) != BIT_CHECK_SUM_OK)
		return;

	fw_ctrl = (fw_ctrl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN;
	rtw_write16(rtwdev, REG_MCUFW_CTRL, fw_ctrl);
}

int rtw_download_firmware(struct rtw_dev *rtwdev, struct rtw_fw_state *fw)
{
	struct rtw_backup_info bckp[DLFW_RESTORE_REG_NUM];
	const u8 *data = fw->firmware->data;
	u32 size = fw->firmware->size;
	u32 ltecoex_bckp;
	int ret;

	if (!check_firmware_size(data, size))
		return -EINVAL;

	if (!ltecoex_read_reg(rtwdev, 0x38, &ltecoex_bckp))
		return -EBUSY;

	wlan_cpu_enable(rtwdev, false);

	download_firmware_reg_backup(rtwdev, bckp);
	download_firmware_reset_platform(rtwdev);

	ret = start_download_firmware(rtwdev, data, size);
	if (ret)
		goto dlfw_fail;

	download_firmware_reg_restore(rtwdev, bckp, DLFW_RESTORE_REG_NUM);

	download_firmware_end_flow(rtwdev);

	wlan_cpu_enable(rtwdev, true);

	if (!ltecoex_reg_write(rtwdev, 0x38, ltecoex_bckp))
		return -EBUSY;

	ret = download_firmware_validate(rtwdev);
	if (ret)
		goto dlfw_fail;

	update_firmware_info(rtwdev, fw);

	/* reset desc and index */
	rtw_hci_setup(rtwdev);

	rtwdev->h2c.last_box_num = 0;
	rtwdev->h2c.seq = 0;

	rtw_flag_set(rtwdev, RTW_FLAG_FW_RUNNING);

	return 0;

dlfw_fail:
	/* Disable FWDL_EN */
	rtw_write8_clr(rtwdev, REG_MCUFW_CTRL, BIT_MCUFWDL_EN);
	rtw_write8_set(rtwdev, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);

	return ret;
}

static int txdma_queue_mapping(struct rtw_dev *rtwdev)
{
	struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_rqpn *rqpn = NULL;
	u16 txdma_pq_map = 0;

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		rqpn = &chip->rqpn_table[1];
		break;
	case RTW_HCI_TYPE_USB:
		if (rtwdev->hci.bulkout_num == 2)
			rqpn = &chip->rqpn_table[2];
		else if (rtwdev->hci.bulkout_num == 3)
			rqpn = &chip->rqpn_table[3];
		else if (rtwdev->hci.bulkout_num == 4)
			rqpn = &chip->rqpn_table[4];
		else
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	txdma_pq_map |= BIT_TXDMA_HIQ_MAP(rqpn->dma_map_hi);
	txdma_pq_map |= BIT_TXDMA_MGQ_MAP(rqpn->dma_map_mg);
	txdma_pq_map |= BIT_TXDMA_BKQ_MAP(rqpn->dma_map_bk);
	txdma_pq_map |= BIT_TXDMA_BEQ_MAP(rqpn->dma_map_be);
	txdma_pq_map |= BIT_TXDMA_VIQ_MAP(rqpn->dma_map_vi);
	txdma_pq_map |= BIT_TXDMA_VOQ_MAP(rqpn->dma_map_vo);
	rtw_write16(rtwdev, REG_TXDMA_PQ_MAP, txdma_pq_map);

	rtw_write8(rtwdev, REG_CR, 0);
	rtw_write8(rtwdev, REG_CR, MAC_TRX_ENABLE);
	rtw_write32(rtwdev, REG_H2CQ_CSR, BIT_H2CQ_FULL);

	return 0;
}

static int set_trx_fifo_info(struct rtw_dev *rtwdev)
{
	struct rtw_fifo_conf *fifo = &rtwdev->fifo;
	struct rtw_chip_info *chip = rtwdev->chip;
	u16 cur_pg_addr;
	u8 csi_buf_pg_num = chip->csi_buf_pg_num;

	/* config rsvd page num */
	fifo->rsvd_drv_pg_num = 8;
	fifo->txff_pg_num = chip->txff_size >> 7;
	fifo->rsvd_pg_num = fifo->rsvd_drv_pg_num +
			   RSVD_PG_H2C_EXTRAINFO_NUM +
			   RSVD_PG_H2C_STATICINFO_NUM +
			   RSVD_PG_H2CQ_NUM +
			   RSVD_PG_CPU_INSTRUCTION_NUM +
			   RSVD_PG_FW_TXBUF_NUM +
			   csi_buf_pg_num;

	if (fifo->rsvd_pg_num > fifo->txff_pg_num)
		return -ENOMEM;

	fifo->acq_pg_num = fifo->txff_pg_num - fifo->rsvd_pg_num;
	fifo->rsvd_boundary = fifo->txff_pg_num - fifo->rsvd_pg_num;

	cur_pg_addr = fifo->txff_pg_num;
	cur_pg_addr -= csi_buf_pg_num;
	fifo->rsvd_csibuf_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_FW_TXBUF_NUM;
	fifo->rsvd_fw_txbuf_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_CPU_INSTRUCTION_NUM;
	fifo->rsvd_cpu_instr_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2CQ_NUM;
	fifo->rsvd_h2cq_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2C_STATICINFO_NUM;
	fifo->rsvd_h2c_sta_info_addr = cur_pg_addr;
	cur_pg_addr -= RSVD_PG_H2C_EXTRAINFO_NUM;
	fifo->rsvd_h2c_info_addr = cur_pg_addr;
	cur_pg_addr -= fifo->rsvd_drv_pg_num;
	fifo->rsvd_drv_addr = cur_pg_addr;

	if (fifo->rsvd_boundary != fifo->rsvd_drv_addr) {
		rtw_err(rtwdev, "wrong rsvd driver address\n");
		return -EINVAL;
	}

	return 0;
}

static int priority_queue_cfg(struct rtw_dev *rtwdev)
{
	struct rtw_fifo_conf *fifo = &rtwdev->fifo;
	struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_page_table *pg_tbl = NULL;
	u16 pubq_num;
	int ret;

	ret = set_trx_fifo_info(rtwdev);
	if (ret)
		return ret;

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		pg_tbl = &chip->page_table[1];
		break;
	case RTW_HCI_TYPE_USB:
		if (rtwdev->hci.bulkout_num == 2)
			pg_tbl = &chip->page_table[2];
		else if (rtwdev->hci.bulkout_num == 3)
			pg_tbl = &chip->page_table[3];
		else if (rtwdev->hci.bulkout_num == 4)
			pg_tbl = &chip->page_table[4];
		else
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	pubq_num = fifo->acq_pg_num - pg_tbl->hq_num - pg_tbl->lq_num -
		   pg_tbl->nq_num - pg_tbl->exq_num - pg_tbl->gapq_num;
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_1, pg_tbl->hq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_2, pg_tbl->lq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_3, pg_tbl->nq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_4, pg_tbl->exq_num);
	rtw_write16(rtwdev, REG_FIFOPAGE_INFO_5, pubq_num);
	rtw_write32_set(rtwdev, REG_RQPN_CTRL_2, BIT_LD_RQPN);

	rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2, fifo->rsvd_boundary);
	rtw_write8_set(rtwdev, REG_FWHW_TXQ_CTRL + 2, BIT_EN_WR_FREE_TAIL >> 16);

	rtw_write16(rtwdev, REG_BCNQ_BDNY_V1, fifo->rsvd_boundary);
	rtw_write16(rtwdev, REG_FIFOPAGE_CTRL_2 + 2, fifo->rsvd_boundary);
	rtw_write16(rtwdev, REG_BCNQ1_BDNY_V1, fifo->rsvd_boundary);
	rtw_write32(rtwdev, REG_RXFF_BNDY, chip->rxff_size - C2H_PKT_BUF - 1);
	rtw_write8_set(rtwdev, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1);

	if (!check_hw_ready(rtwdev, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0))
		return -EBUSY;

	rtw_write8(rtwdev, REG_CR + 3, 0);

	return 0;
}

static int init_h2c(struct rtw_dev *rtwdev)
{
	struct rtw_fifo_conf *fifo = &rtwdev->fifo;
	u8 value8;
	u32 value32;
	u32 h2cq_addr;
	u32 h2cq_size;
	u32 h2cq_free;
	u32 wp, rp;

	h2cq_addr = fifo->rsvd_h2cq_addr << TX_PAGE_SIZE_SHIFT;
	h2cq_size = RSVD_PG_H2CQ_NUM << TX_PAGE_SIZE_SHIFT;

	value32 = rtw_read32(rtwdev, REG_H2C_HEAD);
	value32 = (value32 & 0xFFFC0000) | h2cq_addr;
	rtw_write32(rtwdev, REG_H2C_HEAD, value32);

	value32 = rtw_read32(rtwdev, REG_H2C_READ_ADDR);
	value32 = (value32 & 0xFFFC0000) | h2cq_addr;
	rtw_write32(rtwdev, REG_H2C_READ_ADDR, value32);

	value32 = rtw_read32(rtwdev, REG_H2C_TAIL);
	value32 &= 0xFFFC0000;
	value32 |= (h2cq_addr + h2cq_size);
	rtw_write32(rtwdev, REG_H2C_TAIL, value32);

	value8 = rtw_read8(rtwdev, REG_H2C_INFO);
	value8 = (u8)((value8 & 0xFC) | 0x01);
	rtw_write8(rtwdev, REG_H2C_INFO, value8);

	value8 = rtw_read8(rtwdev, REG_H2C_INFO);
	value8 = (u8)((value8 & 0xFB) | 0x04);
	rtw_write8(rtwdev, REG_H2C_INFO, value8);

	value8 = rtw_read8(rtwdev, REG_TXDMA_OFFSET_CHK + 1);
	value8 = (u8)((value8 & 0x7f) | 0x80);
	rtw_write8(rtwdev, REG_TXDMA_OFFSET_CHK + 1, value8);

	wp = rtw_read32(rtwdev, REG_H2C_PKT_WRITEADDR) & 0x3FFFF;
	rp = rtw_read32(rtwdev, REG_H2C_PKT_READADDR) & 0x3FFFF;
	h2cq_free = wp >= rp ? h2cq_size - (wp - rp) : rp - wp;

	if (h2cq_size != h2cq_free) {
		rtw_err(rtwdev, "H2C queue mismatch\n");
		return -EINVAL;
	}

	return 0;
}

static int rtw_init_trx_cfg(struct rtw_dev *rtwdev)
{
	int ret;

	ret = txdma_queue_mapping(rtwdev);
	if (ret)
		return ret;

	ret = priority_queue_cfg(rtwdev);
	if (ret)
		return ret;

	ret = init_h2c(rtwdev);
	if (ret)
		return ret;

	return 0;
}

static int rtw_drv_info_cfg(struct rtw_dev *rtwdev)
{
	u8 value8;

	rtw_write8(rtwdev, REG_RX_DRVINFO_SZ, PHY_STATUS_SIZE);
	value8 = rtw_read8(rtwdev, REG_TRXFF_BNDY + 1);
	value8 &= 0xF0;
	/* For rxdesc len = 0 issue */
	value8 |= 0xF;
	rtw_write8(rtwdev, REG_TRXFF_BNDY + 1, value8);
	rtw_write32_set(rtwdev, REG_RCR, BIT_APP_PHYSTS);
	rtw_write32_clr(rtwdev, REG_WMAC_OPTION_FUNCTION + 4, BIT(8) | BIT(9));

	return 0;
}

int rtw_mac_init(struct rtw_dev *rtwdev)
{
	struct rtw_chip_info *chip = rtwdev->chip;
	int ret;

	ret = rtw_init_trx_cfg(rtwdev);
	if (ret)
		return ret;

	ret = chip->ops->mac_init(rtwdev);
	if (ret)
		return ret;

	ret = rtw_drv_info_cfg(rtwdev);
	if (ret)
		return ret;

	return 0;
}
