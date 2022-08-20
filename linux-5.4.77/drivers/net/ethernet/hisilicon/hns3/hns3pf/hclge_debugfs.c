// SPDX-License-Identifier: GPL-2.0+
/* Copyright (c) 2018-2019 Hisilicon Limited. */

#include <linux/device.h>

#include "hclge_debugfs.h"
#include "hclge_main.h"
#include "hclge_tm.h"
#include "hnae3.h"

static struct hclge_dbg_reg_type_info hclge_dbg_reg_info[] = {
	{ .reg_type = "bios common",
	  .dfx_msg = &hclge_dbg_bios_common_reg[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_bios_common_reg),
		       .offset = HCLGE_DBG_DFX_BIOS_OFFSET,
		       .cmd = HCLGE_OPC_DFX_BIOS_COMMON_REG } },
	{ .reg_type = "ssu",
	  .dfx_msg = &hclge_dbg_ssu_reg_0[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_ssu_reg_0),
		       .offset = HCLGE_DBG_DFX_SSU_0_OFFSET,
		       .cmd = HCLGE_OPC_DFX_SSU_REG_0 } },
	{ .reg_type = "ssu",
	  .dfx_msg = &hclge_dbg_ssu_reg_1[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_ssu_reg_1),
		       .offset = HCLGE_DBG_DFX_SSU_1_OFFSET,
		       .cmd = HCLGE_OPC_DFX_SSU_REG_1 } },
	{ .reg_type = "ssu",
	  .dfx_msg = &hclge_dbg_ssu_reg_2[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_ssu_reg_2),
		       .offset = HCLGE_DBG_DFX_SSU_2_OFFSET,
		       .cmd = HCLGE_OPC_DFX_SSU_REG_2 } },
	{ .reg_type = "igu egu",
	  .dfx_msg = &hclge_dbg_igu_egu_reg[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_igu_egu_reg),
		       .offset = HCLGE_DBG_DFX_IGU_OFFSET,
		       .cmd = HCLGE_OPC_DFX_IGU_EGU_REG } },
	{ .reg_type = "rpu",
	  .dfx_msg = &hclge_dbg_rpu_reg_0[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_rpu_reg_0),
		       .offset = HCLGE_DBG_DFX_RPU_0_OFFSET,
		       .cmd = HCLGE_OPC_DFX_RPU_REG_0 } },
	{ .reg_type = "rpu",
	  .dfx_msg = &hclge_dbg_rpu_reg_1[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_rpu_reg_1),
		       .offset = HCLGE_DBG_DFX_RPU_1_OFFSET,
		       .cmd = HCLGE_OPC_DFX_RPU_REG_1 } },
	{ .reg_type = "ncsi",
	  .dfx_msg = &hclge_dbg_ncsi_reg[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_ncsi_reg),
		       .offset = HCLGE_DBG_DFX_NCSI_OFFSET,
		       .cmd = HCLGE_OPC_DFX_NCSI_REG } },
	{ .reg_type = "rtc",
	  .dfx_msg = &hclge_dbg_rtc_reg[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_rtc_reg),
		       .offset = HCLGE_DBG_DFX_RTC_OFFSET,
		       .cmd = HCLGE_OPC_DFX_RTC_REG } },
	{ .reg_type = "ppp",
	  .dfx_msg = &hclge_dbg_ppp_reg[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_ppp_reg),
		       .offset = HCLGE_DBG_DFX_PPP_OFFSET,
		       .cmd = HCLGE_OPC_DFX_PPP_REG } },
	{ .reg_type = "rcb",
	  .dfx_msg = &hclge_dbg_rcb_reg[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_rcb_reg),
		       .offset = HCLGE_DBG_DFX_RCB_OFFSET,
		       .cmd = HCLGE_OPC_DFX_RCB_REG } },
	{ .reg_type = "tqp",
	  .dfx_msg = &hclge_dbg_tqp_reg[0],
	  .reg_msg = { .msg_num = ARRAY_SIZE(hclge_dbg_tqp_reg),
		       .offset = HCLGE_DBG_DFX_TQP_OFFSET,
		       .cmd = HCLGE_OPC_DFX_TQP_REG } },
};

static int hclge_dbg_get_dfx_bd_num(struct hclge_dev *hdev, int offset)
{
#define HCLGE_GET_DFX_REG_TYPE_CNT	4

	struct hclge_desc desc[HCLGE_GET_DFX_REG_TYPE_CNT];
	int entries_per_desc;
	int index;
	int ret;

	ret = hclge_query_bd_num_cmd_send(hdev, desc);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"get dfx bdnum fail, ret = %d\n", ret);
		return ret;
	}

	entries_per_desc = ARRAY_SIZE(desc[0].data);
	index = offset % entries_per_desc;
	return (int)desc[offset / entries_per_desc].data[index];
}

static int hclge_dbg_cmd_send(struct hclge_dev *hdev,
			      struct hclge_desc *desc_src,
			      int index, int bd_num,
			      enum hclge_opcode_type cmd)
{
	struct hclge_desc *desc = desc_src;
	int ret, i;

	hclge_cmd_setup_basic_desc(desc, cmd, true);
	desc->data[0] = cpu_to_le32(index);

	for (i = 1; i < bd_num; i++) {
		desc->flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
		desc++;
		hclge_cmd_setup_basic_desc(desc, cmd, true);
	}

	ret = hclge_cmd_send(&hdev->hw, desc_src, bd_num);
	if (ret)
		dev_err(&hdev->pdev->dev,
			"cmd(0x%x) send fail, ret = %d\n", cmd, ret);
	return ret;
}

