// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Common Performance counter support functions for PowerISA v2.07 processors.
 *
 * Copyright 2009 Paul Mackerras, IBM Corporation.
 * Copyright 2013 Michael Ellerman, IBM Corporation.
 * Copyright 2016 Madhavan Srinivasan, IBM Corporation.
 */
#include "isa207-common.h"

PMU_FORMAT_ATTR(event,		"config:0-49");
PMU_FORMAT_ATTR(pmcxsel,	"config:0-7");
PMU_FORMAT_ATTR(mark,		"config:8");
PMU_FORMAT_ATTR(combine,	"config:11");
PMU_FORMAT_ATTR(unit,		"config:12-15");
PMU_FORMAT_ATTR(pmc,		"config:16-19");
PMU_FORMAT_ATTR(cache_sel,	"config:20-23");
PMU_FORMAT_ATTR(sample_mode,	"config:24-28");
PMU_FORMAT_ATTR(thresh_sel,	"config:29-31");
PMU_FORMAT_ATTR(thresh_stop,	"config:32-35");
PMU_FORMAT_ATTR(thresh_start,	"config:36-39");
PMU_FORMAT_ATTR(thresh_cmp,	"config:40-49");

struct attribute *isa207_pmu_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_pmcxsel.attr,
	&format_attr_mark.attr,
	&format_attr_combine.attr,
	&format_attr_unit.attr,
	&format_attr_pmc.attr,
	&format_attr_cache_sel.attr,
	&format_attr_sample_mode.attr,
	&format_attr_thresh_sel.attr,
	&format_attr_thresh_stop.attr,
	&format_attr_thresh_start.attr,
	&format_attr_thresh_cmp.attr,
	NULL,
};

struct attribute_group isa207_pmu_format_group = {
	.name = "format",
	.attrs = isa207_pmu_format_attr,
};

static inline bool event_is_fab_match(u64 event)
{
	/* Only check pmc, unit and pmcxsel, ignore the edge bit (0) */
	event &= 0xff0fe;

	/* PM_MRK_FAB_RSP_MATCH & PM_MRK_FAB_RSP_MATCH_CYC */
	return (event == 0x30056 || event == 0x4f052);
}

static bool is_event_valid(u64 event)
{
	u64 valid_mask = EVENT_VALID_MASK;

	if (cpu_has_feature(CPU_FTR_ARCH_300))
		valid_mask = p9_EVENT_VALID_MASK;

	return !(event & ~valid_mask);
}

static inline bool is_event_marked(u64 event)
{
	if (event & EVENT_IS_MARKED)
		return true;

	return false;
}

static void mmcra_sdar_mode(u64 event, unsigned long *mmcra)
{
	/*
	 * MMCRA[SDAR_MODE] specifices how the SDAR should be updated in
	 * continous sampling mode.
	 *
	 * Incase of Power8:
	 * MMCRA[SDAR_MODE] will be programmed as "0b01" for continous sampling
	 * mode and will be un-changed when setting MMCRA[63] (Marked events).
	 *
	 * Incase of Power9:
	 * Marked event: MMCRA[SDAR_MODE] will be set to 0b00 ('No Updates'),
	 *               or if group already have any marked events.
	 * For rest
	 *	MMCRA[SDAR_MODE] will be set from event code.
	 *      If sdar_mode from event is zero, default to 0b01. Hardware
	 *      requires that we set a non-zero value.
	 */
	if (cpu_has_feature(CPU_FTR_ARCH_300)) {
		if (is_event_marked(event) || (*mmcra & MMCRA_SAMPLE_ENABLE))
			*mmcra &= MMCRA_SDAR_MODE_NO_UPDATES;
		else if (p9_SDAR_MODE(event))
			*mmcra |=  p9_SDAR_MODE(event) << MMCRA_SDAR_MODE_SHIFT;
		else
			*mmcra |= MMCRA_SDAR_MODE_DCACHE;
	} else
		*mmcra |= MMCRA_SDAR_MODE_TLB;
}

static u64 thresh_cmp_val(u64 value)
{
	if (cpu_has_feature(CPU_FTR_ARCH_300))
		return value << p9_MMCRA_THR_CMP_SHIFT;

	return value << MMCRA_THR_CMP_SHIFT;
}

