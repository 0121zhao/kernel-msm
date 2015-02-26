/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
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

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/gpio.h>
#include <linux/qpnp/pin.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/leds.h>
#include <linux/qpnp/pwm.h>
#include <linux/err.h>
#ifdef CONFIG_LCD_CLASS_DEVICE
#include <linux/lcd.h>
#endif
#include <linux/syscalls.h>
#include "mdss_fb.h"
#include "mdss_dsi.h"
#include "mdss_samsung_dsi_panel_msm8x26.h"
#include "mdss_debug.h"


#define DT_CMD_HDR 6

/* #define CMD_DEBUG */
/* #define TE_DEBUG */
#define ALPM_MODE

static struct dsi_panel_cmds display_on_seq;
static struct dsi_panel_cmds display_on_cmd;
static struct dsi_panel_cmds display_off_seq;
static struct dsi_panel_cmds manufacture_id_cmds;
static struct dsi_panel_cmds mtp_id_cmds;
static struct dsi_panel_cmds mtp_enable_cmds;
static struct dsi_panel_cmds gamma_cmds_list;
static struct dsi_panel_cmds backlight_cmds;
static struct dsi_panel_cmds rddpm_cmds;

static struct candella_lux_map candela_map_table;
#if defined(ALPM_MODE)
/* ALPM mode on/off command */
static struct dsi_panel_cmds alpm_on_seq;
static struct dsi_panel_cmds alpm_off_seq;
static int disp_esd_gpio;
static int disp_te_gpio;
/*
 * APIs for ALPM mode
 * alpm_store()
 *	- Check or store status like alpm mode status or brightness level
 */
#endif
static int mdss_dsi_panel_dimming_init(struct mdss_panel_data *pdata);
static int mdss_samsung_disp_send_cmd(
		struct mdss_dsi_ctrl_pdata *ctrl,
		enum mdss_samsung_cmd_list cmd,
		unsigned char lock);
static void alpm_enable(struct mdss_dsi_ctrl_pdata *ctrl, int enable);
DEFINE_LED_TRIGGER(bl_led_trigger);

void mdss_dsi_panel_pwm_cfg(struct mdss_dsi_ctrl_pdata *ctrl)
{
	ctrl->pwm_bl = pwm_request(ctrl->pwm_lpg_chan, "lcd-bklt");
	if (ctrl->pwm_bl == NULL || IS_ERR(ctrl->pwm_bl)) {
		pr_err("%s: Error: lpg_chan=%d pwm request failed",
				__func__, ctrl->pwm_lpg_chan);
	}
}

static void mdss_dsi_panel_bklt_pwm(struct mdss_dsi_ctrl_pdata *ctrl, int level)
{
	int ret;
	u32 duty;
	u32 period_ns;

	if (ctrl->pwm_bl == NULL) {
		pr_err("%s: no PWM\n", __func__);
		return;
	}

	if (level == 0) {
		if (ctrl->pwm_enabled)
			pwm_disable(ctrl->pwm_bl);
		ctrl->pwm_enabled = 0;
		return;
	}

	duty = level * ctrl->pwm_period;
	duty /= ctrl->bklt_max;

	pr_debug("%s: bklt_ctrl=%d pwm_period=%d pwm_gpio=%d pwm_lpg_chan=%d\n",
			__func__, ctrl->bklt_ctrl, ctrl->pwm_period,
			ctrl->pwm_pmic_gpio, ctrl->pwm_lpg_chan);

	pr_debug("%s: ndx=%d level=%d duty=%d\n", __func__,
			ctrl->ndx, level, duty);

	if (ctrl->pwm_enabled) {
		pwm_disable(ctrl->pwm_bl);
		ctrl->pwm_enabled = 0;
	}

	if (ctrl->pwm_period >= USEC_PER_SEC) {
		ret = pwm_config_us(ctrl->pwm_bl, duty, ctrl->pwm_period);
		if (ret) {
			pr_err("%s: pwm_config_us() failed err=%d.\n",
					__func__, ret);
			return;
		}
	} else {
		period_ns = ctrl->pwm_period * NSEC_PER_USEC;
		ret = pwm_config(ctrl->pwm_bl,
				level * period_ns / ctrl->bklt_max,
				period_ns);
		if (ret) {
			pr_err("%s: pwm_config() failed err=%d.\n",
					__func__, ret);
			return;
		}
	}

	ret = pwm_enable(ctrl->pwm_bl);
	if (ret)
		pr_err("%s: pwm_enable() failed err=%d\n", __func__, ret);
	ctrl->pwm_enabled = 1;
}

static char dcs_cmd[2] = {0x54, 0x00}; /* DTYPE_DCS_READ */
static struct dsi_cmd_desc dcs_read_cmd = {
	{DTYPE_DCS_READ, 1, 0, 1, 5, sizeof(dcs_cmd)},
	dcs_cmd
};

u32 mdss_dsi_panel_cmd_read(struct mdss_dsi_ctrl_pdata *ctrl, char cmd0,
		char cmd1, void (*fxn)(int), char *rbuf, int len)
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->partial_update_dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return -EINVAL;
	}

	dcs_cmd[0] = cmd0;
	dcs_cmd[1] = cmd1;
	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = &dcs_read_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_RX | CMD_REQ_COMMIT;
	cmdreq.rlen = len;
	cmdreq.rbuf = rbuf;
	cmdreq.cb = fxn; /* call back */
	mdss_mdp_cmd_clk_enable();
	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
	mdss_mdp_cmd_clk_disable();
	/*
	 * blocked here, until call back called
	 */

	return 0;
}

static void mdss_dsi_panel_cmds_send(struct mdss_dsi_ctrl_pdata *ctrl,
		struct dsi_panel_cmds *pcmds)
{
	struct dcs_cmd_req cmdreq;
	struct mdss_panel_info *pinfo;

	pinfo = &(ctrl->panel_data.panel_info);
	if (pinfo->partial_update_dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			return;
	}

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = pcmds->cmds;
	cmdreq.cmds_cnt = pcmds->cmd_cnt;
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;

	/*Panel ON/Off commands should be sent in DSI Low Power Mode*/
	if (pcmds->link_state == DSI_LP_MODE)
		cmdreq.flags  |= CMD_REQ_LP_MODE;
	else if (pcmds->link_state == DSI_HS_MODE)
		cmdreq.flags  |= CMD_REQ_HS_MODE;

	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
}

static char backlight[28] = {0xD4, };
static struct dsi_cmd_desc backlight_cmd = {
	{DTYPE_DCS_LWRITE, 1, 0, 0, 0, sizeof(backlight)},
	backlight
};

static int get_cmd_idx(int bl_level)
{
	return candela_map_table.cmd_idx[candela_map_table.bkl[bl_level]];
}

static int get_candela_value(int bl_level)
{
	return candela_map_table.lux_tab[candela_map_table.bkl[bl_level]];
}

static void get_gamma_control_set(int candella, struct smartdim_conf *sdimconf)
{
	int cnt;
	/*  Just a safety check to ensure smart dimming data is initialised well */
	BUG_ON(sdimconf->generate_gamma == NULL);
	sdimconf->generate_gamma(candella,
			&gamma_cmds_list.cmds[0].payload[1]);
	for (cnt = 1; cnt < (GAMMA_SET_MAX + 1); cnt++)
		backlight[cnt] = gamma_cmds_list.cmds[0].payload[cnt];
}

static void mdss_dsi_panel_bklt_dcs(struct mdss_dsi_ctrl_pdata *ctrl,
		int bl_level)
{
	struct dcs_cmd_req cmdreq;
	int cd_idx = 0, cd_level = 0;
	static int stored_cd_level;
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;
	struct display_status *dstat = &msd->dstat;

	if (!dstat->on) {
		pr_err("%s: Skip to tx command %d\n", __func__, __LINE__);
		return;
	}

	/*gamma*/
	cd_idx = get_cmd_idx(bl_level);
	cd_level = get_candela_value(bl_level);

	/* gamma control */
	get_gamma_control_set(cd_level, msd->sdimconf);

	memset(&cmdreq, 0, sizeof(cmdreq));
	cmdreq.cmds = &backlight_cmd;
	cmdreq.cmds_cnt = 1;
	cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL;
	cmdreq.rlen = 0;
	cmdreq.cb = NULL;

	mdss_mdp_cmd_clk_enable();
	mdss_dsi_cmd_dma_trigger_sel(ctrl, 1);
	mdss_dsi_cmdlist_put(ctrl, &cmdreq);
	mdss_dsi_cmd_dma_trigger_sel(ctrl, 0);
	stored_cd_level = cd_level;
	mdss_mdp_cmd_clk_disable();
}

static int mdss_dsi_request_gpios(struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	int rc = 0;

	if (gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
		rc = gpio_request(ctrl_pdata->disp_en_gpio,
				"disp_enable");
		if (rc) {
			pr_err("request disp_en gpio failed, rc=%d\n",
					rc);
			goto disp_en_gpio_err;
		}
	}
	rc = gpio_request(ctrl_pdata->rst_gpio, "disp_rst_n");
	if (rc) {
		pr_err("request reset gpio failed, rc=%d\n",
				rc);
		goto rst_gpio_err;
	}
	if (gpio_is_valid(ctrl_pdata->bklt_en_gpio)) {
		rc = gpio_request(ctrl_pdata->bklt_en_gpio,
				"bklt_enable");
		if (rc) {
			pr_err("request bklt gpio failed, rc=%d\n",
					rc);
			goto bklt_en_gpio_err;
		}
	}
	if (gpio_is_valid(ctrl_pdata->mode_gpio)) {
		rc = gpio_request(ctrl_pdata->mode_gpio, "panel_mode");
		if (rc) {
			pr_err("request panel mode gpio failed,rc=%d\n",
					rc);
			goto mode_gpio_err;
		}
	}
	return rc;

mode_gpio_err:
	if (gpio_is_valid(ctrl_pdata->bklt_en_gpio))
		gpio_free(ctrl_pdata->bklt_en_gpio);
bklt_en_gpio_err:
	gpio_free(ctrl_pdata->rst_gpio);
rst_gpio_err:
	if (gpio_is_valid(ctrl_pdata->disp_en_gpio))
		gpio_free(ctrl_pdata->disp_en_gpio);
disp_en_gpio_err:
	return rc;
}

int mdss_dsi_panel_reset(struct mdss_panel_data *pdata, int enable)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_panel_info *pinfo = NULL;
	struct mdss_alpm_data *adata;
	struct display_status *dstat = NULL;
	int ambient_mode_check = 0;
	int i, rc = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);
	dstat = &((struct mdss_samsung_driver_data *)pdata->panel_private)->dstat;

	adata = &pdata->alpm_data;

	if (!gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
		pr_debug("%s:%d, reset line not configured\n",
				__func__, __LINE__);
	}

	if (!gpio_is_valid(ctrl_pdata->rst_gpio)) {
		pr_debug("%s:%d, reset line not configured\n",
				__func__, __LINE__);
		return rc;
	}

	pr_debug("%s: enable = %d\n", __func__, enable);
	pinfo = &(ctrl_pdata->panel_data.panel_info);

	if (adata->alpm_status) {
		if (enable && adata->alpm_status(CHECK_PREVIOUS_STATUS)) {
			if (gpio_get_value(disp_esd_gpio)) {
				pr_info("[ESD_DEBUG] check current alpm status\n");
				ambient_mode_check = adata->alpm_status(CHECK_CURRENT_STATUS);
				adata->alpm_status(CLEAR_MODE_STATUS);
				if (ambient_mode_check == ALPM_MODE_ON)
					adata->alpm_status(ALPM_MODE_ON);
			} else
				return rc;
		} else if (!enable && adata->alpm_status(CHECK_CURRENT_STATUS)) {
			adata->alpm_status(STORE_CURRENT_STATUS);
			return rc;
		}
		pr_debug("[ALPM_DEBUG] %s: Panel reset, enable : %d\n",
				__func__, enable);
	}

	if (enable) {
		rc = mdss_dsi_request_gpios(ctrl_pdata);
		if (rc) {
			pr_err("gpio request failed\n");
			return rc;
		}
		if (!pinfo->cont_splash_enabled && !dstat->on) {
			if (gpio_is_valid(ctrl_pdata->disp_en_gpio))
				gpio_set_value((ctrl_pdata->disp_en_gpio), 1);

			for (i = 0; i < pdata->panel_info.rst_seq_len; ++i) {
				gpio_set_value((ctrl_pdata->rst_gpio),
						pdata->panel_info.rst_seq[i]);
				if (pdata->panel_info.rst_seq[++i])
					usleep(pinfo->rst_seq[i] * 1000);
			}

			if (gpio_is_valid(ctrl_pdata->bklt_en_gpio))
				gpio_set_value((ctrl_pdata->bklt_en_gpio), 1);
		}

		if (gpio_is_valid(ctrl_pdata->mode_gpio)) {
			if (pinfo->mode_gpio_state == MODE_GPIO_HIGH)
				gpio_set_value((ctrl_pdata->mode_gpio), 1);
			else if (pinfo->mode_gpio_state == MODE_GPIO_LOW)
				gpio_set_value((ctrl_pdata->mode_gpio), 0);
		}
		if (ctrl_pdata->ctrl_state & CTRL_STATE_PANEL_INIT) {
			pr_debug("%s: Panel Not properly turned OFF\n",
					__func__);
			ctrl_pdata->ctrl_state &= ~CTRL_STATE_PANEL_INIT;
			pr_debug("%s: Reset panel done\n", __func__);
		}
	} else {
		if (gpio_is_valid(ctrl_pdata->bklt_en_gpio)) {
			gpio_set_value((ctrl_pdata->bklt_en_gpio), 0);
			gpio_free(ctrl_pdata->bklt_en_gpio);
		}
		if (gpio_is_valid(ctrl_pdata->disp_en_gpio)) {
			gpio_set_value((ctrl_pdata->disp_en_gpio), 0);
			gpio_free(ctrl_pdata->disp_en_gpio);
		}
		gpio_set_value((ctrl_pdata->rst_gpio), 0);
		gpio_free(ctrl_pdata->rst_gpio);
		if (gpio_is_valid(ctrl_pdata->mode_gpio))
			gpio_free(ctrl_pdata->mode_gpio);
	}
	return rc;
}