static void hclge_dbg_dump_reg_common(struct hclge_dev *hdev,
				      struct hclge_dbg_reg_type_info *reg_info,
				      const char *cmd_buf)
{
#define IDX_OFFSET	1

	const char *s = &cmd_buf[strlen(reg_info->reg_type) + IDX_OFFSET];
	struct hclge_dbg_dfx_message *dfx_message = reg_info->dfx_msg;
	struct hclge_dbg_reg_common_msg *reg_msg = &reg_info->reg_msg;
	struct hclge_desc *desc_src;
	struct hclge_desc *desc;
	int entries_per_desc;
	int bd_num, buf_len;
	int index = 0;
	int min_num;
	int ret, i;

	if (*s) {
		ret = kstrtouint(s, 0, &index);
		index = (ret != 0) ? 0 : index;
	}

	bd_num = hclge_dbg_get_dfx_bd_num(hdev, reg_msg->offset);
	if (bd_num <= 0) {
		dev_err(&hdev->pdev->dev, "get cmd(%d) bd num(%d) failed\n",
			reg_msg->offset, bd_num);
		return;
	}

	buf_len	 = sizeof(struct hclge_desc) * bd_num;
	desc_src = kzalloc(buf_len, GFP_KERNEL);
	if (!desc_src) {
		dev_err(&hdev->pdev->dev, "call kzalloc failed\n");
		return;
	}

	desc = desc_src;
	ret  = hclge_dbg_cmd_send(hdev, desc, index, bd_num, reg_msg->cmd);
	if (ret) {
		kfree(desc_src);
		return;
	}

	entries_per_desc = ARRAY_SIZE(desc->data);
	min_num = min_t(int, bd_num * entries_per_desc, reg_msg->msg_num);

	desc = desc_src;
	for (i = 0; i < min_num; i++) {
		if (i > 0 && (i % entries_per_desc) == 0)
			desc++;
		if (dfx_message->flag)
			dev_info(&hdev->pdev->dev, "%s: 0x%x\n",
				 dfx_message->message,
				 desc->data[i % entries_per_desc]);

		dfx_message++;
	}

	kfree(desc_src);
}

static void hclge_dbg_dump_dcb(struct hclge_dev *hdev, const char *cmd_buf)
{
	struct device *dev = &hdev->pdev->dev;
	struct hclge_dbg_bitmap_cmd *bitmap;
	int rq_id, pri_id, qset_id;
	int port_id, nq_id, pg_id;
	struct hclge_desc desc[2];

	int cnt, ret;

	cnt = sscanf(cmd_buf, "%i %i %i %i %i %i",
		     &port_id, &pri_id, &pg_id, &rq_id, &nq_id, &qset_id);
	if (cnt != 6) {
		dev_err(&hdev->pdev->dev,
			"dump dcb: bad command parameter, cnt=%d\n", cnt);
		return;
	}

	ret = hclge_dbg_cmd_send(hdev, desc, qset_id, 1,
				 HCLGE_OPC_QSET_DFX_STS);
	if (ret)
		return;

	bitmap = (struct hclge_dbg_bitmap_cmd *)&desc[0].data[1];
	dev_info(dev, "roce_qset_mask: 0x%x\n", bitmap->bit0);
	dev_info(dev, "nic_qs_mask: 0x%x\n", bitmap->bit1);
	dev_info(dev, "qs_shaping_pass: 0x%x\n", bitmap->bit2);
	dev_info(dev, "qs_bp_sts: 0x%x\n", bitmap->bit3);

	ret = hclge_dbg_cmd_send(hdev, desc, pri_id, 1, HCLGE_OPC_PRI_DFX_STS);
	if (ret)
		return;

	bitmap = (struct hclge_dbg_bitmap_cmd *)&desc[0].data[1];
	dev_info(dev, "pri_mask: 0x%x\n", bitmap->bit0);
	dev_info(dev, "pri_cshaping_pass: 0x%x\n", bitmap->bit1);
	dev_info(dev, "pri_pshaping_pass: 0x%x\n", bitmap->bit2);

	ret = hclge_dbg_cmd_send(hdev, desc, pg_id, 1, HCLGE_OPC_PG_DFX_STS);
	if (ret)
		return;

	bitmap = (struct hclge_dbg_bitmap_cmd *)&desc[0].data[1];
	dev_info(dev, "pg_mask: 0x%x\n", bitmap->bit0);
	dev_info(dev, "pg_cshaping_pass: 0x%x\n", bitmap->bit1);
	dev_info(dev, "pg_pshaping_pass: 0x%x\n", bitmap->bit2);

	ret = hclge_dbg_cmd_send(hdev, desc, port_id, 1,
				 HCLGE_OPC_PORT_DFX_STS);
	if (ret)
		return;

	bitmap = (struct hclge_dbg_bitmap_cmd *)&desc[0].data[1];
	dev_info(dev, "port_mask: 0x%x\n", bitmap->bit0);
	dev_info(dev, "port_shaping_pass: 0x%x\n", bitmap->bit1);

	ret = hclge_dbg_cmd_send(hdev, desc, nq_id, 1, HCLGE_OPC_SCH_NQ_CNT);
	if (ret)
		return;

	dev_info(dev, "sch_nq_cnt: 0x%x\n", desc[0].data[1]);

	ret = hclge_dbg_cmd_send(hdev, desc, nq_id, 1, HCLGE_OPC_SCH_RQ_CNT);
	if (ret)
		return;

	dev_info(dev, "sch_rq_cnt: 0x%x\n", desc[0].data[1]);

	ret = hclge_dbg_cmd_send(hdev, desc, 0, 2, HCLGE_OPC_TM_INTERNAL_STS);
	if (ret)
		return;

	dev_info(dev, "pri_bp: 0x%x\n", desc[0].data[1]);
	dev_info(dev, "fifo_dfx_info: 0x%x\n", desc[0].data[2]);
	dev_info(dev, "sch_roce_fifo_afull_gap: 0x%x\n", desc[0].data[3]);
	dev_info(dev, "tx_private_waterline: 0x%x\n", desc[0].data[4]);
	dev_info(dev, "tm_bypass_en: 0x%x\n", desc[0].data[5]);
	dev_info(dev, "SSU_TM_BYPASS_EN: 0x%x\n", desc[1].data[0]);
	dev_info(dev, "SSU_RESERVE_CFG: 0x%x\n", desc[1].data[1]);

	ret = hclge_dbg_cmd_send(hdev, desc, port_id, 1,
				 HCLGE_OPC_TM_INTERNAL_CNT);
	if (ret)
		return;

	dev_info(dev, "SCH_NIC_NUM: 0x%x\n", desc[0].data[1]);
	dev_info(dev, "SCH_ROCE_NUM: 0x%x\n", desc[0].data[2]);

	ret = hclge_dbg_cmd_send(hdev, desc, port_id, 1,
				 HCLGE_OPC_TM_INTERNAL_STS_1);
	if (ret)
		return;

	dev_info(dev, "TC_MAP_SEL: 0x%x\n", desc[0].data[1]);
	dev_info(dev, "IGU_PFC_PRI_EN: 0x%x\n", desc[0].data[2]);
	dev_info(dev, "MAC_PFC_PRI_EN: 0x%x\n", desc[0].data[3]);
	dev_info(dev, "IGU_PRI_MAP_TC_CFG: 0x%x\n", desc[0].data[4]);
	dev_info(dev, "IGU_TX_PRI_MAP_TC_CFG: 0x%x\n", desc[0].data[5]);
}

