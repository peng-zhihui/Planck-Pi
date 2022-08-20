/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2013 Broadcom Corporation.
 */

#include <linux/stddef.h>
#include <linux/stringify.h>

#ifdef CONFIG_CLK_DEBUG
#undef writel
#undef readl
static inline void writel(u32 val, void *addr)
{
	printf("Write [0x%p] = 0x%08x\n", addr, val);
	*(u32 *)addr = val;
}

static inline u32 readl(void *addr)
{
	u32 val = *(u32 *)addr;
	printf("Read  [0x%p] = 0x%08x\n", addr, val);
	return val;
}
#endif

struct clk;

struct clk_lookup {
	const char *dev_id;
	const char *con_id;
	struct clk *clk;
};

extern struct clk_lookup arch_clk_tbl[];
extern unsigned int arch_clk_tbl_array_size;

/**
 * struct clk_ops - standard clock operations
 * @enable: enable/disable clock, see clk_enable() and clk_disable()
 * @set_rate: set the clock rate, see clk_set_rate().
 * @get_rate: get the clock rate, see clk_get_rate().
 * @round_rate: round a given clock rate, see clk_round_rate().
 * @set_parent: set the clock's parent, see clk_set_parent().
 *
 * Group the common clock implementations together so that we
 * don't have to keep setting the same fiels again. We leave
 * enable in struct clk.
 *
 */
struct clk_ops {
	int (*enable)(struct clk *c, int enable);
	int (*set_rate)(struct clk *c, unsigned long rate);
	unsigned long (*get_rate)(struct clk *c);
	unsigned long (*round_rate)(struct clk *c, unsigned long rate);
	int (*set_parent)(struct clk *c, struct clk *parent);
};

struct clk {
	struct clk *parent;
	const char *name;
	int use_cnt;
	unsigned long rate;	/* in HZ */

	/* programmable divider. 0 means fixed ratio to parent clock */
	unsigned long div;

	struct clk_src *src;
	struct clk_ops *ops;

	unsigned long ccu_clk_mgr_base;
	int sel;
};

struct refclk *refclk_str_to_clk(const char *name);

/* The common clock framework uses u8 to represent a parent index */
#define PARENT_COUNT_MAX	((u32)U8_MAX)

#define BAD_CLK_INDEX		U8_MAX	/* Can't ever be valid */
#define BAD_CLK_NAME		((const char *)-1)

#define BAD_SCALED_DIV_VALUE	U64_MAX

/*
 * Utility macros for object flag management. If possible, flags
 * should be defined such that 0 is the desired default value.
 */
#define FLAG(type, flag)		BCM_CLK_ ## type ## _FLAGS_ ## flag
#define FLAG_SET(obj, type, flag)	((obj)->flags |= FLAG(type, flag))
#define FLAG_CLEAR(obj, type, flag)	((obj)->flags &= ~(FLAG(type, flag)))
#define FLAG_FLIP(obj, type, flag)	((obj)->flags ^= FLAG(type, flag))
#define FLAG_TEST(obj, type, flag)	(!!((obj)->flags & FLAG(type, flag)))

/* Clock field state tests */

#define gate_exists(gate)		FLAG_TEST(gate, GATE, EXISTS)
#define gate_is_enabled(gate)		FLAG_TEST(gate, GATE, ENABLED)
#define gate_is_hw_controllable(gate)	FLAG_TEST(gate, GATE, HW)
#define gate_is_sw_controllable(gate)	FLAG_TEST(gate, GATE, SW)
#define gate_is_sw_managed(gate)	FLAG_TEST(gate, GATE, SW_MANAGED)
#define gate_is_no_disable(gate)	FLAG_TEST(gate, GATE, NO_DISABLE)

#define gate_flip_enabled(gate)		FLAG_FLIP(gate, GATE, ENABLED)

#define divider_exists(div)		FLAG_TEST(div, DIV, EXISTS)
#define divider_is_fixed(div)		FLAG_TEST(div, DIV, FIXED)
#define divider_has_fraction(div)	(!divider_is_fixed(div) && \
						(div)->frac_width > 0)

#define selector_exists(sel)		((sel)->width != 0)
#define trigger_exists(trig)		FLAG_TEST(trig, TRIG, EXISTS)

/* Clock type, used to tell common block what it's part of */
enum bcm_clk_type {
	bcm_clk_none,		/* undefined clock type */
	bcm_clk_bus,
	bcm_clk_core,
	bcm_clk_peri
};

