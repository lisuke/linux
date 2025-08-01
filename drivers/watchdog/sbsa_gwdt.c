// SPDX-License-Identifier: GPL-2.0-only
/*
 * SBSA(Server Base System Architecture) Generic Watchdog driver
 *
 * Copyright (c) 2015, Linaro Ltd.
 * Author: Fu Wei <fu.wei@linaro.org>
 *         Suravee Suthikulpanit <Suravee.Suthikulpanit@amd.com>
 *         Al Stone <al.stone@linaro.org>
 *         Timur Tabi <timur@codeaurora.org>
 *
 * ARM SBSA Generic Watchdog has two stage timeouts:
 * the first signal (WS0) is for alerting the system by interrupt,
 * the second one (WS1) is a real hardware reset.
 * More details about the hardware specification of this device:
 * ARM DEN0029B - Server Base System Architecture (SBSA)
 *
 * This driver can operate ARM SBSA Generic Watchdog as a single stage watchdog
 * or a two stages watchdog, it's set up by the module parameter "action".
 * In the single stage mode, when the timeout is reached, your system
 * will be reset by WS1. The first signal (WS0) is ignored.
 * In the two stages mode, when the timeout is reached, the first signal (WS0)
 * will trigger panic. If the system is getting into trouble and cannot be reset
 * by panic or restart properly by the kdump kernel(if supported), then the
 * second stage (as long as the first stage) will be reached, system will be
 * reset by WS1. This function can help administrator to backup the system
 * context info by panic console output or kdump.
 *
 * SBSA GWDT:
 * if action is 1 (the two stages mode):
 * |--------WOR-------WS0--------WOR-------WS1
 * |----timeout-----(panic)----timeout-----reset
 *
 * if action is 0 (the single stage mode):
 * |------WOR-----WS0(ignored)-----WOR------WS1
 * |--------------timeout-------------------reset
 *
 * Note: Since this watchdog timer has two stages, and each stage is determined
 * by WOR, in the single stage mode, the timeout is (WOR * 2); in the two
 * stages mode, the timeout is WOR. The maximum timeout in the two stages mode
 * is half of that in the single stage mode.
 */

#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/watchdog.h>
#include <asm/arch_timer.h>

#define DRV_NAME		"sbsa-gwdt"
#define WATCHDOG_NAME		"SBSA Generic Watchdog"

/* SBSA Generic Watchdog register definitions */
/* refresh frame */
#define SBSA_GWDT_WRR		0x000

/* control frame */
#define SBSA_GWDT_WCS		0x000
#define SBSA_GWDT_WOR		0x008
#define SBSA_GWDT_WCV		0x010

/* refresh/control frame */
#define SBSA_GWDT_W_IIDR	0xfcc
#define SBSA_GWDT_IDR		0xfd0

/* Watchdog Control and Status Register */
#define SBSA_GWDT_WCS_EN	BIT(0)
#define SBSA_GWDT_WCS_WS0	BIT(1)
#define SBSA_GWDT_WCS_WS1	BIT(2)

#define SBSA_GWDT_VERSION_MASK  0xF
#define SBSA_GWDT_VERSION_SHIFT 16

#define SBSA_GWDT_IMPL_MASK	0x7FF
#define SBSA_GWDT_IMPL_SHIFT	0
#define SBSA_GWDT_IMPL_MEDIATEK	0x426

/**
 * struct sbsa_gwdt - Internal representation of the SBSA GWDT
 * @wdd:		kernel watchdog_device structure
 * @clk:		store the System Counter clock frequency, in Hz.
 * @version:            store the architecture version
 * @need_ws0_race_workaround:
 *			indicate whether to adjust wdd->timeout to avoid a race with WS0
 * @refresh_base:	Virtual address of the watchdog refresh frame
 * @control_base:	Virtual address of the watchdog control frame
 */
struct sbsa_gwdt {
	struct watchdog_device	wdd;
	u32			clk;
	int			version;
	bool			need_ws0_race_workaround;
	void __iomem		*refresh_base;
	void __iomem		*control_base;
};

#define DEFAULT_TIMEOUT		10 /* seconds */