static void hclge_dbg_dump_reg_cmd(struct hclge_dev *hdev, const char *cmd_buf)
{
	struct hclge_dbg_reg_type_info *reg_info;
	bool has_dump = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(hclge_dbg_reg_info); i++) {
		reg_info = &hclge_dbg_reg_info[i];
		if (!strncmp(cmd_buf, reg_info->reg_type,
			     strlen(reg_info->reg_type))) {
			hclge_dbg_dump_reg_common(hdev, reg_info, cmd_buf);
			has_dump = true;
		}
	}

	if (strncmp(cmd_buf, "dcb", 3) == 0) {
		hclge_dbg_dump_dcb(hdev, &cmd_buf[sizeof("dcb")]);
		has_dump = true;
	}

	if (!has_dump) {
		dev_info(&hdev->pdev->dev, "unknown command\n");
		return;
	}
}

static void hclge_title_idx_print(struct hclge_dev *hdev, bool flag, int index,
				  char *title_buf, char *true_buf,
				  char *false_buf)
{
	if (flag)
		dev_info(&hdev->pdev->dev, "%s(%d): %s\n", title_buf, index,
			 true_buf);
	else
		dev_info(&hdev->pdev->dev, "%s(%d): %s\n", title_buf, index,
			 false_buf);
}

static void hclge_dbg_dump_tc(struct hclge_dev *hdev)
{
	struct hclge_ets_tc_weight_cmd *ets_weight;
	struct hclge_desc desc;
	int i, ret;

	if (!hnae3_dev_dcb_supported(hdev)) {
		dev_info(&hdev->pdev->dev,
			 "Only DCB-supported dev supports tc\n");
		return;
	}

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_ETS_TC_WEIGHT, true);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev, "dump tc fail, ret = %d\n", ret);
		return;
	}

	ets_weight = (struct hclge_ets_tc_weight_cmd *)desc.data;

	dev_info(&hdev->pdev->dev, "dump tc\n");
	dev_info(&hdev->pdev->dev, "weight_offset: %u\n",
		 ets_weight->weight_offset);

	for (i = 0; i < HNAE3_MAX_TC; i++)
		hclge_title_idx_print(hdev, ets_weight->tc_weight[i], i,
				      "tc", "no sp mode", "sp mode");
}

static void hclge_dbg_dump_tm_pg(struct hclge_dev *hdev)
{
	struct hclge_port_shapping_cmd *port_shap_cfg_cmd;
	struct hclge_bp_to_qs_map_cmd *bp_to_qs_map_cmd;
	struct hclge_pg_shapping_cmd *pg_shap_cfg_cmd;
	enum hclge_opcode_type cmd;
	struct hclge_desc desc;
	int ret;

	cmd = HCLGE_OPC_TM_PG_C_SHAPPING;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_pg_cmd_send;

	pg_shap_cfg_cmd = (struct hclge_pg_shapping_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "PG_C pg_id: %u\n", pg_shap_cfg_cmd->pg_id);
	dev_info(&hdev->pdev->dev, "PG_C pg_shapping: 0x%x\n",
		 pg_shap_cfg_cmd->pg_shapping_para);

	cmd = HCLGE_OPC_TM_PG_P_SHAPPING;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_pg_cmd_send;

	pg_shap_cfg_cmd = (struct hclge_pg_shapping_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "PG_P pg_id: %u\n", pg_shap_cfg_cmd->pg_id);
	dev_info(&hdev->pdev->dev, "PG_P pg_shapping: 0x%x\n",
		 pg_shap_cfg_cmd->pg_shapping_para);

	cmd = HCLGE_OPC_TM_PORT_SHAPPING;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_pg_cmd_send;

	port_shap_cfg_cmd = (struct hclge_port_shapping_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "PORT port_shapping: 0x%x\n",
		 port_shap_cfg_cmd->port_shapping_para);

	cmd = HCLGE_OPC_TM_PG_SCH_MODE_CFG;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_pg_cmd_send;

	dev_info(&hdev->pdev->dev, "PG_SCH pg_id: %u\n", desc.data[0]);

	cmd = HCLGE_OPC_TM_PRI_SCH_MODE_CFG;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_pg_cmd_send;

	dev_info(&hdev->pdev->dev, "PRI_SCH pri_id: %u\n", desc.data[0]);

	cmd = HCLGE_OPC_TM_QS_SCH_MODE_CFG;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_pg_cmd_send;

	dev_info(&hdev->pdev->dev, "QS_SCH qs_id: %u\n", desc.data[0]);

	if (!hnae3_dev_dcb_supported(hdev)) {
		dev_info(&hdev->pdev->dev,
			 "Only DCB-supported dev supports tm mapping\n");
		return;
	}

	cmd = HCLGE_OPC_TM_BP_TO_QSET_MAPPING;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_pg_cmd_send;

	bp_to_qs_map_cmd = (struct hclge_bp_to_qs_map_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "BP_TO_QSET tc_id: %u\n",
		 bp_to_qs_map_cmd->tc_id);
	dev_info(&hdev->pdev->dev, "BP_TO_QSET qs_group_id: 0x%x\n",
		 bp_to_qs_map_cmd->qs_group_id);
	dev_info(&hdev->pdev->dev, "BP_TO_QSET qs_bit_map: 0x%x\n",
		 bp_to_qs_map_cmd->qs_bit_map);
	return;

err_tm_pg_cmd_send:
	dev_err(&hdev->pdev->dev, "dump tm_pg fail(0x%x), ret = %d\n",
		cmd, ret);
}