/**
 * mdss_dsi_roi_merge() -  merge two roi into single roi
 *
 * Function used by partial update with only one dsi intf take 2A/2B
 * (column/page) dcs commands.
 */
static int mdss_dsi_roi_merge(struct mdss_dsi_ctrl_pdata *ctrl,
		struct mdss_rect *roi)
{
	struct mdss_panel_info *l_pinfo;
	struct mdss_rect *l_roi;
	struct mdss_rect *r_roi;
	struct mdss_dsi_ctrl_pdata *other = NULL;
	int ans = 0;

	if (ctrl->ndx == DSI_CTRL_LEFT) {
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_RIGHT);
		if (!other)
			return ans;
		l_pinfo = &(ctrl->panel_data.panel_info);
		l_roi = &(ctrl->panel_data.panel_info.roi);
		r_roi = &(other->panel_data.panel_info.roi);
	} else  {
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_LEFT);
		if (!other)
			return ans;
		l_pinfo = &(other->panel_data.panel_info);
		l_roi = &(other->panel_data.panel_info.roi);
		r_roi = &(ctrl->panel_data.panel_info.roi);
	}

	if (l_roi->w == 0 && l_roi->h == 0) {
		/* right only */
		*roi = *r_roi;
		roi->x += l_pinfo->xres;/* add left full width to x-offset */
	} else {
		/* left only and left+righ */
		*roi = *l_roi;
		roi->w +=  r_roi->w; /* add right width */
		ans = 1;
	}

	return ans;
}

static char caset[] = {0x2a, 0x00, 0x00, 0x03, 0x00};	/* DTYPE_DCS_LWRITE */
static char paset[] = {0x2b, 0x00, 0x00, 0x05, 0x00};	/* DTYPE_DCS_LWRITE */

/* pack into one frame before sent */
static struct dsi_cmd_desc set_col_page_addr_cmd[] = {
	{{DTYPE_DCS_LWRITE, 0, 0, 0, 1, sizeof(caset)}, caset},	/* packed */
	{{DTYPE_DCS_LWRITE, 1, 0, 0, 1, sizeof(paset)}, paset},
};

static int mdss_dsi_set_col_page_addr(struct mdss_panel_data *pdata)
{
	struct mdss_panel_info *pinfo;
	struct mdss_rect roi;
	struct mdss_rect *p_roi;
	struct mdss_rect *c_roi;
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_dsi_ctrl_pdata *other = NULL;
	struct dcs_cmd_req cmdreq;
	int left_or_both = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);

	pinfo = &pdata->panel_info;
	p_roi = &pinfo->roi;

	/*
	 * to avoid keep sending same col_page info to panel,
	 * if roi_merge enabled, the roi of left ctrl is used
	 * to compare against new merged roi and saved new
	 * merged roi to it after comparing.
	 * if roi_merge disabled, then the calling ctrl's roi
	 * and pinfo's roi are used to compare.
	 */
	if (pinfo->partial_update_roi_merge) {
		left_or_both = mdss_dsi_roi_merge(ctrl, &roi);
		other = mdss_dsi_get_ctrl_by_index(DSI_CTRL_LEFT);
		c_roi = &other->roi;
	} else {
		c_roi = &ctrl->roi;
		roi = *p_roi;
	}

	/* roi had changed, do col_page update */
	if (mdss_dsi_sync_wait_enable(ctrl) ||
			!mdss_rect_cmp(c_roi, &roi)) {
		pr_debug("%s: ndx=%d x=%d y=%d w=%d h=%d\n",
				__func__, ctrl->ndx, p_roi->x,
				p_roi->y, p_roi->w, p_roi->h);

		*c_roi = roi; /* keep to ctrl */
		if (c_roi->w == 0 || c_roi->h == 0) {
			/* no new frame update */
			pr_debug("%s: ctrl=%d, no partial roi set\n",
					__func__, ctrl->ndx);
			if (!mdss_dsi_sync_wait_enable(ctrl))
				return 0;
		}

		if (pinfo->partial_update_dcs_cmd_by_left) {
			if (left_or_both && ctrl->ndx == DSI_CTRL_RIGHT) {
				/* 2A/2B sent by left already */
				return 0;
			}
		}

		caset[1] = (((roi.x) & 0xFF00) >> 8);
		caset[2] = (((roi.x) & 0xFF));
		caset[3] = (((roi.x - 1 + roi.w) & 0xFF00) >> 8);
		caset[4] = (((roi.x - 1 + roi.w) & 0xFF));
		set_col_page_addr_cmd[0].payload = caset;

		paset[1] = (((roi.y) & 0xFF00) >> 8);
		paset[2] = (((roi.y) & 0xFF));
		paset[3] = (((roi.y - 1 + roi.h) & 0xFF00) >> 8);
		paset[4] = (((roi.y - 1 + roi.h) & 0xFF));
		set_col_page_addr_cmd[1].payload = paset;

		memset(&cmdreq, 0, sizeof(cmdreq));
		cmdreq.cmds = set_col_page_addr_cmd;
		cmdreq.cmds_cnt = 2;
		cmdreq.flags = CMD_REQ_COMMIT | CMD_CLK_CTRL | CMD_REQ_UNICAST;
		cmdreq.rlen = 0;
		cmdreq.cb = NULL;

		if (pinfo->partial_update_dcs_cmd_by_left)
			ctrl = mdss_dsi_get_ctrl_by_index(DSI_CTRL_LEFT);

		mdss_dsi_cmdlist_put(ctrl, &cmdreq);
	}

	return 0;
}

static void mdss_dsi_panel_switch_mode(struct mdss_panel_data *pdata,
		int mode)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mipi_panel_info *mipi;
	struct dsi_panel_cmds *pcmds;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	mipi  = &pdata->panel_info.mipi;

	if (!mipi->dynamic_switch_enabled)
		return;

	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);

	if (mode == DSI_CMD_MODE)
		pcmds = &ctrl_pdata->video2cmd;
	else
		pcmds = &ctrl_pdata->cmd2video;

	mdss_dsi_panel_cmds_send(ctrl_pdata, pcmds);

	return;
}

static int mdss_samsung_disp_send_cmd(
		struct mdss_dsi_ctrl_pdata *ctrl,
		enum mdss_samsung_cmd_list cmd,
		unsigned char lock)
{
	struct dsi_panel_cmds *cmds = NULL;
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;
	struct mutex *cmd_lock = &msd->lock;
#ifdef CMD_DEBUG
	int i, j;
#endif

	if (lock)
		mutex_lock(cmd_lock);

	switch (cmd) {
		case PANEL_DISPLAY_ON_SEQ:
			cmds = &display_on_seq;
			break;
		case PANEL_DISPLAY_ON:
			cmds = &display_on_cmd;
			break;
		case PANEL_DISP_OFF:
			cmds = &display_off_seq;
			break;
		case PANEL_MTP_ENABLE:
			cmds = &mtp_enable_cmds;
			break;
		case PANEL_BACKLIGHT_CMD:
			cmds = &backlight_cmds;
			break;
#if defined(ALPM_MODE)
		case PANEL_ALPM_ON:
			cmds = &alpm_on_seq;
			break;
		case PANEL_ALPM_OFF:
			cmds = &alpm_off_seq;
			break;
#endif
		default:
			pr_err("%s : unknown_command..\n", __func__);
			goto unknown_command;
	}

	if (!cmds->cmd_cnt) {
		pr_err("%s : cmd_cnt is zero!..\n", __func__);
		goto unknown_command;
	}
#ifdef CMD_DEBUG
	pr_info("CMD : ");
	for (i = 0; i < cmds->cmd_cnt; i++) {
		for (j = 0; j < cmds->cmds[i].dchdr.dlen; j++)
			pr_info("%x ", cmds->cmds[i].payload[j]);
		pr_info("\n");
	}
#endif

	if (cmd != PANEL_DISPLAY_ON_SEQ && cmd != PANEL_DISP_OFF)
		mdss_mdp_cmd_clk_enable();
	mdss_dsi_panel_cmds_send(ctrl, cmds);
	if (cmd != PANEL_DISPLAY_ON_SEQ && cmd != PANEL_DISP_OFF)
		mdss_mdp_cmd_clk_disable();

	if (lock)
		mutex_unlock(cmd_lock);
	return 0;

unknown_command:
	LCD_DEBUG("Undefined command\n");

	if (lock)
		mutex_unlock(cmd_lock);
	return -EINVAL;
}
static int mdss_dsi_panel_registered(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_panel_info *pinfo = NULL;
	struct mdss_samsung_driver_data *msd = NULL;

	if (pdata == NULL) {
		pr_err("%s : Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl_pdata = container_of(pdata,
			struct mdss_dsi_ctrl_pdata, panel_data);
	msd = (struct mdss_samsung_driver_data *)pdata->panel_private;
	msd->mfd = (struct msm_fb_data_type *)registered_fb[0]->par;
	msd->pdata = pdata;
	msd->ctrl_pdata = ctrl_pdata;

	if (pinfo->mipi.mode == DSI_CMD_MODE)
		msd->dstat.on = 1;

	pr_info("%s:%d, panel registered succesfully\n", __func__, __LINE__);
	return 0;
}

static void mdss_dsi_panel_alpm_ctrl(struct mdss_panel_data *pdata,
		bool mode)
{
	struct mdss_dsi_ctrl_pdata *ctrl= NULL;
	struct mdss_alpm_data *adata;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);

	adata = &pdata->alpm_data;

	if (adata->alpm_status(CHECK_PREVIOUS_STATUS)
			&& adata->alpm_status(CHECK_CURRENT_STATUS)) {
		pr_info("[ALPM_DEBUG] %s: ambient -> ambient\n", __func__);
		return;
	}

	if (mode) {
		alpm_enable(ctrl, ALPM_MODE_ON);
		mdss_samsung_disp_send_cmd(ctrl, PANEL_ALPM_ON, true);
	} else {
		/* Turn Off ALPM Mode */
		alpm_enable(ctrl, MODE_OFF);
	}

	pr_info("[ALPM_DEBUG] %s: Send ALPM %s cmds\n", __func__,
			mode ? "on" : "off");

	adata->alpm_status(STORE_CURRENT_STATUS);
}