static unsigned long combine_from_event(u64 event)
{
	if (cpu_has_feature(CPU_FTR_ARCH_300))
		return p9_EVENT_COMBINE(event);

	return EVENT_COMBINE(event);
}

static unsigned long combine_shift(unsigned long pmc)
{
	if (cpu_has_feature(CPU_FTR_ARCH_300))
		return p9_MMCR1_COMBINE_SHIFT(pmc);

	return MMCR1_COMBINE_SHIFT(pmc);
}

static inline bool event_is_threshold(u64 event)
{
	return (event >> EVENT_THR_SEL_SHIFT) & EVENT_THR_SEL_MASK;
}

static bool is_thresh_cmp_valid(u64 event)
{
	unsigned int cmp, exp;

	/*
	 * Check the mantissa upper two bits are not zero, unless the
	 * exponent is also zero. See the THRESH_CMP_MANTISSA doc.
	 */
	cmp = (event >> EVENT_THR_CMP_SHIFT) & EVENT_THR_CMP_MASK;
	exp = cmp >> 7;

	if (exp && (cmp & 0x60) == 0)
		return false;

	return true;
}

static unsigned int dc_ic_rld_quad_l1_sel(u64 event)
{
	unsigned int cache;

	cache = (event >> EVENT_CACHE_SEL_SHIFT) & MMCR1_DC_IC_QUAL_MASK;
	return cache;
}

static inline u64 isa207_find_source(u64 idx, u32 sub_idx)
{
	u64 ret = PERF_MEM_NA;

	switch(idx) {
	case 0:
		/* Nothing to do */
		break;
	case 1:
		ret = PH(LVL, L1);
		break;
	case 2:
		ret = PH(LVL, L2);
		break;
	case 3:
		ret = PH(LVL, L3);
		break;
	case 4:
		if (sub_idx <= 1)
			ret = PH(LVL, LOC_RAM);
		else if (sub_idx > 1 && sub_idx <= 2)
			ret = PH(LVL, REM_RAM1);
		else
			ret = PH(LVL, REM_RAM2);
		ret |= P(SNOOP, HIT);
		break;
	case 5:
		ret = PH(LVL, REM_CCE1);
		if ((sub_idx == 0) || (sub_idx == 2) || (sub_idx == 4))
			ret |= P(SNOOP, HIT);
		else if ((sub_idx == 1) || (sub_idx == 3) || (sub_idx == 5))
			ret |= P(SNOOP, HITM);
		break;
	case 6:
		ret = PH(LVL, REM_CCE2);
		if ((sub_idx == 0) || (sub_idx == 2))
			ret |= P(SNOOP, HIT);
		else if ((sub_idx == 1) || (sub_idx == 3))
			ret |= P(SNOOP, HITM);
		break;
	case 7:
		ret = PM(LVL, L1);
		break;
	}

	return ret;
}

void isa207_get_mem_data_src(union perf_mem_data_src *dsrc, u32 flags,
							struct pt_regs *regs)
{
	u64 idx;
	u32 sub_idx;
	u64 sier;
	u64 val;

	/* Skip if no SIER support */
	if (!(flags & PPMU_HAS_SIER)) {
		dsrc->val = 0;
		return;
	}

	sier = mfspr(SPRN_SIER);
	val = (sier & ISA207_SIER_TYPE_MASK) >> ISA207_SIER_TYPE_SHIFT;
	if (val == 1 || val == 2) {
		idx = (sier & ISA207_SIER_LDST_MASK) >> ISA207_SIER_LDST_SHIFT;
		sub_idx = (sier & ISA207_SIER_DATA_SRC_MASK) >> ISA207_SIER_DATA_SRC_SHIFT;

		dsrc->val = isa207_find_source(idx, sub_idx);
		dsrc->val |= (val == 1) ? P(OP, LOAD) : P(OP, STORE);
	}
}

