// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2019-2020 NXP.
 */

/*
 * Configuration of the Tamper pins in different mode:
 *  - default (no tamper pins): _default_
 *  - passive mode expecting VCC on the line: "_passive_vcc_"
 *  - passive mode expecting VCC on the line: "_passive_gnd_"
 *  - active mode: "_active_"
 */

#include <command.h>
#include <log.h>
#include <stddef.h>
#include <common.h>
#include <asm/arch/sci/sci.h>
#include <asm/arch-imx8/imx8-pins.h>
#include <asm/arch-imx8/snvs_security_sc.h>

/* Access to gd */
DECLARE_GLOBAL_DATA_PTR;

#define SC_WRITE_CONF 1

#define PGD_HEX_VALUE 0x41736166
#define SRTC_EN 0x1
#define DP_EN BIT(5)

struct snvs_security_sc_conf {
	struct snvs_hp_conf {
		u32 lock;		/* HPLR - HP Lock */
		u32 __cmd;		/* HPCOMR - HP Command */
		u32 __ctl;		/* HPCR - HP Control */
		u32 secvio_intcfg;	/* HPSICR - Security Violation Int
					 * Config
					 */
		u32 secvio_ctl;		/* HPSVCR - Security Violation Control*/
		u32 status;		/* HPSR - HP Status */
		u32 secvio_status;	/* HPSVSR - Security Violation Status */
		u32 __ha_counteriv;	/* High Assurance Counter IV */
		u32 __ha_counter;		/* High Assurance Counter */
		u32 __rtc_msb;		/* Real Time Clock/Counter MSB */
		u32 __rtc_lsb;		/* Real Time Counter LSB */
		u32 __time_alarm_msb;	/* Time Alarm MSB */
		u32 __time_alarm_lsb;	/* Time Alarm LSB */
	} hp;
	struct snvs_lp_conf {
		u32 lock;
		u32 __ctl;
		u32 __mstr_key_ctl;	/* Master Key Control */
		u32 secvio_ctl;		/* Security Violation Control */
		u32 tamper_filt_cfg;	/* Tamper Glitch Filters Configuration*/
		u32 tamper_det_cfg;	/* Tamper Detectors Configuration */
		u32 status;
		u32 __srtc_msb;		/* Secure Real Time Clock/Counter MSB */
		u32 __srtc_lsb;		/* Secure Real Time Clock/Counter LSB */
		u32 __time_alarm;		/* Time Alarm */
		u32 __smc_msb;		/* Secure Monotonic Counter MSB */
		u32 __smc_lsb;		/* Secure Monotonic Counter LSB */
		u32 __pwr_glitch_det;	/* Power Glitch Detector */
		u32 __gen_purpose;
		u8 __zmk[32];		/* Zeroizable Master Key */
		u32 __rsvd0;
		u32 __gen_purposes[4];	/* gp0_30 to gp0_33 */
		u32 tamper_det_cfg2;	/* Tamper Detectors Configuration2 */
		u32 tamper_det_status;	/* Tamper Detectors status */
		u32 tamper_filt1_cfg;	/* Tamper Glitch Filter1 Configuration*/
		u32 tamper_filt2_cfg;	/* Tamper Glitch Filter2 Configuration*/
		u32 __rsvd1[4];
		u32 act_tamper1_cfg;	/* Active Tamper1 Configuration */
		u32 act_tamper2_cfg;	/* Active Tamper2 Configuration */
		u32 act_tamper3_cfg;	/* Active Tamper3 Configuration */
		u32 act_tamper4_cfg;	/* Active Tamper4 Configuration */
		u32 act_tamper5_cfg;	/* Active Tamper5 Configuration */
		u32 __rsvd2[3];
		u32 act_tamper_ctl;	/* Active Tamper Control */
		u32 act_tamper_clk_ctl;	/* Active Tamper Clock Control */
		u32 act_tamper_routing_ctl1;/* Active Tamper Routing Control1 */
		u32 act_tamper_routing_ctl2;/* Active Tamper Routing Control2 */
	} lp;
};