static void mdss_dsi_panel_bl_ctrl(struct mdss_panel_data *pdata,
		u32 bl_level)
{
	struct mdss_dsi_ctrl_pdata *ctrl_pdata = NULL;
	struct mdss_dsi_ctrl_pdata *sctrl = NULL;
	struct display_status *dstat = NULL;
	static int stored_bl_level = 255;
	int request_bl_dim = 0;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return;
	}

	dstat = &((struct mdss_samsung_driver_data *)pdata->panel_private)->dstat;

	if (bl_level == PANEL_BACKLIGHT_RESTORE)
		bl_level = stored_bl_level;
	else if (bl_level == PANEL_BACKLIGHT_DIM) {
		request_bl_dim = 1;
		bl_level = 30;
	}

	mdss_dsi_panel_dimming_init(pdata);
	ctrl_pdata = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);

	/*
	 * Some backlight controllers specify a minimum duty cycle
	 * for the backlight brightness. If the brightness is less
	 * than it, the controller can malfunction.
	 */

	if ((bl_level < pdata->panel_info.bl_min) && (bl_level != 0))
		bl_level = pdata->panel_info.bl_min;

	switch (ctrl_pdata->bklt_ctrl) {
		case BL_WLED:
			led_trigger_event(bl_led_trigger, bl_level);
			break;
		case BL_PWM:
			mdss_dsi_panel_bklt_pwm(ctrl_pdata, bl_level);
			break;
		case BL_DCS_CMD:
			dstat->bright_level = bl_level;
			if (!mdss_dsi_sync_wait_enable(ctrl_pdata)) {
				mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
				break;
			}
			/*
			 * DCS commands to update backlight are usually sent at
			 * the same time to both the controllers. However, if
			 * sync_wait is enabled, we need to ensure that the
			 * dcs commands are first sent to the non-trigger
			 * controller so that when the commands are triggered,
			 * both controllers receive it at the same time.
			 */
			sctrl = mdss_dsi_get_other_ctrl(ctrl_pdata);
			if (mdss_dsi_sync_wait_trigger(ctrl_pdata)) {
				if (sctrl)
					mdss_dsi_panel_bklt_dcs(sctrl, bl_level);
				mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
			} else {
				mdss_dsi_panel_bklt_dcs(ctrl_pdata, bl_level);
				if (sctrl)
					mdss_dsi_panel_bklt_dcs(sctrl, bl_level);
			}
			break;
		default:
			pr_err("%s: Unknown bl_ctrl configuration\n",
					__func__);
			break;
	}

	if (pdata->panel_info.first_bl_update) {
		mdss_samsung_disp_send_cmd(ctrl_pdata, PANEL_BACKLIGHT_CMD, true);
		pdata->panel_info.first_bl_update = 0;
	}

	if (request_bl_dim)
		bl_level = stored_bl_level;
	stored_bl_level = bl_level;
}

void mdss_dsi_panel_bl_dim(struct mdss_panel_data *pdata, int flag)
{
	struct display_status *dstat =
		&((struct mdss_samsung_driver_data *)pdata->panel_private)->dstat;

	if (likely(dstat->on)) {
		mdss_dsi_panel_bl_ctrl(pdata, flag);
	} else
		pr_info("[ALPM_DEBUG] %s: The LCD already turned off\n"
				, __func__);
}

static unsigned int mdss_samsung_manufacture_id(struct mdss_panel_data *pdata)
{
	unsigned int id = 0;
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);

	mdss_dsi_panel_cmd_read(ctrl,
			manufacture_id_cmds.cmds[0].payload[0],
			manufacture_id_cmds.cmds[0].payload[1],
			NULL,
			ctrl->rx_buf.data,
			manufacture_id_cmds.read_size[0]);

	id = ctrl->rx_buf.data[0] << 16;
	id |= ctrl->rx_buf.data[1] << 8;
	id |= ctrl->rx_buf.data[2];

	pr_info("%s: manufacture id = %x\n", __func__, id);
	return id;
}
static void mdss_sasmung_mtp_id(struct mdss_panel_data *pdata, char *destbuffer)
{
	int cnt;
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);

	mdss_samsung_disp_send_cmd(ctrl, PANEL_MTP_ENABLE, true);

	mdss_dsi_panel_cmd_read(ctrl,
			mtp_id_cmds.cmds[0].payload[0],
			mtp_id_cmds.cmds[0].payload[1],
			NULL,
			ctrl->rx_buf.data,
			mtp_id_cmds.read_size[0]);

	pr_info("%s : MTP ID :\n", __func__);
	for (cnt = 0; cnt < GAMMA_SET_MAX; cnt++) {
		pr_info("%d, ", ctrl->rx_buf.data[cnt]);
		destbuffer[cnt] = ctrl->rx_buf.data[cnt];
	}
	pr_info("\n");
	return;
}
static int mdss_dsi_panel_dimming_init(struct mdss_panel_data *pdata)
{
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)pdata->panel_private;
	struct smartdim_conf *sdimconf = NULL;
	struct display_status *dstat;

	/* If the ID is not read yet, then read it*/
	if (!msd->manufacture_id)
		msd->manufacture_id = mdss_samsung_manufacture_id(pdata);

	dstat = &msd->dstat;

	if (!dstat->is_smart_dim_loaded) {
		pr_info(" %s ++\n", __func__);
		switch (msd->panel) {
			case PANEL_320_OCTA_S6E63J:
				pr_info("%s : S6E63J\n", __func__);
				msd->sdimconf = smart_S6E63J_get_conf();
				sdimconf = msd->sdimconf;
				break;
		}
		/* Just a safety check to ensure
		   smart dimming data is initialised well */
		BUG_ON(sdimconf == NULL);

		/* Set the mtp read buffer pointer and read the NVM value*/
		mdss_sasmung_mtp_id(pdata, sdimconf->mtp_buffer);

		/* lux_tab setting for 350cd */
		sdimconf->lux_tab = &candela_map_table.lux_tab[0];
		sdimconf->lux_tabsize = candela_map_table.lux_tab_size;
		sdimconf->man_id = msd->manufacture_id;

		/* Just a safety check to ensure
		   smart dimming data is initialised well */
		BUG_ON(sdimconf->init == NULL);
		sdimconf->init();
		dstat->is_smart_dim_loaded = true;

		/*
		 * Since dimming is loaded,
		 * we can assume that device is out of suspend state
		 * and can accept backlight commands.
		 */
		pr_info(" %s --\n", __func__);
	}
	return 0;
}
static int mdss_dsi_panel_on(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;
	struct mdss_alpm_data *adata;
	struct mdss_samsung_driver_data *msd = NULL;
	struct display_status *dstat;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);
	adata = &pdata->alpm_data;
	msd = (struct mdss_samsung_driver_data *)pdata->panel_private;
	dstat = &msd->dstat;

	pr_info("%s: ctrl=%p ndx=%d\n", __func__, ctrl, ctrl->ndx);

	if (pinfo->partial_update_dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			goto end;
	}

	if (ctrl->on_cmds.cmd_cnt)
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->on_cmds);

	if (!msd->manufacture_id)
		msd->manufacture_id = mdss_samsung_manufacture_id(pdata);

	mdss_dsi_panel_dimming_init(pdata);

	/* Normaly the else is working for PANEL_DISP_ON_SEQ */
	if (adata->alpm_status) {
		if (!adata->alpm_status(CHECK_PREVIOUS_STATUS))
			mdss_samsung_disp_send_cmd(ctrl, PANEL_DISPLAY_ON_SEQ, true);
	} else
		mdss_samsung_disp_send_cmd(ctrl, PANEL_DISPLAY_ON_SEQ, true);

	dstat->wait_disp_on = 1;
	dstat->on = 1;

	/* ALPM Mode Change */
	if (adata->alpm_status) {
		if (!adata->alpm_status(CHECK_PREVIOUS_STATUS)
				&& adata->alpm_status(CHECK_CURRENT_STATUS)) {
			/* Turn On ALPM Mode */
			mdss_dsi_panel_bl_dim(pdata, PANEL_BACKLIGHT_DIM);
			mdss_samsung_disp_send_cmd(ctrl, PANEL_ALPM_ON, true);
			adata->alpm_status(STORE_CURRENT_STATUS);
			pr_info("[ALPM_DEBUG] %s: Send ALPM mode on cmds\n",
					__func__);
		} else if (!adata->alpm_status(CHECK_CURRENT_STATUS)
				&& adata->alpm_status(CHECK_PREVIOUS_STATUS)) {
			/* Turn Off ALPM Mode */
			mdss_samsung_disp_send_cmd(ctrl, PANEL_ALPM_OFF, true);
			/*
			   mdss_dsi_panel_bl_dim(pdata, PANEL_BACKLIGHT_RESTORE);
			   adata->alpm_status(CLEAR_MODE_STATUS);
			 */
			pr_info("[ALPM_DEBUG] %s: Send ALPM off cmds\n",
					__func__);
		}
	}

	if (androidboot_mode_charger || androidboot_is_recovery)
		mdss_samsung_disp_send_cmd(ctrl, PANEL_BACKLIGHT_CMD, true);
	pr_info("%s:-\n", __func__);

end:
	pinfo->blank_state = MDSS_PANEL_BLANK_UNBLANK;
	pr_debug("%s:-\n", __func__);
	return 0;
}

static int mdss_dsi_panel_off(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;
	struct mdss_alpm_data *adata;
	struct display_status *dstat;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);
	adata = &pdata->alpm_data;
	dstat = &((struct mdss_samsung_driver_data *)pdata->panel_private)->dstat;

	pr_info("%s: ctrl=%p ndx=%d\n", __func__, ctrl, ctrl->ndx);

	if (pinfo->partial_update_dcs_cmd_by_left) {
		if (ctrl->ndx != DSI_CTRL_LEFT)
			goto end;
	}

	if (ctrl->off_cmds.cmd_cnt)
		mdss_dsi_panel_cmds_send(ctrl, &ctrl->off_cmds);

	dstat->on = 0;
	pdata->panel_info.first_bl_update = 1;
	if (adata->alpm_status &&
			adata->alpm_status(CHECK_CURRENT_STATUS) &&
			!adata->alpm_status(CHECK_PREVIOUS_STATUS)) {
		pr_info("[ALPM_DEBUG] %s: Skip to send panel off cmds\n",
				__func__);
		mdss_dsi_panel_bl_dim(pdata, PANEL_BACKLIGHT_DIM);
		mdss_samsung_disp_send_cmd(ctrl, PANEL_ALPM_ON, true);
		adata->alpm_status(STORE_CURRENT_STATUS);
	} else if (pinfo->is_suspending)
		pr_debug("[ALPM_DEBUG] %s: Skip to send panel off cmds\n",
				__func__);
	else
		mdss_samsung_disp_send_cmd(ctrl, PANEL_DISP_OFF, true);

end:
	pinfo->blank_state = MDSS_PANEL_BLANK_BLANK;
	pr_info("%s:-\n", __func__);
	return 0;
}

static void alpm_enable(struct mdss_dsi_ctrl_pdata *ctrl, int enable)
{
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;
	struct display_status *dstat = &msd->dstat;
	struct mdss_alpm_data *adata = &ctrl->panel_data.alpm_data;;

	pr_info("[ALPM_DEBUG] %s: enable: %d\n", __func__, enable);

	/*
	 * Possible mode status for Blank(0) or Unblank(1)
	 *	* Blank *
	 *		1) ALPM_MODE_ON
	 *		2) MODE_OFF
	 *
	 *		The mode(1, 2) will change when unblank
	 *	* Unblank *
	 *		1) ALPM_MODE_ON
	 *			-> The mode will change when blank
	 *		2) MODE_OFF
	 *			-> The mode will change immediately
	 */

	adata->alpm_status(enable);
	if (enable == MODE_OFF) {
		if (adata->alpm_status(CHECK_PREVIOUS_STATUS) == ALPM_MODE_ON) {
			if (dstat->on) {
				mdss_samsung_disp_send_cmd(ctrl, PANEL_ALPM_OFF, true);
				/* wait 1 frame(more than 16ms) */
				msleep(20);
				adata->alpm_status(CLEAR_MODE_STATUS);
				pr_info("[ALPM_DEBUG] %s: Send ALPM off cmds\n", __func__);
			}
		}
	}
}

