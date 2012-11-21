/*
 * Copyright (c) 2012, Motorola Mobility, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/c55_ctrl.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <mach/msm_xo.h>

/* Device tree node */
#define DT_PATH_C55			"/System@0/Audio@0"
#define DT_PROP_C55_TYPE		"type"
#define DT_TYPE_AUDIO_C55		0x00030016

static struct msm_xo_voter *xo;

int mmi_c55_adc_clk_en(int en)
{
	pr_debug("%s: %sable D1 clock\n", __func__, (en > 0) ? "En" : "Dis");

	if (!xo)
		xo = msm_xo_get(MSM_XO_TCXO_D1, "c55_ctrl");

	return msm_xo_mode_vote(xo, (en > 0) ? MSM_XO_MODE_ON : MSM_XO_MODE_OFF);
}

static struct gpio mmi_c55_gpios[] = {
	{
		.label = "gpio_core",
		.flags = GPIOF_OUT_INIT_HIGH,
	},
	{
		.label = "gpio_reset",
		.flags = GPIOF_OUT_INIT_LOW,
	},
	{
		.label = "gpio_ap_int",
		.flags = GPIOF_IN,
	},
	{
		.label = "gpio_c55_int",
		.flags = GPIOF_OUT_INIT_HIGH,
	},
};

static struct c55_ctrl_platform_data mmi_c55_ctrl_data = {
	.gpios = mmi_c55_gpios,
	.num_gpios = ARRAY_SIZE(mmi_c55_gpios),

	.adc_clk_en = mmi_c55_adc_clk_en,
};

static struct platform_device mmi_c55_ctrl_device = {
	.name	= "c55_ctrl",
	.id	= -1,
	.dev = {
		.platform_data = &mmi_c55_ctrl_data,
	},
};

void __init mmi_audio_dsp_init(void)
{
	struct device_node *node;
	const void *prop;
	int len = 0;
	int type;
	int i;

	node = of_find_node_by_path(DT_PATH_C55);
	if (!node)
		return;

	prop = of_get_property(node, DT_PROP_C55_TYPE, &len);
	if (prop && (len == sizeof(int)))
		type = *((int *)prop);
	else
		goto exit;

	if (type != DT_TYPE_AUDIO_C55) {
		pr_err("%s: DT node wrong type: 0x%08X.\n", __func__, type);
		goto exit;
	}

	for (i = 0; i < ARRAY_SIZE(mmi_c55_gpios); ++i) {
		prop = of_get_property(node, mmi_c55_gpios[i].label, &len);
		if (prop && (len == sizeof(char)))
			mmi_c55_gpios[i].gpio = *((char *)prop);
		else {
			pr_err("%s: Missing %s\n", __func__, mmi_c55_gpios[i].label);
			goto exit;
		}
	}

	platform_device_register(&mmi_c55_ctrl_device);

exit:
	of_node_put(node);
}
