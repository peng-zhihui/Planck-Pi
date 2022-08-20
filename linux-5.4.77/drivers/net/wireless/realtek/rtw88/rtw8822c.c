// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include "main.h"
#include "coex.h"
#include "fw.h"
#include "tx.h"
#include "rx.h"
#include "phy.h"
#include "rtw8822c.h"
#include "rtw8822c_table.h"
#include "mac.h"
#include "reg.h"
#include "debug.h"
#include "util.h"

static void rtw8822c_config_trx_mode(struct rtw_dev *rtwdev, u8 tx_path,
				     u8 rx_path, bool is_tx2_path);

static void rtw8822ce_efuse_parsing(struct rtw_efuse *efuse,
				    struct rtw8822c_efuse *map)
{
	ether_addr_copy(efuse->addr, map->e.mac_addr);
}

static int rtw8822c_read_efuse(struct rtw_dev *rtwdev, u8 *log_map)
{
	struct rtw_efuse *efuse = &rtwdev->efuse;
	struct rtw8822c_efuse *map;
	int i;

	map = (struct rtw8822c_efuse *)log_map;

	efuse->rfe_option = map->rfe_option;
	efuse->rf_board_option = map->rf_board_option;
	efuse->crystal_cap = map->xtal_k;
	efuse->channel_plan = map->channel_plan;
	efuse->country_code[0] = map->country_code[0];
	efuse->country_code[1] = map->country_code[1];
	efuse->bt_setting = map->rf_bt_setting;
	efuse->regd = map->rf_board_option & 0x7;

	for (i = 0; i < 4; i++)
		efuse->txpwr_idx_table[i] = map->txpwr_idx_table[i];

	switch (rtw_hci_type(rtwdev)) {
	case RTW_HCI_TYPE_PCIE:
		rtw8822ce_efuse_parsing(efuse, map);
		break;
	default:
		/* unsupported now */
		return -ENOTSUPP;
	}

	return 0;
}

static void rtw8822c_header_file_init(struct rtw_dev *rtwdev, bool pre)
{
	rtw_write32_set(rtwdev, REG_3WIRE, BIT_3WIRE_TX_EN | BIT_3WIRE_RX_EN);
	rtw_write32_set(rtwdev, REG_3WIRE, BIT_3WIRE_PI_ON);
	rtw_write32_set(rtwdev, REG_3WIRE2, BIT_3WIRE_TX_EN | BIT_3WIRE_RX_EN);
	rtw_write32_set(rtwdev, REG_3WIRE2, BIT_3WIRE_PI_ON);

	if (pre)
		rtw_write32_clr(rtwdev, REG_ENCCK, BIT_CCK_OFDM_BLK_EN);
	else
		rtw_write32_set(rtwdev, REG_ENCCK, BIT_CCK_OFDM_BLK_EN);
}

static void rtw8822c_dac_backup_reg(struct rtw_dev *rtwdev,
				    struct rtw_backup_info *backup,
				    struct rtw_backup_info *backup_rf)
{
	u32 path, i;
	u32 val;
	u32 reg;
	u32 rf_addr[DACK_RF_8822C] = {0x8f};
	u32 addrs[DACK_REG_8822C] = {0x180c, 0x1810, 0x410c, 0x4110,
				     0x1c3c, 0x1c24, 0x1d70, 0x9b4,
				     0x1a00, 0x1a14, 0x1d58, 0x1c38,
				     0x1e24, 0x1e28, 0x1860, 0x4160};

	for (i = 0; i < DACK_REG_8822C; i++) {
		backup[i].len = 4;
		backup[i].reg = addrs[i];
		backup[i].val = rtw_read32(rtwdev, addrs[i]);
	}

	for (path = 0; path < DACK_PATH_8822C; path++) {
		for (i = 0; i < DACK_RF_8822C; i++) {
			reg = rf_addr[i];
			val = rtw_read_rf(rtwdev, path, reg, RFREG_MASK);
			backup_rf[path * i + i].reg = reg;
			backup_rf[path * i + i].val = val;
		}
	}
}

static void rtw8822c_dac_restore_reg(struct rtw_dev *rtwdev,
				     struct rtw_backup_info *backup,
				     struct rtw_backup_info *backup_rf)
{
	u32 path, i;
	u32 val;
	u32 reg;

	rtw_restore_reg(rtwdev, backup, DACK_REG_8822C);

	for (path = 0; path < DACK_PATH_8822C; path++) {
		for (i = 0; i < DACK_RF_8822C; i++) {
			val = backup_rf[path * i + i].val;
			reg = backup_rf[path * i + i].reg;
			rtw_write_rf(rtwdev, path, reg, RFREG_MASK, val);
		}
	}
}

static void rtw8822c_rf_minmax_cmp(struct rtw_dev *rtwdev, u32 value,
				   u32 *min, u32 *max)
{
	if (value >= 0x200) {
		if (*min >= 0x200) {
			if (*min > value)
				*min = value;
		} else {
			*min = value;
		}
		if (*max >= 0x200) {
			if (*max < value)
				*max = value;
		}
	} else {
		if (*min < 0x200) {
			if (*min > value)
				*min = value;
		}

		if (*max  >= 0x200) {
			*max = value;
		} else {
			if (*max < value)
				*max = value;
		}
	}
}

static void swap_u32(u32 *v1, u32 *v2)
{
	u32 tmp;

	tmp = *v1;
	*v1 = *v2;
	*v2 = tmp;
}

static void __rtw8822c_dac_iq_sort(struct rtw_dev *rtwdev, u32 *v1, u32 *v2)
{
	if (*v1 >= 0x200 && *v2 >= 0x200) {
		if (*v1 > *v2)
			swap_u32(v1, v2);
	} else if (*v1 < 0x200 && *v2 < 0x200) {
		if (*v1 > *v2)
			swap_u32(v1, v2);
	} else if (*v1 < 0x200 && *v2 >= 0x200) {
		swap_u32(v1, v2);
	}
}

static void rtw8822c_dac_iq_sort(struct rtw_dev *rtwdev, u32 *iv, u32 *qv)
{
	u32 i, j;

	for (i = 0; i < DACK_SN_8822C - 1; i++) {
		for (j = 0; j < (DACK_SN_8822C - 1 - i) ; j++) {
			__rtw8822c_dac_iq_sort(rtwdev, &iv[j], &iv[j + 1]);
			__rtw8822c_dac_iq_sort(rtwdev, &qv[j], &qv[j + 1]);
		}
	}
}

static void rtw8822c_dac_iq_offset(struct rtw_dev *rtwdev, u32 *vec, u32 *val)
{
	u32 p, m, t, i;

	m = 0;
	p = 0;
	for (i = 10; i < DACK_SN_8822C - 10; i++) {
		if (vec[i] > 0x200)
			m = (0x400 - vec[i]) + m;
		else
			p = vec[i] + p;
	}

	if (p > m) {
		t = p - m;
		t = t / (DACK_SN_8822C - 20);
	} else {
		t = m - p;
		t = t / (DACK_SN_8822C - 20);
		if (t != 0x0)
			t = 0x400 - t;
	}

	*val = t;
}

static u32 rtw8822c_get_path_write_addr(u8 path)
{
	u32 base_addr;

	switch (path) {
	case RF_PATH_A:
		base_addr = 0x1800;
		break;
	case RF_PATH_B:
		base_addr = 0x4100;
		break;
	default:
		WARN_ON(1);
		return -1;
	}

	return base_addr;
}

static u32 rtw8822c_get_path_read_addr(u8 path)
{
	u32 base_addr;

	switch (path) {
	case RF_PATH_A:
		base_addr = 0x2800;
		break;
	case RF_PATH_B:
		base_addr = 0x4500;
		break;
	default:
		WARN_ON(1);
		return -1;
	}

	return base_addr;
}

static bool rtw8822c_dac_iq_check(struct rtw_dev *rtwdev, u32 value)
{
	bool ret = true;

	if ((value >= 0x200 && (0x400 - value) > 0x64) ||
	    (value < 0x200 && value > 0x64)) {
		ret = false;
		rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] Error overflow\n");
	}

	return ret;
}

static void rtw8822c_dac_cal_iq_sample(struct rtw_dev *rtwdev, u32 *iv, u32 *qv)
{
	u32 temp;
	int i = 0, cnt = 0;

	while (i < DACK_SN_8822C && cnt < 10000) {
		cnt++;
		temp = rtw_read32_mask(rtwdev, 0x2dbc, 0x3fffff);
		iv[i] = (temp & 0x3ff000) >> 12;
		qv[i] = temp & 0x3ff;

		if (rtw8822c_dac_iq_check(rtwdev, iv[i]) &&
		    rtw8822c_dac_iq_check(rtwdev, qv[i]))
			i++;
	}
}

static void rtw8822c_dac_cal_iq_search(struct rtw_dev *rtwdev,
				       u32 *iv, u32 *qv,
				       u32 *i_value, u32 *q_value)
{
	u32 i_max = 0, q_max = 0, i_min = 0, q_min = 0;
	u32 i_delta, q_delta;
	u32 temp;
	int i, cnt = 0;

	do {
		i_min = iv[0];
		i_max = iv[0];
		q_min = qv[0];
		q_max = qv[0];
		for (i = 0; i < DACK_SN_8822C; i++) {
			rtw8822c_rf_minmax_cmp(rtwdev, iv[i], &i_min, &i_max);
			rtw8822c_rf_minmax_cmp(rtwdev, qv[i], &q_min, &q_max);
		}

		if (i_max < 0x200 && i_min < 0x200)
			i_delta = i_max - i_min;
		else if (i_max >= 0x200 && i_min >= 0x200)
			i_delta = i_max - i_min;
		else
			i_delta = i_max + (0x400 - i_min);

		if (q_max < 0x200 && q_min < 0x200)
			q_delta = q_max - q_min;
		else if (q_max >= 0x200 && q_min >= 0x200)
			q_delta = q_max - q_min;
		else
			q_delta = q_max + (0x400 - q_min);

		rtw_dbg(rtwdev, RTW_DBG_RFK,
			"[DACK] i: min=0x%08x, max=0x%08x, delta=0x%08x\n",
			i_min, i_max, i_delta);
		rtw_dbg(rtwdev, RTW_DBG_RFK,
			"[DACK] q: min=0x%08x, max=0x%08x, delta=0x%08x\n",
			q_min, q_max, q_delta);

		rtw8822c_dac_iq_sort(rtwdev, iv, qv);

		if (i_delta > 5 || q_delta > 5) {
			temp = rtw_read32_mask(rtwdev, 0x2dbc, 0x3fffff);
			iv[0] = (temp & 0x3ff000) >> 12;
			qv[0] = temp & 0x3ff;
			temp = rtw_read32_mask(rtwdev, 0x2dbc, 0x3fffff);
			iv[DACK_SN_8822C - 1] = (temp & 0x3ff000) >> 12;
			qv[DACK_SN_8822C - 1] = temp & 0x3ff;
		} else {
			break;
		}
	} while (cnt++ < 100);

	rtw8822c_dac_iq_offset(rtwdev, iv, i_value);
	rtw8822c_dac_iq_offset(rtwdev, qv, q_value);
}

static void rtw8822c_dac_cal_rf_mode(struct rtw_dev *rtwdev,
				     u32 *i_value, u32 *q_value)
{
	u32 iv[DACK_SN_8822C], qv[DACK_SN_8822C];
	u32 rf_a, rf_b;

	rf_a = rtw_read_rf(rtwdev, RF_PATH_A, 0x0, RFREG_MASK);
	rf_b = rtw_read_rf(rtwdev, RF_PATH_B, 0x0, RFREG_MASK);

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] RF path-A=0x%05x\n", rf_a);
	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] RF path-B=0x%05x\n", rf_b);

	rtw8822c_dac_cal_iq_sample(rtwdev, iv, qv);
	rtw8822c_dac_cal_iq_search(rtwdev, iv, qv, i_value, q_value);
}

static void rtw8822c_dac_bb_setting(struct rtw_dev *rtwdev)
{
	rtw_write32_mask(rtwdev, 0x1d58, 0xff8, 0x1ff);
	rtw_write32_mask(rtwdev, 0x1a00, 0x3, 0x2);
	rtw_write32_mask(rtwdev, 0x1a14, 0x300, 0x3);
	rtw_write32(rtwdev, 0x1d70, 0x7e7e7e7e);
	rtw_write32_mask(rtwdev, 0x180c, 0x3, 0x0);
	rtw_write32_mask(rtwdev, 0x410c, 0x3, 0x0);
	rtw_write32(rtwdev, 0x1b00, 0x00000008);
	rtw_write8(rtwdev, 0x1bcc, 0x3f);
	rtw_write32(rtwdev, 0x1b00, 0x0000000a);
	rtw_write8(rtwdev, 0x1bcc, 0x3f);
	rtw_write32_mask(rtwdev, 0x1e24, BIT(31), 0x0);
	rtw_write32_mask(rtwdev, 0x1e28, 0xf, 0x3);
}

static void rtw8822c_dac_cal_adc(struct rtw_dev *rtwdev,
				 u8 path, u32 *adc_ic, u32 *adc_qc)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 ic = 0, qc = 0, temp = 0;
	u32 base_addr;
	u32 path_sel;
	int i;

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] ADCK path(%d)\n", path);

	base_addr = rtw8822c_get_path_write_addr(path);
	switch (path) {
	case RF_PATH_A:
		path_sel = 0xa0000;
		break;
	case RF_PATH_B:
		path_sel = 0x80000;
		break;
	default:
		WARN_ON(1);
		return;
	}

	/* ADCK step1 */
	rtw_write32_mask(rtwdev, base_addr + 0x30, BIT(30), 0x0);
	if (path == RF_PATH_B)
		rtw_write32(rtwdev, base_addr + 0x30, 0x30db8041);
	rtw_write32(rtwdev, base_addr + 0x60, 0xf0040ff0);
	rtw_write32(rtwdev, base_addr + 0x0c, 0xdff00220);
	rtw_write32(rtwdev, base_addr + 0x10, 0x02dd08c4);
	rtw_write32(rtwdev, base_addr + 0x0c, 0x10000260);
	rtw_write_rf(rtwdev, RF_PATH_A, 0x0, RFREG_MASK, 0x10000);
	rtw_write_rf(rtwdev, RF_PATH_B, 0x0, RFREG_MASK, 0x10000);
	for (i = 0; i < 10; i++) {
		rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] ADCK count=%d\n", i);
		rtw_write32(rtwdev, 0x1c3c, path_sel + 0x8003);
		rtw_write32(rtwdev, 0x1c24, 0x00010002);
		rtw8822c_dac_cal_rf_mode(rtwdev, &ic, &qc);
		rtw_dbg(rtwdev, RTW_DBG_RFK,
			"[DACK] before: i=0x%x, q=0x%x\n", ic, qc);

		/* compensation value */
		if (ic != 0x0) {
			ic = 0x400 - ic;
			*adc_ic = ic;
		}
		if (qc != 0x0) {
			qc = 0x400 - qc;
			*adc_qc = qc;
		}
		temp = (ic & 0x3ff) | ((qc & 0x3ff) << 10);
		rtw_write32(rtwdev, base_addr + 0x68, temp);
		dm_info->dack_adck[path] = temp;
		rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] ADCK 0x%08x=0x08%x\n",
			base_addr + 0x68, temp);
		/* check ADC DC offset */
		rtw_write32(rtwdev, 0x1c3c, path_sel + 0x8103);
		rtw8822c_dac_cal_rf_mode(rtwdev, &ic, &qc);
		rtw_dbg(rtwdev, RTW_DBG_RFK,
			"[DACK] after:  i=0x%08x, q=0x%08x\n", ic, qc);
		if (ic >= 0x200)
			ic = 0x400 - ic;
		if (qc >= 0x200)
			qc = 0x400 - qc;
		if (ic < 5 && qc < 5)
			break;
	}

	/* ADCK step2 */
	rtw_write32(rtwdev, 0x1c3c, 0x00000003);
	rtw_write32(rtwdev, base_addr + 0x0c, 0x10000260);
	rtw_write32(rtwdev, base_addr + 0x10, 0x02d508c4);

	/* release pull low switch on IQ path */
	rtw_write_rf(rtwdev, path, 0x8f, BIT(13), 0x1);
}

static void rtw8822c_dac_cal_step1(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 base_addr;
	u32 read_addr;

	base_addr = rtw8822c_get_path_write_addr(path);
	read_addr = rtw8822c_get_path_read_addr(path);

	rtw_write32(rtwdev, base_addr + 0x68, dm_info->dack_adck[path]);
	rtw_write32(rtwdev, base_addr + 0x0c, 0xdff00220);
	if (path == RF_PATH_A) {
		rtw_write32(rtwdev, base_addr + 0x60, 0xf0040ff0);
		rtw_write32(rtwdev, 0x1c38, 0xffffffff);
	}
	rtw_write32(rtwdev, base_addr + 0x10, 0x02d508c5);
	rtw_write32(rtwdev, 0x9b4, 0xdb66db00);
	rtw_write32(rtwdev, base_addr + 0xb0, 0x0a11fb88);
	rtw_write32(rtwdev, base_addr + 0xbc, 0x0008ff81);
	rtw_write32(rtwdev, base_addr + 0xc0, 0x0003d208);
	rtw_write32(rtwdev, base_addr + 0xcc, 0x0a11fb88);
	rtw_write32(rtwdev, base_addr + 0xd8, 0x0008ff81);
	rtw_write32(rtwdev, base_addr + 0xdc, 0x0003d208);
	rtw_write32(rtwdev, base_addr + 0xb8, 0x60000000);
	mdelay(2);
	rtw_write32(rtwdev, base_addr + 0xbc, 0x000aff8d);
	mdelay(2);
	rtw_write32(rtwdev, base_addr + 0xb0, 0x0a11fb89);
	rtw_write32(rtwdev, base_addr + 0xcc, 0x0a11fb89);
	mdelay(1);
	rtw_write32(rtwdev, base_addr + 0xb8, 0x62000000);
	rtw_write32(rtwdev, base_addr + 0xd4, 0x62000000);
	mdelay(20);
	if (!check_hw_ready(rtwdev, read_addr + 0x08, 0x7fff80, 0xffff) ||
	    !check_hw_ready(rtwdev, read_addr + 0x34, 0x7fff80, 0xffff))
		rtw_err(rtwdev, "failed to wait for dack ready\n");
	rtw_write32(rtwdev, base_addr + 0xb8, 0x02000000);
	mdelay(1);
	rtw_write32(rtwdev, base_addr + 0xbc, 0x0008ff87);
	rtw_write32(rtwdev, 0x9b4, 0xdb6db600);
	rtw_write32(rtwdev, base_addr + 0x10, 0x02d508c5);
	rtw_write32(rtwdev, base_addr + 0xbc, 0x0008ff87);
	rtw_write32(rtwdev, base_addr + 0x60, 0xf0000000);
}

static void rtw8822c_dac_cal_step2(struct rtw_dev *rtwdev,
				   u8 path, u32 *ic_out, u32 *qc_out)
{
	u32 base_addr;
	u32 ic, qc, ic_in, qc_in;

	base_addr = rtw8822c_get_path_write_addr(path);
	rtw_write32_mask(rtwdev, base_addr + 0xbc, 0xf0000000, 0x0);
	rtw_write32_mask(rtwdev, base_addr + 0xc0, 0xf, 0x8);
	rtw_write32_mask(rtwdev, base_addr + 0xd8, 0xf0000000, 0x0);
	rtw_write32_mask(rtwdev, base_addr + 0xdc, 0xf, 0x8);

	rtw_write32(rtwdev, 0x1b00, 0x00000008);
	rtw_write8(rtwdev, 0x1bcc, 0x03f);
	rtw_write32(rtwdev, base_addr + 0x0c, 0xdff00220);
	rtw_write32(rtwdev, base_addr + 0x10, 0x02d508c5);
	rtw_write32(rtwdev, 0x1c3c, 0x00088103);

	rtw8822c_dac_cal_rf_mode(rtwdev, &ic_in, &qc_in);
	ic = ic_in;
	qc = qc_in;

	/* compensation value */
	if (ic != 0x0)
		ic = 0x400 - ic;
	if (qc != 0x0)
		qc = 0x400 - qc;
	if (ic < 0x300) {
		ic = ic * 2 * 6 / 5;
		ic = ic + 0x80;
	} else {
		ic = (0x400 - ic) * 2 * 6 / 5;
		ic = 0x7f - ic;
	}
	if (qc < 0x300) {
		qc = qc * 2 * 6 / 5;
		qc = qc + 0x80;
	} else {
		qc = (0x400 - qc) * 2 * 6 / 5;
		qc = 0x7f - qc;
	}

	*ic_out = ic;
	*qc_out = qc;

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] before i=0x%x, q=0x%x\n", ic_in, qc_in);
	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] after  i=0x%x, q=0x%x\n", ic, qc);
}