static int mdss_dsi_panel_low_power_config(struct mdss_panel_data *pdata,
		int enable)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct mdss_panel_info *pinfo;
	struct display_status *dstat =
		&((struct mdss_samsung_driver_data *)pdata->panel_private)->dstat;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	pinfo = &pdata->panel_info;
	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);

	pr_debug("%s: ctrl=%p ndx=%d enable=%d\n", __func__, ctrl, ctrl->ndx,
			enable);

	/* Any panel specific low power commands/config */
	if (enable) {
		pinfo->blank_state = MDSS_PANEL_BLANK_LOW_POWER;
		dstat->on = 0;
	} else {
		pinfo->blank_state = MDSS_PANEL_BLANK_UNBLANK;
		dstat->on = 1;
	}

	mdss_dsi_panel_alpm_ctrl(pdata, enable);

	pr_debug("%s:-\n", __func__);
	return 0;
}

static void mdss_dsi_parse_lane_swap(struct device_node *np, char *dlane_swap)
{
	const char *data;

	*dlane_swap = DSI_LANE_MAP_0123;
	data = of_get_property(np, "qcom,mdss-dsi-lane-map", NULL);
	if (data) {
		if (!strcmp(data, "lane_map_3012"))
			*dlane_swap = DSI_LANE_MAP_3012;
		else if (!strcmp(data, "lane_map_2301"))
			*dlane_swap = DSI_LANE_MAP_2301;
		else if (!strcmp(data, "lane_map_1230"))
			*dlane_swap = DSI_LANE_MAP_1230;
		else if (!strcmp(data, "lane_map_0321"))
			*dlane_swap = DSI_LANE_MAP_0321;
		else if (!strcmp(data, "lane_map_1032"))
			*dlane_swap = DSI_LANE_MAP_1032;
		else if (!strcmp(data, "lane_map_2103"))
			*dlane_swap = DSI_LANE_MAP_2103;
		else if (!strcmp(data, "lane_map_3210"))
			*dlane_swap = DSI_LANE_MAP_3210;
	}
}

static void mdss_dsi_parse_trigger(struct device_node *np, char *trigger,
		char *trigger_key)
{
	const char *data;

	*trigger = DSI_CMD_TRIGGER_SW;
	data = of_get_property(np, trigger_key, NULL);
	if (data) {
		if (!strcmp(data, "none"))
			*trigger = DSI_CMD_TRIGGER_NONE;
		else if (!strcmp(data, "trigger_te"))
			*trigger = DSI_CMD_TRIGGER_TE;
		else if (!strcmp(data, "trigger_sw_seof"))
			*trigger = DSI_CMD_TRIGGER_SW_SEOF;
		else if (!strcmp(data, "trigger_sw_te"))
			*trigger = DSI_CMD_TRIGGER_SW_TE;
	}
}

static int mdss_dsi_parse_dcs_cmds(struct device_node *np,
		struct dsi_panel_cmds *pcmds, char *cmd_key, char *link_key)
{
	const char *data;
	int blen = 0, len;
	char *buf, *bp;
	struct dsi_ctrl_hdr *dchdr;
	int i, cnt;
	int is_read = 0, type;

	data = of_get_property(np, cmd_key, &blen);
	if (!data) {
		pr_err("%s: failed, key=%s\n", __func__, cmd_key);
		return -ENOMEM;
	}

	buf = kzalloc(sizeof(char) * blen, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memcpy(buf, data, blen);

	/* scan dcs commands */
	bp = buf;
	len = blen;
	cnt = 0;
	while (len >= sizeof(*dchdr)) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		dchdr->dlen = ntohs(dchdr->dlen);
		if (dchdr->dlen > len) {
			pr_err("%s: dtsi cmd=%x error, len=%d",
					__func__, dchdr->dtype, dchdr->dlen);
			goto exit_free;
		}
		bp += sizeof(*dchdr);
		len -= sizeof(*dchdr);
		bp += dchdr->dlen;
		len -= dchdr->dlen;
		cnt++;
		type = dchdr->dtype;
		if (type == DTYPE_GEN_READ ||
				type == DTYPE_GEN_READ1 ||
				type == DTYPE_GEN_READ2 ||
				type == DTYPE_DCS_READ)	{
			/* Read command :
			   last byte contain read size, read start */
			bp += 2;
			len -= 2;
			is_read = 1;
		}
	}

	if (len != 0) {
		pr_err("%s: dcs_cmd=%x len=%d error!",
				__func__, buf[0], blen);
		goto exit_free;
	}

	if (is_read) {
		/*
		   Allocate an array which will store the number
		   for bytes to read for each read command
		 */
		pcmds->read_size = kzalloc(sizeof(char) *
				cnt, GFP_KERNEL);
		if (!pcmds->read_size) {
			pr_err("%s:%d, Unable to read NV cmds",
					__func__, __LINE__);
			goto exit_free;
		}
		pcmds->read_startoffset = kzalloc(sizeof(char) *
				cnt, GFP_KERNEL);
		if (!pcmds->read_startoffset) {
			pr_err("%s:%d, Unable to read NV cmds",
					__func__, __LINE__);
			goto error1;
		}
	}

	pcmds->cmds = kzalloc(cnt * sizeof(struct dsi_cmd_desc),
			GFP_KERNEL);
	if (!pcmds->cmds)
		goto exit_free;

	pcmds->cmd_cnt = cnt;
	pcmds->buf = buf;
	pcmds->blen = blen;

	bp = buf;
	len = blen;
	for (i = 0; i < cnt; i++) {
		dchdr = (struct dsi_ctrl_hdr *)bp;
		len -= sizeof(*dchdr);
		bp += sizeof(*dchdr);
		pcmds->cmds[i].dchdr = *dchdr;
		pcmds->cmds[i].payload = bp;
		bp += dchdr->dlen;
		len -= dchdr->dlen;
		if (is_read) {
			pcmds->read_size[i] = *bp++;
			pcmds->read_startoffset[i] = *bp++;
			len -= 2;
		}
	}

	/*Set default link state to LP Mode*/
	pcmds->link_state = DSI_LP_MODE;

	if (link_key) {
		data = of_get_property(np, link_key, NULL);
		if (data && !strcmp(data, "dsi_hs_mode"))
			pcmds->link_state = DSI_HS_MODE;
		else
			pcmds->link_state = DSI_LP_MODE;
	}

	pr_info("%s: dcs_cmd=%x len=%d, cmd_cnt=%d link_state=%d\n", __func__,
			pcmds->buf[0], pcmds->blen, pcmds->cmd_cnt, pcmds->link_state);

	return 0;

error1:
	kfree(pcmds->read_size);
exit_free:
	kfree(buf);
	return -ENOMEM;
}

static int mdss_samsung_parse_candella_lux_mapping_table(struct device_node *np,
		struct candella_lux_map *table, char *keystring)
{
	const __be32 *data;
	int  data_offset, len = 0 , i = 0;
	int  cdmap_start = 0, cdmap_end = 0;
	data = of_get_property(np, keystring, &len);
	if (!data) {
		pr_err("%s:%d, Unable to read table %s ",
				__func__, __LINE__, keystring);
		return -EINVAL;
	}
	if ((len % 4) != 0) {
		pr_err("%s:%d, Incorrect table entries for %s",
				__func__, __LINE__, keystring);
		return -EINVAL;
	}
	table->lux_tab_size = len / (sizeof(int)*4);
	table->lux_tab = kzalloc((sizeof(int) * table->lux_tab_size),
			GFP_KERNEL);
	if (!table->lux_tab)
		return -ENOMEM;
	table->cmd_idx = kzalloc((sizeof(int) * table->lux_tab_size),
			GFP_KERNEL);
	if (!table->cmd_idx)
		goto error;
	data_offset = 0;
	for (i = 0; i < table->lux_tab_size; i++) {
		table->cmd_idx[i] = be32_to_cpup(&data[data_offset++]);
		/* 1rst field => <idx> */
		cdmap_start = be32_to_cpup(&data[data_offset++]);
		/* 2nd field => <from> */
		cdmap_end = be32_to_cpup(&data[data_offset++]);
		/* 3rd field => <till> */
		table->lux_tab[i] = be32_to_cpup(&data[data_offset++]);
		/* 4th field => <candella> */
		/* Fill the backlight level to lux mapping array */
		do {
			table->bkl[cdmap_start++] = i;
		} while (cdmap_start <= cdmap_end);
	}
	return 0;
error:
	kfree(table->lux_tab);
	return -ENOMEM;
}

int mdss_panel_get_dst_fmt(u32 bpp, char mipi_mode, u32 pixel_packing,
		char *dst_format)
{
	int rc = 0;
	switch (bpp) {
		case 3:
			*dst_format = DSI_CMD_DST_FORMAT_RGB111;
			break;
		case 8:
			*dst_format = DSI_CMD_DST_FORMAT_RGB332;
			break;
		case 12:
			*dst_format = DSI_CMD_DST_FORMAT_RGB444;
			break;
		case 16:
			switch (mipi_mode) {
				case DSI_VIDEO_MODE:
					*dst_format = DSI_VIDEO_DST_FORMAT_RGB565;
					break;
				case DSI_CMD_MODE:
					*dst_format = DSI_CMD_DST_FORMAT_RGB565;
					break;
				default:
					*dst_format = DSI_VIDEO_DST_FORMAT_RGB565;
					break;
			}
			break;
		case 18:
			switch (mipi_mode) {
				case DSI_VIDEO_MODE:
					if (pixel_packing == 0)
						*dst_format = DSI_VIDEO_DST_FORMAT_RGB666;
					else
						*dst_format = DSI_VIDEO_DST_FORMAT_RGB666_LOOSE;
					break;
				case DSI_CMD_MODE:
					*dst_format = DSI_CMD_DST_FORMAT_RGB666;
					break;
				default:
					if (pixel_packing == 0)
						*dst_format = DSI_VIDEO_DST_FORMAT_RGB666;
					else
						*dst_format = DSI_VIDEO_DST_FORMAT_RGB666_LOOSE;
					break;
			}
			break;
		case 24:
			switch (mipi_mode) {
				case DSI_VIDEO_MODE:
					*dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
					break;
				case DSI_CMD_MODE:
					*dst_format = DSI_CMD_DST_FORMAT_RGB888;
					break;
				default:
					*dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
					break;
			}
			break;
		default:
			rc = -EINVAL;
			break;
	}
	return rc;
}

static int mdss_dsi_parse_fbc_params(struct device_node *np,
		struct mdss_panel_info *panel_info)
{
	int rc, fbc_enabled = 0;
	u32 tmp;

	fbc_enabled = of_property_read_bool(np,	"qcom,mdss-dsi-fbc-enable");
	if (fbc_enabled) {
		pr_debug("%s:%d FBC panel enabled.\n", __func__, __LINE__);
		panel_info->fbc.enabled = 1;
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-bpp", &tmp);
		panel_info->fbc.target_bpp =	(!rc ? tmp : panel_info->bpp);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-packing",
				&tmp);
		panel_info->fbc.comp_mode = (!rc ? tmp : 0);
		panel_info->fbc.qerr_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-quant-error");
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-bias", &tmp);
		panel_info->fbc.cd_bias = (!rc ? tmp : 0);
		panel_info->fbc.pat_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-pat-mode");
		panel_info->fbc.vlc_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-vlc-mode");
		panel_info->fbc.bflc_enable = of_property_read_bool(np,
				"qcom,mdss-dsi-fbc-bflc-mode");
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-h-line-budget",
				&tmp);
		panel_info->fbc.line_x_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-budget-ctrl",
				&tmp);
		panel_info->fbc.block_x_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-block-budget",
				&tmp);
		panel_info->fbc.block_budget = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossless-threshold", &tmp);
		panel_info->fbc.lossless_mode_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossy-threshold", &tmp);
		panel_info->fbc.lossy_mode_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np, "qcom,mdss-dsi-fbc-rgb-threshold",
				&tmp);
		panel_info->fbc.lossy_rgb_thd = (!rc ? tmp : 0);
		rc = of_property_read_u32(np,
				"qcom,mdss-dsi-fbc-lossy-mode-idx", &tmp);
		panel_info->fbc.lossy_mode_idx = (!rc ? tmp : 0);
	} else {
		pr_debug("%s:%d Panel does not support FBC.\n",
				__func__, __LINE__);
		panel_info->fbc.enabled = 0;
		panel_info->fbc.target_bpp =
			panel_info->bpp;
	}
	return 0;
}