void isa207_get_mem_weight(u64 *weight)
{
	u64 mmcra = mfspr(SPRN_MMCRA);
	u64 exp = MMCRA_THR_CTR_EXP(mmcra);
	u64 mantissa = MMCRA_THR_CTR_MANT(mmcra);
	u64 sier = mfspr(SPRN_SIER);
	u64 val = (sier & ISA207_SIER_TYPE_MASK) >> ISA207_SIER_TYPE_SHIFT;

	if (val == 0 || val == 7)
		*weight = 0;
	else
		*weight = mantissa << (2 * exp);
}

int isa207_get_constraint(u64 event, unsigned long *maskp, unsigned long *valp)
{
	unsigned int unit, pmc, cache, ebb;
	unsigned long mask, value;

	mask = value = 0;

	if (!is_event_valid(event))
		return -1;

	pmc   = (event >> EVENT_PMC_SHIFT)        & EVENT_PMC_MASK;
	unit  = (event >> EVENT_UNIT_SHIFT)       & EVENT_UNIT_MASK;
	cache = (event >> EVENT_CACHE_SEL_SHIFT)  & EVENT_CACHE_SEL_MASK;
	ebb   = (event >> EVENT_EBB_SHIFT)        & EVENT_EBB_MASK;

	if (pmc) {
		u64 base_event;

		if (pmc > 6)
			return -1;

		/* Ignore Linux defined bits when checking event below */
		base_event = event & ~EVENT_LINUX_MASK;

		if (pmc >= 5 && base_event != 0x500fa &&
				base_event != 0x600f4)
			return -1;

		mask  |= CNST_PMC_MASK(pmc);
		value |= CNST_PMC_VAL(pmc);

		/*
		 * PMC5 and PMC6 are used to count cycles and instructions and
		 * they do not support most of the constraint bits. Add a check
		 * to exclude PMC5/6 from most of the constraints except for
		 * EBB/BHRB.
		 */
		if (pmc >= 5)
			goto ebb_bhrb;
	}

	if (pmc <= 4) {
		/*
		 * Add to number of counters in use. Note this includes events with
		 * a PMC of 0 - they still need a PMC, it's just assigned later.
		 * Don't count events on PMC 5 & 6, there is only one valid event
		 * on each of those counters, and they are handled above.
		 */
		mask  |= CNST_NC_MASK;
		value |= CNST_NC_VAL;
	}

	if (unit >= 6 && unit <= 9) {
		if (cpu_has_feature(CPU_FTR_ARCH_300)) {
			mask  |= CNST_CACHE_GROUP_MASK;
			value |= CNST_CACHE_GROUP_VAL(event & 0xff);

			mask |= CNST_CACHE_PMC4_MASK;
			if (pmc == 4)
				value |= CNST_CACHE_PMC4_VAL;
		} else if (cache & 0x7) {
			/*
			 * L2/L3 events contain a cache selector field, which is
			 * supposed to be programmed into MMCRC. However MMCRC is only
			 * HV writable, and there is no API for guest kernels to modify
			 * it. The solution is for the hypervisor to initialise the
			 * field to zeroes, and for us to only ever allow events that
			 * have a cache selector of zero. The bank selector (bit 3) is
			 * irrelevant, as long as the rest of the value is 0.
			 */
			return -1;
		}

	} else if (cpu_has_feature(CPU_FTR_ARCH_300) || (event & EVENT_IS_L1)) {
		mask  |= CNST_L1_QUAL_MASK;
		value |= CNST_L1_QUAL_VAL(cache);
	}

	if (is_event_marked(event)) {
		mask  |= CNST_SAMPLE_MASK;
		value |= CNST_SAMPLE_VAL(event >> EVENT_SAMPLE_SHIFT);
	}

	if (cpu_has_feature(CPU_FTR_ARCH_300))  {
		if (event_is_threshold(event) && is_thresh_cmp_valid(event)) {
			mask  |= CNST_THRESH_MASK;
			value |= CNST_THRESH_VAL(event >> EVENT_THRESH_SHIFT);
		}
	} else {
		/*
		 * Special case for PM_MRK_FAB_RSP_MATCH and PM_MRK_FAB_RSP_MATCH_CYC,
		 * the threshold control bits are used for the match value.
		 */
		if (event_is_fab_match(event)) {
			mask  |= CNST_FAB_MATCH_MASK;
			value |= CNST_FAB_MATCH_VAL(event >> EVENT_THR_CTL_SHIFT);
		} else {
			if (!is_thresh_cmp_valid(event))
				return -1;

			mask  |= CNST_THRESH_MASK;
			value |= CNST_THRESH_VAL(event >> EVENT_THRESH_SHIFT);
		}
	}

ebb_bhrb:
	if (!pmc && ebb)
		/* EBB events must specify the PMC */
		return -1;

	if (event & EVENT_WANTS_BHRB) {
		if (!ebb)
			/* Only EBB events can request BHRB */
			return -1;

		mask  |= CNST_IFM_MASK;
		value |= CNST_IFM_VAL(event >> EVENT_IFM_SHIFT);
	}

	/*
	 * All events must agree on EBB, either all request it or none.
	 * EBB events are pinned & exclusive, so this should never actually
	 * hit, but we leave it as a fallback in case.
	 */
	mask  |= CNST_EBB_VAL(ebb);
	value |= CNST_EBB_MASK;

	*maskp = mask;
	*valp = value;

	return 0;
}