static void rtw8822c_dac_cal_step3(struct rtw_dev *rtwdev, u8 path,
				   u32 adc_ic, u32 adc_qc,
				   u32 *ic_in, u32 *qc_in,
				   u32 *i_out, u32 *q_out)
{
	u32 base_addr;
	u32 read_addr;
	u32 ic, qc;
	u32 temp;

	base_addr = rtw8822c_get_path_write_addr(path);
	read_addr = rtw8822c_get_path_read_addr(path);
	ic = *ic_in;
	qc = *qc_in;

	rtw_write32(rtwdev, base_addr + 0x0c, 0xdff00220);
	rtw_write32(rtwdev, base_addr + 0x10, 0x02d508c5);
	rtw_write32(rtwdev, 0x9b4, 0xdb66db00);
	rtw_write32(rtwdev, base_addr + 0xb0, 0x0a11fb88);
	rtw_write32(rtwdev, base_addr + 0xbc, 0xc008ff81);
	rtw_write32(rtwdev, base_addr + 0xc0, 0x0003d208);
	rtw_write32_mask(rtwdev, base_addr + 0xbc, 0xf0000000, ic & 0xf);
	rtw_write32_mask(rtwdev, base_addr + 0xc0, 0xf, (ic & 0xf0) >> 4);
	rtw_write32(rtwdev, base_addr + 0xcc, 0x0a11fb88);
	rtw_write32(rtwdev, base_addr + 0xd8, 0xe008ff81);
	rtw_write32(rtwdev, base_addr + 0xdc, 0x0003d208);
	rtw_write32_mask(rtwdev, base_addr + 0xd8, 0xf0000000, qc & 0xf);
	rtw_write32_mask(rtwdev, base_addr + 0xdc, 0xf, (qc & 0xf0) >> 4);
	rtw_write32(rtwdev, base_addr + 0xb8, 0x60000000);
	mdelay(2);
	rtw_write32_mask(rtwdev, base_addr + 0xbc, 0xe, 0x6);
	mdelay(2);
	rtw_write32(rtwdev, base_addr + 0xb0, 0x0a11fb89);
	rtw_write32(rtwdev, base_addr + 0xcc, 0x0a11fb89);
	mdelay(1);
	rtw_write32(rtwdev, base_addr + 0xb8, 0x62000000);
	rtw_write32(rtwdev, base_addr + 0xd4, 0x62000000);
	mdelay(20);
	if (!check_hw_ready(rtwdev, read_addr + 0x24, 0x07f80000, ic) ||
	    !check_hw_ready(rtwdev, read_addr + 0x50, 0x07f80000, qc))
		rtw_err(rtwdev, "failed to write IQ vector to hardware\n");
	rtw_write32(rtwdev, base_addr + 0xb8, 0x02000000);
	mdelay(1);
	rtw_write32_mask(rtwdev, base_addr + 0xbc, 0xe, 0x3);
	rtw_write32(rtwdev, 0x9b4, 0xdb6db600);

	/* check DAC DC offset */
	temp = ((adc_ic + 0x10) & 0x3ff) | (((adc_qc + 0x10) & 0x3ff) << 10);
	rtw_write32(rtwdev, base_addr + 0x68, temp);
	rtw_write32(rtwdev, base_addr + 0x10, 0x02d508c5);
	rtw_write32(rtwdev, base_addr + 0x60, 0xf0000000);
	rtw8822c_dac_cal_rf_mode(rtwdev, &ic, &qc);
	if (ic >= 0x10)
		ic = ic - 0x10;
	else
		ic = 0x400 - (0x10 - ic);

	if (qc >= 0x10)
		qc = qc - 0x10;
	else
		qc = 0x400 - (0x10 - qc);

	*i_out = ic;
	*q_out = qc;

	if (ic >= 0x200)
		ic = 0x400 - ic;
	if (qc >= 0x200)
		qc = 0x400 - qc;

	*ic_in = ic;
	*qc_in = qc;

	rtw_dbg(rtwdev, RTW_DBG_RFK,
		"[DACK] after  DACK i=0x%x, q=0x%x\n", *i_out, *q_out);
}

static void rtw8822c_dac_cal_step4(struct rtw_dev *rtwdev, u8 path)
{
	u32 base_addr = rtw8822c_get_path_write_addr(path);

	rtw_write32(rtwdev, base_addr + 0x68, 0x0);
	rtw_write32(rtwdev, base_addr + 0x10, 0x02d508c4);
	rtw_write32_mask(rtwdev, base_addr + 0xbc, 0x1, 0x0);
	rtw_write32_mask(rtwdev, base_addr + 0x30, BIT(30), 0x1);
}

static void rtw8822c_dac_cal_backup_vec(struct rtw_dev *rtwdev,
					u8 path, u8 vec, u32 w_addr, u32 r_addr)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u16 val;
	u32 i;

	if (WARN_ON(vec >= 2))
		return;

	for (i = 0; i < DACK_MSBK_BACKUP_NUM; i++) {
		rtw_write32_mask(rtwdev, w_addr, 0xf0000000, i);
		val = (u16)rtw_read32_mask(rtwdev, r_addr, 0x7fc0000);
		dm_info->dack_msbk[path][vec][i] = val;
	}
}

static void rtw8822c_dac_cal_backup_path(struct rtw_dev *rtwdev, u8 path)
{
	u32 w_off = 0x1c;
	u32 r_off = 0x2c;
	u32 w_addr, r_addr;

	if (WARN_ON(path >= 2))
		return;

	/* backup I vector */
	w_addr = rtw8822c_get_path_write_addr(path) + 0xb0;
	r_addr = rtw8822c_get_path_read_addr(path) + 0x10;
	rtw8822c_dac_cal_backup_vec(rtwdev, path, 0, w_addr, r_addr);

	/* backup Q vector */
	w_addr = rtw8822c_get_path_write_addr(path) + 0xb0 + w_off;
	r_addr = rtw8822c_get_path_read_addr(path) + 0x10 + r_off;
	rtw8822c_dac_cal_backup_vec(rtwdev, path, 1, w_addr, r_addr);
}

static void rtw8822c_dac_cal_backup_dck(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u8 val;

	val = (u8)rtw_read32_mask(rtwdev, REG_DCKA_I_0, 0xf0000000);
	dm_info->dack_dck[RF_PATH_A][0][0] = val;
	val = (u8)rtw_read32_mask(rtwdev, REG_DCKA_I_1, 0xf);
	dm_info->dack_dck[RF_PATH_A][0][1] = val;
	val = (u8)rtw_read32_mask(rtwdev, REG_DCKA_Q_0, 0xf0000000);
	dm_info->dack_dck[RF_PATH_A][1][0] = val;
	val = (u8)rtw_read32_mask(rtwdev, REG_DCKA_Q_1, 0xf);
	dm_info->dack_dck[RF_PATH_A][1][1] = val;

	val = (u8)rtw_read32_mask(rtwdev, REG_DCKB_I_0, 0xf0000000);
	dm_info->dack_dck[RF_PATH_B][0][0] = val;
	val = (u8)rtw_read32_mask(rtwdev, REG_DCKB_I_1, 0xf);
	dm_info->dack_dck[RF_PATH_B][1][0] = val;
	val = (u8)rtw_read32_mask(rtwdev, REG_DCKB_Q_0, 0xf0000000);
	dm_info->dack_dck[RF_PATH_B][0][1] = val;
	val = (u8)rtw_read32_mask(rtwdev, REG_DCKB_Q_1, 0xf);
	dm_info->dack_dck[RF_PATH_B][1][1] = val;
}

static void rtw8822c_dac_cal_backup(struct rtw_dev *rtwdev)
{
	u32 temp[3];

	temp[0] = rtw_read32(rtwdev, 0x1860);
	temp[1] = rtw_read32(rtwdev, 0x4160);
	temp[2] = rtw_read32(rtwdev, 0x9b4);

	/* set clock */
	rtw_write32(rtwdev, 0x9b4, 0xdb66db00);

	/* backup path-A I/Q */
	rtw_write32_clr(rtwdev, 0x1830, BIT(30));
	rtw_write32_mask(rtwdev, 0x1860, 0xfc000000, 0x3c);
	rtw8822c_dac_cal_backup_path(rtwdev, RF_PATH_A);

	/* backup path-B I/Q */
	rtw_write32_clr(rtwdev, 0x4130, BIT(30));
	rtw_write32_mask(rtwdev, 0x4160, 0xfc000000, 0x3c);
	rtw8822c_dac_cal_backup_path(rtwdev, RF_PATH_B);

	rtw8822c_dac_cal_backup_dck(rtwdev);
	rtw_write32_set(rtwdev, 0x1830, BIT(30));
	rtw_write32_set(rtwdev, 0x4130, BIT(30));

	rtw_write32(rtwdev, 0x1860, temp[0]);
	rtw_write32(rtwdev, 0x4160, temp[1]);
	rtw_write32(rtwdev, 0x9b4, temp[2]);
}

static void rtw8822c_dac_cal_restore_dck(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u8 val;

	rtw_write32_set(rtwdev, REG_DCKA_I_0, BIT(19));
	val = dm_info->dack_dck[RF_PATH_A][0][0];
	rtw_write32_mask(rtwdev, REG_DCKA_I_0, 0xf0000000, val);
	val = dm_info->dack_dck[RF_PATH_A][0][1];
	rtw_write32_mask(rtwdev, REG_DCKA_I_1, 0xf, val);

	rtw_write32_set(rtwdev, REG_DCKA_Q_0, BIT(19));
	val = dm_info->dack_dck[RF_PATH_A][1][0];
	rtw_write32_mask(rtwdev, REG_DCKA_Q_0, 0xf0000000, val);
	val = dm_info->dack_dck[RF_PATH_A][1][1];
	rtw_write32_mask(rtwdev, REG_DCKA_Q_1, 0xf, val);

	rtw_write32_set(rtwdev, REG_DCKB_I_0, BIT(19));
	val = dm_info->dack_dck[RF_PATH_B][0][0];
	rtw_write32_mask(rtwdev, REG_DCKB_I_0, 0xf0000000, val);
	val = dm_info->dack_dck[RF_PATH_B][0][1];
	rtw_write32_mask(rtwdev, REG_DCKB_I_1, 0xf, val);

	rtw_write32_set(rtwdev, REG_DCKB_Q_0, BIT(19));
	val = dm_info->dack_dck[RF_PATH_B][1][0];
	rtw_write32_mask(rtwdev, REG_DCKB_Q_0, 0xf0000000, val);
	val = dm_info->dack_dck[RF_PATH_B][1][1];
	rtw_write32_mask(rtwdev, REG_DCKB_Q_1, 0xf, val);
}

static void rtw8822c_dac_cal_restore_prepare(struct rtw_dev *rtwdev)
{
	rtw_write32(rtwdev, 0x9b4, 0xdb66db00);

	rtw_write32_mask(rtwdev, 0x18b0, BIT(27), 0x0);
	rtw_write32_mask(rtwdev, 0x18cc, BIT(27), 0x0);
	rtw_write32_mask(rtwdev, 0x41b0, BIT(27), 0x0);
	rtw_write32_mask(rtwdev, 0x41cc, BIT(27), 0x0);

	rtw_write32_mask(rtwdev, 0x1830, BIT(30), 0x0);
	rtw_write32_mask(rtwdev, 0x1860, 0xfc000000, 0x3c);
	rtw_write32_mask(rtwdev, 0x18b4, BIT(0), 0x1);
	rtw_write32_mask(rtwdev, 0x18d0, BIT(0), 0x1);

	rtw_write32_mask(rtwdev, 0x4130, BIT(30), 0x0);
	rtw_write32_mask(rtwdev, 0x4160, 0xfc000000, 0x3c);
	rtw_write32_mask(rtwdev, 0x41b4, BIT(0), 0x1);
	rtw_write32_mask(rtwdev, 0x41d0, BIT(0), 0x1);

	rtw_write32_mask(rtwdev, 0x18b0, 0xf00, 0x0);
	rtw_write32_mask(rtwdev, 0x18c0, BIT(14), 0x0);
	rtw_write32_mask(rtwdev, 0x18cc, 0xf00, 0x0);
	rtw_write32_mask(rtwdev, 0x18dc, BIT(14), 0x0);

	rtw_write32_mask(rtwdev, 0x18b0, BIT(0), 0x0);
	rtw_write32_mask(rtwdev, 0x18cc, BIT(0), 0x0);
	rtw_write32_mask(rtwdev, 0x18b0, BIT(0), 0x1);
	rtw_write32_mask(rtwdev, 0x18cc, BIT(0), 0x1);

	rtw8822c_dac_cal_restore_dck(rtwdev);

	rtw_write32_mask(rtwdev, 0x18c0, 0x38000, 0x7);
	rtw_write32_mask(rtwdev, 0x18dc, 0x38000, 0x7);
	rtw_write32_mask(rtwdev, 0x41c0, 0x38000, 0x7);
	rtw_write32_mask(rtwdev, 0x41dc, 0x38000, 0x7);

	rtw_write32_mask(rtwdev, 0x18b8, BIT(26) | BIT(25), 0x1);
	rtw_write32_mask(rtwdev, 0x18d4, BIT(26) | BIT(25), 0x1);

	rtw_write32_mask(rtwdev, 0x41b0, 0xf00, 0x0);
	rtw_write32_mask(rtwdev, 0x41c0, BIT(14), 0x0);
	rtw_write32_mask(rtwdev, 0x41cc, 0xf00, 0x0);
	rtw_write32_mask(rtwdev, 0x41dc, BIT(14), 0x0);

	rtw_write32_mask(rtwdev, 0x41b0, BIT(0), 0x0);
	rtw_write32_mask(rtwdev, 0x41cc, BIT(0), 0x0);
	rtw_write32_mask(rtwdev, 0x41b0, BIT(0), 0x1);
	rtw_write32_mask(rtwdev, 0x41cc, BIT(0), 0x1);

	rtw_write32_mask(rtwdev, 0x41b8, BIT(26) | BIT(25), 0x1);
	rtw_write32_mask(rtwdev, 0x41d4, BIT(26) | BIT(25), 0x1);
}

static bool rtw8822c_dac_cal_restore_wait(struct rtw_dev *rtwdev,
					  u32 target_addr, u32 toggle_addr)
{
	u32 cnt = 0;

	do {
		rtw_write32_mask(rtwdev, toggle_addr, BIT(26) | BIT(25), 0x0);
		rtw_write32_mask(rtwdev, toggle_addr, BIT(26) | BIT(25), 0x2);

		if (rtw_read32_mask(rtwdev, target_addr, 0xf) == 0x6)
			return true;

	} while (cnt++ < 100);

	return false;
}

static bool rtw8822c_dac_cal_restore_path(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 w_off = 0x1c;
	u32 r_off = 0x2c;
	u32 w_i, r_i, w_q, r_q;
	u32 value;
	u32 i;

	w_i = rtw8822c_get_path_write_addr(path) + 0xb0;
	r_i = rtw8822c_get_path_read_addr(path) + 0x08;
	w_q = rtw8822c_get_path_write_addr(path) + 0xb0 + w_off;
	r_q = rtw8822c_get_path_read_addr(path) + 0x08 + r_off;

	if (!rtw8822c_dac_cal_restore_wait(rtwdev, r_i, w_i + 0x8))
		return false;

	for (i = 0; i < DACK_MSBK_BACKUP_NUM; i++) {
		rtw_write32_mask(rtwdev, w_i + 0x4, BIT(2), 0x0);
		value = dm_info->dack_msbk[path][0][i];
		rtw_write32_mask(rtwdev, w_i + 0x4, 0xff8, value);
		rtw_write32_mask(rtwdev, w_i, 0xf0000000, i);
		rtw_write32_mask(rtwdev, w_i + 0x4, BIT(2), 0x1);
	}

	rtw_write32_mask(rtwdev, w_i + 0x4, BIT(2), 0x0);

	if (!rtw8822c_dac_cal_restore_wait(rtwdev, r_q, w_q + 0x8))
		return false;

	for (i = 0; i < DACK_MSBK_BACKUP_NUM; i++) {
		rtw_write32_mask(rtwdev, w_q + 0x4, BIT(2), 0x0);
		value = dm_info->dack_msbk[path][1][i];
		rtw_write32_mask(rtwdev, w_q + 0x4, 0xff8, value);
		rtw_write32_mask(rtwdev, w_q, 0xf0000000, i);
		rtw_write32_mask(rtwdev, w_q + 0x4, BIT(2), 0x1);
	}
	rtw_write32_mask(rtwdev, w_q + 0x4, BIT(2), 0x0);

	rtw_write32_mask(rtwdev, w_i + 0x8, BIT(26) | BIT(25), 0x0);
	rtw_write32_mask(rtwdev, w_q + 0x8, BIT(26) | BIT(25), 0x0);
	rtw_write32_mask(rtwdev, w_i + 0x4, BIT(0), 0x0);
	rtw_write32_mask(rtwdev, w_q + 0x4, BIT(0), 0x0);

	return true;
}

static bool __rtw8822c_dac_cal_restore(struct rtw_dev *rtwdev)
{
	if (!rtw8822c_dac_cal_restore_path(rtwdev, RF_PATH_A))
		return false;

	if (!rtw8822c_dac_cal_restore_path(rtwdev, RF_PATH_B))
		return false;

	return true;
}

static bool rtw8822c_dac_cal_restore(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 temp[3];

	/* sample the first element for both path's IQ vector */
	if (dm_info->dack_msbk[RF_PATH_A][0][0] == 0 &&
	    dm_info->dack_msbk[RF_PATH_A][1][0] == 0 &&
	    dm_info->dack_msbk[RF_PATH_B][0][0] == 0 &&
	    dm_info->dack_msbk[RF_PATH_B][1][0] == 0)
		return false;

	temp[0] = rtw_read32(rtwdev, 0x1860);
	temp[1] = rtw_read32(rtwdev, 0x4160);
	temp[2] = rtw_read32(rtwdev, 0x9b4);

	rtw8822c_dac_cal_restore_prepare(rtwdev);
	if (!check_hw_ready(rtwdev, 0x2808, 0x7fff80, 0xffff) ||
	    !check_hw_ready(rtwdev, 0x2834, 0x7fff80, 0xffff) ||
	    !check_hw_ready(rtwdev, 0x4508, 0x7fff80, 0xffff) ||
	    !check_hw_ready(rtwdev, 0x4534, 0x7fff80, 0xffff))
		return false;

	if (!__rtw8822c_dac_cal_restore(rtwdev)) {
		rtw_err(rtwdev, "failed to restore dack vectors\n");
		return false;
	}

	rtw_write32_mask(rtwdev, 0x1830, BIT(30), 0x1);
	rtw_write32_mask(rtwdev, 0x4130, BIT(30), 0x1);
	rtw_write32(rtwdev, 0x1860, temp[0]);
	rtw_write32(rtwdev, 0x4160, temp[1]);
	rtw_write32_mask(rtwdev, 0x18b0, BIT(27), 0x1);
	rtw_write32_mask(rtwdev, 0x18cc, BIT(27), 0x1);
	rtw_write32_mask(rtwdev, 0x41b0, BIT(27), 0x1);
	rtw_write32_mask(rtwdev, 0x41cc, BIT(27), 0x1);
	rtw_write32(rtwdev, 0x9b4, temp[2]);

	return true;
}