static void mdss_panel_parse_te_params(struct device_node *np,
		struct mdss_panel_info *panel_info)
{
	u32 tmp;
	int rc = 0;
	/*
	 * TE default: dsi byte clock calculated base on 70 fps;
	 * around 14 ms to complete a kickoff cycle if te disabled;
	 * vclk_line base on 60 fps; write is faster than read;
	 * init == start == rdptr;
	 */
	panel_info->te.tear_check_en =
		!of_property_read_bool(np, "qcom,mdss-tear-check-disable");
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-cfg-height", &tmp);
	panel_info->te.sync_cfg_height = (!rc ? tmp : 0xfff0);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-init-val", &tmp);
	panel_info->te.vsync_init_val = (!rc ? tmp : panel_info->yres);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-threshold-start", &tmp);
	panel_info->te.sync_threshold_start = (!rc ? tmp : 4);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-sync-threshold-continue", &tmp);
	panel_info->te.sync_threshold_continue = (!rc ? tmp : 4);
	rc = of_property_read_u32(np, "qcom,mdss-tear-check-start-pos", &tmp);
	panel_info->te.start_pos = (!rc ? tmp : panel_info->yres);
	rc = of_property_read_u32
		(np, "qcom,mdss-tear-check-rd-ptr-trigger-intr", &tmp);
	panel_info->te.rd_ptr_irq = (!rc ? tmp : panel_info->yres + 1);
	rc = of_property_read_u32(np, "qcom,mdss-tear-check-frame-rate", &tmp);
	panel_info->te.refx100 = (!rc ? tmp : 6000);
}

static int mdss_dsi_parse_reset_seq(struct device_node *np,
		u32 rst_seq[MDSS_DSI_RST_SEQ_LEN], u32 *rst_len,
		const char *name)
{
	int num = 0, i;
	int rc;
	struct property *data;
	u32 tmp[MDSS_DSI_RST_SEQ_LEN];
	*rst_len = 0;
	data = of_find_property(np, name, &num);
	num /= sizeof(u32);
	if (!data || !num || num > MDSS_DSI_RST_SEQ_LEN || num % 2) {
		pr_debug("%s:%d, error reading %s, length found = %d\n",
				__func__, __LINE__, name, num);
	} else {
		rc = of_property_read_u32_array(np, name, tmp, num);
		if (rc)
			pr_debug("%s:%d, error reading %s, rc = %d\n",
					__func__, __LINE__, name, rc);
		else {
			for (i = 0; i < num; ++i)
				rst_seq[i] = tmp[i];
			*rst_len = num;
		}
	}
	return 0;
}

static void mdss_dsi_parse_roi_alignment(struct device_node *np,
		struct mdss_panel_info *pinfo)
{
	int len = 0;
	u32 value[6];
	struct property *data;
	data = of_find_property(np, "qcom,panel-roi-alignment", &len);
	len /= sizeof(u32);
	if (!data || (len != 6)) {
		pr_debug("%s: Panel roi alignment not found", __func__);
	} else {
		int rc = of_property_read_u32_array(np,
				"qcom,panel-roi-alignment", value, len);
		if (rc)
			pr_debug("%s: Error reading panel roi alignment values",
					__func__);
		else {
			pinfo->xstart_pix_align = value[0];
			pinfo->width_pix_align = value[1];
			pinfo->ystart_pix_align = value[2];
			pinfo->height_pix_align = value[3];
			pinfo->min_width = value[4];
			pinfo->min_height = value[5];
		}

		pr_debug("%s: ROI alignment: [%d, %d, %d, %d, %d, %d]",
				__func__, pinfo->xstart_pix_align,
				pinfo->width_pix_align, pinfo->ystart_pix_align,
				pinfo->height_pix_align, pinfo->min_width,
				pinfo->min_height);
	}
}

static int mdss_dsi_parse_panel_features(struct device_node *np,
		struct mdss_dsi_ctrl_pdata *ctrl)
{
	struct mdss_panel_info *pinfo;

	if (!np || !ctrl) {
		pr_err("%s: Invalid arguments\n", __func__);
		return -ENODEV;
	}

	pinfo = &ctrl->panel_data.panel_info;

	pinfo->cont_splash_enabled = of_property_read_bool(np,
			"qcom,cont-splash-enabled");

	if (pinfo->mipi.mode == DSI_CMD_MODE) {
		pinfo->partial_update_enabled = of_property_read_bool(np,
				"qcom,partial-update-enabled");
		pr_info("%s: partial_update_enabled=%d\n", __func__,
				pinfo->partial_update_enabled);
		if (pinfo->partial_update_enabled) {
			ctrl->set_col_page_addr = mdss_dsi_set_col_page_addr;
			pinfo->partial_update_dcs_cmd_by_left =
				of_property_read_bool(np,
						"qcom,partial-update-dcs-cmd-by-left");
			pinfo->partial_update_roi_merge =
				of_property_read_bool(np,
						"qcom,partial-update-roi-merge");
		}
	}

	pinfo->ulps_feature_enabled = of_property_read_bool(np,
			"qcom,ulps-enabled");
	pr_info("%s: ulps feature %s\n", __func__,
			(pinfo->ulps_feature_enabled ? "enabled" : "disabled"));
	pinfo->esd_check_enabled = of_property_read_bool(np,
			"qcom,esd-check-enabled");

	pinfo->mipi.dynamic_switch_enabled = of_property_read_bool(np,
			"qcom,dynamic-mode-switch-enabled");

	if (pinfo->mipi.dynamic_switch_enabled) {
		mdss_dsi_parse_dcs_cmds(np, &ctrl->video2cmd,
				"qcom,video-to-cmd-mode-switch-commands", NULL);

		mdss_dsi_parse_dcs_cmds(np, &ctrl->cmd2video,
				"qcom,cmd-to-video-mode-switch-commands", NULL);

		if (!ctrl->video2cmd.cmd_cnt || !ctrl->cmd2video.cmd_cnt) {
			pr_warn("No commands specified for dynamic switch\n");
			pinfo->mipi.dynamic_switch_enabled = 0;
		}
	}

	pr_info("%s: dynamic switch feature enabled: %d\n", __func__,
			pinfo->mipi.dynamic_switch_enabled);

	return 0;
}

static void mdss_dsi_parse_panel_horizintal_line_idle(struct device_node *np,
		struct mdss_dsi_ctrl_pdata *ctrl)
{
	const u32 *src;
	int i, len, cnt;
	struct panel_horizontal_idle *kp;

	if (!np || !ctrl) {
		pr_err("%s: Invalid arguments\n", __func__);
		return;
	}

	src = of_get_property(np, "qcom,mdss-dsi-hor-line-idle", &len);
	if (!src || len == 0)
		return;

	cnt = len % 3; /* 3 fields per entry */
	if (cnt) {
		pr_err("%s: invalid horizontal idle len=%d\n", __func__, len);
		return;
	}

	cnt = len / sizeof(u32);

	kp = kzalloc(sizeof(*kp) * (cnt / 3), GFP_KERNEL);
	if (kp == NULL) {
		pr_err("%s: No memory\n", __func__);
		return;
	}

	ctrl->line_idle = kp;
	for (i = 0; i < cnt; i += 3) {
		kp->min = be32_to_cpu(src[i]);
		kp->max = be32_to_cpu(src[i+1]);
		kp->idle = be32_to_cpu(src[i+2]);
		kp++;
		ctrl->horizontal_idle_cnt++;
	}

	pr_debug("%s: horizontal_idle_cnt=%d\n", __func__,
			ctrl->horizontal_idle_cnt);
}

static int mdss_panel_parse_dt(struct device_node *np,
		struct mdss_dsi_ctrl_pdata *ctrl_pdata)
{
	u32	tmp;
	int rc, i, len;
	const char *data;
	static const char *pdest;
	struct mdss_panel_info *pinfo = &(ctrl_pdata->panel_data.panel_info);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-width", &tmp);
	if (rc) {
		pr_err("%s:%d, panel width not specified\n",
				__func__, __LINE__);
		return -EINVAL;
	}
	pinfo->xres = (!rc ? tmp : 640);

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-height", &tmp);
	if (rc) {
		pr_err("%s:%d, panel height not specified\n",
				__func__, __LINE__);
		return -EINVAL;
	}
	pinfo->yres = (!rc ? tmp : 480);

	rc = of_property_read_u32(np,
			"qcom,mdss-pan-physical-width-dimension", &tmp);
	pinfo->physical_width = (!rc ? tmp : 0);
	rc = of_property_read_u32(np,
			"qcom,mdss-pan-physical-height-dimension", &tmp);
	pinfo->physical_height = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-left-border", &tmp);
	pinfo->lcdc.xres_pad = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-right-border", &tmp);
	if (!rc)
		pinfo->lcdc.xres_pad += tmp;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-top-border", &tmp);
	pinfo->lcdc.yres_pad = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-bottom-border", &tmp);
	if (!rc)
		pinfo->lcdc.yres_pad += tmp;
	rc = of_property_read_u32(np, "qcom,mdss-dsi-bpp", &tmp);
	if (rc) {
		pr_err("%s:%d, bpp not specified\n", __func__, __LINE__);
		return -EINVAL;
	}
	pinfo->bpp = (!rc ? tmp : 24);
	pinfo->mipi.mode = DSI_VIDEO_MODE;
	data = of_get_property(np, "qcom,mdss-dsi-panel-type", NULL);
	if (data && !strncmp(data, "dsi_cmd_mode", 12))
		pinfo->mipi.mode = DSI_CMD_MODE;
	tmp = 0;
	data = of_get_property(np, "qcom,mdss-dsi-pixel-packing", NULL);
	if (data && !strcmp(data, "loose"))
		pinfo->mipi.pixel_packing = 1;
	else
		pinfo->mipi.pixel_packing = 0;
	rc = mdss_panel_get_dst_fmt(pinfo->bpp,
			pinfo->mipi.mode, pinfo->mipi.pixel_packing,
			&(pinfo->mipi.dst_format));
	if (rc) {
		pr_debug("%s: problem determining dst format. Set Default\n",
				__func__);
		pinfo->mipi.dst_format =
			DSI_VIDEO_DST_FORMAT_RGB888;
	}
	pdest = of_get_property(np,
			"qcom,mdss-dsi-panel-destination", NULL);

	if (pdest) {
		if (strlen(pdest) != 9) {
			pr_err("%s: Unknown pdest specified\n", __func__);
			return -EINVAL;
		}
		if (!strcmp(pdest, "display_1"))
			pinfo->pdest = DISPLAY_1;
		else if (!strcmp(pdest, "display_2"))
			pinfo->pdest = DISPLAY_2;
		else {
			pr_debug("%s: incorrect pdest. Set Default\n",
					__func__);
			pinfo->pdest = DISPLAY_1;
		}
	} else {
		pr_debug("%s: pdest not specified. Set Default\n",
				__func__);
		pinfo->pdest = DISPLAY_1;
	}
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-front-porch", &tmp);
	pinfo->lcdc.h_front_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-back-porch", &tmp);
	pinfo->lcdc.h_back_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-pulse-width", &tmp);
	pinfo->lcdc.h_pulse_width = (!rc ? tmp : 2);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-h-sync-skew", &tmp);
	pinfo->lcdc.hsync_skew = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-back-porch", &tmp);
	pinfo->lcdc.v_back_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-front-porch", &tmp);
	pinfo->lcdc.v_front_porch = (!rc ? tmp : 6);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-v-pulse-width", &tmp);
	pinfo->lcdc.v_pulse_width = (!rc ? tmp : 2);
	rc = of_property_read_u32(np,
			"qcom,mdss-dsi-underflow-color", &tmp);
	pinfo->lcdc.underflow_clr = (!rc ? tmp : 0xff);
	rc = of_property_read_u32(np,
			"qcom,mdss-dsi-border-color", &tmp);
	pinfo->lcdc.border_clr = (!rc ? tmp : 0);
	data = of_get_property(np, "qcom,mdss-dsi-panel-orientation", NULL);
	if (data) {
		pr_debug("panel orientation is %s\n", data);
		if (!strcmp(data, "180"))
			pinfo->panel_orientation = MDP_ROT_180;
		else if (!strcmp(data, "hflip"))
			pinfo->panel_orientation = MDP_FLIP_LR;
		else if (!strcmp(data, "vflip"))
			pinfo->panel_orientation = MDP_FLIP_UD;
	}