int isa207_compute_mmcr(u64 event[], int n_ev,
			       unsigned int hwc[], unsigned long mmcr[],
			       struct perf_event *pevents[])
{
	unsigned long mmcra, mmcr1, mmcr2, unit, combine, psel, cache, val;
	unsigned int pmc, pmc_inuse;
	int i;

	pmc_inuse = 0;

	/* First pass to count resource use */
	for (i = 0; i < n_ev; ++i) {
		pmc = (event[i] >> EVENT_PMC_SHIFT) & EVENT_PMC_MASK;
		if (pmc)
			pmc_inuse |= 1 << pmc;
	}

	mmcra = mmcr1 = mmcr2 = 0;

	/* Second pass: assign PMCs, set all MMCR1 fields */
	for (i = 0; i < n_ev; ++i) {
		pmc     = (event[i] >> EVENT_PMC_SHIFT) & EVENT_PMC_MASK;
		unit    = (event[i] >> EVENT_UNIT_SHIFT) & EVENT_UNIT_MASK;
		combine = combine_from_event(event[i]);
		psel    =  event[i] & EVENT_PSEL_MASK;

		if (!pmc) {
			for (pmc = 1; pmc <= 4; ++pmc) {
				if (!(pmc_inuse & (1 << pmc)))
					break;
			}

			pmc_inuse |= 1 << pmc;
		}

		if (pmc <= 4) {
			mmcr1 |= unit << MMCR1_UNIT_SHIFT(pmc);
			mmcr1 |= combine << combine_shift(pmc);
			mmcr1 |= psel << MMCR1_PMCSEL_SHIFT(pmc);
		}

		/* In continuous sampling mode, update SDAR on TLB miss */
		mmcra_sdar_mode(event[i], &mmcra);

		if (cpu_has_feature(CPU_FTR_ARCH_300)) {
			cache = dc_ic_rld_quad_l1_sel(event[i]);
			mmcr1 |= (cache) << MMCR1_DC_IC_QUAL_SHIFT;
		} else {
			if (event[i] & EVENT_IS_L1) {
				cache = dc_ic_rld_quad_l1_sel(event[i]);
				mmcr1 |= (cache) << MMCR1_DC_IC_QUAL_SHIFT;
			}
		}

		if (is_event_marked(event[i])) {
			mmcra |= MMCRA_SAMPLE_ENABLE;

			val = (event[i] >> EVENT_SAMPLE_SHIFT) & EVENT_SAMPLE_MASK;
			if (val) {
				mmcra |= (val &  3) << MMCRA_SAMP_MODE_SHIFT;
				mmcra |= (val >> 2) << MMCRA_SAMP_ELIG_SHIFT;
			}
		}

		/*
		 * PM_MRK_FAB_RSP_MATCH and PM_MRK_FAB_RSP_MATCH_CYC,
		 * the threshold bits are used for the match value.
		 */
		if (!cpu_has_feature(CPU_FTR_ARCH_300) && event_is_fab_match(event[i])) {
			mmcr1 |= ((event[i] >> EVENT_THR_CTL_SHIFT) &
				  EVENT_THR_CTL_MASK) << MMCR1_FAB_SHIFT;
		} else {
			val = (event[i] >> EVENT_THR_CTL_SHIFT) & EVENT_THR_CTL_MASK;
			mmcra |= val << MMCRA_THR_CTL_SHIFT;
			val = (event[i] >> EVENT_THR_SEL_SHIFT) & EVENT_THR_SEL_MASK;
			mmcra |= val << MMCRA_THR_SEL_SHIFT;
			val = (event[i] >> EVENT_THR_CMP_SHIFT) & EVENT_THR_CMP_MASK;
			mmcra |= thresh_cmp_val(val);
		}

		if (event[i] & EVENT_WANTS_BHRB) {
			val = (event[i] >> EVENT_IFM_SHIFT) & EVENT_IFM_MASK;
			mmcra |= val << MMCRA_IFM_SHIFT;
		}

		if (pevents[i]->attr.exclude_user)
			mmcr2 |= MMCR2_FCP(pmc);

		if (pevents[i]->attr.exclude_hv)
			mmcr2 |= MMCR2_FCH(pmc);

		if (pevents[i]->attr.exclude_kernel) {
			if (cpu_has_feature(CPU_FTR_HVMODE))
				mmcr2 |= MMCR2_FCH(pmc);
			else
				mmcr2 |= MMCR2_FCS(pmc);
		}

		hwc[i] = pmc - 1;
	}