static void rtw8822c_rf_dac_cal(struct rtw_dev *rtwdev)
{
	struct rtw_backup_info backup_rf[DACK_RF_8822C * DACK_PATH_8822C];
	struct rtw_backup_info backup[DACK_REG_8822C];
	u32 ic = 0, qc = 0, i;
	u32 i_a = 0x0, q_a = 0x0, i_b = 0x0, q_b = 0x0;
	u32 ic_a = 0x0, qc_a = 0x0, ic_b = 0x0, qc_b = 0x0;
	u32 adc_ic_a = 0x0, adc_qc_a = 0x0, adc_ic_b = 0x0, adc_qc_b = 0x0;

	if (rtw8822c_dac_cal_restore(rtwdev))
		return;

	/* not able to restore, do it */

	rtw8822c_dac_backup_reg(rtwdev, backup, backup_rf);

	rtw8822c_dac_bb_setting(rtwdev);

	/* path-A */
	rtw8822c_dac_cal_adc(rtwdev, RF_PATH_A, &adc_ic_a, &adc_qc_a);
	for (i = 0; i < 10; i++) {
		rtw8822c_dac_cal_step1(rtwdev, RF_PATH_A);
		rtw8822c_dac_cal_step2(rtwdev, RF_PATH_A, &ic, &qc);
		ic_a = ic;
		qc_a = qc;

		rtw8822c_dac_cal_step3(rtwdev, RF_PATH_A, adc_ic_a, adc_qc_a,
				       &ic, &qc, &i_a, &q_a);

		if (ic < 5 && qc < 5)
			break;
	}
	rtw8822c_dac_cal_step4(rtwdev, RF_PATH_A);

	/* path-B */
	rtw8822c_dac_cal_adc(rtwdev, RF_PATH_B, &adc_ic_b, &adc_qc_b);
	for (i = 0; i < 10; i++) {
		rtw8822c_dac_cal_step1(rtwdev, RF_PATH_B);
		rtw8822c_dac_cal_step2(rtwdev, RF_PATH_B, &ic, &qc);
		ic_b = ic;
		qc_b = qc;

		rtw8822c_dac_cal_step3(rtwdev, RF_PATH_B, adc_ic_b, adc_qc_b,
				       &ic, &qc, &i_b, &q_b);

		if (ic < 5 && qc < 5)
			break;
	}
	rtw8822c_dac_cal_step4(rtwdev, RF_PATH_B);

	rtw_write32(rtwdev, 0x1b00, 0x00000008);
	rtw_write32_mask(rtwdev, 0x4130, BIT(30), 0x1);
	rtw_write8(rtwdev, 0x1bcc, 0x0);
	rtw_write32(rtwdev, 0x1b00, 0x0000000a);
	rtw_write8(rtwdev, 0x1bcc, 0x0);

	rtw8822c_dac_restore_reg(rtwdev, backup, backup_rf);

	/* backup results to restore, saving a lot of time */
	rtw8822c_dac_cal_backup(rtwdev);

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] path A: ic=0x%x, qc=0x%x\n", ic_a, qc_a);
	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] path B: ic=0x%x, qc=0x%x\n", ic_b, qc_b);
	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] path A: i=0x%x, q=0x%x\n", i_a, q_a);
	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DACK] path B: i=0x%x, q=0x%x\n", i_b, q_b);
}

static void rtw8822c_rf_x2_check(struct rtw_dev *rtwdev)
{
	u8 x2k_busy;

	mdelay(1);
	x2k_busy = rtw_read_rf(rtwdev, RF_PATH_A, 0xb8, BIT(15));
	if (x2k_busy == 1) {
		rtw_write_rf(rtwdev, RF_PATH_A, 0xb8, RFREG_MASK, 0xC4440);
		rtw_write_rf(rtwdev, RF_PATH_A, 0xba, RFREG_MASK, 0x6840D);
		rtw_write_rf(rtwdev, RF_PATH_A, 0xb8, RFREG_MASK, 0x80440);
		mdelay(1);
	}
}

static void rtw8822c_rf_init(struct rtw_dev *rtwdev)
{
	rtw8822c_rf_dac_cal(rtwdev);
	rtw8822c_rf_x2_check(rtwdev);
}

static void rtw8822c_phy_set_param(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	struct rtw_hal *hal = &rtwdev->hal;
	u8 crystal_cap;
	u8 cck_gi_u_bnd_msb = 0;
	u8 cck_gi_u_bnd_lsb = 0;
	u8 cck_gi_l_bnd_msb = 0;
	u8 cck_gi_l_bnd_lsb = 0;
	bool is_tx2_path;

	/* power on BB/RF domain */
	rtw_write8_set(rtwdev, REG_SYS_FUNC_EN,
		       BIT_FEN_BB_GLB_RST | BIT_FEN_BB_RSTB);
	rtw_write8_set(rtwdev, REG_RF_CTRL,
		       BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
	rtw_write32_set(rtwdev, REG_WLRF1, BIT_WLRF1_BBRF_EN);

	/* disable low rate DPD */
	rtw_write32_mask(rtwdev, REG_DIS_DPD, DIS_DPD_MASK, DIS_DPD_RATEALL);

	/* pre init before header files config */
	rtw8822c_header_file_init(rtwdev, true);

	rtw_phy_load_tables(rtwdev);

	crystal_cap = rtwdev->efuse.crystal_cap & 0x7f;
	rtw_write32_mask(rtwdev, REG_ANAPAR_XTAL_0, 0xfffc00,
			 crystal_cap | (crystal_cap << 7));

	/* post init after header files config */
	rtw8822c_header_file_init(rtwdev, false);

	is_tx2_path = false;
	rtw8822c_config_trx_mode(rtwdev, hal->antenna_tx, hal->antenna_rx,
				 is_tx2_path);
	rtw_phy_init(rtwdev);

	cck_gi_u_bnd_msb = (u8)rtw_read32_mask(rtwdev, 0x1a98, 0xc000);
	cck_gi_u_bnd_lsb = (u8)rtw_read32_mask(rtwdev, 0x1aa8, 0xf0000);
	cck_gi_l_bnd_msb = (u8)rtw_read32_mask(rtwdev, 0x1a98, 0xc0);
	cck_gi_l_bnd_lsb = (u8)rtw_read32_mask(rtwdev, 0x1a70, 0x0f000000);

	dm_info->cck_gi_u_bnd = ((cck_gi_u_bnd_msb << 4) | (cck_gi_u_bnd_lsb));
	dm_info->cck_gi_l_bnd = ((cck_gi_l_bnd_msb << 4) | (cck_gi_l_bnd_lsb));

	rtw8822c_rf_init(rtwdev);
}

#define WLAN_TXQ_RPT_EN		0x1F
#define WLAN_SLOT_TIME		0x09
#define WLAN_PIFS_TIME		0x1C
#define WLAN_SIFS_CCK_CONT_TX	0x0A
#define WLAN_SIFS_OFDM_CONT_TX	0x0E
#define WLAN_SIFS_CCK_TRX	0x0A
#define WLAN_SIFS_OFDM_TRX	0x10
#define WLAN_NAV_MAX		0xC8
#define WLAN_RDG_NAV		0x05
#define WLAN_TXOP_NAV		0x1B
#define WLAN_CCK_RX_TSF		0x30
#define WLAN_OFDM_RX_TSF	0x30
#define WLAN_TBTT_PROHIBIT	0x04 /* unit : 32us */
#define WLAN_TBTT_HOLD_TIME	0x064 /* unit : 32us */
#define WLAN_DRV_EARLY_INT	0x04
#define WLAN_BCN_CTRL_CLT0	0x10
#define WLAN_BCN_DMA_TIME	0x02
#define WLAN_BCN_MAX_ERR	0xFF
#define WLAN_SIFS_CCK_DUR_TUNE	0x0A
#define WLAN_SIFS_OFDM_DUR_TUNE	0x10
#define WLAN_SIFS_CCK_CTX	0x0A
#define WLAN_SIFS_CCK_IRX	0x0A
#define WLAN_SIFS_OFDM_CTX	0x0E
#define WLAN_SIFS_OFDM_IRX	0x0E
#define WLAN_EIFS_DUR_TUNE	0x40
#define WLAN_EDCA_VO_PARAM	0x002FA226
#define WLAN_EDCA_VI_PARAM	0x005EA328
#define WLAN_EDCA_BE_PARAM	0x005EA42B
#define WLAN_EDCA_BK_PARAM	0x0000A44F

#define WLAN_RX_FILTER0		0xFFFFFFFF
#define WLAN_RX_FILTER2		0xFFFF
#define WLAN_RCR_CFG		0xE400220E
#define WLAN_RXPKT_MAX_SZ	12288
#define WLAN_RXPKT_MAX_SZ_512	(WLAN_RXPKT_MAX_SZ >> 9)

#define WLAN_AMPDU_MAX_TIME		0x70
#define WLAN_RTS_LEN_TH			0xFF
#define WLAN_RTS_TX_TIME_TH		0x08
#define WLAN_MAX_AGG_PKT_LIMIT		0x20
#define WLAN_RTS_MAX_AGG_PKT_LIMIT	0x20
#define WLAN_PRE_TXCNT_TIME_TH		0x1E0
#define FAST_EDCA_VO_TH		0x06
#define FAST_EDCA_VI_TH		0x06
#define FAST_EDCA_BE_TH		0x06
#define FAST_EDCA_BK_TH		0x06
#define WLAN_BAR_RETRY_LIMIT		0x01
#define WLAN_BAR_ACK_TYPE		0x05
#define WLAN_RA_TRY_RATE_AGG_LIMIT	0x08
#define WLAN_RESP_TXRATE		0x84
#define WLAN_ACK_TO			0x21
#define WLAN_ACK_TO_CCK			0x6A
#define WLAN_DATA_RATE_FB_CNT_1_4	0x01000000
#define WLAN_DATA_RATE_FB_CNT_5_8	0x08070504
#define WLAN_RTS_RATE_FB_CNT_5_8	0x08070504
#define WLAN_DATA_RATE_FB_RATE0		0xFE01F010
#define WLAN_DATA_RATE_FB_RATE0_H	0x40000000
#define WLAN_RTS_RATE_FB_RATE1		0x003FF010
#define WLAN_RTS_RATE_FB_RATE1_H	0x40000000
#define WLAN_RTS_RATE_FB_RATE4		0x0600F010
#define WLAN_RTS_RATE_FB_RATE4_H	0x400003E0
#define WLAN_RTS_RATE_FB_RATE5		0x0600F015
#define WLAN_RTS_RATE_FB_RATE5_H	0x000000E0

#define WLAN_TX_FUNC_CFG1		0x30
#define WLAN_TX_FUNC_CFG2		0x30
#define WLAN_MAC_OPT_NORM_FUNC1		0x98
#define WLAN_MAC_OPT_LB_FUNC1		0x80
#define WLAN_MAC_OPT_FUNC2		0x30810041
#define WLAN_MAC_INT_MIG_CFG		0x33330000

#define WLAN_SIFS_CFG	(WLAN_SIFS_CCK_CONT_TX | \
			(WLAN_SIFS_OFDM_CONT_TX << BIT_SHIFT_SIFS_OFDM_CTX) | \
			(WLAN_SIFS_CCK_TRX << BIT_SHIFT_SIFS_CCK_TRX) | \
			(WLAN_SIFS_OFDM_TRX << BIT_SHIFT_SIFS_OFDM_TRX))

#define WLAN_SIFS_DUR_TUNE	(WLAN_SIFS_CCK_DUR_TUNE | \
				(WLAN_SIFS_OFDM_DUR_TUNE << 8))

#define WLAN_TBTT_TIME	(WLAN_TBTT_PROHIBIT |\
			(WLAN_TBTT_HOLD_TIME << BIT_SHIFT_TBTT_HOLD_TIME_AP))

#define WLAN_NAV_CFG		(WLAN_RDG_NAV | (WLAN_TXOP_NAV << 16))
#define WLAN_RX_TSF_CFG		(WLAN_CCK_RX_TSF | (WLAN_OFDM_RX_TSF) << 8)

#define MAC_CLK_SPEED	80 /* 80M */
#define EFUSE_PCB_INFO_OFFSET	0xCA