/*
 * Gating control and status is managed by a 32-bit gate register.
 *
 * There are several types of gating available:
 * - (no gate)
 *     A clock with no gate is assumed to be always enabled.
 * - hardware-only gating (auto-gating)
 *     Enabling or disabling clocks with this type of gate is
 *     managed automatically by the hardware. Such clocks can be
 *     considered by the software to be enabled. The current status
 *     of auto-gated clocks can be read from the gate status bit.
 * - software-only gating
 *     Auto-gating is not available for this type of clock.
 *     Instead, software manages whether it's enabled by setting or
 *     clearing the enable bit. The current gate status of a gate
 *     under software control can be read from the gate status bit.
 *     To ensure a change to the gating status is complete, the
 *     status bit can be polled to verify that the gate has entered
 *     the desired state.
 * - selectable hardware or software gating
 *     Gating for this type of clock can be configured to be either
 *     under software or hardware control. Which type is in use is
 *     determined by the hw_sw_sel bit of the gate register.
 */
struct bcm_clk_gate {
	u32 offset;		/* gate register offset */
	u32 status_bit;		/* 0: gate is disabled; 0: gatge is enabled */
	u32 en_bit;		/* 0: disable; 1: enable */
	u32 hw_sw_sel_bit;	/* 0: hardware gating; 1: software gating */
	u32 flags;		/* BCM_CLK_GATE_FLAGS_* below */
};

/*
 * Gate flags:
 *   HW         means this gate can be auto-gated
 *   SW         means the state of this gate can be software controlled
 *   NO_DISABLE means this gate is (only) enabled if under software control
 *   SW_MANAGED means the status of this gate is under software control
 *   ENABLED    means this software-managed gate is *supposed* to be enabled
 */
#define BCM_CLK_GATE_FLAGS_EXISTS	((u32)1 << 0)	/* Gate is valid */
#define BCM_CLK_GATE_FLAGS_HW		((u32)1 << 1)	/* Can auto-gate */
#define BCM_CLK_GATE_FLAGS_SW		((u32)1 << 2)	/* Software control */
#define BCM_CLK_GATE_FLAGS_NO_DISABLE	((u32)1 << 3)	/* HW or enabled */
#define BCM_CLK_GATE_FLAGS_SW_MANAGED	((u32)1 << 4)	/* SW now in control */
#define BCM_CLK_GATE_FLAGS_ENABLED	((u32)1 << 5)	/* If SW_MANAGED */

/*
 * Gate initialization macros.
 *
 * Any gate initially under software control will be enabled.
 */

/* A hardware/software gate initially under software control */
#define HW_SW_GATE(_offset, _status_bit, _en_bit, _hw_sw_sel_bit)	\
	{								\
		.offset = (_offset),					\
		.status_bit = (_status_bit),				\
		.en_bit = (_en_bit),					\
		.hw_sw_sel_bit = (_hw_sw_sel_bit),			\
		.flags = FLAG(GATE, HW)|FLAG(GATE, SW)|			\
			FLAG(GATE, SW_MANAGED)|FLAG(GATE, ENABLED)|	\
			FLAG(GATE, EXISTS),				\
	}

/* A hardware/software gate initially under hardware control */
#define HW_SW_GATE_AUTO(_offset, _status_bit, _en_bit, _hw_sw_sel_bit)	\
	{								\
		.offset = (_offset),					\
		.status_bit = (_status_bit),				\
		.en_bit = (_en_bit),					\
		.hw_sw_sel_bit = (_hw_sw_sel_bit),			\
		.flags = FLAG(GATE, HW)|FLAG(GATE, SW)|			\
			FLAG(GATE, EXISTS),				\
	}

/* A hardware-or-enabled gate (enabled if not under hardware control) */
#define HW_ENABLE_GATE(_offset, _status_bit, _en_bit, _hw_sw_sel_bit)	\
	{								\
		.offset = (_offset),					\
		.status_bit = (_status_bit),				\
		.en_bit = (_en_bit),					\
		.hw_sw_sel_bit = (_hw_sw_sel_bit),			\
		.flags = FLAG(GATE, HW)|FLAG(GATE, SW)|			\
			FLAG(GATE, NO_DISABLE)|FLAG(GATE, EXISTS),	\
	}

/* A software-only gate */
#define SW_ONLY_GATE(_offset, _status_bit, _en_bit)			\
	{								\
		.offset = (_offset),					\
		.status_bit = (_status_bit),				\
		.en_bit = (_en_bit),					\
		.flags = FLAG(GATE, SW)|FLAG(GATE, SW_MANAGED)|		\
			FLAG(GATE, ENABLED)|FLAG(GATE, EXISTS),		\
	}