	/* Return MMCRx values */
	mmcr[0] = 0;

	/* pmc_inuse is 1-based */
	if (pmc_inuse & 2)
		mmcr[0] = MMCR0_PMC1CE;

	if (pmc_inuse & 0x7c)
		mmcr[0] |= MMCR0_PMCjCE;

	/* If we're not using PMC 5 or 6, freeze them */
	if (!(pmc_inuse & 0x60))
		mmcr[0] |= MMCR0_FC56;

	mmcr[1] = mmcr1;
	mmcr[2] = mmcra;
	mmcr[3] = mmcr2;

	return 0;
}

void isa207_disable_pmc(unsigned int pmc, unsigned long mmcr[])
{
	if (pmc <= 3)
		mmcr[1] &= ~(0xffUL << MMCR1_PMCSEL_SHIFT(pmc + 1));
}

static int find_alternative(u64 event, const unsigned int ev_alt[][MAX_ALT], int size)
{
	int i, j;

	for (i = 0; i < size; ++i) {
		if (event < ev_alt[i][0])
			break;

		for (j = 0; j < MAX_ALT && ev_alt[i][j]; ++j)
			if (event == ev_alt[i][j])
				return i;
	}

	return -1;
}

int isa207_get_alternatives(u64 event, u64 alt[], int size, unsigned int flags,
					const unsigned int ev_alt[][MAX_ALT])
{
	int i, j, num_alt = 0;
	u64 alt_event;

	alt[num_alt++] = event;
	i = find_alternative(event, ev_alt, size);
	if (i >= 0) {
		/* Filter out the original event, it's already in alt[0] */
		for (j = 0; j < MAX_ALT; ++j) {
			alt_event = ev_alt[i][j];
			if (alt_event && alt_event != event)
				alt[num_alt++] = alt_event;
		}
	}

	if (flags & PPMU_ONLY_COUNT_RUN) {
		/*
		 * We're only counting in RUN state, so PM_CYC is equivalent to
		 * PM_RUN_CYC and PM_INST_CMPL === PM_RUN_INST_CMPL.
		 */
		j = num_alt;
		for (i = 0; i < num_alt; ++i) {
			switch (alt[i]) {
			case 0x1e:			/* PMC_CYC */
				alt[j++] = 0x600f4;	/* PM_RUN_CYC */
				break;
			case 0x600f4:
				alt[j++] = 0x1e;
				break;
			case 0x2:			/* PM_INST_CMPL */
				alt[j++] = 0x500fa;	/* PM_RUN_INST_CMPL */
				break;
			case 0x500fa:
				alt[j++] = 0x2;
				break;
			}
		}
		num_alt = j;
	}

	return num_alt;
}