static struct snvs_security_sc_conf snvs_default_config = {
	.hp = {
		.lock = 0x1f0703ff,
		.secvio_ctl = 0x3000007f,
	},
	.lp = {
		.lock = 0x1f0003ff,
		.secvio_ctl = 0x36,
		.tamper_filt_cfg = 0,
		.tamper_det_cfg = 0x76, /* analogic tampers
					 * + rollover tampers
					 */
		.tamper_det_cfg2 = 0,
		.tamper_filt1_cfg = 0,
		.tamper_filt2_cfg = 0,
		.act_tamper1_cfg = 0,
		.act_tamper2_cfg = 0,
		.act_tamper3_cfg = 0,
		.act_tamper4_cfg = 0,
		.act_tamper5_cfg = 0,
		.act_tamper_ctl = 0,
		.act_tamper_clk_ctl = 0,
		.act_tamper_routing_ctl1 = 0,
		.act_tamper_routing_ctl2 = 0,
	}
};

static struct snvs_security_sc_conf snvs_passive_vcc_config = {
	.hp = {
		.lock = 0x1f0703ff,
		.secvio_ctl = 0x3000007f,
	},
	.lp = {
		.lock = 0x1f0003ff,
		.secvio_ctl = 0x36,
		.tamper_filt_cfg = 0,
		.tamper_det_cfg = 0x276, /* ET1 will trig on line at GND
					  *  + analogic tampers
					  *  + rollover tampers
					  */
		.tamper_det_cfg2 = 0,
		.tamper_filt1_cfg = 0,
		.tamper_filt2_cfg = 0,
		.act_tamper1_cfg = 0,
		.act_tamper2_cfg = 0,
		.act_tamper3_cfg = 0,
		.act_tamper4_cfg = 0,
		.act_tamper5_cfg = 0,
		.act_tamper_ctl = 0,
		.act_tamper_clk_ctl = 0,
		.act_tamper_routing_ctl1 = 0,
		.act_tamper_routing_ctl2 = 0,
	}
};

static struct snvs_security_sc_conf snvs_passive_gnd_config = {
	.hp = {
		.lock = 0x1f0703ff,
		.secvio_ctl = 0x3000007f,
	},
	.lp = {
		.lock = 0x1f0003ff,
		.secvio_ctl = 0x36,
		.tamper_filt_cfg = 0,
		.tamper_det_cfg = 0xa76, /* ET1 will trig on line at VCC
					  *  + analogic tampers
					  *  + rollover tampers
					  */
		.tamper_det_cfg2 = 0,
		.tamper_filt1_cfg = 0,
		.tamper_filt2_cfg = 0,
		.act_tamper1_cfg = 0,
		.act_tamper2_cfg = 0,
		.act_tamper3_cfg = 0,
		.act_tamper4_cfg = 0,
		.act_tamper5_cfg = 0,
		.act_tamper_ctl = 0,
		.act_tamper_clk_ctl = 0,
		.act_tamper_routing_ctl1 = 0,
		.act_tamper_routing_ctl2 = 0,
	}
};

static struct snvs_security_sc_conf snvs_active_config = {
	.hp = {
		.lock = 0x1f0703ff,
		.secvio_ctl = 0x3000007f,
	},
	.lp = {
		.lock = 0x1f0003ff,
		.secvio_ctl = 0x36,
		.tamper_filt_cfg = 0x00800000, /* Enable filtering */
		.tamper_det_cfg = 0x276, /* ET1 enabled + analogic tampers
					  *  + rollover tampers
					  */
		.tamper_det_cfg2 = 0,
		.tamper_filt1_cfg = 0,
		.tamper_filt2_cfg = 0,
		.act_tamper1_cfg = 0x84001111,
		.act_tamper2_cfg = 0,
		.act_tamper3_cfg = 0,
		.act_tamper4_cfg = 0,
		.act_tamper5_cfg = 0,
		.act_tamper_ctl = 0x00010001,
		.act_tamper_clk_ctl = 0,
		.act_tamper_routing_ctl1 = 0x1,
		.act_tamper_routing_ctl2 = 0,
	}
};

static struct snvs_security_sc_conf *get_snvs_config(void)
{
	return &snvs_default_config;
}

struct snvs_dgo_conf {
	u32 tamper_offset_ctl;
	u32 tamper_pull_ctl;
	u32 tamper_ana_test_ctl;
	u32 tamper_sensor_trim_ctl;
	u32 tamper_misc_ctl;
	u32 tamper_core_volt_mon_ctl;
};

