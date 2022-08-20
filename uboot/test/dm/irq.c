// SPDX-License-Identifier: GPL-2.0+
/*
 * Test for irq uclass
 *
 * Copyright 2019 Google LLC
 */

#include <common.h>
#include <dm.h>
#include <irq.h>
#include <asm/test.h>
#include <dm/test.h>
#include <test/ut.h>

/* Base test of the irq uclass */
static int dm_test_irq_base(struct unit_test_state *uts)
{
	struct udevice *dev;

	ut_assertok(uclass_first_device_err(UCLASS_IRQ, &dev));

	ut_asserteq(5, irq_route_pmc_gpio_gpe(dev, 4));
	ut_asserteq(-ENOENT, irq_route_pmc_gpio_gpe(dev, 14));

	ut_assertok(irq_set_polarity(dev, 4, true));
	ut_asserteq(-EINVAL, irq_set_polarity(dev, 14, true));

	ut_assertok(irq_snapshot_polarities(dev));
	ut_assertok(irq_restore_polarities(dev));

	return 0;
}
DM_TEST(dm_test_irq_base, DM_TESTF_SCAN_PDATA | DM_TESTF_SCAN_FDT);

/* Test of irq_first_device_type() */
static int dm_test_irq_type(struct unit_test_state *uts)
{
	struct udevice *dev;

	ut_assertok(irq_first_device_type(SANDBOX_IRQT_BASE, &dev));
	ut_asserteq(-ENODEV, irq_first_device_type(X86_IRQT_BASE, &dev));

	return 0;
}
DM_TEST(dm_test_irq_type, DM_TESTF_SCAN_PDATA | DM_TESTF_SCAN_FDT);

/* Test of irq_read_and_clear() */
static int dm_test_read_and_clear(struct unit_test_state *uts)
{
	struct irq irq;

	ut_assertok(irq_first_device_type(SANDBOX_IRQT_BASE, &irq.dev));
	irq.id = SANDBOX_IRQN_PEND;
	ut_asserteq(0, irq_read_and_clear(&irq));
	ut_asserteq(0, irq_read_and_clear(&irq));
	ut_asserteq(0, irq_read_and_clear(&irq));
	ut_asserteq(1, irq_read_and_clear(&irq));
	ut_asserteq(0, irq_read_and_clear(&irq));

	return 0;
}
DM_TEST(dm_test_read_and_clear, DM_TESTF_SCAN_PDATA | DM_TESTF_SCAN_FDT);

/* Test of irq_request() */
static int dm_test_request(struct unit_test_state *uts)
{
	struct udevice *dev;
	struct irq irq;

	ut_assertok(uclass_first_device_err(UCLASS_TEST_FDT, &dev));
	ut_asserteq_str("a-test", dev->name);
	ut_assertok(irq_get_by_index(dev, 0, &irq));
	ut_asserteq(3, irq.id);

	return 0;
}
DM_TEST(dm_test_request, DM_TESTF_SCAN_PDATA | DM_TESTF_SCAN_FDT);