static unsigned int timeout;
module_param(timeout, uint, 0);
MODULE_PARM_DESC(timeout,
		 "Watchdog timeout in seconds. (>=0, default="
		 __MODULE_STRING(DEFAULT_TIMEOUT) ")");

/*
 * action refers to action taken when watchdog gets WS0
 * 0 = skip
 * 1 = panic
 * defaults to skip (0)
 */
static int action;
module_param(action, int, 0);
MODULE_PARM_DESC(action, "after watchdog gets WS0 interrupt, do: "
		 "0 = skip(*)  1 = panic");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, S_IRUGO);
MODULE_PARM_DESC(nowayout,
		 "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

/*
 * Arm Base System Architecture 1.0 introduces watchdog v1 which
 * increases the length watchdog offset register to 48 bits.
 * - For version 0: WOR is 32 bits;
 * - For version 1: WOR is 48 bits which comprises the register
 * offset 0x8 and 0xC, and the bits [63:48] are reserved which are
 * Read-As-Zero and Writes-Ignored.
 */
static u64 sbsa_gwdt_reg_read(struct sbsa_gwdt *gwdt)
{
	if (gwdt->version == 0)
		return readl(gwdt->control_base + SBSA_GWDT_WOR);
	else
		return lo_hi_readq(gwdt->control_base + SBSA_GWDT_WOR);
}

static void sbsa_gwdt_reg_write(u64 val, struct sbsa_gwdt *gwdt)
{
	if (gwdt->version == 0)
		writel((u32)val, gwdt->control_base + SBSA_GWDT_WOR);
	else
		lo_hi_writeq(val, gwdt->control_base + SBSA_GWDT_WOR);
}

/*
 * watchdog operation functions
 */
static int sbsa_gwdt_set_timeout(struct watchdog_device *wdd,
				 unsigned int timeout)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	wdd->timeout = timeout;
	timeout = clamp_t(unsigned int, timeout, 1, wdd->max_hw_heartbeat_ms / 1000);

	if (action)
		sbsa_gwdt_reg_write((u64)gwdt->clk * timeout, gwdt);
	else
		/*
		 * In the single stage mode, The first signal (WS0) is ignored,
		 * the timeout is (WOR * 2), so the WOR should be configured
		 * to half value of timeout.
		 */
		sbsa_gwdt_reg_write(((u64)gwdt->clk / 2) * timeout, gwdt);

	/*
	 * Some watchdog hardware has a race condition where it will ignore
	 * sbsa_gwdt_keepalive() if it is called at the exact moment that a
	 * timeout occurs and WS0 is being asserted. Unfortunately, the default
	 * behavior of the watchdog core is very likely to trigger this race
	 * when action=0 because it programs WOR to be half of the desired
	 * timeout, and watchdog_next_keepalive() chooses the exact same time to
	 * send keepalive pings.
	 *
	 * This triggers a race where sbsa_gwdt_keepalive() can be called right
	 * as WS0 is being asserted, and affected hardware will ignore that
	 * write and continue to assert WS0. After another (timeout / 2)
	 * seconds, the same race happens again. If the driver wins then the
	 * explicit refresh will reset WS0 to false but if the hardware wins,
	 * then WS1 is asserted and the system resets.
	 *
	 * Avoid the problem by scheduling keepalive heartbeats one second later
	 * than the WOR timeout.
	 *
	 * This workaround might not be needed in a future revision of the
	 * hardware.
	 */
	if (gwdt->need_ws0_race_workaround)
		wdd->min_hw_heartbeat_ms = timeout * 500 + 1000;

	return 0;
}

static unsigned int sbsa_gwdt_get_timeleft(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);
	u64 timeleft = 0;

	/*
	 * In the single stage mode, if WS0 is deasserted
	 * (watchdog is in the first stage),
	 * timeleft = WOR + (WCV - system counter)
	 */
	if (!action &&
	    !(readl(gwdt->control_base + SBSA_GWDT_WCS) & SBSA_GWDT_WCS_WS0))
		timeleft += sbsa_gwdt_reg_read(gwdt);

	timeleft += lo_hi_readq(gwdt->control_base + SBSA_GWDT_WCV) -
		    arch_timer_read_counter();

	do_div(timeleft, gwdt->clk);

	return timeleft;
}