static void hclge_dbg_dump_tm(struct hclge_dev *hdev)
{
	struct hclge_priority_weight_cmd *priority_weight;
	struct hclge_pg_to_pri_link_cmd *pg_to_pri_map;
	struct hclge_qs_to_pri_link_cmd *qs_to_pri_map;
	struct hclge_nq_to_qs_link_cmd *nq_to_qs_map;
	struct hclge_pri_shapping_cmd *shap_cfg_cmd;
	struct hclge_pg_weight_cmd *pg_weight;
	struct hclge_qs_weight_cmd *qs_weight;
	enum hclge_opcode_type cmd;
	struct hclge_desc desc;
	int ret;

	cmd = HCLGE_OPC_TM_PG_TO_PRI_LINK;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	pg_to_pri_map = (struct hclge_pg_to_pri_link_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "dump tm\n");
	dev_info(&hdev->pdev->dev, "PG_TO_PRI gp_id: %u\n",
		 pg_to_pri_map->pg_id);
	dev_info(&hdev->pdev->dev, "PG_TO_PRI map: 0x%x\n",
		 pg_to_pri_map->pri_bit_map);

	cmd = HCLGE_OPC_TM_QS_TO_PRI_LINK;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	qs_to_pri_map = (struct hclge_qs_to_pri_link_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "QS_TO_PRI qs_id: %u\n",
		 qs_to_pri_map->qs_id);
	dev_info(&hdev->pdev->dev, "QS_TO_PRI priority: %u\n",
		 qs_to_pri_map->priority);
	dev_info(&hdev->pdev->dev, "QS_TO_PRI link_vld: %u\n",
		 qs_to_pri_map->link_vld);

	cmd = HCLGE_OPC_TM_NQ_TO_QS_LINK;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	nq_to_qs_map = (struct hclge_nq_to_qs_link_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "NQ_TO_QS nq_id: %u\n", nq_to_qs_map->nq_id);
	dev_info(&hdev->pdev->dev, "NQ_TO_QS qset_id: 0x%x\n",
		 nq_to_qs_map->qset_id);

	cmd = HCLGE_OPC_TM_PG_WEIGHT;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	pg_weight = (struct hclge_pg_weight_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "PG pg_id: %u\n", pg_weight->pg_id);
	dev_info(&hdev->pdev->dev, "PG dwrr: %u\n", pg_weight->dwrr);

	cmd = HCLGE_OPC_TM_QS_WEIGHT;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	qs_weight = (struct hclge_qs_weight_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "QS qs_id: %u\n", qs_weight->qs_id);
	dev_info(&hdev->pdev->dev, "QS dwrr: %u\n", qs_weight->dwrr);

	cmd = HCLGE_OPC_TM_PRI_WEIGHT;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	priority_weight = (struct hclge_priority_weight_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "PRI pri_id: %u\n", priority_weight->pri_id);
	dev_info(&hdev->pdev->dev, "PRI dwrr: %u\n", priority_weight->dwrr);

	cmd = HCLGE_OPC_TM_PRI_C_SHAPPING;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	shap_cfg_cmd = (struct hclge_pri_shapping_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "PRI_C pri_id: %u\n", shap_cfg_cmd->pri_id);
	dev_info(&hdev->pdev->dev, "PRI_C pri_shapping: 0x%x\n",
		 shap_cfg_cmd->pri_shapping_para);

	cmd = HCLGE_OPC_TM_PRI_P_SHAPPING;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_cmd_send;

	shap_cfg_cmd = (struct hclge_pri_shapping_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "PRI_P pri_id: %u\n", shap_cfg_cmd->pri_id);
	dev_info(&hdev->pdev->dev, "PRI_P pri_shapping: 0x%x\n",
		 shap_cfg_cmd->pri_shapping_para);

	hclge_dbg_dump_tm_pg(hdev);

	return;

err_tm_cmd_send:
	dev_err(&hdev->pdev->dev, "dump tm fail(0x%x), ret = %d\n",
		cmd, ret);
}

static void hclge_dbg_dump_tm_map(struct hclge_dev *hdev,
				  const char *cmd_buf)
{
	struct hclge_bp_to_qs_map_cmd *bp_to_qs_map_cmd;
	struct hclge_nq_to_qs_link_cmd *nq_to_qs_map;
	struct hclge_qs_to_pri_link_cmd *map;
	struct hclge_tqp_tx_queue_tc_cmd *tc;
	enum hclge_opcode_type cmd;
	struct hclge_desc desc;
	int queue_id, group_id;
	u32 qset_maping[32];
	int tc_id, qset_id;
	int pri_id, ret;
	u32 i;

	ret = kstrtouint(cmd_buf, 0, &queue_id);
	queue_id = (ret != 0) ? 0 : queue_id;

	cmd = HCLGE_OPC_TM_NQ_TO_QS_LINK;
	nq_to_qs_map = (struct hclge_nq_to_qs_link_cmd *)desc.data;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	nq_to_qs_map->nq_id = cpu_to_le16(queue_id);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_map_cmd_send;
	qset_id = nq_to_qs_map->qset_id & 0x3FF;

	cmd = HCLGE_OPC_TM_QS_TO_PRI_LINK;
	map = (struct hclge_qs_to_pri_link_cmd *)desc.data;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	map->qs_id = cpu_to_le16(qset_id);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_map_cmd_send;
	pri_id = map->priority;

	cmd = HCLGE_OPC_TQP_TX_QUEUE_TC;
	tc = (struct hclge_tqp_tx_queue_tc_cmd *)desc.data;
	hclge_cmd_setup_basic_desc(&desc, cmd, true);
	tc->queue_id = cpu_to_le16(queue_id);
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret)
		goto err_tm_map_cmd_send;
	tc_id = tc->tc_id & 0x7;

	dev_info(&hdev->pdev->dev, "queue_id | qset_id | pri_id | tc_id\n");
	dev_info(&hdev->pdev->dev, "%04d     | %04d    | %02d     | %02d\n",
		 queue_id, qset_id, pri_id, tc_id);

	if (!hnae3_dev_dcb_supported(hdev)) {
		dev_info(&hdev->pdev->dev,
			 "Only DCB-supported dev supports tm mapping\n");
		return;
	}

	cmd = HCLGE_OPC_TM_BP_TO_QSET_MAPPING;
	bp_to_qs_map_cmd = (struct hclge_bp_to_qs_map_cmd *)desc.data;
	for (group_id = 0; group_id < 32; group_id++) {
		hclge_cmd_setup_basic_desc(&desc, cmd, true);
		bp_to_qs_map_cmd->tc_id = tc_id;
		bp_to_qs_map_cmd->qs_group_id = group_id;
		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret)
			goto err_tm_map_cmd_send;

		qset_maping[group_id] = bp_to_qs_map_cmd->qs_bit_map;
	}

	dev_info(&hdev->pdev->dev, "index | tm bp qset maping:\n");

	i = 0;
	for (group_id = 0; group_id < 4; group_id++) {
		dev_info(&hdev->pdev->dev,
			 "%04d  | %08x:%08x:%08x:%08x:%08x:%08x:%08x:%08x\n",
			 group_id * 256, qset_maping[(u32)(i + 7)],
			 qset_maping[(u32)(i + 6)], qset_maping[(u32)(i + 5)],
			 qset_maping[(u32)(i + 4)], qset_maping[(u32)(i + 3)],
			 qset_maping[(u32)(i + 2)], qset_maping[(u32)(i + 1)],
			 qset_maping[i]);
		i += 8;
	}

	return;