static int rtw8822c_mac_init(struct rtw_dev *rtwdev)
{
	u8 value8;
	u16 value16;
	u32 value32;
	u16 pre_txcnt;

	/* txq control */
	value8 = rtw_read8(rtwdev, REG_FWHW_TXQ_CTRL);
	value8 |= (BIT(7) & ~BIT(1) & ~BIT(2));
	rtw_write8(rtwdev, REG_FWHW_TXQ_CTRL, value8);
	rtw_write8(rtwdev, REG_FWHW_TXQ_CTRL + 1, WLAN_TXQ_RPT_EN);
	/* sifs control */
	rtw_write16(rtwdev, REG_SPEC_SIFS, WLAN_SIFS_DUR_TUNE);
	rtw_write32(rtwdev, REG_SIFS, WLAN_SIFS_CFG);
	rtw_write16(rtwdev, REG_RESP_SIFS_CCK,
		    WLAN_SIFS_CCK_CTX | WLAN_SIFS_CCK_IRX << 8);
	rtw_write16(rtwdev, REG_RESP_SIFS_OFDM,
		    WLAN_SIFS_OFDM_CTX | WLAN_SIFS_OFDM_IRX << 8);
	/* rate fallback control */
	rtw_write32(rtwdev, REG_DARFRC, WLAN_DATA_RATE_FB_CNT_1_4);
	rtw_write32(rtwdev, REG_DARFRCH, WLAN_DATA_RATE_FB_CNT_5_8);
	rtw_write32(rtwdev, REG_RARFRCH, WLAN_RTS_RATE_FB_CNT_5_8);
	rtw_write32(rtwdev, REG_ARFR0, WLAN_DATA_RATE_FB_RATE0);
	rtw_write32(rtwdev, REG_ARFRH0, WLAN_DATA_RATE_FB_RATE0_H);
	rtw_write32(rtwdev, REG_ARFR1_V1, WLAN_RTS_RATE_FB_RATE1);
	rtw_write32(rtwdev, REG_ARFRH1_V1, WLAN_RTS_RATE_FB_RATE1_H);
	rtw_write32(rtwdev, REG_ARFR4, WLAN_RTS_RATE_FB_RATE4);
	rtw_write32(rtwdev, REG_ARFRH4, WLAN_RTS_RATE_FB_RATE4_H);
	rtw_write32(rtwdev, REG_ARFR5, WLAN_RTS_RATE_FB_RATE5);
	rtw_write32(rtwdev, REG_ARFRH5, WLAN_RTS_RATE_FB_RATE5_H);
	/* protocol configuration */
	rtw_write8(rtwdev, REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
	rtw_write8_set(rtwdev, REG_TX_HANG_CTRL, BIT_EN_EOF_V1);
	pre_txcnt = WLAN_PRE_TXCNT_TIME_TH | BIT_EN_PRECNT;
	rtw_write8(rtwdev, REG_PRECNT_CTRL, (u8)(pre_txcnt & 0xFF));
	rtw_write8(rtwdev, REG_PRECNT_CTRL + 1, (u8)(pre_txcnt >> 8));
	value32 = WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
		  (WLAN_MAX_AGG_PKT_LIMIT << 16) |
		  (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24);
	rtw_write32(rtwdev, REG_PROT_MODE_CTRL, value32);
	rtw_write16(rtwdev, REG_BAR_MODE_CTRL + 2,
		    WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING, FAST_EDCA_VO_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_VOVI_SETTING + 2, FAST_EDCA_VI_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING, FAST_EDCA_BE_TH);
	rtw_write8(rtwdev, REG_FAST_EDCA_BEBK_SETTING + 2, FAST_EDCA_BK_TH);
	/* close BA parser */
	rtw_write8_clr(rtwdev, REG_LIFETIME_EN, BIT_BA_PARSER_EN);
	rtw_write32_clr(rtwdev, REG_RRSR, BITS_RRSR_RSC);

	/* EDCA configuration */
	rtw_write32(rtwdev, REG_EDCA_VO_PARAM, WLAN_EDCA_VO_PARAM);
	rtw_write32(rtwdev, REG_EDCA_VI_PARAM, WLAN_EDCA_VI_PARAM);
	rtw_write32(rtwdev, REG_EDCA_BE_PARAM, WLAN_EDCA_BE_PARAM);
	rtw_write32(rtwdev, REG_EDCA_BK_PARAM, WLAN_EDCA_BK_PARAM);
	rtw_write8(rtwdev, REG_PIFS, WLAN_PIFS_TIME);
	rtw_write8_clr(rtwdev, REG_TX_PTCL_CTRL + 1, BIT_SIFS_BK_EN >> 8);
	rtw_write8_set(rtwdev, REG_RD_CTRL + 1,
		       (BIT_DIS_TXOP_CFE | BIT_DIS_LSIG_CFE |
			BIT_DIS_STBC_CFE) >> 8);

	/* MAC clock configuration */
	rtw_write32_clr(rtwdev, REG_AFE_CTRL1, BIT_MAC_CLK_SEL);
	rtw_write8(rtwdev, REG_USTIME_TSF, MAC_CLK_SPEED);
	rtw_write8(rtwdev, REG_USTIME_EDCA, MAC_CLK_SPEED);

	rtw_write8_set(rtwdev, REG_MISC_CTRL,
		       BIT_EN_FREE_CNT | BIT_DIS_SECOND_CCA);
	rtw_write8_clr(rtwdev, REG_TIMER0_SRC_SEL, BIT_TSFT_SEL_TIMER0);
	rtw_write16(rtwdev, REG_TXPAUSE, 0x0000);
	rtw_write8(rtwdev, REG_SLOT, WLAN_SLOT_TIME);
	rtw_write32(rtwdev, REG_RD_NAV_NXT, WLAN_NAV_CFG);
	rtw_write16(rtwdev, REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
	/* Set beacon cotnrol - enable TSF and other related functions */
	rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);
	/* Set send beacon related registers */
	rtw_write32(rtwdev, REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
	rtw_write8(rtwdev, REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
	rtw_write8(rtwdev, REG_BCN_CTRL_CLINT0, WLAN_BCN_CTRL_CLT0);
	rtw_write8(rtwdev, REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
	rtw_write8(rtwdev, REG_BCN_MAX_ERR, WLAN_BCN_MAX_ERR);

	/* WMAC configuration */
	rtw_write8(rtwdev, REG_BBPSF_CTRL + 2, WLAN_RESP_TXRATE);
	rtw_write8(rtwdev, REG_ACKTO, WLAN_ACK_TO);
	rtw_write8(rtwdev, REG_ACKTO_CCK, WLAN_ACK_TO_CCK);
	rtw_write16(rtwdev, REG_EIFS, WLAN_EIFS_DUR_TUNE);
	rtw_write8(rtwdev, REG_NAV_CTRL + 2, WLAN_NAV_MAX);
	rtw_write8(rtwdev, REG_WMAC_TRXPTCL_CTL_H  + 2, WLAN_BAR_ACK_TYPE);
	rtw_write32(rtwdev, REG_RXFLTMAP0, WLAN_RX_FILTER0);
	rtw_write16(rtwdev, REG_RXFLTMAP2, WLAN_RX_FILTER2);
	rtw_write32(rtwdev, REG_RCR, WLAN_RCR_CFG);
	rtw_write8(rtwdev, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
	rtw_write8(rtwdev, REG_TCR + 2, WLAN_TX_FUNC_CFG2);
	rtw_write8(rtwdev, REG_TCR + 1, WLAN_TX_FUNC_CFG1);
	rtw_write32_set(rtwdev, REG_GENERAL_OPTION, BIT_DUMMY_FCS_READY_MASK_EN);
	rtw_write32(rtwdev, REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
	rtw_write8(rtwdev, REG_WMAC_OPTION_FUNCTION_1, WLAN_MAC_OPT_NORM_FUNC1);

	/* init low power */
	value16 = rtw_read16(rtwdev, REG_RXPSF_CTRL + 2) & 0xF00F;
	value16 |= (BIT_RXGCK_VHT_FIFOTHR(1) | BIT_RXGCK_HT_FIFOTHR(1) |
		    BIT_RXGCK_OFDM_FIFOTHR(1) | BIT_RXGCK_CCK_FIFOTHR(1)) >> 16;
	rtw_write16(rtwdev, REG_RXPSF_CTRL + 2, value16);
	value16 = 0;
	value16 = BIT_SET_RXPSF_PKTLENTHR(value16, 1);
	value16 |= BIT_RXPSF_CTRLEN | BIT_RXPSF_VHTCHKEN | BIT_RXPSF_HTCHKEN
		| BIT_RXPSF_OFDMCHKEN | BIT_RXPSF_CCKCHKEN
		| BIT_RXPSF_OFDMRST;
	rtw_write16(rtwdev, REG_RXPSF_CTRL, value16);
	rtw_write32(rtwdev, REG_RXPSF_TYPE_CTRL, 0xFFFFFFFF);
	/* rx ignore configuration */
	value16 = rtw_read16(rtwdev, REG_RXPSF_CTRL);
	value16 &= ~(BIT_RXPSF_MHCHKEN | BIT_RXPSF_CCKRST |
		     BIT_RXPSF_CONT_ERRCHKEN);
	value16 = BIT_SET_RXPSF_ERRTHR(value16, 0x07);
	rtw_write16(rtwdev, REG_RXPSF_CTRL, value16);

	/* Interrupt migration configuration */
	rtw_write32(rtwdev, REG_INT_MIG, WLAN_MAC_INT_MIG_CFG);

	return 0;
}

static void rtw8822c_set_channel_rf(struct rtw_dev *rtwdev, u8 channel, u8 bw)
{
#define RF18_BAND_MASK		(BIT(16) | BIT(9) | BIT(8))
#define RF18_BAND_2G		(0)
#define RF18_BAND_5G		(BIT(16) | BIT(8))
#define RF18_CHANNEL_MASK	(MASKBYTE0)
#define RF18_RFSI_MASK		(BIT(18) | BIT(17))
#define RF18_RFSI_GE_CH80	(BIT(17))
#define RF18_RFSI_GT_CH140	(BIT(18))
#define RF18_BW_MASK		(BIT(13) | BIT(12))
#define RF18_BW_20M		(BIT(13) | BIT(12))
#define RF18_BW_40M		(BIT(13))
#define RF18_BW_80M		(BIT(12))

	u32 rf_reg18 = 0;
	u32 rf_rxbb = 0;

	rf_reg18 = rtw_read_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK);

	rf_reg18 &= ~(RF18_BAND_MASK | RF18_CHANNEL_MASK | RF18_RFSI_MASK |
		      RF18_BW_MASK);

	rf_reg18 |= (channel <= 14 ? RF18_BAND_2G : RF18_BAND_5G);
	rf_reg18 |= (channel & RF18_CHANNEL_MASK);
	if (channel > 144)
		rf_reg18 |= RF18_RFSI_GT_CH140;
	else if (channel >= 80)
		rf_reg18 |= RF18_RFSI_GE_CH80;

	switch (bw) {
	case RTW_CHANNEL_WIDTH_5:
	case RTW_CHANNEL_WIDTH_10:
	case RTW_CHANNEL_WIDTH_20:
	default:
		rf_reg18 |= RF18_BW_20M;
		rf_rxbb = 0x18;
		break;
	case RTW_CHANNEL_WIDTH_40:
		/* RF bandwidth */
		rf_reg18 |= RF18_BW_40M;
		rf_rxbb = 0x10;
		break;
	case RTW_CHANNEL_WIDTH_80:
		rf_reg18 |= RF18_BW_80M;
		rf_rxbb = 0x8;
		break;
	}

	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE2, 0x04, 0x01);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWA, 0x1f, 0x12);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWD0, 0xfffff, rf_rxbb);
	rtw_write_rf(rtwdev, RF_PATH_A, RF_LUTWE2, 0x04, 0x00);

	rtw_write_rf(rtwdev, RF_PATH_B, RF_LUTWE2, 0x04, 0x01);
	rtw_write_rf(rtwdev, RF_PATH_B, RF_LUTWA, 0x1f, 0x12);
	rtw_write_rf(rtwdev, RF_PATH_B, RF_LUTWD0, 0xfffff, rf_rxbb);
	rtw_write_rf(rtwdev, RF_PATH_B, RF_LUTWE2, 0x04, 0x00);

	rtw_write_rf(rtwdev, RF_PATH_A, RF_CFGCH, RFREG_MASK, rf_reg18);
	rtw_write_rf(rtwdev, RF_PATH_B, RF_CFGCH, RFREG_MASK, rf_reg18);
}

static void rtw8822c_toggle_igi(struct rtw_dev *rtwdev)
{
	u32 igi;

	igi = rtw_read32_mask(rtwdev, REG_RXIGI, 0x7f);
	rtw_write32_mask(rtwdev, REG_RXIGI, 0x7f, igi - 2);
	rtw_write32_mask(rtwdev, REG_RXIGI, 0x7f00, igi - 2);
	rtw_write32_mask(rtwdev, REG_RXIGI, 0x7f, igi);
	rtw_write32_mask(rtwdev, REG_RXIGI, 0x7f00, igi);
}

static void rtw8822c_set_channel_bb(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				    u8 primary_ch_idx)
{
	if (channel <= 14) {
		rtw_write32_clr(rtwdev, REG_BGCTRL, BITS_RX_IQ_WEIGHT);
		rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0xf0000000, 0x8);
		rtw_write32_set(rtwdev, REG_TXF4, BIT(20));
		rtw_write32_clr(rtwdev, REG_CCK_CHECK, BIT_CHECK_CCK_EN);
		rtw_write32_clr(rtwdev, REG_CCKTXONLY, BIT_BB_CCK_CHECK_EN);
		rtw_write32_mask(rtwdev, REG_CCAMSK, 0x3F000000, 0xF);

		switch (bw) {
		case RTW_CHANNEL_WIDTH_20:
			rtw_write32_mask(rtwdev, REG_RXAGCCTL0, BITS_RXAGC_CCK,
					 0x5);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL, BITS_RXAGC_CCK,
					 0x5);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL0, BITS_RXAGC_OFDM,
					 0x6);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL, BITS_RXAGC_OFDM,
					 0x6);
			break;
		case RTW_CHANNEL_WIDTH_40:
			rtw_write32_mask(rtwdev, REG_RXAGCCTL0, BITS_RXAGC_CCK,
					 0x4);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL, BITS_RXAGC_CCK,
					 0x4);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL0, BITS_RXAGC_OFDM,
					 0x0);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL, BITS_RXAGC_OFDM,
					 0x0);
			break;
		}
		if (channel == 13 || channel == 14)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x969);
		else if (channel == 11 || channel == 12)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x96a);
		else
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x9aa);
		if (channel == 14) {
			rtw_write32_mask(rtwdev, REG_TXF0, MASKHWORD, 0x3da0);
			rtw_write32_mask(rtwdev, REG_TXF1, MASKDWORD,
					 0x4962c931);
			rtw_write32_mask(rtwdev, REG_TXF2, MASKLWORD, 0x6aa3);
			rtw_write32_mask(rtwdev, REG_TXF3, MASKHWORD, 0xaa7b);
			rtw_write32_mask(rtwdev, REG_TXF4, MASKLWORD, 0xf3d7);
			rtw_write32_mask(rtwdev, REG_TXF5, MASKDWORD, 0x0);
			rtw_write32_mask(rtwdev, REG_TXF6, MASKDWORD,
					 0xff012455);
			rtw_write32_mask(rtwdev, REG_TXF7, MASKDWORD, 0xffff);
		} else {
			rtw_write32_mask(rtwdev, REG_TXF0, MASKHWORD, 0x5284);
			rtw_write32_mask(rtwdev, REG_TXF1, MASKDWORD,
					 0x3e18fec8);
			rtw_write32_mask(rtwdev, REG_TXF2, MASKLWORD, 0x0a88);
			rtw_write32_mask(rtwdev, REG_TXF3, MASKHWORD, 0xacc4);
			rtw_write32_mask(rtwdev, REG_TXF4, MASKLWORD, 0xc8b2);
			rtw_write32_mask(rtwdev, REG_TXF5, MASKDWORD,
					 0x00faf0de);
			rtw_write32_mask(rtwdev, REG_TXF6, MASKDWORD,
					 0x00122344);
			rtw_write32_mask(rtwdev, REG_TXF7, MASKDWORD,
					 0x0fffffff);
		}
		if (channel == 13)
			rtw_write32_mask(rtwdev, REG_TXDFIR0, 0x70, 0x3);
		else
			rtw_write32_mask(rtwdev, REG_TXDFIR0, 0x70, 0x1);
	} else if (channel > 35) {
		rtw_write32_set(rtwdev, REG_CCKTXONLY, BIT_BB_CCK_CHECK_EN);
		rtw_write32_set(rtwdev, REG_CCK_CHECK, BIT_CHECK_CCK_EN);
		rtw_write32_set(rtwdev, REG_BGCTRL, BITS_RX_IQ_WEIGHT);
		rtw_write32_clr(rtwdev, REG_TXF4, BIT(20));
		rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0xf0000000, 0x0);
		rtw_write32_mask(rtwdev, REG_CCAMSK, 0x3F000000, 0x22);
		rtw_write32_mask(rtwdev, REG_TXDFIR0, 0x70, 0x3);
		if (channel >= 36 && channel <= 64) {
			rtw_write32_mask(rtwdev, REG_RXAGCCTL0, BITS_RXAGC_OFDM,
					 0x1);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL, BITS_RXAGC_OFDM,
					 0x1);
		} else if (channel >= 100 && channel <= 144) {
			rtw_write32_mask(rtwdev, REG_RXAGCCTL0, BITS_RXAGC_OFDM,
					 0x2);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL, BITS_RXAGC_OFDM,
					 0x2);
		} else if (channel >= 149) {
			rtw_write32_mask(rtwdev, REG_RXAGCCTL0, BITS_RXAGC_OFDM,
					 0x3);
			rtw_write32_mask(rtwdev, REG_RXAGCCTL, BITS_RXAGC_OFDM,
					 0x3);
		}

		if (channel >= 36 && channel <= 51)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x494);
		else if (channel >= 52 && channel <= 55)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x493);
		else if (channel >= 56 && channel <= 111)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x453);
		else if (channel >= 112 && channel <= 119)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x452);
		else if (channel >= 120 && channel <= 172)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x412);
		else if (channel >= 173 && channel <= 177)
			rtw_write32_mask(rtwdev, REG_SCOTRK, 0xfff, 0x411);
	}

	switch (bw) {
	case RTW_CHANNEL_WIDTH_20:
		rtw_write32_mask(rtwdev, REG_DFIRBW, 0x3FF0, 0x19B);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xf, 0x0);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xffc0, 0x0);
		rtw_write32_mask(rtwdev, REG_TXCLK, 0x700, 0x7);
		rtw_write32_mask(rtwdev, REG_TXCLK, 0x700000, 0x6);
		rtw_write32_mask(rtwdev, REG_CCK_SOURCE, BIT_NBI_EN, 0x0);
		rtw_write32_mask(rtwdev, REG_SBD, BITS_SUBTUNE, 0x1);
		rtw_write32_mask(rtwdev, REG_PT_CHSMO, BIT_PT_OPT, 0x0);
		break;
	case RTW_CHANNEL_WIDTH_40:
		rtw_write32_mask(rtwdev, REG_CCKSB, BIT(4),
				 (primary_ch_idx == 1 ? 1 : 0));
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xf, 0x5);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xc0, 0x0);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xff00,
				 (primary_ch_idx | (primary_ch_idx << 4)));
		rtw_write32_mask(rtwdev, REG_CCK_SOURCE, BIT_NBI_EN, 0x1);
		rtw_write32_mask(rtwdev, REG_SBD, BITS_SUBTUNE, 0x1);
		rtw_write32_mask(rtwdev, REG_PT_CHSMO, BIT_PT_OPT, 0x1);
		break;
	case RTW_CHANNEL_WIDTH_80:
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xf, 0xa);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xc0, 0x0);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xff00,
				 (primary_ch_idx | (primary_ch_idx << 4)));
		rtw_write32_mask(rtwdev, REG_SBD, BITS_SUBTUNE, 0x6);
		rtw_write32_mask(rtwdev, REG_PT_CHSMO, BIT_PT_OPT, 0x1);
		break;
	case RTW_CHANNEL_WIDTH_5:
		rtw_write32_mask(rtwdev, REG_DFIRBW, 0x3FF0, 0x2AB);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xf, 0x0);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xffc0, 0x1);
		rtw_write32_mask(rtwdev, REG_TXCLK, 0x700, 0x4);
		rtw_write32_mask(rtwdev, REG_TXCLK, 0x700000, 0x4);
		rtw_write32_mask(rtwdev, REG_CCK_SOURCE, BIT_NBI_EN, 0x0);
		rtw_write32_mask(rtwdev, REG_SBD, BITS_SUBTUNE, 0x1);
		rtw_write32_mask(rtwdev, REG_PT_CHSMO, BIT_PT_OPT, 0x0);
		break;
	case RTW_CHANNEL_WIDTH_10:
		rtw_write32_mask(rtwdev, REG_DFIRBW, 0x3FF0, 0x2AB);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xf, 0x0);
		rtw_write32_mask(rtwdev, REG_TXBWCTL, 0xffc0, 0x2);
		rtw_write32_mask(rtwdev, REG_TXCLK, 0x700, 0x6);
		rtw_write32_mask(rtwdev, REG_TXCLK, 0x700000, 0x5);
		rtw_write32_mask(rtwdev, REG_CCK_SOURCE, BIT_NBI_EN, 0x0);
		rtw_write32_mask(rtwdev, REG_SBD, BITS_SUBTUNE, 0x1);
		rtw_write32_mask(rtwdev, REG_PT_CHSMO, BIT_PT_OPT, 0x0);
		break;
	}
}

static void rtw8822c_set_channel(struct rtw_dev *rtwdev, u8 channel, u8 bw,
				 u8 primary_chan_idx)
{
	rtw8822c_set_channel_bb(rtwdev, channel, bw, primary_chan_idx);
	rtw_set_channel_mac(rtwdev, channel, bw, primary_chan_idx);
	rtw8822c_set_channel_rf(rtwdev, channel, bw);
	rtw8822c_toggle_igi(rtwdev);
}

static void rtw8822c_config_cck_rx_path(struct rtw_dev *rtwdev, u8 rx_path)
{
	if (rx_path == BB_PATH_A || rx_path == BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_CCANRX, 0x00060000, 0x0);
		rtw_write32_mask(rtwdev, REG_CCANRX, 0x00600000, 0x0);
	} else if (rx_path == BB_PATH_AB) {
		rtw_write32_mask(rtwdev, REG_CCANRX, 0x00600000, 0x1);
		rtw_write32_mask(rtwdev, REG_CCANRX, 0x00060000, 0x1);
	}

	if (rx_path == BB_PATH_A)
		rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0x0f000000, 0x0);
	else if (rx_path == BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0x0f000000, 0x5);
	else if (rx_path == BB_PATH_AB)
		rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0x0f000000, 0x1);
}

static void rtw8822c_config_ofdm_rx_path(struct rtw_dev *rtwdev, u8 rx_path)
{
	if (rx_path == BB_PATH_A || rx_path == BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_RXFNCTL, 0x300, 0x0);
		rtw_write32_mask(rtwdev, REG_RXFNCTL, 0x600000, 0x0);
		rtw_write32_mask(rtwdev, REG_AGCSWSH, BIT(17), 0x0);
		rtw_write32_mask(rtwdev, REG_ANTWTPD, BIT(20), 0x0);
		rtw_write32_mask(rtwdev, REG_MRCM, BIT(24), 0x0);
	} else if (rx_path == BB_PATH_AB) {
		rtw_write32_mask(rtwdev, REG_RXFNCTL, 0x300, 0x1);
		rtw_write32_mask(rtwdev, REG_RXFNCTL, 0x600000, 0x1);
		rtw_write32_mask(rtwdev, REG_AGCSWSH, BIT(17), 0x1);
		rtw_write32_mask(rtwdev, REG_ANTWTPD, BIT(20), 0x1);
		rtw_write32_mask(rtwdev, REG_MRCM, BIT(24), 0x1);
	}

	rtw_write32_mask(rtwdev, 0x824, 0x0f000000, rx_path);
	rtw_write32_mask(rtwdev, 0x824, 0x000f0000, rx_path);
}

static void rtw8822c_config_rx_path(struct rtw_dev *rtwdev, u8 rx_path)
{
	rtw8822c_config_cck_rx_path(rtwdev, rx_path);
	rtw8822c_config_ofdm_rx_path(rtwdev, rx_path);
}

static void rtw8822c_config_cck_tx_path(struct rtw_dev *rtwdev, u8 tx_path,
					bool is_tx2_path)
{
	if (tx_path == BB_PATH_A) {
		rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0xf0000000, 0x8);
	} else if (tx_path == BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0xf0000000, 0x4);
	} else {
		if (is_tx2_path)
			rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0xf0000000, 0xc);
		else
			rtw_write32_mask(rtwdev, REG_RXCCKSEL, 0xf0000000, 0x8);
	}
}

static void rtw8822c_config_ofdm_tx_path(struct rtw_dev *rtwdev, u8 tx_path,
					 bool is_tx2_path)
{
	if (tx_path == BB_PATH_A) {
		rtw_write32_mask(rtwdev, REG_ANTMAP0, 0xff, 0x11);
		rtw_write32_mask(rtwdev, REG_TXLGMAP, 0xff, 0x0);
	} else if (tx_path == BB_PATH_B) {
		rtw_write32_mask(rtwdev, REG_ANTMAP0, 0xff, 0x12);
		rtw_write32_mask(rtwdev, REG_TXLGMAP, 0xff, 0x0);
	} else {
		if (is_tx2_path) {
			rtw_write32_mask(rtwdev, REG_ANTMAP0, 0xff, 0x33);
			rtw_write32_mask(rtwdev, REG_TXLGMAP, 0xffff, 0x0404);
		} else {
			rtw_write32_mask(rtwdev, REG_ANTMAP0, 0xff, 0x31);
			rtw_write32_mask(rtwdev, REG_TXLGMAP, 0xffff, 0x0400);
		}
	}
}

static void rtw8822c_config_tx_path(struct rtw_dev *rtwdev, u8 tx_path,
				    bool is_tx2_path)
{
	rtw8822c_config_cck_tx_path(rtwdev, tx_path, is_tx2_path);
	rtw8822c_config_ofdm_tx_path(rtwdev, tx_path, is_tx2_path);
}

static void rtw8822c_config_trx_mode(struct rtw_dev *rtwdev, u8 tx_path,
				     u8 rx_path, bool is_tx2_path)
{
	if ((tx_path | rx_path) & BB_PATH_A)
		rtw_write32_mask(rtwdev, REG_ORITXCODE, MASK20BITS, 0x33312);
	else
		rtw_write32_mask(rtwdev, REG_ORITXCODE, MASK20BITS, 0x11111);
	if ((tx_path | rx_path) & BB_PATH_B)
		rtw_write32_mask(rtwdev, REG_ORITXCODE2, MASK20BITS, 0x33312);
	else
		rtw_write32_mask(rtwdev, REG_ORITXCODE2, MASK20BITS, 0x11111);

	rtw8822c_config_rx_path(rtwdev, rx_path);
	rtw8822c_config_tx_path(rtwdev, tx_path, is_tx2_path);

	rtw8822c_toggle_igi(rtwdev);
}

static void query_phy_status_page0(struct rtw_dev *rtwdev, u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u8 l_bnd, u_bnd;
	u8 gain_a, gain_b;
	s8 rx_power[RTW_RF_PATH_MAX];
	s8 min_rx_power = -120;

	rx_power[RF_PATH_A] = GET_PHY_STAT_P0_PWDB_A(phy_status);
	rx_power[RF_PATH_B] = GET_PHY_STAT_P0_PWDB_B(phy_status);
	l_bnd = dm_info->cck_gi_l_bnd;
	u_bnd = dm_info->cck_gi_u_bnd;
	gain_a = GET_PHY_STAT_P0_GAIN_A(phy_status);
	gain_b = GET_PHY_STAT_P0_GAIN_B(phy_status);
	if (gain_a < l_bnd)
		rx_power[RF_PATH_A] += (l_bnd - gain_a) << 1;
	else if (gain_a > u_bnd)
		rx_power[RF_PATH_A] -= (gain_a - u_bnd) << 1;
	if (gain_b < l_bnd)
		rx_power[RF_PATH_B] += (l_bnd - gain_b) << 1;
	else if (gain_b > u_bnd)
		rx_power[RF_PATH_B] -= (gain_b - u_bnd) << 1;

	rx_power[RF_PATH_A] -= 110;
	rx_power[RF_PATH_B] -= 110;

	pkt_stat->rx_power[RF_PATH_A] = rx_power[RF_PATH_A];
	pkt_stat->rx_power[RF_PATH_B] = rx_power[RF_PATH_B];

	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 1);
	pkt_stat->bw = RTW_CHANNEL_WIDTH_20;
	pkt_stat->signal_power = max(pkt_stat->rx_power[RF_PATH_A],
				     min_rx_power);
}

static void query_phy_status_page1(struct rtw_dev *rtwdev, u8 *phy_status,
				   struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 rxsc, bw;
	s8 min_rx_power = -120;

	if (pkt_stat->rate > DESC_RATE11M && pkt_stat->rate < DESC_RATEMCS0)
		rxsc = GET_PHY_STAT_P1_L_RXSC(phy_status);
	else
		rxsc = GET_PHY_STAT_P1_HT_RXSC(phy_status);

	if (rxsc >= 9 && rxsc <= 12)
		bw = RTW_CHANNEL_WIDTH_40;
	else if (rxsc >= 13)
		bw = RTW_CHANNEL_WIDTH_80;
	else
		bw = RTW_CHANNEL_WIDTH_20;

	pkt_stat->rx_power[RF_PATH_A] = GET_PHY_STAT_P1_PWDB_A(phy_status) - 110;
	pkt_stat->rx_power[RF_PATH_B] = GET_PHY_STAT_P1_PWDB_B(phy_status) - 110;
	pkt_stat->rssi = rtw_phy_rf_power_2_rssi(pkt_stat->rx_power, 2);
	pkt_stat->bw = bw;
	pkt_stat->signal_power = max3(pkt_stat->rx_power[RF_PATH_A],
				      pkt_stat->rx_power[RF_PATH_B],
				      min_rx_power);
}

static void query_phy_status(struct rtw_dev *rtwdev, u8 *phy_status,
			     struct rtw_rx_pkt_stat *pkt_stat)
{
	u8 page;

	page = *phy_status & 0xf;

	switch (page) {
	case 0:
		query_phy_status_page0(rtwdev, phy_status, pkt_stat);
		break;
	case 1:
		query_phy_status_page1(rtwdev, phy_status, pkt_stat);
		break;
	default:
		rtw_warn(rtwdev, "unused phy status page (%d)\n", page);
		return;
	}
}