static int sbsa_gwdt_keepalive(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	/*
	 * Writing WRR for an explicit watchdog refresh.
	 * You can write anyting (like 0).
	 */
	writel(0, gwdt->refresh_base + SBSA_GWDT_WRR);

	return 0;
}

static void sbsa_gwdt_get_version(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);
	int iidr, ver, impl;

	iidr = readl(gwdt->control_base + SBSA_GWDT_W_IIDR);
	ver = (iidr >> SBSA_GWDT_VERSION_SHIFT) & SBSA_GWDT_VERSION_MASK;
	impl = (iidr >> SBSA_GWDT_IMPL_SHIFT) & SBSA_GWDT_IMPL_MASK;

	gwdt->version = ver;
	gwdt->need_ws0_race_workaround =
		!action && (impl == SBSA_GWDT_IMPL_MEDIATEK);
}

static int sbsa_gwdt_start(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	/* writing WCS will cause an explicit watchdog refresh */
	writel(SBSA_GWDT_WCS_EN, gwdt->control_base + SBSA_GWDT_WCS);

	return 0;
}

static int sbsa_gwdt_stop(struct watchdog_device *wdd)
{
	struct sbsa_gwdt *gwdt = watchdog_get_drvdata(wdd);

	/* Simply write 0 to WCS to clean WCS_EN bit */
	writel(0, gwdt->control_base + SBSA_GWDT_WCS);

	return 0;
}

static irqreturn_t sbsa_gwdt_interrupt(int irq, void *dev_id)
{
	panic(WATCHDOG_NAME " timeout");

	return IRQ_HANDLED;
}

static const struct watchdog_info sbsa_gwdt_info = {
	.identity	= WATCHDOG_NAME,
	.options	= WDIOF_SETTIMEOUT |
			  WDIOF_KEEPALIVEPING |
			  WDIOF_MAGICCLOSE |
			  WDIOF_CARDRESET,
};

static const struct watchdog_ops sbsa_gwdt_ops = {
	.owner		= THIS_MODULE,
	.start		= sbsa_gwdt_start,
	.stop		= sbsa_gwdt_stop,
	.ping		= sbsa_gwdt_keepalive,
	.set_timeout	= sbsa_gwdt_set_timeout,
	.get_timeleft	= sbsa_gwdt_get_timeleft,
};

