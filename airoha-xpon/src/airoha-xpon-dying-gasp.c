// SPDX-License-Identifier: GPL-2.0-only
/*
 * Dying gasp. The SoC raises a dedicated interrupt when the supply voltage
 * drops. Stock firmware sets the SCU DMTC dying-gasp bit at init and in each
 * interrupt, switches board loads off to stretch the hold-up time and, with an
 * EPON LLID, sends an Information OAMPDU carrying the Dying Gasp flag. XG-PON
 * and XGS-PON signal dying gasp in hardware through
 * AIROHA_XGPON_DYING_GASP_CTRL.
 */

#include <dt-bindings/gpio/gpio.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/if_ether.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "airoha-xpon-private.h"
#include "airoha-xpon-oam.h"

#define AIROHA_SCU_DMTC            0x84
#define AIROHA_SCU_DMTC_DYING_GASP BIT(16)

#define AIROHA_DYING_GASP_SHED_PROP "airoha,dying-gasp-shed-gpios"
#define AIROHA_DYING_GASP_MAX_SHED  4
/* Stock kills tcwdog on dying gasp; its 28 s watchdog then resets the SoC. */
#define AIROHA_DYING_GASP_REBOOT_MS  28000
#define AIROHA_DYING_GASP_OAMPDU_LEN 60

struct airoha_xpon_dying_gasp {
	struct airoha_xpon *xpon;
	struct gpio_desc *shed[AIROHA_DYING_GASP_MAX_SHED];
	bool shed_level[AIROHA_DYING_GASP_MAX_SHED];
	unsigned int shed_count;
	struct work_struct notify_work;
	struct work_struct report_work;
	struct delayed_work reboot_work;
	int irq;
	int oam_ret;
};

/*
 * Information OAMPDU of stock xmit_dying_gasp_oam(). Flags are Dying Gasp,
 * Local Stable and Remote Stable; both Information TLVs are the fixed stock
 * values. The source address is filled in at transmit time.
 */
static const u8 airoha_dying_gasp_oampdu[AIROHA_DYING_GASP_OAMPDU_LEN] = {
	/* Slow Protocols multicast, source address */
	0x01, 0x80, 0xc2, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* Slow Protocols, OAM subtype, flags, Information code */
	0x88, 0x09, 0x03, 0x00, 0x52, 0x00,
	/* Local Information TLV */
	0x01, 0x10, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xee, 0x00, 0x13, 0x25,
	0x00, 0x22, 0x01, 0x00,
	/* Remote Information TLV */
	0x02, 0x10, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x05, 0xee, 0x00, 0x13, 0x25,
	0x00, 0x00, 0x00, 0x00
};

static void airoha_xpon_dying_gasp_ack(struct airoha_xpon *xpon)
{
	/* Stock: SET_DMTC(GET_DMTC() | BIT(16)) at init and in the handler. */
	regmap_write_bits(xpon->scu, AIROHA_SCU_DMTC,
	                  AIROHA_SCU_DMTC_DYING_GASP,
	                  AIROHA_SCU_DMTC_DYING_GASP);
}

static irqreturn_t airoha_xpon_dying_gasp_irq(int irq, void *data)
{
	struct airoha_xpon_dying_gasp *dg = data;
	unsigned int i;

	/*
	 * The supply is collapsing. Console output costs milliseconds, so the
	 * event is logged after the OAMPDU has been queued.
	 */
	airoha_xpon_dying_gasp_ack(dg->xpon);
	for (i = 0; i < dg->shed_count; i++)
		gpiod_set_raw_value(dg->shed[i], dg->shed_level[i]);

	/* The line is level triggered: handle one event per boot. */
	disable_irq_nosync(irq);
	queue_work(system_bh_wq, &dg->notify_work);

	return IRQ_HANDLED;
}

static void airoha_xpon_dying_gasp_notify(struct work_struct *work)
{
	struct airoha_xpon_dying_gasp *dg =
		container_of(work, struct airoha_xpon_dying_gasp, notify_work);
	struct airoha_xpon *xpon = dg->xpon;
	u8 pdu[AIROHA_DYING_GASP_OAMPDU_LEN];

	memcpy(pdu, airoha_dying_gasp_oampdu, sizeof(pdu));
	airoha_xpon_identity_epon_mac(xpon, &pdu[ETH_ALEN]);
	dg->oam_ret = airoha_xpon_oam_send(xpon->oam, pdu, sizeof(pdu));
	schedule_work(&dg->report_work);
}

static void airoha_xpon_dying_gasp_report(struct work_struct *work)
{
	struct airoha_xpon_dying_gasp *dg =
		container_of(work, struct airoha_xpon_dying_gasp, report_work);
	struct device *dev = dg->xpon->dev;
	const char *oam;

	if (!dg->oam_ret)
		oam = "Dying Gasp OAMPDU queued";
	else if (dg->oam_ret == -ENOLINK)
		oam = "no EPON LLID for an OAMPDU";
	else
		oam = "Dying Gasp OAMPDU not sent";
	dev_crit(
		dev,
		"dying gasp: supply voltage dropped, %s (%d), %u load line(s) switched off\n",
		oam, dg->oam_ret, dg->shed_count);

	/* A board that outlives the event reboots to restore the loads. */
	if (dg->shed_count)
		schedule_delayed_work(
			&dg->reboot_work,
			msecs_to_jiffies(AIROHA_DYING_GASP_REBOOT_MS));
}