	ctrl_pdata->bklt_ctrl = UNKNOWN_CTRL;
	data = of_get_property(np, "qcom,mdss-dsi-bl-pmic-control-type", NULL);
	if (data) {
		if (!strncmp(data, "bl_ctrl_wled", 12)) {
			led_trigger_register_simple("bkl-trigger",
					&bl_led_trigger);
			pr_debug("%s: SUCCESS-> WLED TRIGGER register\n",
					__func__);
			ctrl_pdata->bklt_ctrl = BL_WLED;
		} else if (!strncmp(data, "bl_ctrl_pwm", 11)) {
			ctrl_pdata->bklt_ctrl = BL_PWM;
			rc = of_property_read_u32(np,
					"qcom,mdss-dsi-bl-pmic-pwm-frequency", &tmp);
			if (rc) {
				pr_err("%s:%d, Error, panel pwm_period\n",
						__func__, __LINE__);
				return -EINVAL;
			}
			ctrl_pdata->pwm_period = tmp;
			rc = of_property_read_u32(np,
					"qcom,mdss-dsi-bl-pmic-bank-select", &tmp);
			if (rc) {
				pr_err("%s:%d, Error, dsi lpg channel\n",
						__func__, __LINE__);
				return -EINVAL;
			}
			ctrl_pdata->pwm_lpg_chan = tmp;
			tmp = of_get_named_gpio(np,
					"qcom,mdss-dsi-pwm-gpio", 0);
			ctrl_pdata->pwm_pmic_gpio = tmp;
			pr_debug("%s: Configured PWM bklt ctrl\n", __func__);
		} else if (!strncmp(data, "bl_ctrl_dcs", 11)) {
			ctrl_pdata->bklt_ctrl = BL_DCS_CMD;
			pr_debug("%s: Configured DCS_CMD bklt ctrl\n",
					__func__);
		}
	}
	rc = of_property_read_u32(np, "qcom,mdss-brightness-max-level", &tmp);
	pinfo->brightness_max = (!rc ? tmp : MDSS_MAX_BL_BRIGHTNESS);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-bl-min-level", &tmp);
	pinfo->bl_min = (!rc ? tmp : 0);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-bl-max-level", &tmp);
	pinfo->bl_max = (!rc ? tmp : 255);
	ctrl_pdata->bklt_max = pinfo->bl_max;

	rc = of_property_read_u32(np, "qcom,mdss-dsi-interleave-mode", &tmp);
	pinfo->mipi.interleave_mode = (!rc ? tmp : 0);

	pinfo->mipi.vsync_enable = of_property_read_bool(np,
			"qcom,mdss-dsi-te-check-enable");
	pinfo->mipi.hw_vsync_mode = of_property_read_bool(np,
			"qcom,mdss-dsi-te-using-te-pin");

	rc = of_property_read_u32(np,
			"qcom,mdss-dsi-h-sync-pulse", &tmp);
	pinfo->mipi.pulse_mode_hsa_he = (!rc ? tmp : false);

	pinfo->mipi.hfp_power_stop = of_property_read_bool(np,
			"qcom,mdss-dsi-hfp-power-mode");
	pinfo->mipi.hsa_power_stop = of_property_read_bool(np,
			"qcom,mdss-dsi-hsa-power-mode");
	pinfo->mipi.hbp_power_stop = of_property_read_bool(np,
			"qcom,mdss-dsi-hbp-power-mode");
	pinfo->mipi.last_line_interleave_en = of_property_read_bool(np,
			"qcom,mdss-dsi-last-line-interleave");
	pinfo->mipi.bllp_power_stop = of_property_read_bool(np,
			"qcom,mdss-dsi-bllp-power-mode");
	pinfo->mipi.eof_bllp_power_stop = of_property_read_bool(
			np, "qcom,mdss-dsi-bllp-eof-power-mode");
	pinfo->mipi.traffic_mode = DSI_NON_BURST_SYNCH_PULSE;
	data = of_get_property(np, "qcom,mdss-dsi-traffic-mode", NULL);
	if (data) {
		if (!strcmp(data, "non_burst_sync_event"))
			pinfo->mipi.traffic_mode = DSI_NON_BURST_SYNCH_EVENT;
		else if (!strcmp(data, "burst_mode"))
			pinfo->mipi.traffic_mode = DSI_BURST_MODE;
	}
	rc = of_property_read_u32(np,
			"qcom,mdss-dsi-te-dcs-command", &tmp);
	pinfo->mipi.insert_dcs_cmd =
		(!rc ? tmp : 1);
	rc = of_property_read_u32(np,
			"qcom,mdss-dsi-wr-mem-continue", &tmp);
	pinfo->mipi.wr_mem_continue =
		(!rc ? tmp : 0x3c);
	rc = of_property_read_u32(np,
			"qcom,mdss-dsi-wr-mem-start", &tmp);
	pinfo->mipi.wr_mem_start =
		(!rc ? tmp : 0x2c);
	rc = of_property_read_u32(np,
			"qcom,mdss-dsi-te-pin-select", &tmp);
	pinfo->mipi.te_sel =
		(!rc ? tmp : 1);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-virtual-channel-id", &tmp);
	pinfo->mipi.vc = (!rc ? tmp : 0);
	pinfo->mipi.rgb_swap = DSI_RGB_SWAP_RGB;
	data = of_get_property(np, "qcom,mdss-dsi-color-order", NULL);
	if (data) {
		if (!strcmp(data, "rgb_swap_rbg"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_RBG;
		else if (!strcmp(data, "rgb_swap_bgr"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_BGR;
		else if (!strcmp(data, "rgb_swap_brg"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_BRG;
		else if (!strcmp(data, "rgb_swap_grb"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_GRB;
		else if (!strcmp(data, "rgb_swap_gbr"))
			pinfo->mipi.rgb_swap = DSI_RGB_SWAP_GBR;
	}
	pinfo->mipi.data_lane0 = of_property_read_bool(np,
			"qcom,mdss-dsi-lane-0-state");
	pinfo->mipi.data_lane1 = of_property_read_bool(np,
			"qcom,mdss-dsi-lane-1-state");
	pinfo->mipi.data_lane2 = of_property_read_bool(np,
			"qcom,mdss-dsi-lane-2-state");
	pinfo->mipi.data_lane3 = of_property_read_bool(np,
			"qcom,mdss-dsi-lane-3-state");

	rc = of_property_read_u32(np, "qcom,mdss-dsi-t-clk-pre", &tmp);
	pinfo->mipi.t_clk_pre = (!rc ? tmp : 0x24);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-t-clk-post", &tmp);
	pinfo->mipi.t_clk_post = (!rc ? tmp : 0x03);

	pinfo->mipi.rx_eot_ignore = of_property_read_bool(np,
			"qcom,mdss-dsi-rx-eot-ignore");
	pinfo->mipi.tx_eot_append = of_property_read_bool(np,
			"qcom,mdss-dsi-tx-eot-append");

	rc = of_property_read_u32(np, "qcom,mdss-dsi-stream", &tmp);
	pinfo->mipi.stream = (!rc ? tmp : 0);

	data = of_get_property(np, "qcom,mdss-dsi-panel-mode-gpio-state", NULL);
	if (data) {
		if (!strcmp(data, "high"))
			pinfo->mode_gpio_state = MODE_GPIO_HIGH;
		else if (!strcmp(data, "low"))
			pinfo->mode_gpio_state = MODE_GPIO_LOW;
	} else {
		pinfo->mode_gpio_state = MODE_GPIO_NOT_VALID;
	}

	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-framerate", &tmp);
	pinfo->mipi.frame_rate = (!rc ? tmp : 60);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-clockrate", &tmp);
	pinfo->clk_rate = (!rc ? tmp : 0);
	data = of_get_property(np, "qcom,mdss-dsi-panel-timings", &len);
	if ((!data) || (len != 12)) {
		pr_err("%s:%d, Unable to read Phy timing settings",
				__func__, __LINE__);
		goto error;
	}
	for (i = 0; i < len; i++)
		pinfo->mipi.dsi_phy_db.timing[i] = data[i];

	pinfo->mipi.lp11_init = of_property_read_bool(np,
			"qcom,mdss-dsi-lp11-init");
	rc = of_property_read_u32(np, "qcom,mdss-dsi-init-delay-us", &tmp);
	pinfo->mipi.init_delay = (!rc ? tmp : 0);

	mdss_dsi_parse_roi_alignment(np, pinfo);

	mdss_dsi_parse_trigger(np, &(pinfo->mipi.mdp_trigger),
			"qcom,mdss-dsi-mdp-trigger");

	mdss_dsi_parse_trigger(np, &(pinfo->mipi.dma_trigger),
			"qcom,mdss-dsi-dma-trigger");

	mdss_dsi_parse_lane_swap(np, &(pinfo->mipi.dlane_swap));

	mdss_dsi_parse_fbc_params(np, pinfo);

	mdss_panel_parse_te_params(np, pinfo);

	mdss_dsi_parse_reset_seq(np, pinfo->rst_seq, &(pinfo->rst_seq_len),
			"qcom,mdss-dsi-reset-sequence");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->on_cmds,
			"qcom,mdss-dsi-on-command", "qcom,mdss-dsi-on-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->off_cmds,
			"qcom,mdss-dsi-off-command", "qcom,mdss-dsi-off-command-state");

	mdss_dsi_parse_dcs_cmds(np, &ctrl_pdata->status_cmds,
			"qcom,mdss-dsi-panel-status-command",
			"qcom,mdss-dsi-panel-status-command-state");
	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-status-value", &tmp);
	ctrl_pdata->status_value = (!rc ? tmp : 0);


	ctrl_pdata->status_mode = ESD_MAX;
	rc = of_property_read_string(np,
			"qcom,mdss-dsi-panel-status-check-mode", &data);
	if (!rc) {
		if (!strcmp(data, "bta_check"))
			ctrl_pdata->status_mode = ESD_BTA;
		else if (!strcmp(data, "reg_read"))
			ctrl_pdata->status_mode = ESD_REG;
	}

	rc = mdss_dsi_parse_panel_features(np, ctrl_pdata);
	if (rc) {
		pr_err("%s: failed to parse panel features\n", __func__);
		goto error;
	}

	mdss_dsi_parse_panel_horizintal_line_idle(np, ctrl_pdata);

	rc = of_property_read_u32(np, "qcom,mdss-force-clk-lane-hs", &tmp);
	pinfo->mipi.force_clk_lane_hs = (!rc ? tmp : 0);

	pinfo->ulps_feature_enabled = of_property_read_bool(np,
			"qcom,ulps-enabled");
	pr_info("%s: ulps feature %s", __func__,
			(pinfo->ulps_feature_enabled ? "enabled" : "disabled"));

	mdss_panel_parse_te_params(np, pinfo);
	mdss_dsi_parse_dcs_cmds(np, &display_on_seq,
			"qcom,mdss-display-on-seq", "qcom,mdss-dsi-on-command-state");
	mdss_dsi_parse_dcs_cmds(np, &display_on_cmd,
			"qcom,mdss-display-on-cmd", NULL);
	mdss_dsi_parse_dcs_cmds(np, &display_off_seq,
			"qcom,mdss-display-off-seq", "qcom,mdss-dsi-off-command-state");
	mdss_dsi_parse_dcs_cmds(np, &manufacture_id_cmds,
			"samsung,panel-manufacture-id-read-cmds", NULL);
	mdss_dsi_parse_dcs_cmds(np, &mtp_id_cmds,
			"samsung,panel-mtp-id-read-cmds", NULL);
	mdss_dsi_parse_dcs_cmds(np, &mtp_enable_cmds,
			"samsung,panel-mtp-enable-cmds", NULL);
	mdss_dsi_parse_dcs_cmds(np, &gamma_cmds_list,
			"samsung,panel-gamma-cmds-list", NULL);
	mdss_dsi_parse_dcs_cmds(np, &backlight_cmds,
			"samsung,panel-backlight-cmds", NULL);
	mdss_dsi_parse_dcs_cmds(np, &rddpm_cmds,
			"samsung,panel-rddpm-read-cmds", NULL);
#if defined(ALPM_MODE)
	mdss_dsi_parse_dcs_cmds(np, &alpm_on_seq,
			"samsung,panel-alpm-on-seq", NULL);
	mdss_dsi_parse_dcs_cmds(np, &alpm_off_seq,
			"samsung,panel-alpm-off-seq", NULL);
	rc = of_property_read_u32(np, "qcom,mdss-dsi-panel-alpm-framerate", &tmp);
	pinfo->mipi.alpm_frame_rate = (!rc ? tmp : 30);
#endif
	mdss_samsung_parse_candella_lux_mapping_table(np,
			&candela_map_table,
			"samsung,panel-candella-mapping-table-300");

	return 0;
error:
	return -EINVAL;
}

#if defined(CONFIG_LCD_CLASS_DEVICE)
static ssize_t mdss_dsi_disp_get_power(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	pr_info("mdss_samsung_disp_get_power(0)\n");
	return 0;
}

static ssize_t mdss_dsi_disp_set_power(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int power;
	if (sscanf(buf, "%u", &power) != 1)
		return -EINVAL;
	pr_info("mdss_samsung_disp_set_power:%d\n", power);
	return size;
}

static DEVICE_ATTR(lcd_power, S_IRUGO | S_IWUSR | S_IWGRP,
		mdss_dsi_disp_get_power,
		mdss_dsi_disp_set_power);

static ssize_t mdss_siop_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int rc;
	struct mdss_dsi_ctrl_pdata *ctrl =
		(struct mdss_dsi_ctrl_pdata *)dev_get_drvdata(dev);
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;
	struct display_status *dstat = &msd->dstat;

	rc = snprintf(buf, PAGE_SIZE, "%d\n", dstat->siop_status);
	pr_info("%s : siop status : %d\n", __func__, dstat->siop_status);
	return rc;
}
static ssize_t mdss_siop_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct mdss_dsi_ctrl_pdata *ctrl =
		(struct mdss_dsi_ctrl_pdata *)dev_get_drvdata(dev);
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;
	struct display_status *dstat = &msd->dstat;

	if (sysfs_streq(buf, "1") && !dstat->siop_status)
		dstat->siop_status = true;
	else if (sysfs_streq(buf, "0") && dstat->siop_status)
		dstat->siop_status = false;
	else
		pr_info("%s: Invalid argument!!", __func__);

	return size;

}

static DEVICE_ATTR(siop_enable, S_IRUGO | S_IWUSR | S_IWGRP,
		mdss_siop_enable_show,
		mdss_siop_enable_store);


static struct lcd_ops mdss_dsi_disp_props = {

	.get_power = NULL,
	.set_power = NULL,

};

static ssize_t mdss_disp_lcdtype_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdss_dsi_ctrl_pdata *ctrl =
		(struct mdss_dsi_ctrl_pdata *)dev_get_drvdata(dev);
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;

	return snprintf(buf, PAGE_SIZE, "SDC_%x\n", msd->manufacture_id);
}
static DEVICE_ATTR(lcd_type, S_IRUGO, mdss_disp_lcdtype_show, NULL);

#endif

static int mdss_samsung_rddpm_status(struct mdss_panel_data *pdata)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata, panel_data);

	mdss_dsi_panel_cmd_read(ctrl,
			rddpm_cmds.cmds[0].payload[0],
			rddpm_cmds.cmds[0].payload[1],
			NULL,
			ctrl->rx_buf.data,
			rddpm_cmds.read_size[0]);

	return (int)ctrl->rx_buf.data[0];
}

static int
samsung_dsi_panel_event_handler(struct mdss_panel_data *pdata, int event)
{
	struct mdss_dsi_ctrl_pdata *ctrl = NULL;
	struct display_status *dstat = NULL;

	if (pdata == NULL) {
		pr_err("%s: Invalid input data\n", __func__);
		return -EINVAL;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
			panel_data);
	dstat = &((struct mdss_samsung_driver_data *)pdata->panel_private)->dstat;

	pr_debug("%s : %d", __func__, event);
	switch (event) {
		case MDSS_EVENT_FRAME_UPDATE:
			if (dstat->wait_disp_on) {
				mdss_samsung_disp_send_cmd(ctrl, PANEL_DISPLAY_ON, true);
				dstat->wait_disp_on = 0;
				if (rddpm_cmds.cmd_cnt)
					pr_info("DISPLAY_ON(rddpm: 0x%x)\n",
							mdss_samsung_rddpm_status(pdata));
				else
					pr_info("DISPLAY_ON\n");
			}
			break;
		default:
			pr_debug("%s : unknown event (%d)\n", __func__, event);
			break;
	}

	return 0;
}

static ssize_t mdss_samsung_ambient_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mdss_dsi_ctrl_pdata *ctrl =
		(struct mdss_dsi_ctrl_pdata *)dev_get_drvdata(dev);
	struct mdss_panel_info *pinfo;

	if (!ctrl) {
		pr_err("%s: Invalid arguments\n", __func__);
		goto err;
	}
	pinfo = &ctrl->panel_data.panel_info;

	pr_info("[ALPM_DEBUG] %s: current status : %d\n",
			__func__, pinfo->is_suspending);

err:
	return 0;
}

static ssize_t mdss_samsung_ambient_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int ambient_mode = 0;
	struct mdss_dsi_ctrl_pdata *ctrl =
		(struct mdss_dsi_ctrl_pdata *)dev_get_drvdata(dev);
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;
	struct display_status *dstat = &msd->dstat;