static struct snvs_dgo_conf snvs_dgo_default_config = {
	.tamper_misc_ctl = 0x80000000, /* Lock the DGO */
};

static struct snvs_dgo_conf snvs_dgo_passive_vcc_config = {
	.tamper_misc_ctl = 0x80000000, /* Lock the DGO */
	.tamper_pull_ctl = 0x00000001, /* Pull down ET1 */
	.tamper_ana_test_ctl = 0x20000000, /* Enable tamper */
};

static struct snvs_dgo_conf snvs_dgo_passive_gnd_config = {
	.tamper_misc_ctl = 0x80000000, /* Lock the DGO */
	.tamper_pull_ctl = 0x00000401, /* Pull up ET1 */
	.tamper_ana_test_ctl = 0x20000000, /* Enable tamper */
};

static struct snvs_dgo_conf snvs_dgo_active_config = {
	.tamper_misc_ctl = 0x80000000, /* Lock the DGO */
	.tamper_ana_test_ctl = 0x20000000, /* Enable tamper */
};

static struct snvs_dgo_conf *get_snvs_dgo_config(void)
{
	return &snvs_dgo_default_config;
}

struct tamper_pin_cfg {
	u32 pad;
	u32 mux_conf;
};

static struct tamper_pin_cfg tamper_pin_list_default_config[] = {
	{SC_P_CSI_D00, 0}, /* Tamp_Out0 */
	{SC_P_CSI_D01, 0}, /* Tamp_Out1 */
	{SC_P_CSI_D02, 0}, /* Tamp_Out2 */
	{SC_P_CSI_D03, 0}, /* Tamp_Out3 */
	{SC_P_CSI_D04, 0}, /* Tamp_Out4 */
	{SC_P_CSI_D05, 0}, /* Tamp_In0 */
	{SC_P_CSI_D06, 0}, /* Tamp_In1 */
	{SC_P_CSI_D07, 0}, /* Tamp_In2 */
	{SC_P_CSI_HSYNC, 0}, /* Tamp_In3 */
	{SC_P_CSI_VSYNC, 0}, /* Tamp_In4 */
};

static struct tamper_pin_cfg tamper_pin_list_passive_vcc_config[] = {
	{SC_P_CSI_D05, 0x1c000060}, /* Tamp_In0 */ /* Sel tamper + OD input */
};

static struct tamper_pin_cfg tamper_pin_list_passive_gnd_config[] = {
	{SC_P_CSI_D05, 0x1c000060}, /* Tamp_In0 */ /* Sel tamper + OD input */
};

static struct tamper_pin_cfg tamper_pin_list_active_config[] = {
	{SC_P_CSI_D00, 0x1a000060}, /* Tamp_Out0 */ /* Sel tamper + OD */
	{SC_P_CSI_D05, 0x1c000060}, /* Tamp_In0 */ /* Sel tamper + OD input */
};

#define TAMPER_PIN_LIST_CHOSEN tamper_pin_list_default_config

static struct tamper_pin_cfg *get_tamper_pin_cfg_list(u32 *size)
{
	*size = sizeof(TAMPER_PIN_LIST_CHOSEN) /
		sizeof(TAMPER_PIN_LIST_CHOSEN[0]);

	return TAMPER_PIN_LIST_CHOSEN;
}

#define SC_CONF_OFFSET_OF(_field) \
	(offsetof(struct snvs_security_sc_conf, _field))

static u32 ptr_value(u32 *_p)
{
	return (_p) ? *_p : 0xdeadbeef;
}