static void airoha_xpon_dying_gasp_reboot(struct work_struct *work)
{
	struct airoha_xpon_dying_gasp *dg =
		container_of(to_delayed_work(work),
		             struct airoha_xpon_dying_gasp, reboot_work);

	dev_crit(
		dg->xpon->dev,
		"dying gasp: still running after %u s, rebooting to switch the loads back on\n",
		AIROHA_DYING_GASP_REBOOT_MS / 1000);
	orderly_reboot();
}

static void airoha_xpon_dying_gasp_put_gpio_device(void *data)
{
	gpio_device_put(data);
}

static void airoha_xpon_dying_gasp_drain(void *data)
{
	struct airoha_xpon_dying_gasp *dg = data;

	disable_irq(dg->irq);
	cancel_work_sync(&dg->notify_work);
	cancel_work_sync(&dg->report_work);
	cancel_delayed_work_sync(&dg->reboot_work);
}

/*
 * The load lines stay owned by their regular consumers (regulator-fixed, the
 * MDIO reset line), which keep them configured as outputs. This driver looks
 * the descriptors up without requesting them and only overrides the level in
 * the interrupt handler. An active-low entry is driven low.
 */
static int airoha_xpon_dying_gasp_get_shed(struct device *dev,
                                           struct airoha_xpon_dying_gasp *dg)
{
	struct device_node *np = dev->of_node;
	struct of_phandle_args args;
	struct gpio_device *gdev;
	struct gpio_desc *desc;
	int count, i, ret;

	count = of_count_phandle_with_args(np, AIROHA_DYING_GASP_SHED_PROP,
	                                   "#gpio-cells");
	if (count <= 0)
		return 0;
	if (count > AIROHA_DYING_GASP_MAX_SHED) {
		dev_warn(dev, "%s lists %d lines, using the first %d\n",
		         AIROHA_DYING_GASP_SHED_PROP, count,
		         AIROHA_DYING_GASP_MAX_SHED);
		count = AIROHA_DYING_GASP_MAX_SHED;
	}

	for (i = 0; i < count; i++) {
		ret = of_parse_phandle_with_args(np,
		                                 AIROHA_DYING_GASP_SHED_PROP,
		                                 "#gpio-cells", i, &args);
		if (ret)
			return ret;

		gdev = gpio_device_find_by_fwnode(of_fwnode_handle(args.np));
		of_node_put(args.np);
		if (!gdev || args.args_count != 2) {
			if (gdev)
				gpio_device_put(gdev);
			dev_warn(dev, "%s entry %d is unusable, skipped\n",
			         AIROHA_DYING_GASP_SHED_PROP, i);
			continue;
		}
		ret = devm_add_action_or_reset(
			dev, airoha_xpon_dying_gasp_put_gpio_device, gdev);
		if (ret)
			return ret;

		desc = gpio_device_get_desc(gdev, args.args[0]);
		if (IS_ERR(desc)) {
			dev_warn(dev, "%s entry %d: no GPIO line %u (%ld)\n",
			         AIROHA_DYING_GASP_SHED_PROP, i, args.args[0],
			         PTR_ERR(desc));
			continue;
		}

		dg->shed[dg->shed_count] = desc;
		dg->shed_level[dg->shed_count] =
			!(args.args[1] & GPIO_ACTIVE_LOW);
		dg->shed_count++;
	}

	return 0;
}

int airoha_xpon_dying_gasp_init(struct platform_device *pdev,
                                struct airoha_xpon *xpon)
{
	struct device *dev = &pdev->dev;
	struct airoha_xpon_dying_gasp *dg;
	bool asserted = false;
	int irq, ret;

	/* Boards opt in by naming the interrupt. */
	irq = platform_get_irq_byname_optional(pdev, "dying-gasp");
	if (irq == -ENXIO)
		return 0;
	if (irq < 0)
		return irq;

	dg = devm_kzalloc(dev, sizeof(*dg), GFP_KERNEL);
	if (!dg)
		return -ENOMEM;

	dg->xpon = xpon;
	dg->irq = irq;
	INIT_WORK(&dg->notify_work, airoha_xpon_dying_gasp_notify);
	INIT_WORK(&dg->report_work, airoha_xpon_dying_gasp_report);
	INIT_DELAYED_WORK(&dg->reboot_work, airoha_xpon_dying_gasp_reboot);

	ret = airoha_xpon_dying_gasp_get_shed(dev, dg);
	if (ret)
		return ret;

	ret = devm_request_irq(dev, irq, airoha_xpon_dying_gasp_irq,
	                       IRQF_NO_AUTOEN | IRQF_NO_THREAD,
	                       "airoha-xpon-dying-gasp", dg);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, airoha_xpon_dying_gasp_drain, dg);
	if (ret)
		return ret;

	/* From here on the XG-PON MAC is set up for hardware dying gasp. */
	xpon->dying_gasp = dg;

	/*
	 * A level-triggered line that is high now would fire on every boot and
	 * end in a reboot loop, so the interrupt stays disabled in that case.
	 */
	airoha_xpon_dying_gasp_ack(xpon);
	usleep_range(1000, 2000);
	ret = irq_get_irqchip_state(irq, IRQCHIP_STATE_PENDING, &asserted);
	if (ret || asserted) {
		dev_warn(
			dev,
			"dying-gasp interrupt %d is asserted at probe (%d), left disabled\n",
			irq, ret);
		return 0;
	}

	enable_irq(irq);
	dev_info(dev, "dying gasp armed: irq=%d, %u load line(s)\n", irq,
	         dg->shed_count);

	return 0;
}