static void rtw8822c_query_rx_desc(struct rtw_dev *rtwdev, u8 *rx_desc,
				   struct rtw_rx_pkt_stat *pkt_stat,
				   struct ieee80211_rx_status *rx_status)
{
	struct ieee80211_hdr *hdr;
	u32 desc_sz = rtwdev->chip->rx_pkt_desc_sz;
	u8 *phy_status = NULL;

	memset(pkt_stat, 0, sizeof(*pkt_stat));

	pkt_stat->phy_status = GET_RX_DESC_PHYST(rx_desc);
	pkt_stat->icv_err = GET_RX_DESC_ICV_ERR(rx_desc);
	pkt_stat->crc_err = GET_RX_DESC_CRC32(rx_desc);
	pkt_stat->decrypted = !GET_RX_DESC_SWDEC(rx_desc);
	pkt_stat->is_c2h = GET_RX_DESC_C2H(rx_desc);
	pkt_stat->pkt_len = GET_RX_DESC_PKT_LEN(rx_desc);
	pkt_stat->drv_info_sz = GET_RX_DESC_DRV_INFO_SIZE(rx_desc);
	pkt_stat->shift = GET_RX_DESC_SHIFT(rx_desc);
	pkt_stat->rate = GET_RX_DESC_RX_RATE(rx_desc);
	pkt_stat->cam_id = GET_RX_DESC_MACID(rx_desc);
	pkt_stat->ppdu_cnt = GET_RX_DESC_PPDU_CNT(rx_desc);
	pkt_stat->tsf_low = GET_RX_DESC_TSFL(rx_desc);

	/* drv_info_sz is in unit of 8-bytes */
	pkt_stat->drv_info_sz *= 8;

	/* c2h cmd pkt's rx/phy status is not interested */
	if (pkt_stat->is_c2h)
		return;

	hdr = (struct ieee80211_hdr *)(rx_desc + desc_sz + pkt_stat->shift +
				       pkt_stat->drv_info_sz);
	if (pkt_stat->phy_status) {
		phy_status = rx_desc + desc_sz + pkt_stat->shift;
		query_phy_status(rtwdev, phy_status, pkt_stat);
	}

	rtw_rx_fill_rx_status(rtwdev, pkt_stat, hdr, rx_status, phy_status);
}

static void
rtw8822c_set_write_tx_power_ref(struct rtw_dev *rtwdev, u8 *tx_pwr_ref_cck,
				u8 *tx_pwr_ref_ofdm)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u32 txref_cck[2] = {0x18a0, 0x41a0};
	u32 txref_ofdm[2] = {0x18e8, 0x41e8};
	u8 path;

	for (path = 0; path < hal->rf_path_num; path++) {
		rtw_write32_mask(rtwdev, 0x1c90, BIT(15), 0);
		rtw_write32_mask(rtwdev, txref_cck[path], 0x7f0000,
				 tx_pwr_ref_cck[path]);
	}
	for (path = 0; path < hal->rf_path_num; path++) {
		rtw_write32_mask(rtwdev, 0x1c90, BIT(15), 0);
		rtw_write32_mask(rtwdev, txref_ofdm[path], 0x1fc00,
				 tx_pwr_ref_ofdm[path]);
	}
}

static void rtw8822c_set_tx_power_diff(struct rtw_dev *rtwdev, u8 rate,
				       s8 *diff_idx)
{
	u32 offset_txagc = 0x3a00;
	u8 rate_idx = rate & 0xfc;
	u8 pwr_idx[4];
	u32 phy_pwr_idx;
	int i;

	for (i = 0; i < 4; i++)
		pwr_idx[i] = diff_idx[i] & 0x7f;

	phy_pwr_idx = pwr_idx[0] |
		      (pwr_idx[1] << 8) |
		      (pwr_idx[2] << 16) |
		      (pwr_idx[3] << 24);

	rtw_write32_mask(rtwdev, 0x1c90, BIT(15), 0x0);
	rtw_write32_mask(rtwdev, offset_txagc + rate_idx, MASKDWORD,
			 phy_pwr_idx);
}

static void rtw8822c_set_tx_power_index(struct rtw_dev *rtwdev)
{
	struct rtw_hal *hal = &rtwdev->hal;
	u8 rs, rate, j;
	u8 pwr_ref_cck[2] = {hal->tx_pwr_tbl[RF_PATH_A][DESC_RATE11M],
			     hal->tx_pwr_tbl[RF_PATH_B][DESC_RATE11M]};
	u8 pwr_ref_ofdm[2] = {hal->tx_pwr_tbl[RF_PATH_A][DESC_RATEMCS7],
			      hal->tx_pwr_tbl[RF_PATH_B][DESC_RATEMCS7]};
	s8 diff_a, diff_b;
	u8 pwr_a, pwr_b;
	s8 diff_idx[4];

	rtw8822c_set_write_tx_power_ref(rtwdev, pwr_ref_cck, pwr_ref_ofdm);
	for (rs = 0; rs < RTW_RATE_SECTION_MAX; rs++) {
		for (j = 0; j < rtw_rate_size[rs]; j++) {
			rate = rtw_rate_section[rs][j];
			pwr_a = hal->tx_pwr_tbl[RF_PATH_A][rate];
			pwr_b = hal->tx_pwr_tbl[RF_PATH_B][rate];
			if (rs == 0) {
				diff_a = (s8)pwr_a - (s8)pwr_ref_cck[0];
				diff_b = (s8)pwr_b - (s8)pwr_ref_cck[1];
			} else {
				diff_a = (s8)pwr_a - (s8)pwr_ref_ofdm[0];
				diff_b = (s8)pwr_b - (s8)pwr_ref_ofdm[1];
			}
			diff_idx[rate % 4] = min(diff_a, diff_b);
			if (rate % 4 == 3)
				rtw8822c_set_tx_power_diff(rtwdev, rate - 3,
							   diff_idx);
		}
	}
}

static void rtw8822c_cfg_ldo25(struct rtw_dev *rtwdev, bool enable)
{
	u8 ldo_pwr;

	ldo_pwr = rtw_read8(rtwdev, REG_ANAPARLDO_POW_MAC);
	ldo_pwr = enable ? ldo_pwr | BIT_LDOE25_PON : ldo_pwr & ~BIT_LDOE25_PON;
	rtw_write8(rtwdev, REG_ANAPARLDO_POW_MAC, ldo_pwr);
}

static void rtw8822c_false_alarm_statistics(struct rtw_dev *rtwdev)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	u32 cck_enable;
	u32 cck_fa_cnt;
	u32 crc32_cnt;
	u32 ofdm_fa_cnt;
	u32 ofdm_fa_cnt1, ofdm_fa_cnt2, ofdm_fa_cnt3, ofdm_fa_cnt4, ofdm_fa_cnt5;
	u16 parity_fail, rate_illegal, crc8_fail, mcs_fail, sb_search_fail,
	    fast_fsync, crc8_fail_vhta, mcs_fail_vht;

	cck_enable = rtw_read32(rtwdev, REG_ENCCK) & BIT_CCK_BLK_EN;
	cck_fa_cnt = rtw_read16(rtwdev, REG_CCK_FACNT);

	ofdm_fa_cnt1 = rtw_read32(rtwdev, REG_OFDM_FACNT1);
	ofdm_fa_cnt2 = rtw_read32(rtwdev, REG_OFDM_FACNT2);
	ofdm_fa_cnt3 = rtw_read32(rtwdev, REG_OFDM_FACNT3);
	ofdm_fa_cnt4 = rtw_read32(rtwdev, REG_OFDM_FACNT4);
	ofdm_fa_cnt5 = rtw_read32(rtwdev, REG_OFDM_FACNT5);

	parity_fail	= FIELD_GET(GENMASK(31, 16), ofdm_fa_cnt1);
	rate_illegal	= FIELD_GET(GENMASK(15, 0), ofdm_fa_cnt2);
	crc8_fail	= FIELD_GET(GENMASK(31, 16), ofdm_fa_cnt2);
	crc8_fail_vhta	= FIELD_GET(GENMASK(15, 0), ofdm_fa_cnt3);
	mcs_fail	= FIELD_GET(GENMASK(15, 0), ofdm_fa_cnt4);
	mcs_fail_vht	= FIELD_GET(GENMASK(31, 16), ofdm_fa_cnt4);
	fast_fsync	= FIELD_GET(GENMASK(15, 0), ofdm_fa_cnt5);
	sb_search_fail	= FIELD_GET(GENMASK(31, 16), ofdm_fa_cnt5);

	ofdm_fa_cnt = parity_fail + rate_illegal + crc8_fail + crc8_fail_vhta +
		      mcs_fail + mcs_fail_vht + fast_fsync + sb_search_fail;

	dm_info->cck_fa_cnt = cck_fa_cnt;
	dm_info->ofdm_fa_cnt = ofdm_fa_cnt;
	dm_info->total_fa_cnt = ofdm_fa_cnt;
	dm_info->total_fa_cnt += cck_enable ? cck_fa_cnt : 0;

	crc32_cnt = rtw_read32(rtwdev, 0x2c04);
	dm_info->cck_ok_cnt = crc32_cnt & 0xffff;
	dm_info->cck_err_cnt = (crc32_cnt & 0xffff0000) >> 16;
	crc32_cnt = rtw_read32(rtwdev, 0x2c14);
	dm_info->ofdm_ok_cnt = crc32_cnt & 0xffff;
	dm_info->ofdm_err_cnt = (crc32_cnt & 0xffff0000) >> 16;
	crc32_cnt = rtw_read32(rtwdev, 0x2c10);
	dm_info->ht_ok_cnt = crc32_cnt & 0xffff;
	dm_info->ht_err_cnt = (crc32_cnt & 0xffff0000) >> 16;
	crc32_cnt = rtw_read32(rtwdev, 0x2c0c);
	dm_info->vht_ok_cnt = crc32_cnt & 0xffff;
	dm_info->vht_err_cnt = (crc32_cnt & 0xffff0000) >> 16;

	rtw_write32_mask(rtwdev, REG_CCANRX, BIT_CCK_FA_RST, 0);
	rtw_write32_mask(rtwdev, REG_CCANRX, BIT_CCK_FA_RST, 2);
	rtw_write32_mask(rtwdev, REG_CCANRX, BIT_OFDM_FA_RST, 0);
	rtw_write32_mask(rtwdev, REG_CCANRX, BIT_OFDM_FA_RST, 2);

	/* disable rx clk gating to reset counters */
	rtw_write32_clr(rtwdev, REG_RX_BREAK, BIT_COM_RX_GCK_EN);
	rtw_write32_set(rtwdev, REG_CNT_CTRL, BIT_ALL_CNT_RST);
	rtw_write32_clr(rtwdev, REG_CNT_CTRL, BIT_ALL_CNT_RST);
	rtw_write32_set(rtwdev, REG_RX_BREAK, BIT_COM_RX_GCK_EN);
}

static void rtw8822c_do_iqk(struct rtw_dev *rtwdev)
{
	struct rtw_iqk_para para = {0};
	u8 iqk_chk;
	int counter;

	para.clear = 1;
	rtw_fw_do_iqk(rtwdev, &para);

	for (counter = 0; counter < 300; counter++) {
		iqk_chk = rtw_read8(rtwdev, REG_RPT_CIP);
		if (iqk_chk == 0xaa)
			break;
		msleep(20);
	}
	rtw_write8(rtwdev, REG_IQKSTAT, 0x0);

	rtw_dbg(rtwdev, RTW_DBG_RFK, "iqk counter=%d\n", counter);
}

/* for coex */
static void rtw8822c_coex_cfg_init(struct rtw_dev *rtwdev)
{
	/* enable TBTT nterrupt */
	rtw_write8_set(rtwdev, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);

	/* BT report packet sample rate	 */
	/* 0x790[5:0]=0x5 */
	rtw_write8_set(rtwdev, REG_BT_TDMA_TIME, 0x05);

	/* enable BT counter statistics */
	rtw_write8(rtwdev, REG_BT_STAT_CTRL, 0x1);

	/* enable PTA (3-wire function form BT side) */
	rtw_write32_set(rtwdev, REG_GPIO_MUXCFG, BIT_BT_PTA_EN);
	rtw_write32_set(rtwdev, REG_GPIO_MUXCFG, BIT_BT_AOD_GPIO3);

	/* enable PTA (tx/rx signal form WiFi side) */
	rtw_write8_set(rtwdev, REG_QUEUE_CTRL, BIT_PTA_WL_TX_EN);
	/* wl tx signal to PTA not case EDCCA */
	rtw_write8_clr(rtwdev, REG_QUEUE_CTRL, BIT_PTA_EDCCA_EN);
	/* GNT_BT=1 while select both */
	rtw_write8_set(rtwdev, REG_BT_COEX_V2, BIT_GNT_BT_POLARITY);
	/* BT_CCA = ~GNT_WL_BB, (not or GNT_BT_BB, LTE_Rx */
	rtw_write8_clr(rtwdev, REG_DUMMY_PAGE4_V1, BIT_BTCCA_CTRL);

	/* to avoid RF parameter error */
	rtw_write_rf(rtwdev, RF_PATH_B, 0x1, 0xfffff, 0x40000);
}

static void rtw8822c_coex_cfg_gnt_fix(struct rtw_dev *rtwdev)
{
	struct rtw_coex *coex = &rtwdev->coex;
	struct rtw_coex_stat *coex_stat = &coex->stat;
	struct rtw_efuse *efuse = &rtwdev->efuse;
	u32 rf_0x1;

	if (coex_stat->gnt_workaround_state == coex_stat->wl_coex_mode)
		return;

	coex_stat->gnt_workaround_state = coex_stat->wl_coex_mode;

	if ((coex_stat->kt_ver == 0 && coex->under_5g) || coex->freerun)
		rf_0x1 = 0x40021;
	else
		rf_0x1 = 0x40000;

	/* BT at S1 for Shared-Ant */
	if (efuse->share_ant)
		rf_0x1 |= BIT(13);

	rtw_write_rf(rtwdev, RF_PATH_B, 0x1, 0xfffff, rf_0x1);

	/* WL-S0 2G RF TRX cannot be masked by GNT_BT
	 * enable "WLS0 BB chage RF mode if GNT_BT = 1" for shared-antenna type
	 * disable:0x1860[3] = 1, enable:0x1860[3] = 0
	 *
	 * enable "DAC off if GNT_WL = 0" for non-shared-antenna
	 * disable 0x1c30[22] = 0,
	 * enable: 0x1c30[22] = 1, 0x1c38[12] = 0, 0x1c38[28] = 1
	 *
	 * disable WL-S1 BB chage RF mode if GNT_BT
	 * since RF TRx mask can do it
	 */
	rtw_write8_mask(rtwdev, 0x1c32, BIT(6), 1);
	rtw_write8_mask(rtwdev, 0x1c39, BIT(4), 0);
	rtw_write8_mask(rtwdev, 0x1c3b, BIT(4), 1);
	rtw_write8_mask(rtwdev, 0x4160, BIT(3), 1);

	/* disable WL-S0 BB chage RF mode if wifi is at 5G,
	 * or antenna path is separated
	 */
	if (coex_stat->wl_coex_mode == COEX_WLINK_5G ||
	    coex->under_5g || !efuse->share_ant) {
		if (coex_stat->kt_ver >= 3) {
			rtw_write8_mask(rtwdev, 0x1860, BIT(3), 0);
			rtw_write8_mask(rtwdev, 0x1ca7, BIT(3), 1);
		} else {
			rtw_write8_mask(rtwdev, 0x1860, BIT(3), 1);
		}
	} else {
		/* shared-antenna */
		rtw_write8_mask(rtwdev, 0x1860, BIT(3), 0);
		if (coex_stat->kt_ver >= 3)
			rtw_write8_mask(rtwdev, 0x1ca7, BIT(3), 0);
	}
}

static void rtw8822c_coex_cfg_gnt_debug(struct rtw_dev *rtwdev)
{
	rtw_write8_mask(rtwdev, 0x66, BIT(4), 0);
	rtw_write8_mask(rtwdev, 0x67, BIT(0), 0);
	rtw_write8_mask(rtwdev, 0x42, BIT(3), 0);
	rtw_write8_mask(rtwdev, 0x65, BIT(7), 0);
	rtw_write8_mask(rtwdev, 0x73, BIT(3), 0);
}

static void rtw8822c_coex_cfg_rfe_type(struct rtw_dev *rtwdev)
{
	struct rtw_coex *coex = &rtwdev->coex;
	struct rtw_coex_rfe *coex_rfe = &coex->rfe;
	struct rtw_efuse *efuse = &rtwdev->efuse;

	coex_rfe->rfe_module_type = rtwdev->efuse.rfe_option;
	coex_rfe->ant_switch_polarity = 0;
	coex_rfe->ant_switch_exist = false;
	coex_rfe->ant_switch_with_bt = false;
	coex_rfe->ant_switch_diversity = false;

	if (efuse->share_ant)
		coex_rfe->wlg_at_btg = true;
	else
		coex_rfe->wlg_at_btg = false;

	/* disable LTE coex in wifi side */
	rtw_coex_write_indirect_reg(rtwdev, 0x38, BIT_LTE_COEX_EN, 0x0);
	rtw_coex_write_indirect_reg(rtwdev, 0xa0, MASKLWORD, 0xffff);
	rtw_coex_write_indirect_reg(rtwdev, 0xa4, MASKLWORD, 0xffff);
}

static void rtw8822c_coex_cfg_wl_tx_power(struct rtw_dev *rtwdev, u8 wl_pwr)
{
	struct rtw_coex *coex = &rtwdev->coex;
	struct rtw_coex_dm *coex_dm = &coex->dm;

	if (wl_pwr == coex_dm->cur_wl_pwr_lvl)
		return;

	coex_dm->cur_wl_pwr_lvl = wl_pwr;
}

static void rtw8822c_coex_cfg_wl_rx_gain(struct rtw_dev *rtwdev, bool low_gain)
{
	struct rtw_coex *coex = &rtwdev->coex;
	struct rtw_coex_dm *coex_dm = &coex->dm;

	if (low_gain == coex_dm->cur_wl_rx_low_gain_en)
		return;

	coex_dm->cur_wl_rx_low_gain_en = low_gain;

	if (coex_dm->cur_wl_rx_low_gain_en) {
		/* set Rx filter corner RCK offset */
		rtw_write_rf(rtwdev, RF_PATH_A, 0xde, 0xfffff, 0x22);
		rtw_write_rf(rtwdev, RF_PATH_A, 0x1d, 0xfffff, 0x36);
		rtw_write_rf(rtwdev, RF_PATH_B, 0xde, 0xfffff, 0x22);
		rtw_write_rf(rtwdev, RF_PATH_B, 0x1d, 0xfffff, 0x36);
	} else {
		/* set Rx filter corner RCK offset */
		rtw_write_rf(rtwdev, RF_PATH_A, 0xde, 0xfffff, 0x20);
		rtw_write_rf(rtwdev, RF_PATH_A, 0x1d, 0xfffff, 0x0);
		rtw_write_rf(rtwdev, RF_PATH_B, 0x1d, 0xfffff, 0x0);
	}
}

struct dpk_cfg_pair {
	u32 addr;
	u32 bitmask;
	u32 data;
};

void rtw8822c_parse_tbl_dpk(struct rtw_dev *rtwdev,
			    const struct rtw_table *tbl)
{
	const struct dpk_cfg_pair *p = tbl->data;
	const struct dpk_cfg_pair *end = p + tbl->size / 3;

	BUILD_BUG_ON(sizeof(struct dpk_cfg_pair) != sizeof(u32) * 3);

	for (; p < end; p++)
		rtw_write32_mask(rtwdev, p->addr, p->bitmask, p->data);
}

static void rtw8822c_dpk_set_gnt_wl(struct rtw_dev *rtwdev, bool is_before_k)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;

	if (is_before_k) {
		dpk_info->gnt_control = rtw_read32(rtwdev, 0x70);
		dpk_info->gnt_value = rtw_coex_read_indirect_reg(rtwdev, 0x38);
		rtw_write32_mask(rtwdev, 0x70, BIT(26), 0x1);
		rtw_coex_write_indirect_reg(rtwdev, 0x38, MASKBYTE1, 0x77);
	} else {
		rtw_coex_write_indirect_reg(rtwdev, 0x38, MASKDWORD,
					    dpk_info->gnt_value);
		rtw_write32(rtwdev, 0x70, dpk_info->gnt_control);
	}
}

static void
rtw8822c_dpk_restore_registers(struct rtw_dev *rtwdev, u32 reg_num,
			       struct rtw_backup_info *bckp)
{
	rtw_restore_reg(rtwdev, bckp, reg_num);
	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0xc);
	rtw_write32_mask(rtwdev, REG_RXSRAM_CTL, BIT_DPD_CLK, 0x4);
}

static void
rtw8822c_dpk_backup_registers(struct rtw_dev *rtwdev, u32 *reg,
			      u32 reg_num, struct rtw_backup_info *bckp)
{
	u32 i;

	for (i = 0; i < reg_num; i++) {
		bckp[i].len = 4;
		bckp[i].reg = reg[i];
		bckp[i].val = rtw_read32(rtwdev, reg[i]);
	}
}