err_tm_map_cmd_send:
	dev_err(&hdev->pdev->dev, "dump tqp map fail(0x%x), ret = %d\n",
		cmd, ret);
}

static void hclge_dbg_dump_qos_pause_cfg(struct hclge_dev *hdev)
{
	struct hclge_cfg_pause_param_cmd *pause_param;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CFG_MAC_PARA, true);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev, "dump checksum fail, ret = %d\n",
			ret);
		return;
	}

	pause_param = (struct hclge_cfg_pause_param_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "dump qos pause cfg\n");
	dev_info(&hdev->pdev->dev, "pause_trans_gap: 0x%x\n",
		 pause_param->pause_trans_gap);
	dev_info(&hdev->pdev->dev, "pause_trans_time: 0x%x\n",
		 pause_param->pause_trans_time);
}

static void hclge_dbg_dump_qos_pri_map(struct hclge_dev *hdev)
{
	struct hclge_qos_pri_map_cmd *pri_map;
	struct hclge_desc desc;
	int ret;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_PRI_TO_TC_MAPPING, true);

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"dump qos pri map fail, ret = %d\n", ret);
		return;
	}

	pri_map = (struct hclge_qos_pri_map_cmd *)desc.data;
	dev_info(&hdev->pdev->dev, "dump qos pri map\n");
	dev_info(&hdev->pdev->dev, "vlan_to_pri: 0x%x\n", pri_map->vlan_pri);
	dev_info(&hdev->pdev->dev, "pri_0_to_tc: 0x%x\n", pri_map->pri0_tc);
	dev_info(&hdev->pdev->dev, "pri_1_to_tc: 0x%x\n", pri_map->pri1_tc);
	dev_info(&hdev->pdev->dev, "pri_2_to_tc: 0x%x\n", pri_map->pri2_tc);
	dev_info(&hdev->pdev->dev, "pri_3_to_tc: 0x%x\n", pri_map->pri3_tc);
	dev_info(&hdev->pdev->dev, "pri_4_to_tc: 0x%x\n", pri_map->pri4_tc);
	dev_info(&hdev->pdev->dev, "pri_5_to_tc: 0x%x\n", pri_map->pri5_tc);
	dev_info(&hdev->pdev->dev, "pri_6_to_tc: 0x%x\n", pri_map->pri6_tc);
	dev_info(&hdev->pdev->dev, "pri_7_to_tc: 0x%x\n", pri_map->pri7_tc);
}

static void hclge_dbg_dump_qos_buf_cfg(struct hclge_dev *hdev)
{
	struct hclge_tx_buff_alloc_cmd *tx_buf_cmd;
	struct hclge_rx_priv_buff_cmd *rx_buf_cmd;
	struct hclge_rx_priv_wl_buf *rx_priv_wl;
	struct hclge_rx_com_wl *rx_packet_cnt;
	struct hclge_rx_com_thrd *rx_com_thrd;
	struct hclge_rx_com_wl *rx_com_wl;
	enum hclge_opcode_type cmd;
	struct hclge_desc desc[2];
	int i, ret;

	cmd = HCLGE_OPC_TX_BUFF_ALLOC;
	hclge_cmd_setup_basic_desc(desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, desc, 1);
	if (ret)
		goto err_qos_cmd_send;

	dev_info(&hdev->pdev->dev, "dump qos buf cfg\n");

	tx_buf_cmd = (struct hclge_tx_buff_alloc_cmd *)desc[0].data;
	for (i = 0; i < HCLGE_MAX_TC_NUM; i++)
		dev_info(&hdev->pdev->dev, "tx_packet_buf_tc_%d: 0x%x\n", i,
			 tx_buf_cmd->tx_pkt_buff[i]);

	cmd = HCLGE_OPC_RX_PRIV_BUFF_ALLOC;
	hclge_cmd_setup_basic_desc(desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, desc, 1);
	if (ret)
		goto err_qos_cmd_send;

	dev_info(&hdev->pdev->dev, "\n");
	rx_buf_cmd = (struct hclge_rx_priv_buff_cmd *)desc[0].data;
	for (i = 0; i < HCLGE_MAX_TC_NUM; i++)
		dev_info(&hdev->pdev->dev, "rx_packet_buf_tc_%d: 0x%x\n", i,
			 rx_buf_cmd->buf_num[i]);

	dev_info(&hdev->pdev->dev, "rx_share_buf: 0x%x\n",
		 rx_buf_cmd->shared_buf);

	cmd = HCLGE_OPC_RX_COM_WL_ALLOC;
	hclge_cmd_setup_basic_desc(desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, desc, 1);
	if (ret)
		goto err_qos_cmd_send;

	rx_com_wl = (struct hclge_rx_com_wl *)desc[0].data;
	dev_info(&hdev->pdev->dev, "\n");
	dev_info(&hdev->pdev->dev, "rx_com_wl: high: 0x%x, low: 0x%x\n",
		 rx_com_wl->com_wl.high, rx_com_wl->com_wl.low);

	cmd = HCLGE_OPC_RX_GBL_PKT_CNT;
	hclge_cmd_setup_basic_desc(desc, cmd, true);
	ret = hclge_cmd_send(&hdev->hw, desc, 1);
	if (ret)
		goto err_qos_cmd_send;

	rx_packet_cnt = (struct hclge_rx_com_wl *)desc[0].data;
	dev_info(&hdev->pdev->dev,
		 "rx_global_packet_cnt: high: 0x%x, low: 0x%x\n",
		 rx_packet_cnt->com_wl.high, rx_packet_cnt->com_wl.low);
	dev_info(&hdev->pdev->dev, "\n");

	if (!hnae3_dev_dcb_supported(hdev)) {
		dev_info(&hdev->pdev->dev,
			 "Only DCB-supported dev supports rx priv wl\n");
		return;
	}
	cmd = HCLGE_OPC_RX_PRIV_WL_ALLOC;
	hclge_cmd_setup_basic_desc(&desc[0], cmd, true);
	desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[1], cmd, true);
	ret = hclge_cmd_send(&hdev->hw, desc, 2);
	if (ret)
		goto err_qos_cmd_send;

	rx_priv_wl = (struct hclge_rx_priv_wl_buf *)desc[0].data;
	for (i = 0; i < HCLGE_TC_NUM_ONE_DESC; i++)
		dev_info(&hdev->pdev->dev,
			 "rx_priv_wl_tc_%d: high: 0x%x, low: 0x%x\n", i,
			 rx_priv_wl->tc_wl[i].high, rx_priv_wl->tc_wl[i].low);

	rx_priv_wl = (struct hclge_rx_priv_wl_buf *)desc[1].data;
	for (i = 0; i < HCLGE_TC_NUM_ONE_DESC; i++)
		dev_info(&hdev->pdev->dev,
			 "rx_priv_wl_tc_%d: high: 0x%x, low: 0x%x\n",
			 i + HCLGE_TC_NUM_ONE_DESC,
			 rx_priv_wl->tc_wl[i].high, rx_priv_wl->tc_wl[i].low);

	cmd = HCLGE_OPC_RX_COM_THRD_ALLOC;
	hclge_cmd_setup_basic_desc(&desc[0], cmd, true);
	desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[1], cmd, true);
	ret = hclge_cmd_send(&hdev->hw, desc, 2);
	if (ret)
		goto err_qos_cmd_send;

	dev_info(&hdev->pdev->dev, "\n");
	rx_com_thrd = (struct hclge_rx_com_thrd *)desc[0].data;
	for (i = 0; i < HCLGE_TC_NUM_ONE_DESC; i++)
		dev_info(&hdev->pdev->dev,
			 "rx_com_thrd_tc_%d: high: 0x%x, low: 0x%x\n", i,
			 rx_com_thrd->com_thrd[i].high,
			 rx_com_thrd->com_thrd[i].low);

	rx_com_thrd = (struct hclge_rx_com_thrd *)desc[1].data;
	for (i = 0; i < HCLGE_TC_NUM_ONE_DESC; i++)
		dev_info(&hdev->pdev->dev,
			 "rx_com_thrd_tc_%d: high: 0x%x, low: 0x%x\n",
			 i + HCLGE_TC_NUM_ONE_DESC,
			 rx_com_thrd->com_thrd[i].high,
			 rx_com_thrd->com_thrd[i].low);
	return;