/* A hardware-only gate */
#define HW_ONLY_GATE(_offset, _status_bit)				\
	{								\
		.offset = (_offset),					\
		.status_bit = (_status_bit),				\
		.flags = FLAG(GATE, HW)|FLAG(GATE, EXISTS),		\
	}

/*
 * Each clock can have zero, one, or two dividers which change the
 * output rate of the clock. Each divider can be either fixed or
 * variable. If there are two dividers, they are the "pre-divider"
 * and the "regular" or "downstream" divider. If there is only one,
 * there is no pre-divider.
 *
 * A fixed divider is any non-zero (positive) value, and it
 * indicates how the input rate is affected by the divider.
 *
 * The value of a variable divider is maintained in a sub-field of a
 * 32-bit divider register. The position of the field in the
 * register is defined by its offset and width. The value recorded
 * in this field is always 1 less than the value it represents.
 *
 * In addition, a variable divider can indicate that some subset
 * of its bits represent a "fractional" part of the divider. Such
 * bits comprise the low-order portion of the divider field, and can
 * be viewed as representing the portion of the divider that lies to
 * the right of the decimal point. Most variable dividers have zero
 * fractional bits. Variable dividers with non-zero fraction width
 * still record a value 1 less than the value they represent; the
 * added 1 does *not* affect the low-order bit in this case, it
 * affects the bits above the fractional part only. (Often in this
 * code a divider field value is distinguished from the value it
 * represents by referring to the latter as a "divisor".)
 *
 * In order to avoid dealing with fractions, divider arithmetic is
 * performed using "scaled" values. A scaled value is one that's
 * been left-shifted by the fractional width of a divider. Dividing
 * a scaled value by a scaled divisor produces the desired quotient
 * without loss of precision and without any other special handling
 * for fractions.
 *
 * The recorded value of a variable divider can be modified. To
 * modify either divider (or both), a clock must be enabled (i.e.,
 * using its gate). In addition, a trigger register (described
 * below) must be used to commit the change, and polled to verify
 * the change is complete.
 */
struct bcm_clk_div {
	union {
		struct {	/* variable divider */
			u32 offset;	/* divider register offset */
			u32 shift;	/* field shift */
			u32 width;	/* field width */
			u32 frac_width;	/* field fraction width */

			u64 scaled_div;	/* scaled divider value */
		};
		u32 fixed;	/* non-zero fixed divider value */
	};
	u32 flags;		/* BCM_CLK_DIV_FLAGS_* below */
};

/*
 * Divider flags:
 *   EXISTS means this divider exists
 *   FIXED means it is a fixed-rate divider
 */
#define BCM_CLK_DIV_FLAGS_EXISTS	((u32)1 << 0)	/* Divider is valid */
#define BCM_CLK_DIV_FLAGS_FIXED		((u32)1 << 1)	/* Fixed-value */

/* Divider initialization macros */

/* A fixed (non-zero) divider */
#define FIXED_DIVIDER(_value)						\
	{								\
		.fixed = (_value),					\
		.flags = FLAG(DIV, EXISTS)|FLAG(DIV, FIXED),		\
	}

/* A divider with an integral divisor */
#define DIVIDER(_offset, _shift, _width)				\
	{								\
		.offset = (_offset),					\
		.shift = (_shift),					\
		.width = (_width),					\
		.scaled_div = BAD_SCALED_DIV_VALUE,			\
		.flags = FLAG(DIV, EXISTS),				\
	}

/* A divider whose divisor has an integer and fractional part */
#define FRAC_DIVIDER(_offset, _shift, _width, _frac_width)		\
	{								\
		.offset = (_offset),					\
		.shift = (_shift),					\
		.width = (_width),					\
		.frac_width = (_frac_width),				\
		.scaled_div = BAD_SCALED_DIV_VALUE,			\
		.flags = FLAG(DIV, EXISTS),				\
	}

/*
 * Clocks may have multiple "parent" clocks. If there is more than
 * one, a selector must be specified to define which of the parent
 * clocks is currently in use. The selected clock is indicated in a
 * sub-field of a 32-bit selector register. The range of
 * representable selector values typically exceeds the number of
 * available parent clocks. Occasionally the reset value of a
 * selector field is explicitly set to a (specific) value that does
 * not correspond to a defined input clock.
 *
 * We register all known parent clocks with the common clock code
 * using a packed array (i.e., no empty slots) of (parent) clock
 * names, and refer to them later using indexes into that array.
 * We maintain an array of selector values indexed by common clock
 * index values in order to map between these common clock indexes
 * and the selector values used by the hardware.
 *
 * Like dividers, a selector can be modified, but to do so a clock
 * must be enabled, and a trigger must be used to commit the change.
 */