static void rtw8822c_dpk_backup_rf_registers(struct rtw_dev *rtwdev,
					     u32 *rf_reg,
					     u32 rf_reg_bak[][2])
{
	u32 i;

	for (i = 0; i < DPK_RF_REG_NUM; i++) {
		rf_reg_bak[i][RF_PATH_A] = rtw_read_rf(rtwdev, RF_PATH_A,
						       rf_reg[i], RFREG_MASK);
		rf_reg_bak[i][RF_PATH_B] = rtw_read_rf(rtwdev, RF_PATH_B,
						       rf_reg[i], RFREG_MASK);
	}
}

static void rtw8822c_dpk_reload_rf_registers(struct rtw_dev *rtwdev,
					     u32 *rf_reg,
					     u32 rf_reg_bak[][2])
{
	u32 i;

	for (i = 0; i < DPK_RF_REG_NUM; i++) {
		rtw_write_rf(rtwdev, RF_PATH_A, rf_reg[i], RFREG_MASK,
			     rf_reg_bak[i][RF_PATH_A]);
		rtw_write_rf(rtwdev, RF_PATH_B, rf_reg[i], RFREG_MASK,
			     rf_reg_bak[i][RF_PATH_B]);
	}
}

static void rtw8822c_dpk_information(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u32  reg;
	u8 band_shift;

	reg = rtw_read_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK);

	band_shift = FIELD_GET(BIT(16), reg);
	dpk_info->dpk_band = 1 << band_shift;
	dpk_info->dpk_ch = FIELD_GET(0xff, reg);
	dpk_info->dpk_bw = FIELD_GET(0x3000, reg);
}

static void rtw8822c_dpk_rxbb_dc_cal(struct rtw_dev *rtwdev, u8 path)
{
	rtw_write_rf(rtwdev, path, 0x92, RFREG_MASK, 0x84800);
	udelay(5);
	rtw_write_rf(rtwdev, path, 0x92, RFREG_MASK, 0x84801);
	usleep_range(600, 610);
	rtw_write_rf(rtwdev, path, 0x92, RFREG_MASK, 0x84800);
}

static u8 rtw8822c_dpk_dc_corr_check(struct rtw_dev *rtwdev, u8 path)
{
	u16 dc_i, dc_q;
	u8 corr_val, corr_idx;

	rtw_write32(rtwdev, REG_RXSRAM_CTL, 0x000900f0);
	dc_i = (u16)rtw_read32_mask(rtwdev, REG_STAT_RPT, GENMASK(27, 16));
	dc_q = (u16)rtw_read32_mask(rtwdev, REG_STAT_RPT, GENMASK(11, 0));

	if (dc_i & BIT(11))
		dc_i = 0x1000 - dc_i;
	if (dc_q & BIT(11))
		dc_q = 0x1000 - dc_q;

	rtw_write32(rtwdev, REG_RXSRAM_CTL, 0x000000f0);
	corr_idx = (u8)rtw_read32_mask(rtwdev, REG_STAT_RPT, GENMASK(7, 0));
	corr_val = (u8)rtw_read32_mask(rtwdev, REG_STAT_RPT, GENMASK(15, 8));

	if (dc_i > 200 || dc_q > 200 || corr_idx < 40 || corr_idx > 65)
		return 1;
	else
		return 0;

}

static void rtw8822c_dpk_tx_pause(struct rtw_dev *rtwdev)
{
	u8 reg_a, reg_b;
	u16 count = 0;

	rtw_write8(rtwdev, 0x522, 0xff);
	rtw_write32_mask(rtwdev, 0x1e70, 0xf, 0x2);

	do {
		reg_a = (u8)rtw_read_rf(rtwdev, RF_PATH_A, 0x00, 0xf0000);
		reg_b = (u8)rtw_read_rf(rtwdev, RF_PATH_B, 0x00, 0xf0000);
		udelay(2);
		count++;
	} while ((reg_a == 2 || reg_b == 2) && count < 2500);
}

static void rtw8822c_dpk_mac_bb_setting(struct rtw_dev *rtwdev)
{
	rtw8822c_dpk_tx_pause(rtwdev);
	rtw_load_table(rtwdev, &rtw8822c_dpk_mac_bb_tbl);
}

static void rtw8822c_dpk_afe_setting(struct rtw_dev *rtwdev, bool is_do_dpk)
{
	if (is_do_dpk)
		rtw_load_table(rtwdev, &rtw8822c_dpk_afe_is_dpk_tbl);
	else
		rtw_load_table(rtwdev, &rtw8822c_dpk_afe_no_dpk_tbl);
}

static void rtw8822c_dpk_pre_setting(struct rtw_dev *rtwdev)
{
	u8 path;

	for (path = 0; path < rtwdev->hal.rf_path_num; path++) {
		rtw_write_rf(rtwdev, path, RF_RXAGC_OFFSET, RFREG_MASK, 0x0);
		rtw_write32(rtwdev, REG_NCTL0, 0x8 | (path << 1));
		if (rtwdev->dm_info.dpk_info.dpk_band == RTW_BAND_2G)
			rtw_write32(rtwdev, REG_DPD_LUT3, 0x1f100000);
		else
			rtw_write32(rtwdev, REG_DPD_LUT3, 0x1f0d0000);
		rtw_write32_mask(rtwdev, REG_DPD_LUT0, BIT_GLOSS_DB, 0x4);
		rtw_write32_mask(rtwdev, REG_IQK_CTL1, BIT_TX_CFIR, 0x3);
	}
	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0xc);
	rtw_write32(rtwdev, REG_DPD_CTL11, 0x3b23170b);
	rtw_write32(rtwdev, REG_DPD_CTL12, 0x775f5347);
}

static u32 rtw8822c_dpk_rf_setting(struct rtw_dev *rtwdev, u8 path)
{
	u32 ori_txbb;

	rtw_write_rf(rtwdev, path, RF_MODE_TRXAGC, RFREG_MASK, 0x50017);
	ori_txbb = rtw_read_rf(rtwdev, path, RF_TX_GAIN, RFREG_MASK);

	rtw_write_rf(rtwdev, path, RF_DEBUG, BIT_DE_TX_GAIN, 0x1);
	rtw_write_rf(rtwdev, path, RF_DEBUG, BIT_DE_PWR_TRIM, 0x1);
	rtw_write_rf(rtwdev, path, RF_TX_GAIN_OFFSET, BIT_TX_OFFSET_VAL, 0x0);
	rtw_write_rf(rtwdev, path, RF_TX_GAIN, RFREG_MASK, ori_txbb);

	if (rtwdev->dm_info.dpk_info.dpk_band == RTW_BAND_2G) {
		rtw_write_rf(rtwdev, path, RF_TX_GAIN_OFFSET, BIT_LB_ATT, 0x1);
		rtw_write_rf(rtwdev, path, RF_RXG_GAIN, BIT_RXG_GAIN, 0x0);
	} else {
		rtw_write_rf(rtwdev, path, RF_TXA_LB_SW, BIT_TXA_LB_ATT, 0x0);
		rtw_write_rf(rtwdev, path, RF_TXA_LB_SW, BIT_LB_ATT, 0x6);
		rtw_write_rf(rtwdev, path, RF_TXA_LB_SW, BIT_LB_SW, 0x1);
		rtw_write_rf(rtwdev, path, RF_RXA_MIX_GAIN, BIT_RXA_MIX_GAIN, 0);
	}

	rtw_write_rf(rtwdev, path, RF_MODE_TRXAGC, BIT_RXAGC, 0xf);
	rtw_write_rf(rtwdev, path, RF_DEBUG, BIT_DE_TRXBW, 0x1);
	rtw_write_rf(rtwdev, path, RF_BW_TRXBB, BIT_BW_RXBB, 0x0);

	if (rtwdev->dm_info.dpk_info.dpk_bw == DPK_CHANNEL_WIDTH_80)
		rtw_write_rf(rtwdev, path, RF_BW_TRXBB, BIT_BW_TXBB, 0x2);
	else
		rtw_write_rf(rtwdev, path, RF_BW_TRXBB, BIT_BW_TXBB, 0x1);

	rtw_write_rf(rtwdev, path, RF_EXT_TIA_BW, BIT(1), 0x1);

	usleep_range(100, 110);

	return ori_txbb & 0x1f;
}

static u16 rtw8822c_dpk_get_cmd(struct rtw_dev *rtwdev, u8 action, u8 path)
{
	u16 cmd;
	u8 bw = rtwdev->dm_info.dpk_info.dpk_bw == DPK_CHANNEL_WIDTH_80 ? 2 : 0;

	switch (action) {
	case RTW_DPK_GAIN_LOSS:
		cmd = 0x14 + path;
		break;
	case RTW_DPK_DO_DPK:
		cmd = 0x16 + path + bw;
		break;
	case RTW_DPK_DPK_ON:
		cmd = 0x1a + path;
		break;
	case RTW_DPK_DAGC:
		cmd = 0x1c + path + bw;
		break;
	default:
		return 0;
	}

	return (cmd << 8) | 0x48;
}

static u8 rtw8822c_dpk_one_shot(struct rtw_dev *rtwdev, u8 path, u8 action)
{
	u16 dpk_cmd;
	u8 result = 0;

	rtw8822c_dpk_set_gnt_wl(rtwdev, true);

	if (action == RTW_DPK_CAL_PWR) {
		rtw_write32_mask(rtwdev, REG_DPD_CTL0, BIT(12), 0x1);
		rtw_write32_mask(rtwdev, REG_DPD_CTL0, BIT(12), 0x0);
		rtw_write32_mask(rtwdev, REG_RXSRAM_CTL, BIT_RPT_SEL, 0x0);
		msleep(10);
		if (!check_hw_ready(rtwdev, REG_STAT_RPT, BIT(31), 0x1)) {
			result = 1;
			rtw_dbg(rtwdev, RTW_DBG_RFK, "[DPK] one-shot over 20ms\n");
		}
	} else {
		rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE,
				 0x8 | (path << 1));
		rtw_write32_mask(rtwdev, REG_R_CONFIG, BIT_IQ_SWITCH, 0x9);

		dpk_cmd = rtw8822c_dpk_get_cmd(rtwdev, action, path);
		rtw_write32(rtwdev, REG_NCTL0, dpk_cmd);
		rtw_write32(rtwdev, REG_NCTL0, dpk_cmd + 1);
		msleep(10);
		if (!check_hw_ready(rtwdev, 0x2d9c, 0xff, 0x55)) {
			result = 1;
			rtw_dbg(rtwdev, RTW_DBG_RFK, "[DPK] one-shot over 20ms\n");
		}
		rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE,
				 0x8 | (path << 1));
		rtw_write32_mask(rtwdev, REG_R_CONFIG, BIT_IQ_SWITCH, 0x0);
	}

	rtw8822c_dpk_set_gnt_wl(rtwdev, false);

	rtw_write8(rtwdev, 0x1b10, 0x0);

	return result;
}

static u16 rtw8822c_dpk_dgain_read(struct rtw_dev *rtwdev, u8 path)
{
	u16 dgain;

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0xc);
	rtw_write32_mask(rtwdev, REG_RXSRAM_CTL, 0x00ff0000, 0x0);

	dgain = (u16)rtw_read32_mask(rtwdev, REG_STAT_RPT, GENMASK(27, 16));

	return dgain;
}

static u8 rtw8822c_dpk_thermal_read(struct rtw_dev *rtwdev, u8 path)
{
	rtw_write_rf(rtwdev, path, RF_T_METER, BIT(19), 0x1);
	rtw_write_rf(rtwdev, path, RF_T_METER, BIT(19), 0x0);
	rtw_write_rf(rtwdev, path, RF_T_METER, BIT(19), 0x1);
	udelay(15);

	return (u8)rtw_read_rf(rtwdev, path, RF_T_METER, 0x0007e);
}

static u32 rtw8822c_dpk_pas_read(struct rtw_dev *rtwdev, u8 path)
{
	u32 i_val, q_val;

	rtw_write32(rtwdev, REG_NCTL0, 0x8 | (path << 1));
	rtw_write32_mask(rtwdev, 0x1b48, BIT(14), 0x0);
	rtw_write32(rtwdev, REG_RXSRAM_CTL, 0x00060001);
	rtw_write32(rtwdev, 0x1b4c, 0x00000000);
	rtw_write32(rtwdev, 0x1b4c, 0x00080000);

	q_val = rtw_read32_mask(rtwdev, REG_STAT_RPT, MASKHWORD);
	i_val = rtw_read32_mask(rtwdev, REG_STAT_RPT, MASKLWORD);

	if (i_val & BIT(15))
		i_val = 0x10000 - i_val;
	if (q_val & BIT(15))
		q_val = 0x10000 - q_val;

	rtw_write32(rtwdev, 0x1b4c, 0x00000000);

	return i_val * i_val + q_val * q_val;
}

static u32 rtw8822c_psd_log2base(u32 val)
{
	u32 tmp, val_integerd_b, tindex;
	u32 result, val_fractiond_b;
	u32 table_fraction[21] = {0, 432, 332, 274, 232, 200, 174,
				  151, 132, 115, 100, 86, 74, 62, 51,
				  42, 32, 23, 15, 7, 0};

	if (val == 0)
		return 0;

	val_integerd_b = __fls(val) + 1;

	tmp = (val * 100) / (1 << val_integerd_b);
	tindex = tmp / 5;

	if (tindex >= ARRAY_SIZE(table_fraction))
		tindex = ARRAY_SIZE(table_fraction) - 1;

	val_fractiond_b = table_fraction[tindex];

	result = val_integerd_b * 100 - val_fractiond_b;

	return result;
}

static u8 rtw8822c_dpk_gainloss_result(struct rtw_dev *rtwdev, u8 path)
{
	u8 result;

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x8 | (path << 1));
	rtw_write32_mask(rtwdev, 0x1b48, BIT(14), 0x1);
	rtw_write32(rtwdev, REG_RXSRAM_CTL, 0x00060000);

	result = (u8)rtw_read32_mask(rtwdev, REG_STAT_RPT, 0x000000f0);

	rtw_write32_mask(rtwdev, 0x1b48, BIT(14), 0x0);

	return result;
}

static u8 rtw8822c_dpk_agc_gain_chk(struct rtw_dev *rtwdev, u8 path,
				    u8 limited_pga)
{
	u8 result = 0;
	u16 dgain;

	rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_DAGC);
	dgain = rtw8822c_dpk_dgain_read(rtwdev, path);

	if (dgain > 1535 && !limited_pga)
		return RTW_DPK_GAIN_LESS;
	else if (dgain < 768 && !limited_pga)
		return RTW_DPK_GAIN_LARGE;
	else
		return result;
}

static u8 rtw8822c_dpk_agc_loss_chk(struct rtw_dev *rtwdev, u8 path)
{
	u32 loss, loss_db;

	loss = rtw8822c_dpk_pas_read(rtwdev, path);
	if (loss < 0x4000000)
		return RTW_DPK_GL_LESS;
	loss_db = 3 * rtw8822c_psd_log2base(loss >> 13) - 3870;

	if (loss_db > 1000)
		return RTW_DPK_GL_LARGE;
	else if (loss_db < 250)
		return RTW_DPK_GL_LESS;
	else
		return RTW_DPK_AGC_OUT;
}

struct rtw8822c_dpk_data {
	u8 txbb;
	u8 pga;
	u8 limited_pga;
	u8 agc_cnt;
	bool loss_only;
	bool gain_only;
	u8 path;
};

static u8 rtw8822c_gain_check_state(struct rtw_dev *rtwdev,
				    struct rtw8822c_dpk_data *data)
{
	u8 state;

	data->txbb = (u8)rtw_read_rf(rtwdev, data->path, RF_TX_GAIN,
				     BIT_GAIN_TXBB);
	data->pga = (u8)rtw_read_rf(rtwdev, data->path, RF_MODE_TRXAGC,
				    BIT_RXAGC);

	if (data->loss_only) {
		state = RTW_DPK_LOSS_CHECK;
		goto check_end;
	}

	state = rtw8822c_dpk_agc_gain_chk(rtwdev, data->path,
					  data->limited_pga);
	if (state == RTW_DPK_GAIN_CHECK && data->gain_only)
		state = RTW_DPK_AGC_OUT;
	else if (state == RTW_DPK_GAIN_CHECK)
		state = RTW_DPK_LOSS_CHECK;

check_end:
	data->agc_cnt++;
	if (data->agc_cnt >= 6)
		state = RTW_DPK_AGC_OUT;

	return state;
}

static u8 rtw8822c_gain_large_state(struct rtw_dev *rtwdev,
				    struct rtw8822c_dpk_data *data)
{
	u8 pga = data->pga;

	if (pga > 0xe)
		rtw_write_rf(rtwdev, data->path, RF_MODE_TRXAGC, BIT_RXAGC, 0xc);
	else if (pga > 0xb && pga < 0xf)
		rtw_write_rf(rtwdev, data->path, RF_MODE_TRXAGC, BIT_RXAGC, 0x0);
	else if (pga < 0xc)
		data->limited_pga = 1;

	return RTW_DPK_GAIN_CHECK;
}

static u8 rtw8822c_gain_less_state(struct rtw_dev *rtwdev,
				   struct rtw8822c_dpk_data *data)
{
	u8 pga = data->pga;

	if (pga < 0xc)
		rtw_write_rf(rtwdev, data->path, RF_MODE_TRXAGC, BIT_RXAGC, 0xc);
	else if (pga > 0xb && pga < 0xf)
		rtw_write_rf(rtwdev, data->path, RF_MODE_TRXAGC, BIT_RXAGC, 0xf);
	else if (pga > 0xe)
		data->limited_pga = 1;

	return RTW_DPK_GAIN_CHECK;
}

static u8 rtw8822c_gl_state(struct rtw_dev *rtwdev,
			    struct rtw8822c_dpk_data *data, u8 is_large)
{
	u8 txbb_bound[] = {0x1f, 0};

	if (data->txbb == txbb_bound[is_large])
		return RTW_DPK_AGC_OUT;

	if (is_large == 1)
		data->txbb -= 2;
	else
		data->txbb += 3;

	rtw_write_rf(rtwdev, data->path, RF_TX_GAIN, BIT_GAIN_TXBB, data->txbb);
	data->limited_pga = 0;

	return RTW_DPK_GAIN_CHECK;
}

static u8 rtw8822c_gl_large_state(struct rtw_dev *rtwdev,
				  struct rtw8822c_dpk_data *data)
{
	return rtw8822c_gl_state(rtwdev, data, 1);
}

static u8 rtw8822c_gl_less_state(struct rtw_dev *rtwdev,
				 struct rtw8822c_dpk_data *data)
{
	return rtw8822c_gl_state(rtwdev, data, 0);
}

static u8 rtw8822c_loss_check_state(struct rtw_dev *rtwdev,
				    struct rtw8822c_dpk_data *data)
{
	u8 path = data->path;
	u8 state;

	rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_GAIN_LOSS);
	state = rtw8822c_dpk_agc_loss_chk(rtwdev, path);

	return state;
}

static u8 (*dpk_state[])(struct rtw_dev *rtwdev,
			  struct rtw8822c_dpk_data *data) = {
	rtw8822c_gain_check_state, rtw8822c_gain_large_state,
	rtw8822c_gain_less_state, rtw8822c_gl_large_state,
	rtw8822c_gl_less_state, rtw8822c_loss_check_state };

static u8 rtw8822c_dpk_pas_agc(struct rtw_dev *rtwdev, u8 path,
			       bool gain_only, bool loss_only)
{
	struct rtw8822c_dpk_data data = {0};
	u8 (*func)(struct rtw_dev *rtwdev, struct rtw8822c_dpk_data *data);
	u8 state = RTW_DPK_GAIN_CHECK;

	data.loss_only = loss_only;
	data.gain_only = gain_only;
	data.path = path;

	for (;;) {
		func = dpk_state[state];
		state = func(rtwdev, &data);
		if (state == RTW_DPK_AGC_OUT)
			break;
	}

	return data.txbb;
}

static bool rtw8822c_dpk_coef_iq_check(struct rtw_dev *rtwdev,
				       u16 coef_i, u16 coef_q)
{
	if (coef_i == 0x1000 || coef_i == 0x0fff ||
	    coef_q == 0x1000 || coef_q == 0x0fff)
		return 1;
	else
		return 0;
}

static u32 rtw8822c_dpk_coef_transfer(struct rtw_dev *rtwdev)
{
	u32 reg = 0;
	u16 coef_i = 0, coef_q = 0;

	reg = rtw_read32(rtwdev, REG_STAT_RPT);

	coef_i = (u16)rtw_read32_mask(rtwdev, REG_STAT_RPT, MASKHWORD) & 0x1fff;
	coef_q = (u16)rtw_read32_mask(rtwdev, REG_STAT_RPT, MASKLWORD) & 0x1fff;

	coef_q = ((0x2000 - coef_q) & 0x1fff) - 1;

	reg = (coef_i << 16) | coef_q;

	return reg;
}