static int sbsa_gwdt_probe(struct platform_device *pdev)
{
	void __iomem *rf_base, *cf_base;
	struct device *dev = &pdev->dev;
	struct watchdog_device *wdd;
	struct sbsa_gwdt *gwdt;
	int ret, irq;
	u32 status;

	gwdt = devm_kzalloc(dev, sizeof(*gwdt), GFP_KERNEL);
	if (!gwdt)
		return -ENOMEM;
	platform_set_drvdata(pdev, gwdt);

	cf_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cf_base))
		return PTR_ERR(cf_base);

	rf_base = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(rf_base))
		return PTR_ERR(rf_base);

	/*
	 * Get the frequency of system counter from the cp15 interface of ARM
	 * Generic timer. We don't need to check it, because if it returns "0",
	 * system would panic in very early stage.
	 */
	gwdt->clk = arch_timer_get_cntfrq();
	gwdt->refresh_base = rf_base;
	gwdt->control_base = cf_base;

	wdd = &gwdt->wdd;
	wdd->parent = dev;
	wdd->info = &sbsa_gwdt_info;
	wdd->ops = &sbsa_gwdt_ops;
	wdd->min_timeout = 1;
	wdd->timeout = DEFAULT_TIMEOUT;
	watchdog_set_drvdata(wdd, gwdt);
	watchdog_set_nowayout(wdd, nowayout);
	sbsa_gwdt_get_version(wdd);
	if (gwdt->version == 0)
		wdd->max_hw_heartbeat_ms = U32_MAX / gwdt->clk * 1000;
	else
		wdd->max_hw_heartbeat_ms = GENMASK_ULL(47, 0) / gwdt->clk * 1000;

	if (gwdt->need_ws0_race_workaround) {
		/*
		 * A timeout of 3 seconds means that WOR will be set to 1.5
		 * seconds and the heartbeat will be scheduled every 2.5
		 * seconds.
		 */
		wdd->min_timeout = 3;
	}

	status = readl(cf_base + SBSA_GWDT_WCS);
	if (status & SBSA_GWDT_WCS_WS1) {
		dev_warn(dev, "System reset by WDT.\n");
		wdd->bootstatus |= WDIOF_CARDRESET;
	}
	if (status & SBSA_GWDT_WCS_EN)
		set_bit(WDOG_HW_RUNNING, &wdd->status);

	if (action) {
		irq = platform_get_irq(pdev, 0);
		if (irq < 0) {
			action = 0;
			dev_warn(dev, "unable to get ws0 interrupt.\n");
		} else {
			/*
			 * In case there is a pending ws0 interrupt, just ping
			 * the watchdog before registering the interrupt routine
			 */
			writel(0, rf_base + SBSA_GWDT_WRR);
			if (devm_request_irq(dev, irq, sbsa_gwdt_interrupt, 0,
					     pdev->name, gwdt)) {
				action = 0;
				dev_warn(dev, "unable to request IRQ %d.\n",
					 irq);
			}
		}
		if (!action)
			dev_warn(dev, "falling back to single stage mode.\n");
	}
	/*
	 * In the single stage mode, The first signal (WS0) is ignored,
	 * the timeout is (WOR * 2), so the maximum timeout should be doubled.
	 */
	if (!action)
		wdd->max_hw_heartbeat_ms *= 2;

	watchdog_init_timeout(wdd, timeout, dev);
	/*
	 * Update timeout to WOR.
	 * Because of the explicit watchdog refresh mechanism,
	 * it's also a ping, if watchdog is enabled.
	 */
	sbsa_gwdt_set_timeout(wdd, wdd->timeout);

	watchdog_stop_on_reboot(wdd);
	ret = devm_watchdog_register_device(dev, wdd);
	if (ret)
		return ret;

	dev_info(dev, "Initialized with %ds timeout @ %u Hz, action=%d.%s\n",
		 wdd->timeout, gwdt->clk, action,
		 status & SBSA_GWDT_WCS_EN ? " [enabled]" : "");

	return 0;
}

/* Disable watchdog if it is active during suspend */
static int __maybe_unused sbsa_gwdt_suspend(struct device *dev)
{
	struct sbsa_gwdt *gwdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&gwdt->wdd))
		sbsa_gwdt_stop(&gwdt->wdd);

	return 0;
}

/* Enable watchdog if necessary */
static int __maybe_unused sbsa_gwdt_resume(struct device *dev)
{
	struct sbsa_gwdt *gwdt = dev_get_drvdata(dev);

	if (watchdog_hw_running(&gwdt->wdd))
		sbsa_gwdt_start(&gwdt->wdd);

	return 0;
}

static const struct dev_pm_ops sbsa_gwdt_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(sbsa_gwdt_suspend, sbsa_gwdt_resume)
};

static const struct of_device_id sbsa_gwdt_of_match[] = {
	{ .compatible = "arm,sbsa-gwdt", },
	{},
};
MODULE_DEVICE_TABLE(of, sbsa_gwdt_of_match);

static const struct platform_device_id sbsa_gwdt_pdev_match[] = {
	{ .name = DRV_NAME, },
	{},
};
MODULE_DEVICE_TABLE(platform, sbsa_gwdt_pdev_match);

static struct platform_driver sbsa_gwdt_driver = {
	.driver = {
		.name = DRV_NAME,
		.pm = &sbsa_gwdt_pm_ops,
		.of_match_table = sbsa_gwdt_of_match,
	},
	.probe = sbsa_gwdt_probe,
	.id_table = sbsa_gwdt_pdev_match,
};

module_platform_driver(sbsa_gwdt_driver);

MODULE_DESCRIPTION("SBSA Generic Watchdog Driver");
MODULE_AUTHOR("Fu Wei <fu.wei@linaro.org>");
MODULE_AUTHOR("Suravee Suthikulpanit <Suravee.Suthikulpanit@amd.com>");
MODULE_AUTHOR("Al Stone <al.stone@linaro.org>");
MODULE_AUTHOR("Timur Tabi <timur@codeaurora.org>");
MODULE_LICENSE("GPL v2");