static int check_write_secvio_config(u32 id, u32 *_p1, u32 *_p2,
				     u32 *_p3, u32 *_p4, u32 *_p5,
				     u32 _cnt)
{
	int scierr = 0;
	u32 d1 = ptr_value(_p1);
	u32 d2 = ptr_value(_p2);
	u32 d3 = ptr_value(_p3);
	u32 d4 = ptr_value(_p4);
	u32 d5 = ptr_value(_p5);

	scierr = sc_seco_secvio_config(-1, id, SC_WRITE_CONF, &d1, &d2, &d3,
				       &d4, &d4, _cnt);
	if (scierr != SC_ERR_NONE) {
		printf("Failed to set secvio configuration\n");
		debug("Failed to set conf id 0x%x with values ", id);
		debug("0x%.8x 0x%.8x 0x%.8x 0x%.8x 0x%.8x (cnt: %d)\n",
		      d1, d2, d3, d4, d5, _cnt);
		goto exit;
	}

	if (_p1)
		*(u32 *)_p1 = d1;
	if (_p2)
		*(u32 *)_p2 = d2;
	if (_p3)
		*(u32 *)_p3 = d3;
	if (_p4)
		*(u32 *)_p4 = d4;
	if (_p5)
		*(u32 *)_p5 = d5;

exit:
	return scierr;
}

#define SC_CHECK_WRITE1(id, _p1) \
	check_write_secvio_config(id, _p1, NULL, NULL, NULL, NULL, 1)