static const u32 rtw8822c_dpk_get_coef_tbl[] = {
	0x000400f0, 0x040400f0, 0x080400f0, 0x010400f0, 0x050400f0,
	0x090400f0, 0x020400f0, 0x060400f0, 0x0a0400f0, 0x030400f0,
	0x070400f0, 0x0b0400f0, 0x0c0400f0, 0x100400f0, 0x0d0400f0,
	0x110400f0, 0x0e0400f0, 0x120400f0, 0x0f0400f0, 0x130400f0,
};

static void rtw8822c_dpk_coef_tbl_apply(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	int i;

	for (i = 0; i < 20; i++) {
		rtw_write32(rtwdev, REG_RXSRAM_CTL,
			    rtw8822c_dpk_get_coef_tbl[i]);
		dpk_info->coef[path][i] = rtw8822c_dpk_coef_transfer(rtwdev);
	}
}

static void rtw8822c_dpk_get_coef(struct rtw_dev *rtwdev, u8 path)
{
	rtw_write32(rtwdev, REG_NCTL0, 0x0000000c);

	if (path == RF_PATH_A) {
		rtw_write32_mask(rtwdev, REG_DPD_CTL0, BIT(24), 0x0);
		rtw_write32(rtwdev, REG_DPD_CTL0_S0, 0x30000080);
	} else if (path == RF_PATH_B) {
		rtw_write32_mask(rtwdev, REG_DPD_CTL0, BIT(24), 0x1);
		rtw_write32(rtwdev, REG_DPD_CTL0_S1, 0x30000080);
	}

	rtw8822c_dpk_coef_tbl_apply(rtwdev, path);
}

static u8 rtw8822c_dpk_coef_read(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u8 addr, result = 1;
	u16 coef_i, coef_q;

	for (addr = 0; addr < 20; addr++) {
		coef_i = FIELD_GET(0x1fff0000, dpk_info->coef[path][addr]);
		coef_q = FIELD_GET(0x1fff, dpk_info->coef[path][addr]);

		if (rtw8822c_dpk_coef_iq_check(rtwdev, coef_i, coef_q)) {
			result = 0;
			break;
		}
	}
	return result;
}

static void rtw8822c_dpk_coef_write(struct rtw_dev *rtwdev, u8 path, u8 result)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u16 reg[DPK_RF_PATH_NUM] = {0x1b0c, 0x1b64};
	u32 coef;
	u8 addr;

	rtw_write32(rtwdev, REG_NCTL0, 0x0000000c);
	rtw_write32(rtwdev, REG_RXSRAM_CTL, 0x000000f0);

	for (addr = 0; addr < 20; addr++) {
		if (result == 0) {
			if (addr == 3)
				coef = 0x04001fff;
			else
				coef = 0x00001fff;
		} else {
			coef = dpk_info->coef[path][addr];
		}
		rtw_write32(rtwdev, reg[path] + addr * 4, coef);
	}
}

static void rtw8822c_dpk_fill_result(struct rtw_dev *rtwdev, u32 dpk_txagc,
				     u8 path, u8 result)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x8 | (path << 1));

	if (result)
		rtw_write8(rtwdev, REG_DPD_AGC, (u8)(dpk_txagc - 6));
	else
		rtw_write8(rtwdev, REG_DPD_AGC, 0x00);

	dpk_info->result[path] = result;
	dpk_info->dpk_txagc[path] = rtw_read8(rtwdev, REG_DPD_AGC);

	rtw8822c_dpk_coef_write(rtwdev, path, result);
}

static u32 rtw8822c_dpk_gainloss(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u8 tx_agc, tx_bb, ori_txbb, ori_txagc, tx_agc_search, t1, t2;

	ori_txbb = rtw8822c_dpk_rf_setting(rtwdev, path);
	ori_txagc = (u8)rtw_read_rf(rtwdev, path, RF_MODE_TRXAGC, BIT_TXAGC);

	rtw8822c_dpk_rxbb_dc_cal(rtwdev, path);
	rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_DAGC);
	rtw8822c_dpk_dgain_read(rtwdev, path);

	if (rtw8822c_dpk_dc_corr_check(rtwdev, path)) {
		rtw8822c_dpk_rxbb_dc_cal(rtwdev, path);
		rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_DAGC);
		rtw8822c_dpk_dc_corr_check(rtwdev, path);
	}

	t1 = rtw8822c_dpk_thermal_read(rtwdev, path);
	tx_bb = rtw8822c_dpk_pas_agc(rtwdev, path, false, true);
	tx_agc_search = rtw8822c_dpk_gainloss_result(rtwdev, path);

	if (tx_bb < tx_agc_search)
		tx_bb = 0;
	else
		tx_bb = tx_bb - tx_agc_search;

	rtw_write_rf(rtwdev, path, RF_TX_GAIN, BIT_GAIN_TXBB, tx_bb);

	tx_agc = ori_txagc - (ori_txbb - tx_bb);

	t2 = rtw8822c_dpk_thermal_read(rtwdev, path);

	dpk_info->thermal_dpk_delta[path] = abs(t2 - t1);

	return tx_agc;
}

static u8 rtw8822c_dpk_by_path(struct rtw_dev *rtwdev, u32 tx_agc, u8 path)
{
	u8 result;

	result = rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_DO_DPK);

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x8 | (path << 1));

	result = result | (u8)rtw_read32_mask(rtwdev, REG_DPD_CTL1_S0, BIT(26));

	rtw_write_rf(rtwdev, path, RF_MODE_TRXAGC, RFREG_MASK, 0x33e14);

	rtw8822c_dpk_get_coef(rtwdev, path);

	return result;
}

static void rtw8822c_dpk_cal_gs(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u32 tmp_gs = 0;

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x8 | (path << 1));
	rtw_write32_mask(rtwdev, REG_IQK_CTL1, BIT_BYPASS_DPD, 0x0);
	rtw_write32_mask(rtwdev, REG_IQK_CTL1, BIT_TX_CFIR, 0x0);
	rtw_write32_mask(rtwdev, REG_R_CONFIG, BIT_IQ_SWITCH, 0x9);
	rtw_write32_mask(rtwdev, REG_R_CONFIG, BIT_INNER_LB, 0x1);
	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0xc);
	rtw_write32_mask(rtwdev, REG_RXSRAM_CTL, BIT_DPD_CLK, 0xf);

	if (path == RF_PATH_A) {
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S0, BIT_GS_PWSF,
				 0x1066680);
		rtw_write32_mask(rtwdev, REG_DPD_CTL1_S0, BIT_DPD_EN, 0x1);
	} else {
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S1, BIT_GS_PWSF,
				 0x1066680);
		rtw_write32_mask(rtwdev, REG_DPD_CTL1_S1, BIT_DPD_EN, 0x1);
	}

	if (dpk_info->dpk_bw == DPK_CHANNEL_WIDTH_80) {
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x80001310);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x00001310);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x810000db);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x010000db);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x0000b428);
		rtw_write32(rtwdev, REG_DPD_CTL15,
			    0x05020000 | (BIT(path) << 28));
	} else {
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x8200190c);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x0200190c);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x8301ee14);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x0301ee14);
		rtw_write32(rtwdev, REG_DPD_CTL16, 0x0000b428);
		rtw_write32(rtwdev, REG_DPD_CTL15,
			    0x05020008 | (BIT(path) << 28));
	}

	rtw_write32_mask(rtwdev, REG_DPD_CTL0, MASKBYTE3, 0x8 | path);

	rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_CAL_PWR);

	rtw_write32_mask(rtwdev, REG_DPD_CTL15, MASKBYTE3, 0x0);
	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x8 | (path << 1));
	rtw_write32_mask(rtwdev, REG_R_CONFIG, BIT_IQ_SWITCH, 0x0);
	rtw_write32_mask(rtwdev, REG_R_CONFIG, BIT_INNER_LB, 0x0);
	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0xc);

	if (path == RF_PATH_A)
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S0, BIT_GS_PWSF, 0x5b);
	else
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S1, BIT_GS_PWSF, 0x5b);

	rtw_write32_mask(rtwdev, REG_RXSRAM_CTL, BIT_RPT_SEL, 0x0);

	tmp_gs = (u16)rtw_read32_mask(rtwdev, REG_STAT_RPT, BIT_RPT_DGAIN);
	tmp_gs = (tmp_gs * 910) >> 10;
	tmp_gs = DIV_ROUND_CLOSEST(tmp_gs, 10);

	if (path == RF_PATH_A)
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S0, BIT_GS_PWSF, tmp_gs);
	else
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S1, BIT_GS_PWSF, tmp_gs);

	dpk_info->dpk_gs[path] = tmp_gs;
}

void rtw8822c_dpk_cal_coef1(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u32 offset[DPK_RF_PATH_NUM] = {0, 0x58};
	u32 i_scaling;
	u8 path;

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x0000000c);
	rtw_write32(rtwdev, REG_RXSRAM_CTL, 0x000000f0);
	rtw_write32(rtwdev, REG_NCTL0, 0x00001148);
	rtw_write32(rtwdev, REG_NCTL0, 0x00001149);

	check_hw_ready(rtwdev, 0x2d9c, MASKBYTE0, 0x55);

	rtw_write8(rtwdev, 0x1b10, 0x0);
	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x0000000c);

	for (path = 0; path < rtwdev->hal.rf_path_num; path++) {
		i_scaling = 0x16c00 / dpk_info->dpk_gs[path];

		rtw_write32_mask(rtwdev, 0x1b18 + offset[path], MASKHWORD,
				 i_scaling);
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S0 + offset[path],
				 GENMASK(31, 28), 0x9);
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S0 + offset[path],
				 GENMASK(31, 28), 0x1);
		rtw_write32_mask(rtwdev, REG_DPD_CTL0_S0 + offset[path],
				 GENMASK(31, 28), 0x0);
		rtw_write32_mask(rtwdev, REG_DPD_CTL1_S0 + offset[path],
				 BIT(14), 0x0);
	}
}

static void rtw8822c_dpk_on(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;

	rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_DPK_ON);

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0x8 | (path << 1));
	rtw_write32_mask(rtwdev, REG_IQK_CTL1, BIT_TX_CFIR, 0x0);

	if (test_bit(path, dpk_info->dpk_path_ok))
		rtw8822c_dpk_cal_gs(rtwdev, path);
}

static bool rtw8822c_dpk_check_pass(struct rtw_dev *rtwdev, bool is_fail,
				    u32 dpk_txagc, u8 path)
{
	bool result;

	if (!is_fail) {
		if (rtw8822c_dpk_coef_read(rtwdev, path))
			result = true;
		else
			result = false;
	} else {
		result = false;
	}

	rtw8822c_dpk_fill_result(rtwdev, dpk_txagc, path, result);

	return result;
}

static void rtw8822c_dpk_result_reset(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u8 path;

	for (path = 0; path < rtwdev->hal.rf_path_num; path++) {
		clear_bit(path, dpk_info->dpk_path_ok);
		rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE,
				 0x8 | (path << 1));
		rtw_write32_mask(rtwdev, 0x1b58, 0x0000007f, 0x0);

		dpk_info->dpk_txagc[path] = 0;
		dpk_info->result[path] = 0;
		dpk_info->dpk_gs[path] = 0x5b;
		dpk_info->pre_pwsf[path] = 0;
		dpk_info->thermal_dpk[path] = rtw8822c_dpk_thermal_read(rtwdev,
									path);
	}
}

static void rtw8822c_dpk_calibrate(struct rtw_dev *rtwdev, u8 path)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u32 dpk_txagc;
	u8 dpk_fail;

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DPK] s%d dpk start\n", path);

	dpk_txagc = rtw8822c_dpk_gainloss(rtwdev, path);

	dpk_fail = rtw8822c_dpk_by_path(rtwdev, dpk_txagc, path);

	if (!rtw8822c_dpk_check_pass(rtwdev, dpk_fail, dpk_txagc, path))
		rtw_err(rtwdev, "failed to do dpk calibration\n");

	rtw_dbg(rtwdev, RTW_DBG_RFK, "[DPK] s%d dpk finish\n", path);

	if (dpk_info->result[path])
		set_bit(path, dpk_info->dpk_path_ok);
}

static void rtw8822c_dpk_path_select(struct rtw_dev *rtwdev)
{
	rtw8822c_dpk_calibrate(rtwdev, RF_PATH_A);
	rtw8822c_dpk_calibrate(rtwdev, RF_PATH_B);
	rtw8822c_dpk_on(rtwdev, RF_PATH_A);
	rtw8822c_dpk_on(rtwdev, RF_PATH_B);
	rtw8822c_dpk_cal_coef1(rtwdev);
}

static void rtw8822c_dpk_enable_disable(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u32 mask = BIT(15) | BIT(14);

	rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0xc);

	rtw_write32_mask(rtwdev, REG_DPD_CTL1_S0, BIT_DPD_EN,
			 dpk_info->is_dpk_pwr_on);
	rtw_write32_mask(rtwdev, REG_DPD_CTL1_S1, BIT_DPD_EN,
			 dpk_info->is_dpk_pwr_on);

	if (test_bit(RF_PATH_A, dpk_info->dpk_path_ok)) {
		rtw_write32_mask(rtwdev, REG_DPD_CTL1_S0, mask, 0x0);
		rtw_write8(rtwdev, REG_DPD_CTL0_S0, dpk_info->dpk_gs[RF_PATH_A]);
	}
	if (test_bit(RF_PATH_B, dpk_info->dpk_path_ok)) {
		rtw_write32_mask(rtwdev, REG_DPD_CTL1_S1, mask, 0x0);
		rtw_write8(rtwdev, REG_DPD_CTL0_S1, dpk_info->dpk_gs[RF_PATH_B]);
	}
}

static void rtw8822c_dpk_reload_data(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u8 path;

	if (!test_bit(RF_PATH_A, dpk_info->dpk_path_ok) &&
	    !test_bit(RF_PATH_B, dpk_info->dpk_path_ok) &&
	    dpk_info->dpk_ch == 0)
		return;

	for (path = 0; path < rtwdev->hal.rf_path_num; path++) {
		rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE,
				 0x8 | (path << 1));
		if (dpk_info->dpk_band == RTW_BAND_2G)
			rtw_write32(rtwdev, REG_DPD_LUT3, 0x1f100000);
		else
			rtw_write32(rtwdev, REG_DPD_LUT3, 0x1f0d0000);

		rtw_write8(rtwdev, REG_DPD_AGC, dpk_info->dpk_txagc[path]);

		rtw8822c_dpk_coef_write(rtwdev, path,
					test_bit(path, dpk_info->dpk_path_ok));

		rtw8822c_dpk_one_shot(rtwdev, path, RTW_DPK_DPK_ON);

		rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE, 0xc);

		if (path == RF_PATH_A)
			rtw_write32_mask(rtwdev, REG_DPD_CTL0_S0, BIT_GS_PWSF,
					 dpk_info->dpk_gs[path]);
		else
			rtw_write32_mask(rtwdev, REG_DPD_CTL0_S1, BIT_GS_PWSF,
					 dpk_info->dpk_gs[path]);
	}
	rtw8822c_dpk_cal_coef1(rtwdev);
}

static bool rtw8822c_dpk_reload(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u8 channel;

	dpk_info->is_reload = false;

	channel = (u8)(rtw_read_rf(rtwdev, RF_PATH_A, 0x18, RFREG_MASK) & 0xff);

	if (channel == dpk_info->dpk_ch) {
		rtw_dbg(rtwdev, RTW_DBG_RFK,
			"[DPK] DPK reload for CH%d!!\n", dpk_info->dpk_ch);
		rtw8822c_dpk_reload_data(rtwdev);
		dpk_info->is_reload = true;
	}

	return dpk_info->is_reload;
}

static void rtw8822c_do_dpk(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	struct rtw_backup_info bckp[DPK_BB_REG_NUM];
	u32 rf_reg_backup[DPK_RF_REG_NUM][DPK_RF_PATH_NUM];
	u32 bb_reg[DPK_BB_REG_NUM] = {
		0x520, 0x820, 0x824, 0x1c3c, 0x1d58, 0x1864,
		0x4164, 0x180c, 0x410c, 0x186c, 0x416c,
		0x1a14, 0x1e70, 0x80c, 0x1d70, 0x1e7c, 0x18a4, 0x41a4};
	u32 rf_reg[DPK_RF_REG_NUM] = {
		0x0, 0x1a, 0x55, 0x63, 0x87, 0x8f, 0xde};
	u8 path;

	if (!dpk_info->is_dpk_pwr_on) {
		rtw_dbg(rtwdev, RTW_DBG_RFK, "[DPK] Skip DPK due to DPD PWR off\n");
		return;
	} else if (rtw8822c_dpk_reload(rtwdev)) {
		return;
	}

	for (path = RF_PATH_A; path < DPK_RF_PATH_NUM; path++)
		ewma_thermal_init(&dpk_info->avg_thermal[path]);

	rtw8822c_dpk_information(rtwdev);

	rtw8822c_dpk_backup_registers(rtwdev, bb_reg, DPK_BB_REG_NUM, bckp);
	rtw8822c_dpk_backup_rf_registers(rtwdev, rf_reg, rf_reg_backup);

	rtw8822c_dpk_mac_bb_setting(rtwdev);
	rtw8822c_dpk_afe_setting(rtwdev, true);
	rtw8822c_dpk_pre_setting(rtwdev);
	rtw8822c_dpk_result_reset(rtwdev);
	rtw8822c_dpk_path_select(rtwdev);
	rtw8822c_dpk_afe_setting(rtwdev, false);
	rtw8822c_dpk_enable_disable(rtwdev);

	rtw8822c_dpk_reload_rf_registers(rtwdev, rf_reg, rf_reg_backup);
	for (path = 0; path < rtwdev->hal.rf_path_num; path++)
		rtw8822c_dpk_rxbb_dc_cal(rtwdev, path);
	rtw8822c_dpk_restore_registers(rtwdev, DPK_BB_REG_NUM, bckp);
}

static void rtw8822c_phy_calibration(struct rtw_dev *rtwdev)
{
	rtw8822c_do_iqk(rtwdev);
	rtw8822c_do_dpk(rtwdev);
}

void rtw8822c_dpk_track(struct rtw_dev *rtwdev)
{
	struct rtw_dpk_info *dpk_info = &rtwdev->dm_info.dpk_info;
	u8 path;
	u8 thermal_value[DPK_RF_PATH_NUM] = {0};
	s8 offset[DPK_RF_PATH_NUM], delta_dpk[DPK_RF_PATH_NUM];

	if (dpk_info->thermal_dpk[0] == 0 && dpk_info->thermal_dpk[1] == 0)
		return;

	for (path = 0; path < DPK_RF_PATH_NUM; path++) {
		thermal_value[path] = rtw8822c_dpk_thermal_read(rtwdev, path);
		ewma_thermal_add(&dpk_info->avg_thermal[path],
				 thermal_value[path]);
		thermal_value[path] =
			ewma_thermal_read(&dpk_info->avg_thermal[path]);
		delta_dpk[path] = dpk_info->thermal_dpk[path] -
				  thermal_value[path];
		offset[path] = delta_dpk[path] -
			       dpk_info->thermal_dpk_delta[path];
		offset[path] &= 0x7f;

		if (offset[path] != dpk_info->pre_pwsf[path]) {
			rtw_write32_mask(rtwdev, REG_NCTL0, BIT_SUBPAGE,
					 0x8 | (path << 1));
			rtw_write32_mask(rtwdev, 0x1b58, GENMASK(6, 0),
					 offset[path]);
			dpk_info->pre_pwsf[path] = offset[path];
		}
	}
}

static const struct rtw_phy_cck_pd_reg
rtw8822c_cck_pd_reg[RTW_CHANNEL_WIDTH_40 + 1][RTW_RF_PATH_MAX] = {
	{
		{0x1ac8, 0x00ff, 0x1ad0, 0x01f},
		{0x1ac8, 0xff00, 0x1ad0, 0x3e0}
	},
	{
		{0x1acc, 0x00ff, 0x1ad0, 0x01F00000},
		{0x1acc, 0xff00, 0x1ad0, 0x3E000000}
	},
};