struct bcm_clk_sel {
	u32 offset;		/* selector register offset */
	u32 shift;		/* field shift */
	u32 width;		/* field width */

	u32 parent_count;	/* number of entries in parent_sel[] */
	u32 *parent_sel;	/* array of parent selector values */
	u8 clk_index;		/* current selected index in parent_sel[] */
};

/* Selector initialization macro */
#define SELECTOR(_offset, _shift, _width)				\
	{								\
		.offset = (_offset),					\
		.shift = (_shift),					\
		.width = (_width),					\
		.clk_index = BAD_CLK_INDEX,				\
	}

/*
 * Making changes to a variable divider or a selector for a clock
 * requires the use of a trigger. A trigger is defined by a single
 * bit within a register. To signal a change, a 1 is written into
 * that bit. To determine when the change has been completed, that
 * trigger bit is polled; the read value will be 1 while the change
 * is in progress, and 0 when it is complete.
 *
 * Occasionally a clock will have more than one trigger. In this
 * case, the "pre-trigger" will be used when changing a clock's
 * selector and/or its pre-divider.
 */
struct bcm_clk_trig {
	u32 offset;		/* trigger register offset */
	u32 bit;		/* trigger bit */
	u32 flags;		/* BCM_CLK_TRIG_FLAGS_* below */
};

/*
 * Trigger flags:
 *   EXISTS means this trigger exists
 */
#define BCM_CLK_TRIG_FLAGS_EXISTS	((u32)1 << 0)	/* Trigger is valid */

/* Trigger initialization macro */
#define TRIGGER(_offset, _bit)						\
	{								\
		.offset = (_offset),					\
		.bit = (_bit),						\
		.flags = FLAG(TRIG, EXISTS),				\
	}

struct bus_clk_data {
	struct bcm_clk_gate gate;
};

struct core_clk_data {
	struct bcm_clk_gate gate;
};

struct peri_clk_data {
	struct bcm_clk_gate gate;
	struct bcm_clk_trig pre_trig;
	struct bcm_clk_div pre_div;
	struct bcm_clk_trig trig;
	struct bcm_clk_div div;
	struct bcm_clk_sel sel;
	const char *clocks[];	/* must be last; use CLOCKS() to declare */
};
#define CLOCKS(...)	{ __VA_ARGS__, NULL, }
#define NO_CLOCKS	{ NULL, }	/* Must use of no parent clocks */

struct refclk {
	struct clk clk;
};

struct peri_clock {
	struct clk clk;
	struct peri_clk_data *data;
};

struct ccu_clock {
	struct clk clk;

	int num_policy_masks;
	unsigned long policy_freq_offset;
	int freq_bit_shift;	/* 8 for most CCUs */
	unsigned long policy_ctl_offset;
	unsigned long policy0_mask_offset;
	unsigned long policy1_mask_offset;
	unsigned long policy2_mask_offset;
	unsigned long policy3_mask_offset;
	unsigned long policy0_mask2_offset;
	unsigned long policy1_mask2_offset;
	unsigned long policy2_mask2_offset;
	unsigned long policy3_mask2_offset;
	unsigned long lvm_en_offset;

	int freq_id;
	unsigned long *freq_tbl;
};

struct bus_clock {
	struct clk clk;
	struct bus_clk_data *data;
	unsigned long *freq_tbl;
};

struct ref_clock {
	struct clk clk;
};

static inline int is_same_clock(struct clk *a, struct clk *b)
{
	return a == b;
}

#define to_clk(p) (&((p)->clk))
#define name_to_clk(name) (&((name##_clk).clk))
/* declare a struct clk_lookup */
#define CLK_LK(name) \
{.con_id = __stringify(name##_clk), .clk = name_to_clk(name),}

static inline struct refclk *to_refclk(struct clk *clock)
{
	return container_of(clock, struct refclk, clk);
}

static inline struct peri_clock *to_peri_clk(struct clk *clock)
{
	return container_of(clock, struct peri_clock, clk);
}

static inline struct ccu_clock *to_ccu_clk(struct clk *clock)
{
	return container_of(clock, struct ccu_clock, clk);
}

static inline struct bus_clock *to_bus_clk(struct clk *clock)
{
	return container_of(clock, struct bus_clock, clk);
}

static inline struct ref_clock *to_ref_clk(struct clk *clock)
{
	return container_of(clock, struct ref_clock, clk);
}

extern struct clk_ops peri_clk_ops;
extern struct clk_ops ccu_clk_ops;
extern struct clk_ops bus_clk_ops;
extern struct clk_ops ref_clk_ops;

int clk_get_and_enable(char *clkstr);