static int apply_snvs_config(struct snvs_security_sc_conf *cnf)
{
	int scierr = 0;

	debug("%s\n", __func__);

	debug("Applying config:\n"
		  "\thp.lock = 0x%.8x\n"
		  "\thp.secvio_ctl = 0x%.8x\n"
		  "\tlp.lock = 0x%.8x\n"
		  "\tlp.secvio_ctl = 0x%.8x\n"
		  "\tlp.tamper_filt_cfg = 0x%.8x\n"
		  "\tlp.tamper_det_cfg = 0x%.8x\n"
		  "\tlp.tamper_det_cfg2 = 0x%.8x\n"
		  "\tlp.tamper_filt1_cfg = 0x%.8x\n"
		  "\tlp.tamper_filt2_cfg = 0x%.8x\n"
		  "\tlp.act_tamper1_cfg = 0x%.8x\n"
		  "\tlp.act_tamper2_cfg = 0x%.8x\n"
		  "\tlp.act_tamper3_cfg = 0x%.8x\n"
		  "\tlp.act_tamper4_cfg = 0x%.8x\n"
		  "\tlp.act_tamper5_cfg = 0x%.8x\n"
		  "\tlp.act_tamper_ctl = 0x%.8x\n"
		  "\tlp.act_tamper_clk_ctl = 0x%.8x\n"
		  "\tlp.act_tamper_routing_ctl1 = 0x%.8x\n"
		  "\tlp.act_tamper_routing_ctl2 = 0x%.8x\n",
			cnf->hp.lock,
			cnf->hp.secvio_ctl,
			cnf->lp.lock,
			cnf->lp.secvio_ctl,
			cnf->lp.tamper_filt_cfg,
			cnf->lp.tamper_det_cfg,
			cnf->lp.tamper_det_cfg2,
			cnf->lp.tamper_filt1_cfg,
			cnf->lp.tamper_filt2_cfg,
			cnf->lp.act_tamper1_cfg,
			cnf->lp.act_tamper2_cfg,
			cnf->lp.act_tamper3_cfg,
			cnf->lp.act_tamper4_cfg,
			cnf->lp.act_tamper5_cfg,
			cnf->lp.act_tamper_ctl,
			cnf->lp.act_tamper_clk_ctl,
			cnf->lp.act_tamper_routing_ctl1,
			cnf->lp.act_tamper_routing_ctl2);

	scierr = check_write_secvio_config(SC_CONF_OFFSET_OF(lp.tamper_filt_cfg),
					   &cnf->lp.tamper_filt_cfg,
					   &cnf->lp.tamper_filt1_cfg,
					   &cnf->lp.tamper_filt2_cfg, NULL,
					   NULL, 3);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Configure AT */
	scierr = check_write_secvio_config(SC_CONF_OFFSET_OF(lp.act_tamper1_cfg),
					   &cnf->lp.act_tamper1_cfg,
					   &cnf->lp.act_tamper2_cfg,
					   &cnf->lp.act_tamper2_cfg,
					   &cnf->lp.act_tamper2_cfg,
					   &cnf->lp.act_tamper2_cfg, 5);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Configure AT routing */
	scierr = check_write_secvio_config(SC_CONF_OFFSET_OF(lp.act_tamper_routing_ctl1),
					   &cnf->lp.act_tamper_routing_ctl1,
					   &cnf->lp.act_tamper_routing_ctl2,
					   NULL, NULL, NULL, 2);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Configure AT frequency */
	scierr = SC_CHECK_WRITE1(SC_CONF_OFFSET_OF(lp.act_tamper_clk_ctl),
				 &cnf->lp.act_tamper_clk_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Activate the ATs */
	scierr = SC_CHECK_WRITE1(SC_CONF_OFFSET_OF(lp.act_tamper_ctl),
				 &cnf->lp.act_tamper_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Activate the detectors */
	scierr = check_write_secvio_config(SC_CONF_OFFSET_OF(lp.tamper_det_cfg),
					   &cnf->lp.tamper_det_cfg,
					   &cnf->lp.tamper_det_cfg2, NULL, NULL,
					   NULL, 2);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Configure LP secvio */
	scierr = SC_CHECK_WRITE1(SC_CONF_OFFSET_OF(lp.secvio_ctl),
				 &cnf->lp.secvio_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Configure HP secvio */
	scierr = SC_CHECK_WRITE1(SC_CONF_OFFSET_OF(hp.secvio_ctl),
				 &cnf->hp.secvio_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Lock access */
	scierr = SC_CHECK_WRITE1(SC_CONF_OFFSET_OF(hp.lock), &cnf->hp.lock);
	if (scierr != SC_ERR_NONE)
		goto exit;

	scierr = SC_CHECK_WRITE1(SC_CONF_OFFSET_OF(lp.lock), &cnf->lp.lock);
	if (scierr != SC_ERR_NONE)
		goto exit;

exit:
	return (scierr == SC_ERR_NONE) ? 0 : -EIO;
}

static int dgo_write(u32 _id, u8 _access, u32 *_pdata)
{
	int scierr = sc_seco_secvio_dgo_config(-1, _id, _access, _pdata);

	if (scierr != SC_ERR_NONE) {
		printf("Failed to set dgo configuration\n");
		debug("Failed to set conf id 0x%x : 0x%.8x", _id, *_pdata);
	}

	return scierr;
}

static int apply_snvs_dgo_config(struct snvs_dgo_conf *cnf)
{
	int scierr = 0;

	debug("%s\n", __func__);

	debug("Applying config:\n"
		"\ttamper_offset_ctl = 0x%.8x\n"
		"\ttamper_pull_ctl = 0x%.8x\n"
		"\ttamper_ana_test_ctl = 0x%.8x\n"
		"\ttamper_sensor_trim_ctl = 0x%.8x\n"
		"\ttamper_misc_ctl = 0x%.8x\n"
		"\ttamper_core_volt_mon_ctl = 0x%.8x\n",
			cnf->tamper_offset_ctl,
			cnf->tamper_pull_ctl,
			cnf->tamper_ana_test_ctl,
			cnf->tamper_sensor_trim_ctl,
			cnf->tamper_misc_ctl,
			cnf->tamper_core_volt_mon_ctl);

	dgo_write(0x04, 1, &cnf->tamper_offset_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	dgo_write(0x14, 1, &cnf->tamper_pull_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	dgo_write(0x24, 1, &cnf->tamper_ana_test_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	dgo_write(0x34, 1, &cnf->tamper_sensor_trim_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	dgo_write(0x54, 1, &cnf->tamper_core_volt_mon_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

	/* Last as it could lock the writes */
	dgo_write(0x44, 1, &cnf->tamper_misc_ctl);
	if (scierr != SC_ERR_NONE)
		goto exit;

exit:
	return (scierr == SC_ERR_NONE) ? 0 : -EIO;
}

static int pad_write(u32 _pad, u32 _value)
{
	int scierr = sc_pad_set(-1, _pad, _value);

	if (scierr != SC_ERR_NONE) {
		printf("Failed to set pad configuration\n");
		debug("Failed to set conf pad 0x%x : 0x%.8x", _pad, _value);
	}

	return scierr;
}

static int apply_tamper_pin_list_config(struct tamper_pin_cfg *confs, u32 size)
{
	int scierr = 0;
	u32 idx;

	debug("%s\n", __func__);

	for (idx = 0; idx < size; idx++) {
		debug("\t idx %d: pad %d: 0x%.8x\n", idx, confs[idx].pad,
		      confs[idx].mux_conf);
		pad_write(confs[idx].pad, 3 << 30 | confs[idx].mux_conf);
		if (scierr != SC_ERR_NONE)
			goto exit;
	}

exit:
	return (scierr == SC_ERR_NONE) ? 0 : -EIO;
}

int examples(void)
{
	u32 size;
	struct snvs_security_sc_conf *snvs_conf;
	struct snvs_dgo_conf *snvs_dgo_conf;
	struct tamper_pin_cfg *tamper_pin_conf;

	/* Caller */
	snvs_conf = get_snvs_config();
	snvs_dgo_conf = get_snvs_dgo_config();
	tamper_pin_conf = get_tamper_pin_cfg_list(&size);

	/* Default */
	snvs_conf = &snvs_default_config;
	snvs_dgo_conf = &snvs_dgo_default_config;
	tamper_pin_conf = tamper_pin_list_default_config;

	/* Passive tamper expecting VCC on the line */
	snvs_conf = &snvs_passive_vcc_config;
	snvs_dgo_conf = &snvs_dgo_passive_vcc_config;
	tamper_pin_conf = tamper_pin_list_passive_vcc_config;

	/* Passive tamper expecting GND on the line */
	snvs_conf = &snvs_passive_gnd_config;
	snvs_dgo_conf = &snvs_dgo_passive_gnd_config;
	tamper_pin_conf = tamper_pin_list_passive_gnd_config;

	/* Active tamper */
	snvs_conf = &snvs_active_config;
	snvs_dgo_conf = &snvs_dgo_active_config;
	tamper_pin_conf = tamper_pin_list_active_config;

	return !snvs_conf + !snvs_dgo_conf + !tamper_pin_conf;
}

#ifdef CONFIG_IMX_SNVS_SEC_SC_AUTO
int snvs_security_sc_init(void)
{
	int err = 0;

	struct snvs_security_sc_conf *snvs_conf;
	struct snvs_dgo_conf *snvs_dgo_conf;
	struct tamper_pin_cfg *tamper_pin_conf;
	u32 size;

	debug("%s\n", __func__);

	snvs_conf = get_snvs_config();
	snvs_dgo_conf = get_snvs_dgo_config();

	tamper_pin_conf = get_tamper_pin_cfg_list(&size);

	err = apply_tamper_pin_list_config(tamper_pin_conf, size);
	if (err) {
		debug("Failed to set pins\n");
		goto exit;
	}

	err = apply_snvs_dgo_config(snvs_dgo_conf);
	if (err) {
		debug("Failed to set dgo\n");
		goto exit;
	}

	err = apply_snvs_config(snvs_conf);
	if (err) {
		debug("Failed to set snvs\n");
		goto exit;
	}

exit:
	return err;
}
#endif /* CONFIG_IMX_SNVS_SEC_SC_AUTO */

static char snvs_cfg_help_text[] =
	"snvs_cfg\n"
	"\thp.lock\n"
	"\thp.secvio_ctl\n"
	"\tlp.lock\n"
	"\tlp.secvio_ctl\n"
	"\tlp.tamper_filt_cfg\n"
	"\tlp.tamper_det_cfg\n"
	"\tlp.tamper_det_cfg2\n"
	"\tlp.tamper_filt1_cfg\n"
	"\tlp.tamper_filt2_cfg\n"
	"\tlp.act_tamper1_cfg\n"
	"\tlp.act_tamper2_cfg\n"
	"\tlp.act_tamper3_cfg\n"
	"\tlp.act_tamper4_cfg\n"
	"\tlp.act_tamper5_cfg\n"
	"\tlp.act_tamper_ctl\n"
	"\tlp.act_tamper_clk_ctl\n"
	"\tlp.act_tamper_routing_ctl1\n"
	"\tlp.act_tamper_routing_ctl2\n"
	"\n"
	"ALL values should be in hexadecimal format";

#define NB_REGISTERS 18
static int do_snvs_cfg(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	int err = 0;
	u32 idx = 0;

	struct snvs_security_sc_conf conf = {0};

	if (argc != (NB_REGISTERS + 1))
		return CMD_RET_USAGE;

	conf.hp.lock = simple_strtoul(argv[++idx], NULL, 16);
	conf.hp.secvio_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.lock = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.secvio_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.tamper_filt_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.tamper_det_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.tamper_det_cfg2 = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.tamper_filt1_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.tamper_filt2_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper1_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper2_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper3_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper4_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper5_cfg = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper_clk_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper_routing_ctl1 = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.act_tamper_routing_ctl2 = simple_strtoul(argv[++idx], NULL, 16);

	err = apply_snvs_config(&conf);

	return err;
}

U_BOOT_CMD(snvs_cfg,
	   NB_REGISTERS + 1, 1, do_snvs_cfg,
	   "Security violation configuration",
	   snvs_cfg_help_text
);

static char snvs_dgo_cfg_help_text[] =
	"snvs_dgo_cfg\n"
	"\ttamper_offset_ctl\n"
	"\ttamper_pull_ctl\n"
	"\ttamper_ana_test_ctl\n"
	"\ttamper_sensor_trim_ctl\n"
	"\ttamper_misc_ctl\n"
	"\ttamper_core_volt_mon_ctl\n"
	"\n"
	"ALL values should be in hexadecimal format";

static int do_snvs_dgo_cfg(struct cmd_tbl *cmdtp, int flag, int argc,
			   char *const argv[])
{
	int err = 0;
	u32 idx = 0;

	struct snvs_dgo_conf conf = {0};

	if (argc != (6 + 1))
		return CMD_RET_USAGE;

	conf.tamper_offset_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.tamper_pull_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.tamper_ana_test_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.tamper_sensor_trim_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.tamper_misc_ctl = simple_strtoul(argv[++idx], NULL, 16);
	conf.tamper_core_volt_mon_ctl = simple_strtoul(argv[++idx], NULL, 16);

	err = apply_snvs_dgo_config(&conf);

	return err;
}

U_BOOT_CMD(snvs_dgo_cfg,
	   7, 1, do_snvs_dgo_cfg,
	   "SNVS DGO configuration",
	   snvs_dgo_cfg_help_text
);

static char tamper_pin_cfg_help_text[] =
	"snvs_dgo_cfg\n"
	"\tpad\n"
	"\tvalue\n"
	"\n"
	"ALL values should be in hexadecimal format";

static int do_tamper_pin_cfg(struct cmd_tbl *cmdtp, int flag, int argc,
			     char *const argv[])
{
	int err = 0;
	u32 idx = 0;

	struct tamper_pin_cfg conf = {0};

	if (argc != (2 + 1))
		return CMD_RET_USAGE;

	conf.pad = simple_strtoul(argv[++idx], NULL, 10);
	conf.mux_conf = simple_strtoul(argv[++idx], NULL, 16);

	err = apply_tamper_pin_list_config(&conf, 1);

	return err;
}

U_BOOT_CMD(tamper_pin_cfg,
	   3, 1, do_tamper_pin_cfg,
	   "tamper pin configuration",
	   tamper_pin_cfg_help_text
);

static char snvs_clear_status_help_text[] =
	"snvs_clear_status\n"
	"\tHPSR\n"
	"\tHPSVSR\n"
	"\tLPSR\n"
	"\tLPTDSR\n"
	"\n"
	"Write the status registers with the value provided,"
	" clearing the status";

static int do_snvs_clear_status(struct cmd_tbl *cmdtp, int flag, int argc,
				char *const argv[])
{
	int scierr = 0;
	u32 idx = 0;

	struct snvs_security_sc_conf conf = {0};

	if (argc != (2 + 1))
		return CMD_RET_USAGE;

	conf.lp.status = simple_strtoul(argv[++idx], NULL, 16);
	conf.lp.tamper_det_status = simple_strtoul(argv[++idx], NULL, 16);

	scierr = check_write_secvio_config(SC_CONF_OFFSET_OF(lp.status),
					   &conf.lp.status, NULL, NULL, NULL,
					   NULL, 1);
	if (scierr != SC_ERR_NONE)
		goto exit;

	scierr = check_write_secvio_config(SC_CONF_OFFSET_OF(lp.tamper_det_status),
					   &conf.lp.tamper_det_status, NULL,
					   NULL, NULL, NULL, 1);
	if (scierr != SC_ERR_NONE)
		goto exit;

exit:
	return (scierr == SC_ERR_NONE) ? 0 : 1;
}

U_BOOT_CMD(snvs_clear_status,
	   3, 1, do_snvs_clear_status,
	   "snvs clear status",
	   snvs_clear_status_help_text
);

static char snvs_sec_status_help_text[] =
	"snvs_sec_status\n"
	"Display information about the security related to tamper and secvio";

static int do_snvs_sec_status(struct cmd_tbl *cmdtp, int flag, int argc,
			      char *const argv[])
{
	int scierr;
	u32 idx;

	u32 data[5];

	u32 pads[] = {
		SC_P_CSI_D00,
		SC_P_CSI_D01,
		SC_P_CSI_D02,
		SC_P_CSI_D03,
		SC_P_CSI_D04,
		SC_P_CSI_D05,
		SC_P_CSI_D06,
		SC_P_CSI_D07,
		SC_P_CSI_HSYNC,
		SC_P_CSI_VSYNC,
	};

	u32 fuses[] = {
		14,
		30,
		31,
		260,
		261,
		262,
		263,
		768,
	};

	struct snvs_reg {
		u32 id;
		u32 nb;
	} snvs[] = {
		/* Locks */
		{0x0,  1},
		{0x34, 1},
		/* Security violation */
		{0xc,  1},
		{0x10, 1},
		{0x18, 1},
		{0x40, 1},
		/* Temper detectors */
		{0x48, 2},
		{0x4c, 1},
		{0xa4, 1},
		/* */
		{0x44, 3},
		{0xe0, 1},
		{0xe4, 1},
		{0xe8, 2},
		/* Misc */
		{0x3c, 1},
		{0x5c, 2},
		{0x64, 1},
		{0xf8, 2},
	};

	u32 dgo[] = {
		0x0,
		0x10,
		0x20,
		0x30,
		0x40,
		0x50,
	};

	/* Pins */
	printf("Pins:\n");
	for (idx = 0; idx < ARRAY_SIZE(pads); idx++) {
		u8 pad_id = pads[idx];

		scierr = sc_pad_get(-1, pad_id, &data[0]);
		if (scierr == 0)
			printf("\t- Pin %d: %.8x\n", pad_id, data[0]);
		else
			printf("Failed to read Pin %d\n", pad_id);
	}

	/* Fuses */
	printf("Fuses:\n");
	for (idx = 0; idx < ARRAY_SIZE(fuses); idx++) {
		u32 fuse_id = fuses[idx];

		scierr = sc_misc_otp_fuse_read(-1, fuse_id, &data[0]);
		if (scierr == 0)
			printf("\t- Fuse %d: %.8x\n", fuse_id, data[0]);
		else
			printf("Failed to read Fuse %d\n", fuse_id);
	}

	/* SNVS */
	printf("SNVS:\n");
	for (idx = 0; idx < ARRAY_SIZE(snvs); idx++) {
		struct snvs_reg *reg = &snvs[idx];

		scierr = sc_seco_secvio_config(-1, reg->id, 0, &data[0],
					       &data[1], &data[2], &data[3],
					       &data[4], reg->nb);
		if (scierr == 0) {
			int subidx;

			printf("\t- SNVS %.2x(%d):", reg->id, reg->nb);
			for (subidx = 0; subidx < reg->nb; subidx++)
				printf(" %.8x", data[subidx]);
			printf("\n");
		} else {
			printf("Failed to read SNVS %d\n", reg->id);
		}
	}

	/* DGO */
	printf("DGO:\n");
	for (idx = 0; idx < ARRAY_SIZE(dgo); idx++) {
		u8 dgo_id = dgo[idx];

		scierr = sc_seco_secvio_dgo_config(-1, dgo_id, 0, &data[0]);
		if (scierr == 0)
			printf("\t- DGO %.2x: %.8x\n", dgo_id, data[0]);
		else
			printf("Failed to read DGO %d\n", dgo_id);
	}

	return 0;
}

U_BOOT_CMD(snvs_sec_status,
	   1, 1, do_snvs_sec_status,
	   "tamper pin configuration",
	   snvs_sec_status_help_text
);