#define RTW_CCK_PD_MAX 255
#define RTW_CCK_CS_MAX 31
#define RTW_CCK_CS_ERR1 27
#define RTW_CCK_CS_ERR2 29
static void
rtw8822c_phy_cck_pd_set_reg(struct rtw_dev *rtwdev,
			    s8 pd_diff, s8 cs_diff, u8 bw, u8 nrx)
{
	u32 pd, cs;

	if (WARN_ON(bw > RTW_CHANNEL_WIDTH_40 || nrx >= RTW_RF_PATH_MAX))
		return;

	pd = rtw_read32_mask(rtwdev,
			     rtw8822c_cck_pd_reg[bw][nrx].reg_pd,
			     rtw8822c_cck_pd_reg[bw][nrx].mask_pd);
	cs = rtw_read32_mask(rtwdev,
			     rtw8822c_cck_pd_reg[bw][nrx].reg_cs,
			     rtw8822c_cck_pd_reg[bw][nrx].mask_cs);
	pd += pd_diff;
	cs += cs_diff;
	if (pd > RTW_CCK_PD_MAX)
		pd = RTW_CCK_PD_MAX;
	if (cs == RTW_CCK_CS_ERR1 || cs == RTW_CCK_CS_ERR2)
		cs++;
	else if (cs > RTW_CCK_CS_MAX)
		cs = RTW_CCK_CS_MAX;
	rtw_write32_mask(rtwdev,
			 rtw8822c_cck_pd_reg[bw][nrx].reg_pd,
			 rtw8822c_cck_pd_reg[bw][nrx].mask_pd,
			 pd);
	rtw_write32_mask(rtwdev,
			 rtw8822c_cck_pd_reg[bw][nrx].reg_cs,
			 rtw8822c_cck_pd_reg[bw][nrx].mask_cs,
			 cs);
}

static void rtw8822c_phy_cck_pd_set(struct rtw_dev *rtwdev, u8 new_lvl)
{
	struct rtw_dm_info *dm_info = &rtwdev->dm_info;
	s8 pd_lvl[CCK_PD_LV_MAX] = {0, 2, 4, 6, 8};
	s8 cs_lvl[CCK_PD_LV_MAX] = {0, 2, 2, 2, 4};
	u8 cur_lvl;
	u8 nrx, bw;

	nrx = (u8)rtw_read32_mask(rtwdev, 0x1a2c, 0x60000);
	bw = (u8)rtw_read32_mask(rtwdev, 0x9b0, 0xc);

	if (dm_info->cck_pd_lv[bw][nrx] == new_lvl)
		return;

	cur_lvl = dm_info->cck_pd_lv[bw][nrx];

	/* update cck pd info */
	dm_info->cck_fa_avg = CCK_FA_AVG_RESET;

	rtw8822c_phy_cck_pd_set_reg(rtwdev,
				    pd_lvl[new_lvl] - pd_lvl[cur_lvl],
				    cs_lvl[new_lvl] - cs_lvl[cur_lvl],
				    bw, nrx);
	dm_info->cck_pd_lv[bw][nrx] = new_lvl;
}

static struct rtw_pwr_seq_cmd trans_carddis_to_cardemu_8822c[] = {
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x002E,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), BIT(2)},
	{0x002D,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x007F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), 0},
	{0x004A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4) | BIT(7), 0},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_cardemu_to_act_8822c[] = {
	{0x0000,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(4) | BIT(3) | BIT(2)), 0},
	{0x0075,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), BIT(1)},
	{0x0075,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0xFF1A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x002E,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), 0},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(4) | BIT(3)), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(0), 0},
	{0x0074,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
	{0x0071,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), 0},
	{0x0062,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(7) | BIT(6) | BIT(5)),
	 (BIT(7) | BIT(6) | BIT(5))},
	{0x0061,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(7) | BIT(6) | BIT(5)), 0},
	{0x001F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(7) | BIT(6)), BIT(7)},
	{0x00EF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, (BIT(7) | BIT(6)), BIT(7)},
	{0x1045,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), BIT(4)},
	{0x0010,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), BIT(2)},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_act_to_cardemu_8822c[] = {
	{0x0093,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3), 0},
	{0x001F,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x00EF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0},
	{0x1045,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(4), 0},
	{0xFF1A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x30},
	{0x0049,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0006,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0x0002,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), BIT(1)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_POLLING, BIT(1), 0},
	{0x0000,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), BIT(5)},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd trans_cardemu_to_carddis_8822c[] = {
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7), BIT(7)},
	{0x0007,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x00},
	{0x0067,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(5), 0},
	{0x004A,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(0), 0},
	{0x0081,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(7) | BIT(6), 0},
	{0x0090,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(1), 0},
	{0x0092,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x20},
	{0x0093,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, 0xFF, 0x04},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_USB_MSK | RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(3) | BIT(4), BIT(3)},
	{0x0005,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_PCI_MSK,
	 RTW_PWR_ADDR_MAC,
	 RTW_PWR_CMD_WRITE, BIT(2), BIT(2)},
	{0x0086,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_SDIO_MSK,
	 RTW_PWR_ADDR_SDIO,
	 RTW_PWR_CMD_WRITE, BIT(0), BIT(0)},
	{0xFFFF,
	 RTW_PWR_CUT_ALL_MSK,
	 RTW_PWR_INTF_ALL_MSK,
	 0,
	 RTW_PWR_CMD_END, 0, 0},
};

static struct rtw_pwr_seq_cmd *card_enable_flow_8822c[] = {
	trans_carddis_to_cardemu_8822c,
	trans_cardemu_to_act_8822c,
	NULL
};

static struct rtw_pwr_seq_cmd *card_disable_flow_8822c[] = {
	trans_act_to_cardemu_8822c,
	trans_cardemu_to_carddis_8822c,
	NULL
};

static struct rtw_intf_phy_para usb2_param_8822c[] = {
	{0xFFFF, 0x00,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para usb3_param_8822c[] = {
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para pcie_gen1_param_8822c[] = {
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para pcie_gen2_param_8822c[] = {
	{0xFFFF, 0x0000,
	 RTW_IP_SEL_PHY,
	 RTW_INTF_PHY_CUT_ALL,
	 RTW_INTF_PHY_PLATFORM_ALL},
};

static struct rtw_intf_phy_para_table phy_para_table_8822c = {
	.usb2_para	= usb2_param_8822c,
	.usb3_para	= usb3_param_8822c,
	.gen1_para	= pcie_gen1_param_8822c,
	.gen2_para	= pcie_gen2_param_8822c,
	.n_usb2_para	= ARRAY_SIZE(usb2_param_8822c),
	.n_usb3_para	= ARRAY_SIZE(usb2_param_8822c),
	.n_gen1_para	= ARRAY_SIZE(pcie_gen1_param_8822c),
	.n_gen2_para	= ARRAY_SIZE(pcie_gen2_param_8822c),
};

static const struct rtw_rfe_def rtw8822c_rfe_defs[] = {
	[0] = RTW_DEF_RFE(8822c, 0, 0),
	[1] = RTW_DEF_RFE(8822c, 0, 0),
	[2] = RTW_DEF_RFE(8822c, 0, 0),
};

static struct rtw_hw_reg rtw8822c_dig[] = {
	[0] = { .addr = 0x1d70, .mask = 0x7f },
	[1] = { .addr = 0x1d70, .mask = 0x7f00 },
};

static struct rtw_page_table page_table_8822c[] = {
	{64, 64, 64, 64, 1},
	{64, 64, 64, 64, 1},
	{64, 64, 0, 0, 1},
	{64, 64, 64, 0, 1},
	{64, 64, 64, 64, 1},
};

static struct rtw_rqpn rqpn_table_8822c[] = {
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_HIGH,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_HIGH, RTW_DMA_MAPPING_HIGH},
	{RTW_DMA_MAPPING_NORMAL, RTW_DMA_MAPPING_NORMAL,
	 RTW_DMA_MAPPING_LOW, RTW_DMA_MAPPING_LOW,
	 RTW_DMA_MAPPING_EXTRA, RTW_DMA_MAPPING_HIGH},
};

static struct rtw_chip_ops rtw8822c_ops = {
	.phy_set_param		= rtw8822c_phy_set_param,
	.read_efuse		= rtw8822c_read_efuse,
	.query_rx_desc		= rtw8822c_query_rx_desc,
	.set_channel		= rtw8822c_set_channel,
	.mac_init		= rtw8822c_mac_init,
	.read_rf		= rtw_phy_read_rf,
	.write_rf		= rtw_phy_write_rf_reg_mix,
	.set_tx_power_index	= rtw8822c_set_tx_power_index,
	.cfg_ldo25		= rtw8822c_cfg_ldo25,
	.false_alarm_statistics	= rtw8822c_false_alarm_statistics,
	.dpk_track		= rtw8822c_dpk_track,
	.phy_calibration	= rtw8822c_phy_calibration,
	.cck_pd_set		= rtw8822c_phy_cck_pd_set,

	.coex_set_init		= rtw8822c_coex_cfg_init,
	.coex_set_ant_switch	= NULL,
	.coex_set_gnt_fix	= rtw8822c_coex_cfg_gnt_fix,
	.coex_set_gnt_debug	= rtw8822c_coex_cfg_gnt_debug,
	.coex_set_rfe_type	= rtw8822c_coex_cfg_rfe_type,
	.coex_set_wl_tx_power	= rtw8822c_coex_cfg_wl_tx_power,
	.coex_set_wl_rx_gain	= rtw8822c_coex_cfg_wl_rx_gain,
};

/* Shared-Antenna Coex Table */
static const struct coex_table_para table_sant_8822c[] = {
	{0xffffffff, 0xffffffff}, /* case-0 */
	{0x55555555, 0x55555555},
	{0x66555555, 0x66555555},
	{0xaaaaaaaa, 0xaaaaaaaa},
	{0x5a5a5a5a, 0x5a5a5a5a},
	{0xfafafafa, 0xfafafafa}, /* case-5 */
	{0x6a5a6a5a, 0xaaaaaaaa},
	{0x6a5a56aa, 0x6a5a56aa},
	{0x6a5a5a5a, 0x6a5a5a5a},
	{0x66555555, 0x5a5a5a5a},
	{0x66555555, 0x6a5a5a5a}, /* case-10 */
	{0x66555555, 0xfafafafa},
	{0x66555555, 0x6a5a5aaa},
	{0x66555555, 0x5aaa5aaa},
	{0x66555555, 0xaaaa5aaa},
	{0x66555555, 0xaaaaaaaa}, /* case-15 */
	{0xffff55ff, 0xfafafafa},
	{0xffff55ff, 0x6afa5afa},
	{0xaaffffaa, 0xfafafafa},
	{0xaa5555aa, 0x5a5a5a5a},
	{0xaa5555aa, 0x6a5a5a5a}, /* case-20 */
	{0xaa5555aa, 0xaaaaaaaa},
	{0xffffffff, 0x5a5a5a5a},
	{0xffffffff, 0x6a5a5a5a},
	{0xffffffff, 0x55555555},
	{0xffffffff, 0x6a5a5aaa}, /* case-25 */
	{0x55555555, 0x5a5a5a5a},
	{0x55555555, 0xaaaaaaaa},
	{0x55555555, 0x6a5a6a5a},
	{0x66556655, 0x66556655}
};

/* Non-Shared-Antenna Coex Table */
static const struct coex_table_para table_nsant_8822c[] = {
	{0xffffffff, 0xffffffff}, /* case-100 */
	{0x55555555, 0x55555555},
	{0x66555555, 0x66555555},
	{0xaaaaaaaa, 0xaaaaaaaa},
	{0x5a5a5a5a, 0x5a5a5a5a},
	{0xfafafafa, 0xfafafafa}, /* case-105 */
	{0x5afa5afa, 0x5afa5afa},
	{0x55555555, 0xfafafafa},
	{0x66555555, 0xfafafafa},
	{0x66555555, 0x5a5a5a5a},
	{0x66555555, 0x6a5a5a5a}, /* case-110 */
	{0x66555555, 0xaaaaaaaa},
	{0xffff55ff, 0xfafafafa},
	{0xffff55ff, 0x5afa5afa},
	{0xffff55ff, 0xaaaaaaaa},
	{0xaaffffaa, 0xfafafafa}, /* case-115 */
	{0xaaffffaa, 0x5afa5afa},
	{0xaaffffaa, 0xaaaaaaaa},
	{0xffffffff, 0xfafafafa},
	{0xffffffff, 0x5afa5afa},
	{0xffffffff, 0xaaaaaaaa},/* case-120 */
	{0x55ff55ff, 0x5afa5afa},
	{0x55ff55ff, 0xaaaaaaaa},
	{0x55ff55ff, 0x55ff55ff}
};

/* Shared-Antenna TDMA */
static const struct coex_tdma_para tdma_sant_8822c[] = {
	{ {0x00, 0x00, 0x00, 0x00, 0x00} }, /* case-0 */
	{ {0x61, 0x45, 0x03, 0x11, 0x11} },
	{ {0x61, 0x3a, 0x03, 0x11, 0x11} },
	{ {0x61, 0x30, 0x03, 0x11, 0x11} },
	{ {0x61, 0x20, 0x03, 0x11, 0x11} },
	{ {0x61, 0x10, 0x03, 0x11, 0x11} }, /* case-5 */
	{ {0x61, 0x45, 0x03, 0x11, 0x10} },
	{ {0x61, 0x3a, 0x03, 0x11, 0x10} },
	{ {0x61, 0x30, 0x03, 0x11, 0x10} },
	{ {0x61, 0x20, 0x03, 0x11, 0x10} },
	{ {0x61, 0x10, 0x03, 0x11, 0x10} }, /* case-10 */
	{ {0x61, 0x08, 0x03, 0x11, 0x14} },
	{ {0x61, 0x08, 0x03, 0x10, 0x14} },
	{ {0x51, 0x08, 0x03, 0x10, 0x54} },
	{ {0x51, 0x08, 0x03, 0x10, 0x55} },
	{ {0x51, 0x08, 0x07, 0x10, 0x54} }, /* case-15 */
	{ {0x51, 0x45, 0x03, 0x10, 0x10} },
	{ {0x51, 0x3a, 0x03, 0x10, 0x50} },
	{ {0x51, 0x30, 0x03, 0x10, 0x50} },
	{ {0x51, 0x20, 0x03, 0x10, 0x50} },
	{ {0x51, 0x10, 0x03, 0x10, 0x50} }, /* case-20 */
	{ {0x51, 0x4a, 0x03, 0x10, 0x50} },
	{ {0x51, 0x0c, 0x03, 0x10, 0x54} },
	{ {0x55, 0x08, 0x03, 0x10, 0x54} },
	{ {0x65, 0x10, 0x03, 0x11, 0x11} },
	{ {0x51, 0x10, 0x03, 0x10, 0x51} }, /* case-25 */
	{ {0x51, 0x08, 0x03, 0x10, 0x50} }
};

/* Non-Shared-Antenna TDMA */
static const struct coex_tdma_para tdma_nsant_8822c[] = {
	{ {0x00, 0x00, 0x00, 0x00, 0x00} }, /* case-100 */
	{ {0x61, 0x45, 0x03, 0x11, 0x11} },
	{ {0x61, 0x3a, 0x03, 0x11, 0x11} },
	{ {0x61, 0x30, 0x03, 0x11, 0x11} },
	{ {0x61, 0x20, 0x03, 0x11, 0x11} },
	{ {0x61, 0x10, 0x03, 0x11, 0x11} }, /* case-105 */
	{ {0x61, 0x45, 0x03, 0x11, 0x10} },
	{ {0x61, 0x3a, 0x03, 0x11, 0x10} },
	{ {0x61, 0x30, 0x03, 0x11, 0x10} },
	{ {0x61, 0x20, 0x03, 0x11, 0x10} },
	{ {0x61, 0x10, 0x03, 0x11, 0x10} }, /* case-110 */
	{ {0x61, 0x08, 0x03, 0x11, 0x14} },
	{ {0x61, 0x08, 0x03, 0x10, 0x14} },
	{ {0x51, 0x08, 0x03, 0x10, 0x54} },
	{ {0x51, 0x08, 0x03, 0x10, 0x55} },
	{ {0x51, 0x08, 0x07, 0x10, 0x54} }, /* case-115 */
	{ {0x51, 0x45, 0x03, 0x10, 0x50} },
	{ {0x51, 0x3a, 0x03, 0x10, 0x50} },
	{ {0x51, 0x30, 0x03, 0x10, 0x50} },
	{ {0x51, 0x20, 0x03, 0x10, 0x50} },
	{ {0x51, 0x10, 0x03, 0x10, 0x50} }  /* case-120 */
};

/* rssi in percentage % (dbm = % - 100) */
static const u8 wl_rssi_step_8822c[] = {60, 50, 44, 30};
static const u8 bt_rssi_step_8822c[] = {8, 15, 20, 25};
static const struct coex_5g_afh_map afh_5g_8822c[] = { {0, 0, 0} };

/* wl_tx_dec_power, bt_tx_dec_power, wl_rx_gain, bt_rx_lna_constrain */
static const struct coex_rf_para rf_para_tx_8822c[] = {
	{0, 0, false, 7},  /* for normal */
	{0, 16, false, 7}, /* for WL-CPT */
	{8, 17, true, 4},
	{7, 18, true, 4},
	{6, 19, true, 4},
	{5, 20, true, 4}
};

static const struct coex_rf_para rf_para_rx_8822c[] = {
	{0, 0, false, 7},  /* for normal */
	{0, 16, false, 7}, /* for WL-CPT */
	{3, 24, true, 5},
	{2, 26, true, 5},
	{1, 27, true, 5},
	{0, 28, true, 5}
};

static_assert(ARRAY_SIZE(rf_para_tx_8822c) == ARRAY_SIZE(rf_para_rx_8822c));

struct rtw_chip_info rtw8822c_hw_spec = {
	.ops = &rtw8822c_ops,
	.id = RTW_CHIP_TYPE_8822C,
	.fw_name = "rtw88/rtw8822c_fw.bin",
	.tx_pkt_desc_sz = 48,
	.tx_buf_desc_sz = 16,
	.rx_pkt_desc_sz = 24,
	.rx_buf_desc_sz = 8,
	.phy_efuse_size = 512,
	.log_efuse_size = 768,
	.ptct_efuse_size = 124,
	.txff_size = 262144,
	.rxff_size = 24576,
	.txgi_factor = 2,
	.is_pwr_by_rate_dec = false,
	.max_power_index = 0x7f,
	.csi_buf_pg_num = 50,
	.band = RTW_BAND_2G | RTW_BAND_5G,
	.page_size = 128,
	.dig_min = 0x20,
	.ht_supported = true,
	.vht_supported = true,
	.sys_func_en = 0xD8,
	.pwr_on_seq = card_enable_flow_8822c,
	.pwr_off_seq = card_disable_flow_8822c,
	.page_table = page_table_8822c,
	.rqpn_table = rqpn_table_8822c,
	.intf_table = &phy_para_table_8822c,
	.dig = rtw8822c_dig,
	.rf_base_addr = {0x3c00, 0x4c00},
	.rf_sipi_addr = {0x1808, 0x4108},
	.mac_tbl = &rtw8822c_mac_tbl,
	.agc_tbl = &rtw8822c_agc_tbl,
	.bb_tbl = &rtw8822c_bb_tbl,
	.rfk_init_tbl = &rtw8822c_array_mp_cal_init_tbl,
	.rf_tbl = {&rtw8822c_rf_a_tbl, &rtw8822c_rf_b_tbl},
	.rfe_defs = rtw8822c_rfe_defs,
	.rfe_defs_size = ARRAY_SIZE(rtw8822c_rfe_defs),
	.en_dis_dpd = true,
	.dpd_ratemask = DIS_DPD_RATEALL,

	.coex_para_ver = 0x19062706,
	.bt_desired_ver = 0x6,
	.scbd_support = true,
	.new_scbd10_def = true,
	.pstdma_type = COEX_PSTDMA_FORCE_LPSOFF,
	.bt_rssi_type = COEX_BTRSSI_DBM,
	.ant_isolation = 15,
	.rssi_tolerance = 2,
	.wl_rssi_step = wl_rssi_step_8822c,
	.bt_rssi_step = bt_rssi_step_8822c,
	.table_sant_num = ARRAY_SIZE(table_sant_8822c),
	.table_sant = table_sant_8822c,
	.table_nsant_num = ARRAY_SIZE(table_nsant_8822c),
	.table_nsant = table_nsant_8822c,
	.tdma_sant_num = ARRAY_SIZE(tdma_sant_8822c),
	.tdma_sant = tdma_sant_8822c,
	.tdma_nsant_num = ARRAY_SIZE(tdma_nsant_8822c),
	.tdma_nsant = tdma_nsant_8822c,
	.wl_rf_para_num = ARRAY_SIZE(rf_para_tx_8822c),
	.wl_rf_para_tx = rf_para_tx_8822c,
	.wl_rf_para_rx = rf_para_rx_8822c,
	.bt_afh_span_bw20 = 0x24,
	.bt_afh_span_bw40 = 0x36,
	.afh_5g_num = ARRAY_SIZE(afh_5g_8822c),
	.afh_5g = afh_5g_8822c,
};
EXPORT_SYMBOL(rtw8822c_hw_spec);

MODULE_FIRMWARE("rtw88/rtw8822c_fw.bin");