err_qos_cmd_send:
	dev_err(&hdev->pdev->dev,
		"dump qos buf cfg fail(0x%x), ret = %d\n", cmd, ret);
}

static void hclge_dbg_dump_mng_table(struct hclge_dev *hdev)
{
	struct hclge_mac_ethertype_idx_rd_cmd *req0;
	char printf_buf[HCLGE_DBG_BUF_LEN];
	struct hclge_desc desc;
	int ret, i;

	dev_info(&hdev->pdev->dev, "mng tab:\n");
	memset(printf_buf, 0, HCLGE_DBG_BUF_LEN);
	strncat(printf_buf,
		"entry|mac_addr         |mask|ether|mask|vlan|mask",
		HCLGE_DBG_BUF_LEN - 1);
	strncat(printf_buf + strlen(printf_buf),
		"|i_map|i_dir|e_type|pf_id|vf_id|q_id|drop\n",
		HCLGE_DBG_BUF_LEN - strlen(printf_buf) - 1);

	dev_info(&hdev->pdev->dev, "%s", printf_buf);

	for (i = 0; i < HCLGE_DBG_MNG_TBL_MAX; i++) {
		hclge_cmd_setup_basic_desc(&desc, HCLGE_MAC_ETHERTYPE_IDX_RD,
					   true);
		req0 = (struct hclge_mac_ethertype_idx_rd_cmd *)&desc.data;
		req0->index = cpu_to_le16(i);

		ret = hclge_cmd_send(&hdev->hw, &desc, 1);
		if (ret) {
			dev_err(&hdev->pdev->dev,
				"call hclge_cmd_send fail, ret = %d\n", ret);
			return;
		}

		if (!req0->resp_code)
			continue;

		memset(printf_buf, 0, HCLGE_DBG_BUF_LEN);
		snprintf(printf_buf, HCLGE_DBG_BUF_LEN,
			 "%02u   |%02x:%02x:%02x:%02x:%02x:%02x|",
			 req0->index, req0->mac_addr[0], req0->mac_addr[1],
			 req0->mac_addr[2], req0->mac_addr[3],
			 req0->mac_addr[4], req0->mac_addr[5]);

		snprintf(printf_buf + strlen(printf_buf),
			 HCLGE_DBG_BUF_LEN - strlen(printf_buf),
			 "%x   |%04x |%x   |%04x|%x   |%02x   |%02x   |",
			 !!(req0->flags & HCLGE_DBG_MNG_MAC_MASK_B),
			 req0->ethter_type,
			 !!(req0->flags & HCLGE_DBG_MNG_ETHER_MASK_B),
			 req0->vlan_tag & HCLGE_DBG_MNG_VLAN_TAG,
			 !!(req0->flags & HCLGE_DBG_MNG_VLAN_MASK_B),
			 req0->i_port_bitmap, req0->i_port_direction);

		snprintf(printf_buf + strlen(printf_buf),
			 HCLGE_DBG_BUF_LEN - strlen(printf_buf),
			 "%d     |%d    |%02d   |%04d|%x\n",
			 !!(req0->egress_port & HCLGE_DBG_MNG_E_TYPE_B),
			 req0->egress_port & HCLGE_DBG_MNG_PF_ID,
			 (req0->egress_port >> 3) & HCLGE_DBG_MNG_VF_ID,
			 req0->egress_queue,
			 !!(req0->egress_port & HCLGE_DBG_MNG_DROP_B));

		dev_info(&hdev->pdev->dev, "%s", printf_buf);
	}
}