	sscanf(buf, "%d" , &ambient_mode);

	pr_info("[ALPM_DEBUG] %s: mode : %d\n", __func__, ambient_mode);

	if (dstat->on) {
		if (ambient_mode)
			mdss_dsi_panel_bl_dim(&ctrl->panel_data, PANEL_BACKLIGHT_RESTORE);
		else
			mdss_dsi_panel_bl_dim(&ctrl->panel_data, PANEL_BACKLIGHT_DIM);
	} else
		pr_info("[ALPM_DEBUG] %s: The LCD already turned off\n"
				, __func__);

	alpm_enable(ctrl, ambient_mode ? MODE_OFF : ALPM_MODE_ON);

	return size;
}
static DEVICE_ATTR(ambient, S_IRUGO | S_IWUSR | S_IWGRP,
		mdss_samsung_ambient_show,
		mdss_samsung_ambient_store);
#if defined(ALPM_MODE)
static ssize_t mdss_samsung_alpm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int rc;
	struct mdss_dsi_ctrl_pdata *ctrl =
		(struct mdss_dsi_ctrl_pdata *)dev_get_drvdata(dev);
	struct mdss_alpm_data *adata = &ctrl->panel_data.alpm_data;
	int current_status = 0;

	if (adata && adata->alpm_status)
		current_status = (int)adata->alpm_status(CHECK_CURRENT_STATUS);

	rc = snprintf(buf, PAGE_SIZE, "%d\n", current_status);
	pr_info("[ALPM_DEBUG] %s: current status : %d\n",
			__func__, current_status);

	return rc;
}

static ssize_t mdss_samsung_alpm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int mode = 0;
	struct mdss_dsi_ctrl_pdata *ctrl =
		(struct mdss_dsi_ctrl_pdata *)dev_get_drvdata(dev);
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)ctrl->panel_data.panel_private;
	struct display_status *dstat = &msd->dstat;
	struct mdss_alpm_data *adata = &ctrl->panel_data.alpm_data;

	sscanf(buf, "%d" , &mode);
	pr_info("[ALPM_DEBUG] %s: mode : %d\n", __func__, mode);

	/*
	 * Possible mode status for Blank(0) or Unblank(1)
	 *	* Blank *
	 *		1) ALPM_MODE_ON
	 *			-> That will set during wakeup
	 *	* Unblank *
	 */
	if (mode == ALPM_MODE_ON) {
		adata->alpm_status(mode);
		/*
		 * This will work if the ALPM must be on or chagne partial area
		 * if that already in the status of unblank
		 */
		if (dstat->on) {
			if (!adata->alpm_status(CHECK_PREVIOUS_STATUS) &&
					adata->alpm_status(CHECK_CURRENT_STATUS)) {
				/* Turn On ALPM Mode */
				mdss_samsung_disp_send_cmd(ctrl, PANEL_ALPM_ON, true);
				if (dstat->wait_disp_on == 0) {
					/* wait 1 frame(more than 16ms) */
					msleep(20);
					mdss_samsung_disp_send_cmd(ctrl, PANEL_DISPLAY_ON, true);
				}
				adata->alpm_status(STORE_CURRENT_STATUS);
				pr_info("[ALPM_DEBUG] %s: Send ALPM mode on cmds\n",
						__func__);
			}
		}
	} else if (mode == MODE_OFF) {
		if (adata->alpm_status) {
			adata->alpm_status(mode);
			if (adata->alpm_status(CHECK_PREVIOUS_STATUS)
					== ALPM_MODE_ON) {
				if (dstat->on) {
					mdss_samsung_disp_send_cmd(ctrl, PANEL_ALPM_OFF, true);
					/* wait 1 frame(more than 16ms) */
					msleep(20);
					adata->alpm_status(CLEAR_MODE_STATUS);
					pr_info("[ALPM_DEBUG] %s: Send ALPM off cmds\n", __func__);

				}
			}
		}
	} else {
		pr_info("[ALPM_DEBUG] %s: no operation\n", __func__);
	}

	return size;
}

/*
 * This will use to enable/disable or check the status of ALPM
 * * Description for STATUS_OR_EVENT_FLAG *
 *	1) ALPM_MODE_ON
 *	2) CHECK_CURRENT_STATUS
 *		-> Check current status
 *			that will return current status
 *			 like ALPM_MODE_ON or MODE_OFF
 *	3) CHECK_PREVIOUS_STATUS
 *		-> Check previous status that will return previous status like
 *			 ALPM_MODE_ON or MODE_OFF
 *	4) STORE_CURRENT_STATUS
 *		-> Store current status to previous status because that will use
 *			for next turn on sequence
 *	5) CLEAR_MODE_STATUS
 *		-> Clear current and previous status as MODE_OFF status
 *			 that can use with
 *	* Usage *
 *		Call function "alpm_status_func(STATUS_OR_EVENT_FLAG)"
 */
static u8 alpm_status_func(u8 flag)
{
	static u8 current_status;
	static u8 previous_status;
	u8 ret = 0;

	switch (flag) {
		case ALPM_MODE_ON:
			current_status = ALPM_MODE_ON;
			break;
		case MODE_OFF:
			current_status = MODE_OFF;
			break;
		case CHECK_CURRENT_STATUS:
			ret = current_status;
			break;
		case CHECK_PREVIOUS_STATUS:
			ret = previous_status;
			break;
		case STORE_CURRENT_STATUS:
			previous_status = current_status;
			break;
		case CLEAR_MODE_STATUS:
			previous_status = 0;
			current_status = 0;
			break;
		default:
			break;
	}

	pr_debug("[ALPM_DEBUG] current_status: %d, previous_status: %d, ret: %d\n",
			current_status, previous_status, ret);

	return ret;
}

static DEVICE_ATTR(alpm, S_IRUGO | S_IWUSR | S_IWGRP,
		mdss_samsung_alpm_show,
		mdss_samsung_alpm_store);
#endif

static struct attribute *panel_sysfs_attributes[] = {
	&dev_attr_lcd_power.attr,
	&dev_attr_siop_enable.attr,
	&dev_attr_lcd_type.attr,
#if defined(ALPM_MODE)
	&dev_attr_alpm.attr,
#endif
	&dev_attr_ambient.attr,
	NULL
};
static const struct attribute_group panel_sysfs_group = {
	.attrs = panel_sysfs_attributes,
};

#if defined(TE_DEBUG)
static irqreturn_t samsung_te_check_handler(int irq, void *handle)
{
	pr_info("%s: HW VSYNC\n", __func__);

	return IRQ_HANDLED;
}
#endif