static void hclge_dbg_fd_tcam_read(struct hclge_dev *hdev, u8 stage,
				   bool sel_x, u32 loc)
{
	struct hclge_fd_tcam_config_1_cmd *req1;
	struct hclge_fd_tcam_config_2_cmd *req2;
	struct hclge_fd_tcam_config_3_cmd *req3;
	struct hclge_desc desc[3];
	int ret, i;
	u32 *req;

	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_FD_TCAM_OP, true);
	desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[1], HCLGE_OPC_FD_TCAM_OP, true);
	desc[1].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[2], HCLGE_OPC_FD_TCAM_OP, true);

	req1 = (struct hclge_fd_tcam_config_1_cmd *)desc[0].data;
	req2 = (struct hclge_fd_tcam_config_2_cmd *)desc[1].data;
	req3 = (struct hclge_fd_tcam_config_3_cmd *)desc[2].data;

	req1->stage  = stage;
	req1->xy_sel = sel_x ? 1 : 0;
	req1->index  = cpu_to_le32(loc);

	ret = hclge_cmd_send(&hdev->hw, desc, 3);
	if (ret)
		return;

	dev_info(&hdev->pdev->dev, " read result tcam key %s(%u):\n",
		 sel_x ? "x" : "y", loc);

	/* tcam_data0 ~ tcam_data1 */
	req = (u32 *)req1->tcam_data;
	for (i = 0; i < 2; i++)
		dev_info(&hdev->pdev->dev, "%08x\n", *req++);

	/* tcam_data2 ~ tcam_data7 */
	req = (u32 *)req2->tcam_data;
	for (i = 0; i < 6; i++)
		dev_info(&hdev->pdev->dev, "%08x\n", *req++);

	/* tcam_data8 ~ tcam_data12 */
	req = (u32 *)req3->tcam_data;
	for (i = 0; i < 5; i++)
		dev_info(&hdev->pdev->dev, "%08x\n", *req++);
}

static void hclge_dbg_fd_tcam(struct hclge_dev *hdev)
{
	u32 i;

	for (i = 0; i < hdev->fd_cfg.rule_num[0]; i++) {
		hclge_dbg_fd_tcam_read(hdev, 0, true, i);
		hclge_dbg_fd_tcam_read(hdev, 0, false, i);
	}
}

static void hclge_dbg_dump_rst_info(struct hclge_dev *hdev)
{
	dev_info(&hdev->pdev->dev, "PF reset count: %u\n",
		 hdev->rst_stats.pf_rst_cnt);
	dev_info(&hdev->pdev->dev, "FLR reset count: %u\n",
		 hdev->rst_stats.flr_rst_cnt);
	dev_info(&hdev->pdev->dev, "GLOBAL reset count: %u\n",
		 hdev->rst_stats.global_rst_cnt);
	dev_info(&hdev->pdev->dev, "IMP reset count: %u\n",
		 hdev->rst_stats.imp_rst_cnt);
	dev_info(&hdev->pdev->dev, "reset done count: %u\n",
		 hdev->rst_stats.reset_done_cnt);
	dev_info(&hdev->pdev->dev, "HW reset done count: %u\n",
		 hdev->rst_stats.hw_reset_done_cnt);
	dev_info(&hdev->pdev->dev, "reset count: %u\n",
		 hdev->rst_stats.reset_cnt);
	dev_info(&hdev->pdev->dev, "reset count: %u\n",
		 hdev->rst_stats.reset_cnt);
	dev_info(&hdev->pdev->dev, "reset fail count: %u\n",
		 hdev->rst_stats.reset_fail_cnt);
	dev_info(&hdev->pdev->dev, "vector0 interrupt enable status: 0x%x\n",
		 hclge_read_dev(&hdev->hw, HCLGE_MISC_VECTOR_REG_BASE));
	dev_info(&hdev->pdev->dev, "reset interrupt source: 0x%x\n",
		 hclge_read_dev(&hdev->hw, HCLGE_MISC_RESET_STS_REG));
	dev_info(&hdev->pdev->dev, "reset interrupt status: 0x%x\n",
		 hclge_read_dev(&hdev->hw, HCLGE_MISC_VECTOR_INT_STS));
	dev_info(&hdev->pdev->dev, "hardware reset status: 0x%x\n",
		 hclge_read_dev(&hdev->hw, HCLGE_GLOBAL_RESET_REG));
	dev_info(&hdev->pdev->dev, "handshake status: 0x%x\n",
		 hclge_read_dev(&hdev->hw, HCLGE_NIC_CSQ_DEPTH_REG));
	dev_info(&hdev->pdev->dev, "function reset status: 0x%x\n",
		 hclge_read_dev(&hdev->hw, HCLGE_FUN_RST_ING));
}

static void hclge_dbg_get_m7_stats_info(struct hclge_dev *hdev)
{
	struct hclge_desc *desc_src, *desc_tmp;
	struct hclge_get_m7_bd_cmd *req;
	struct hclge_desc desc;
	u32 bd_num, buf_len;
	int ret, i;

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_M7_STATS_BD, true);

	req = (struct hclge_get_m7_bd_cmd *)desc.data;
	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"get firmware statistics bd number failed, ret = %d\n",
			ret);
		return;
	}

	bd_num = le32_to_cpu(req->bd_num);

	buf_len	 = sizeof(struct hclge_desc) * bd_num;
	desc_src = kzalloc(buf_len, GFP_KERNEL);
	if (!desc_src) {
		dev_err(&hdev->pdev->dev,
			"allocate desc for get_m7_stats failed\n");
		return;
	}

	desc_tmp = desc_src;
	ret  = hclge_dbg_cmd_send(hdev, desc_tmp, 0, bd_num,
				  HCLGE_OPC_M7_STATS_INFO);
	if (ret) {
		kfree(desc_src);
		dev_err(&hdev->pdev->dev,
			"get firmware statistics failed, ret = %d\n", ret);
		return;
	}

	for (i = 0; i < bd_num; i++) {
		dev_info(&hdev->pdev->dev, "0x%08x  0x%08x  0x%08x\n",
			 le32_to_cpu(desc_tmp->data[0]),
			 le32_to_cpu(desc_tmp->data[1]),
			 le32_to_cpu(desc_tmp->data[2]));
		dev_info(&hdev->pdev->dev, "0x%08x  0x%08x  0x%08x\n",
			 le32_to_cpu(desc_tmp->data[3]),
			 le32_to_cpu(desc_tmp->data[4]),
			 le32_to_cpu(desc_tmp->data[5]));

		desc_tmp++;
	}

	kfree(desc_src);
}

#define HCLGE_CMD_NCL_CONFIG_BD_NUM	5

static void hclge_ncl_config_data_print(struct hclge_dev *hdev,
					struct hclge_desc *desc, int *offset,
					int *length)
{
#define HCLGE_CMD_DATA_NUM		6

	int i;
	int j;

	for (i = 0; i < HCLGE_CMD_NCL_CONFIG_BD_NUM; i++) {
		for (j = 0; j < HCLGE_CMD_DATA_NUM; j++) {
			if (i == 0 && j == 0)
				continue;

			dev_info(&hdev->pdev->dev, "0x%04x | 0x%08x\n",
				 *offset,
				 le32_to_cpu(desc[i].data[j]));
			*offset += sizeof(u32);
			*length -= sizeof(u32);
			if (*length <= 0)
				return;
		}
	}
}

/* hclge_dbg_dump_ncl_config: print specified range of NCL_CONFIG file
 * @hdev: pointer to struct hclge_dev
 * @cmd_buf: string that contains offset and length
 */
static void hclge_dbg_dump_ncl_config(struct hclge_dev *hdev,
				      const char *cmd_buf)
{
#define HCLGE_MAX_NCL_CONFIG_OFFSET	4096
#define HCLGE_MAX_NCL_CONFIG_LENGTH	(20 + 24 * 4)

	struct hclge_desc desc[HCLGE_CMD_NCL_CONFIG_BD_NUM];
	int bd_num = HCLGE_CMD_NCL_CONFIG_BD_NUM;
	int offset;
	int length;
	int data0;
	int ret;

	ret = sscanf(cmd_buf, "%x %x", &offset, &length);
	if (ret != 2 || offset >= HCLGE_MAX_NCL_CONFIG_OFFSET ||
	    length > HCLGE_MAX_NCL_CONFIG_OFFSET - offset) {
		dev_err(&hdev->pdev->dev, "Invalid offset or length.\n");
		return;
	}
	if (offset < 0 || length <= 0) {
		dev_err(&hdev->pdev->dev, "Non-positive offset or length.\n");
		return;
	}

	dev_info(&hdev->pdev->dev, "offset |    data\n");

	while (length > 0) {
		data0 = offset;
		if (length >= HCLGE_MAX_NCL_CONFIG_LENGTH)
			data0 |= HCLGE_MAX_NCL_CONFIG_LENGTH << 16;
		else
			data0 |= length << 16;
		ret = hclge_dbg_cmd_send(hdev, desc, data0, bd_num,
					 HCLGE_OPC_QUERY_NCL_CONFIG);
		if (ret)
			return;

		hclge_ncl_config_data_print(hdev, desc, &offset, &length);
	}
}

/* hclge_dbg_dump_mac_tnl_status: print message about mac tnl interrupt
 * @hdev: pointer to struct hclge_dev
 */
static void hclge_dbg_dump_mac_tnl_status(struct hclge_dev *hdev)
{
#define HCLGE_BILLION_NANO_SECONDS 1000000000

	struct hclge_mac_tnl_stats stats;
	unsigned long rem_nsec;

	dev_info(&hdev->pdev->dev, "Recently generated mac tnl interruption:\n");

	while (kfifo_get(&hdev->mac_tnl_log, &stats)) {
		rem_nsec = do_div(stats.time, HCLGE_BILLION_NANO_SECONDS);
		dev_info(&hdev->pdev->dev, "[%07lu.%03lu] status = 0x%x\n",
			 (unsigned long)stats.time, rem_nsec / 1000,
			 stats.status);
	}
}

int hclge_dbg_run_cmd(struct hnae3_handle *handle, const char *cmd_buf)
{
#define DUMP_REG	"dump reg"
#define DUMP_TM_MAP	"dump tm map"

	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (strncmp(cmd_buf, "dump fd tcam", 12) == 0) {
		hclge_dbg_fd_tcam(hdev);
	} else if (strncmp(cmd_buf, "dump tc", 7) == 0) {
		hclge_dbg_dump_tc(hdev);
	} else if (strncmp(cmd_buf, DUMP_TM_MAP, strlen(DUMP_TM_MAP)) == 0) {
		hclge_dbg_dump_tm_map(hdev, &cmd_buf[sizeof(DUMP_TM_MAP)]);
	} else if (strncmp(cmd_buf, "dump tm", 7) == 0) {
		hclge_dbg_dump_tm(hdev);
	} else if (strncmp(cmd_buf, "dump qos pause cfg", 18) == 0) {
		hclge_dbg_dump_qos_pause_cfg(hdev);
	} else if (strncmp(cmd_buf, "dump qos pri map", 16) == 0) {
		hclge_dbg_dump_qos_pri_map(hdev);
	} else if (strncmp(cmd_buf, "dump qos buf cfg", 16) == 0) {
		hclge_dbg_dump_qos_buf_cfg(hdev);
	} else if (strncmp(cmd_buf, "dump mng tbl", 12) == 0) {
		hclge_dbg_dump_mng_table(hdev);
	} else if (strncmp(cmd_buf, DUMP_REG, strlen(DUMP_REG)) == 0) {
		hclge_dbg_dump_reg_cmd(hdev, &cmd_buf[sizeof(DUMP_REG)]);
	} else if (strncmp(cmd_buf, "dump reset info", 15) == 0) {
		hclge_dbg_dump_rst_info(hdev);
	} else if (strncmp(cmd_buf, "dump m7 info", 12) == 0) {
		hclge_dbg_get_m7_stats_info(hdev);
	} else if (strncmp(cmd_buf, "dump ncl_config", 15) == 0) {
		hclge_dbg_dump_ncl_config(hdev,
					  &cmd_buf[sizeof("dump ncl_config")]);
	} else if (strncmp(cmd_buf, "dump mac tnl status", 19) == 0) {
		hclge_dbg_dump_mac_tnl_status(hdev);
	} else {
		dev_info(&hdev->pdev->dev, "unknown command\n");
		return -EINVAL;
	}

	return 0;
}