size_t kvaddr_to_paddr(unsigned long vaddr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	size_t paddr;

	pgd = pgd_offset_k(vaddr);
	if (unlikely(pgd_none(*pgd) || pgd_bad(*pgd)))
		return 0;

	pud = pud_offset(pgd, vaddr);
	if (unlikely(pud_none(*pud) || pud_bad(*pud)))
		return 0;

	pmd = pmd_offset(pud, vaddr);
	if (unlikely(pmd_none(*pmd) || pmd_bad(*pmd)))
		return 0;

	pte = pte_offset_kernel(pmd, vaddr);
	if (!pte_present(*pte))
		return 0;

	paddr = (unsigned long)pte_pfn(*pte) << PAGE_SHIFT;
	paddr += (vaddr & (PAGE_SIZE - 1));

	return paddr;
}

void mdss_samsung_dump_regs(void)
{
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	char name[32];
	int loop;

	snprintf(name, sizeof(name), "MDP BASE");
	pr_err("=============%s 0x%08zx ==============\n", name,
			kvaddr_to_paddr((unsigned long)mdata->mdss_io.base));
	mdss_dump_reg(mdata->mdss_io.base, 0x100);

	snprintf(name, sizeof(name), "MDP REG");
	pr_err("=============%s 0x%08zx ==============\n", name,
			kvaddr_to_paddr((unsigned long)mdata->mdp_base));
	mdss_dump_reg(mdata->mdp_base, 0x500);

	for (loop = 0; loop < mdata->nctl; loop++) {
		snprintf(name, sizeof(name), "CTRL%d", loop);
		pr_err("=============%s 0x%08zx ==============\n", name,
				kvaddr_to_paddr((unsigned long)mdata->ctl_off[loop].base));
		mdss_dump_reg(mdata->ctl_off[loop].base, 0x200);
	}

	for (loop = 0; loop < mdata->nvig_pipes; loop++) {
		snprintf(name, sizeof(name), "VG%d", loop);
		pr_err("=============%s 0x%08zx ==============\n", name,
				kvaddr_to_paddr((unsigned long)mdata->vig_pipes[loop].base));
		mdss_dump_reg(mdata->vig_pipes[loop].base, 0x100);
	}

	for (loop = 0; loop < mdata->nrgb_pipes; loop++) {
		snprintf(name, sizeof(name), "RGB%d", loop);
		pr_err("=============%s 0x%08zx ==============\n", name,
				kvaddr_to_paddr((unsigned long)mdata->rgb_pipes[loop].base));
		mdss_dump_reg(mdata->rgb_pipes[loop].base, 0x100);
	}

	for (loop = 0; loop < mdata->ndma_pipes; loop++) {
		snprintf(name, sizeof(name), "DMA%d", loop);
		pr_err("=============%s 0x%08zx ==============\n", name,
				kvaddr_to_paddr((unsigned long)mdata->dma_pipes[loop].base));
		mdss_dump_reg(mdata->dma_pipes[loop].base, 0x100);
	}

	for (loop = 0; loop < mdata->nmixers_intf; loop++) {
		snprintf(name, sizeof(name), "MIXER_INTF_%d", loop);
		pr_err("=============%s 0x%08zx ==============\n", name,
				kvaddr_to_paddr((unsigned long)mdata->mixer_intf[loop].base));
		mdss_dump_reg(mdata->mixer_intf[loop].base, 0x100);
	}

	for (loop = 0; loop < mdata->nmixers_wb; loop++) {
		snprintf(name, sizeof(name), "MIXER_WB_%d", loop);
		pr_err("=============%s 0x%08zx ==============\n", name,
				kvaddr_to_paddr((unsigned long)mdata->mixer_wb[loop].base));
		mdss_dump_reg(mdata->mixer_wb[loop].base, 0x100);
	}

	for (loop = 0; loop < mdata->nmixers_intf; loop++) {
		snprintf(name, sizeof(name), "PING_PONG%d", loop);
		pr_err("=============%s 0x%08zx ==============\n", name,
				kvaddr_to_paddr((unsigned long)mdata->mixer_intf[loop].pingpong_base));
		mdss_dump_reg(mdata->mixer_intf[loop].pingpong_base, 0x40);
	}
}

void mdss_samsung_dsi_dump_regs(struct mdss_panel_data *pdata, int dsi_num)
{
	struct mdss_samsung_driver_data *msd =
		(struct mdss_samsung_driver_data *)pdata->panel_private;
	struct mdss_dsi_ctrl_pdata **dsi_ctrl = &msd->ctrl_pdata;
	char name[32];

	snprintf(name, sizeof(name), "DSI%d CTL", dsi_num);
	pr_err("=============%s 0x%08zx ==============\n", name,
			kvaddr_to_paddr((unsigned long)dsi_ctrl[dsi_num]->ctrl_io.base));
	mdss_dump_reg((char *)dsi_ctrl[dsi_num]->ctrl_io.base, dsi_ctrl[dsi_num]->ctrl_io.len);

	snprintf(name, sizeof(name), "DSI%d PHY", dsi_num);
	pr_err("=============%s 0x%08zx ==============\n", name,
			kvaddr_to_paddr((unsigned long)dsi_ctrl[dsi_num]->phy_io.base));
	mdss_dump_reg((char *)dsi_ctrl[dsi_num]->phy_io.base, (size_t)dsi_ctrl[dsi_num]->phy_io.len);

	if (!msd->dump_info[dsi_num].dsi_pll.virtual_addr)
		msd->dump_info[dsi_num].dsi_pll.virtual_addr = get_mdss_dsi_base();
	snprintf(name, sizeof(name), "DSI%d PLL", dsi_num);
	pr_err("=============%s 0x%08zx ==============\n", name,
			kvaddr_to_paddr((unsigned long)msd->dump_info[dsi_num].dsi_pll.virtual_addr));
	mdss_dump_reg((char *)msd->dump_info[dsi_num].dsi_pll.virtual_addr, 0x200);
}

void mdss_samsung_dsi_te_check(struct mdss_panel_data *pdata)
{
	int rc, te_count = 0;
	int te_max = 20000; /*samspling 200ms */

	if (gpio_is_valid(disp_te_gpio)) {
		pr_err(" ============ start waiting for TE ============\n");

		for (te_count = 0;  te_count < te_max; te_count++) {
			rc = gpio_get_value(disp_te_gpio);
			if (rc == 1) {
				pr_err("%s: gpio_get_value(disp_te_gpio) = %d ",
						__func__, rc);
				pr_err("te_count = %d\n", te_count);
				break;
			}
			/* usleep suspends the calling thread whereas udelay is a
			 * busy wait. Here the value of te_gpio is checked in a loop of
			 * max count = 250. If this loop has to iterate multiple
			 * times before the te_gpio is 1, the calling thread will end
			 * up in suspend/wakeup sequence multiple times if usleep is
			 * used, which is an overhead. So use udelay instead of usleep.
			 */
			udelay(10);
		}

		if (te_count == te_max) {
			pr_err("LDI doesn't generate TE\n");
		} else
			pr_err("LDI generate TE\n");

		pr_err(" ============ finish waiting for TE ============\n");
	} else
		pr_err("disp_te_gpio is not valid\n");
}

int mdss_dsi_panel_init(struct device_node *node,
		struct mdss_dsi_ctrl_pdata *ctrl_pdata,
		bool cmd_cfg_cont_splash)
{
	int rc = 0;
	static const char *panel_name;
	struct mdss_panel_info *pinfo;
#if defined(CONFIG_LCD_CLASS_DEVICE)
	struct lcd_device *lcd_device;
#if defined(CONFIG_BACKLIGHT_CLASS_DEVICE)
	struct backlight_device *bd = NULL;
#endif
#endif
#if defined(CONFIG_LCD_CLASS_DEVICE)
	struct device_node *np = NULL;
	struct platform_device *pdev = NULL;
	static struct mdss_samsung_driver_data msd;
	np = of_parse_phandle(node,
			"qcom,mdss-dsi-panel-controller", 0);
	if (!np) {
		pr_err("%s: Dsi controller node not initialized\n", __func__);
		return -EPROBE_DEFER;
	}

	pdev = of_find_device_by_node(np);
#endif

	ctrl_pdata->panel_data.panel_private = &msd;
	mutex_init(&msd.lock);
	if (!node || !ctrl_pdata) {
		pr_err("%s: Invalid arguments\n", __func__);
		return -ENODEV;
	}

	pinfo = &ctrl_pdata->panel_data.panel_info;

	pr_debug("%s:%d\n", __func__, __LINE__);
	panel_name = of_get_property(node, "qcom,mdss-dsi-panel-name", NULL);
	if (!panel_name)
		pr_info("%s:%d, Panel name not specified\n",
				__func__, __LINE__);
	else
		pr_info("%s: Panel Name = %s\n", __func__, panel_name);

	rc = mdss_panel_parse_dt(node, ctrl_pdata);
	if (rc) {
		pr_err("%s:%d panel dt parse failed\n", __func__, __LINE__);
		return rc;
	}

	if (!cmd_cfg_cont_splash)
		pinfo->cont_splash_enabled = false;
	pr_info("%s: Continuous splash %s\n", __func__,
			pinfo->cont_splash_enabled ? "enabled" : "disabled");

	ctrl_pdata->on = mdss_dsi_panel_on;
	ctrl_pdata->off = mdss_dsi_panel_off;
	ctrl_pdata->low_power_config = mdss_dsi_panel_low_power_config;
	ctrl_pdata->panel_data.set_backlight = mdss_dsi_panel_bl_ctrl;
	ctrl_pdata->switch_mode = mdss_dsi_panel_switch_mode;
	ctrl_pdata->event_handler = samsung_dsi_panel_event_handler;
#if defined(CONFIG_FB_MSM_MDSS_PANEL_ALWAYS_ON)
	ctrl_pdata->panel_data.send_alpm = mdss_dsi_panel_alpm_ctrl;
#endif
	ctrl_pdata->panel_reset = mdss_dsi_panel_reset;
	ctrl_pdata->registered = mdss_dsi_panel_registered;
	ctrl_pdata->panel_data.alpm_data.alpm_status = alpm_status_func;
#if defined(CONFIG_LCD_CLASS_DEVICE)
	lcd_device = lcd_device_register("panel", &pdev->dev, ctrl_pdata,
			&mdss_dsi_disp_props);

	if (IS_ERR(lcd_device)) {
		rc = PTR_ERR(lcd_device);
		pr_info("lcd : failed to register device\n");
		return rc;
	}

	sysfs_remove_file(&lcd_device->dev.kobj, &dev_attr_lcd_power.attr);

	rc = sysfs_create_group(&lcd_device->dev.kobj, &panel_sysfs_group);
	if (rc) {
		pr_err("Failed to create panel sysfs group..\n");
		sysfs_remove_group(&lcd_device->dev.kobj, &panel_sysfs_group);
		return rc;
	}

#if defined(CONFIG_BACKLIGHT_CLASS_DEVICE)
	bd = backlight_device_register("panel", &lcd_device->dev,
			NULL, NULL, NULL);
	if (IS_ERR(bd)) {
		rc = PTR_ERR(bd);
		pr_info("backlight : failed to register device\n");
		return rc;
	}
#endif
#endif

	msd.msm_pdev = pdev;
	msd.dstat.on = 0;
	if (pinfo->cont_splash_enabled)
		msd.dstat.on = 1;

	disp_esd_gpio = of_get_named_gpio(node, "qcom,esd-det-gpio", 0);
	rc = gpio_request(disp_esd_gpio, "err_fg");
	if (rc) {
		pr_err("request gpio GPIO_ESD failed, ret=%d\n", rc);
		gpio_free(disp_esd_gpio);
		return rc;
	}
	gpio_tlmm_config(GPIO_CFG(disp_esd_gpio, 0, GPIO_CFG_INPUT,
				GPIO_CFG_NO_PULL, GPIO_CFG_2MA), GPIO_CFG_ENABLE);
	rc = gpio_direction_input(disp_esd_gpio);
	if (unlikely(rc < 0)) {
		pr_err("%s: failed to set gpio %d as input (%d)\n",
				__func__, disp_esd_gpio, rc);
	}

	disp_te_gpio = of_get_named_gpio(node, "qcom,te-gpio", 0);

#if defined(TE_DEBUG)
	rc = request_threaded_irq(
			gpio_to_irq(disp_te_gpio),
			samsung_te_check_handler,
			NULL,
			IRQF_TRIGGER_FALLING,
			"VSYNC_GPIO",
			(void *)0);
	if (rc)
		pr_err("%s : Failed to request_irq, ret=%d\n",
				__func__, rc);
#endif

	return 0;
}
