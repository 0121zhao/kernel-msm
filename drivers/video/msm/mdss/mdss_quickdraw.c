/* Copyright (c) 2014, Motorola Mobility, LLC.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/msm_mdp.h>
#include <linux/fb_quickdraw.h>
#include <linux/fb_quickdraw_ops.h>
#include <linux/vmalloc.h>

#include "mdss.h"
#include "mdss_dsi.h"
#include "mdss_fb.h"
#include "mdss_mdp.h"

struct mdss_quickdraw_buffer {
	struct fb_quickdraw_buffer buffer;
	int overlay_id;
};

static struct mdss_quickdraw_buffer *active_mdss_buffer;

/* Quickdraw Helper Functions */

static int set_overlay(struct msm_fb_data_type *mfd,
	struct mdss_quickdraw_buffer *mdss_buffer, int x, int y)
{
	int ret;
	struct mdp_overlay overlay;
	struct fb_quickdraw_buffer *buffer;

	pr_debug("%s+: (mdss_buffer: %p)\n", __func__, mdss_buffer);

	if (!mdss_buffer) {
		pr_err("%s: mdss_buffer is NULL\n", __func__);
		ret = -EINVAL;
		goto exit;
	}

	buffer = &mdss_buffer->buffer;

	memset(&overlay, 0, sizeof(struct mdp_overlay));
	overlay.src.width  = buffer->data.w;
	overlay.src.height = buffer->data.h;
	overlay.src.format = buffer->data.format;
	overlay.src_rect.x = 0;
	overlay.src_rect.y = 0;
	overlay.src_rect.w = buffer->data.w;
	overlay.src_rect.h = buffer->data.h;
	overlay.dst_rect.x = x;
	overlay.dst_rect.y = y;
	overlay.dst_rect.w = buffer->data.w;
	overlay.dst_rect.h = buffer->data.h;
	overlay.z_order = 0;
	overlay.alpha = 0xff;
	overlay.flags = 0;
	overlay.is_fg = 0;
	overlay.id = mdss_buffer->overlay_id;

	ret = mdss_mdp_overlay_set(mfd, &overlay);
	if (ret) {
		pr_err("%s: error setting overlay for buffer %d\n", __func__,
			buffer->data.buffer_id);
		goto exit;
	}

	mdss_buffer->overlay_id = overlay.id;
exit:
	pr_debug("%s-: (mdss_buffer: %p) (ret: %d)\n", __func__, mdss_buffer,
		ret);

	return ret;
}

static int unset_overlay(struct msm_fb_data_type *mfd,
	struct mdss_quickdraw_buffer *mdss_buffer)
{
	int ret = -EINVAL;
	struct fb_quickdraw_buffer *buffer;

	pr_debug("%s+: (mdss_buffer: %p)\n", __func__, mdss_buffer);

	if (!mdss_buffer) {
		pr_err("%s: mdss_buffer is NULL\n", __func__);
		goto exit;
	}

	buffer = &mdss_buffer->buffer;

	/* The erase buffer doesnt have a file,
		so it doesnt use overlays */
	if (!buffer->file) {
		ret = 0;
		goto exit;
	}

	if (mdss_buffer->overlay_id != MSMFB_NEW_REQUEST) {
		ret = mdss_mdp_overlay_unset(mfd, mdss_buffer->overlay_id);
		mdss_buffer->overlay_id = MSMFB_NEW_REQUEST;
	} else
		pr_err("%s: invalid buffer (overlay_id = MSMFB_NEW_REQUEST)!\n",
			__func__);

exit:
	pr_debug("%s-: (mdss_buffer: %p) (ret: %d)\n", __func__, mdss_buffer,
		ret);

	return ret;
}

static int play_overlay(struct msm_fb_data_type *mfd,
	struct mdss_quickdraw_buffer *mdss_buffer)
{
	int ret = -EINVAL;
	struct msmfb_overlay_data ovdata;
	struct fb_quickdraw_buffer *buffer;

	pr_debug("%s+: (mdss_buffer: %p)\n", __func__, mdss_buffer);

	if (!mdss_buffer) {
		pr_err("%s: mdss_buffer is NULL\n", __func__);
		goto exit;
	}

	buffer = &mdss_buffer->buffer;

	ovdata.id = mdss_buffer->overlay_id;
	ovdata.data.flags = 0;
	ovdata.data.offset = 0;
	ovdata.data.memory_id = buffer->mem_fd;

	ret = mdss_mdp_overlay_play(mfd, &ovdata);
exit:
	pr_debug("%s-: (mdss_buffer: %p) (ret: %d)\n", __func__, mdss_buffer,
		ret);
	return ret;
}

static void mdss_dsi_cmd_mdp_busy_wait(struct msm_fb_data_type *mfd)
{
	struct mdss_panel_data *pdata;
	struct mdss_dsi_ctrl_pdata *ctrl;

	pr_debug("%s+\n", __func__);

	pdata = dev_get_platdata(&mfd->pdev->dev);
	if (!pdata) {
		pr_err("%s: Panel data not available\n", __func__);
		return;
	}

	ctrl = container_of(pdata, struct mdss_dsi_ctrl_pdata,
				panel_data);

	mdss_dsi_cmd_mdp_busy(ctrl);

	pr_debug("%s-\n", __func__);
}

/* Quickdraw External Interface */

static struct fb_quickdraw_buffer *mdss_quickdraw_alloc_buffer(void *data,
	struct fb_quickdraw_buffer_data *buffer_data)
{
	struct fb_quickdraw_buffer *buffer;

	pr_debug("%s+\n", __func__);

	buffer = fb_quickdraw_alloc_buffer(buffer_data,
		sizeof(struct mdss_quickdraw_buffer));
	if (buffer) {
		struct mdss_quickdraw_buffer *mdss_buffer = container_of(buffer,
			struct mdss_quickdraw_buffer, buffer);
		mdss_buffer->overlay_id = MSMFB_NEW_REQUEST;
	}

	pr_debug("%s- (buffer: %p)\n", __func__, buffer);

	return buffer;
}

static int mdss_quickdraw_delete_buffer(void *data,
	struct fb_quickdraw_buffer *buffer)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)(data);
	struct mdss_quickdraw_buffer *mdss_buffer;
	int ret = 0;

	pr_debug("%s+\n", __func__);

	if (!buffer) {
		pr_err("%s: buffer is NULL\n", __func__);
		ret = -EINVAL;
		goto exit;
	}

	mdss_buffer = container_of(buffer, struct mdss_quickdraw_buffer,
		buffer);
	if (mdss_buffer->overlay_id != MSMFB_NEW_REQUEST)
		ret = unset_overlay(mfd, mdss_buffer);

exit:
	pr_debug("%s- (ret: %d)\n", __func__, ret);

	return ret;
}

static int mdss_quickdraw_prepare(void *data, unsigned char panel_state)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)(data);
	struct mdss_mdp_ctl *ctl = NULL;
	int ret = 0;

	pr_debug("%s+\n", __func__);

	mfd->quickdraw_panel_state = panel_state;
	mfd->quickdraw_in_progress = 1;

	mfd->quickdraw_fb_prepare(mfd);

	ctl = mfd_to_ctl(mfd);
	memset(&ctl->roi, 0, sizeof(ctl->roi));

	pr_debug("%s- (ret: %d)\n", __func__, ret);

	return ret;
}

static int mdss_quickdraw_execute(void *data,
	struct fb_quickdraw_buffer *buffer, int x, int y)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)(data);
	struct mdp_display_commit prim_commit;
	struct mdss_quickdraw_buffer *mdss_buffer;
	int w, h;
	int ret = 0;

	pr_debug("%s+\n", __func__);

	ret = fb_quickdraw_get_buffer(buffer);
	if (ret != 0) {
		pr_err("%s: Unable to use buffer [%p] [err: %d]\n", __func__,
			buffer, ret);
		ret = -EINVAL;
		goto exit;
	}

	mdss_buffer = container_of(buffer, struct mdss_quickdraw_buffer,
		buffer);

	if (x == COORD_NO_OVERRIDE)
		x = buffer->data.x;
	if (y == COORD_NO_OVERRIDE)
		y = buffer->data.y;

	w = buffer->data.w;
	h = buffer->data.h;

	if (x < 0 || y < 0 || w <= 0 || h <= 0 ||
	    (x + w) > mfd->panel_info->xres ||
	    (y + h) > mfd->panel_info->yres) {
		pr_err("%s: Invalid coordinates [x:%d y:%d w:%d h:%d]\n",
			__func__, x, y, w, h);
		ret = -EINVAL;
		fb_quickdraw_put_buffer(buffer);
		goto exit;
	}

	/* Make sure dsi link is idle */
	mdss_dsi_cmd_mdp_busy_wait(mfd);

	/* Unlock previous buffer */
	if (active_mdss_buffer)
		fb_quickdraw_unlock_buffer(&active_mdss_buffer->buffer);

	fb_quickdraw_lock_buffer(buffer);

	if (buffer->file) {
		ret = set_overlay(mfd, mdss_buffer, x, y);

		if (!ret)
			ret = play_overlay(mfd, mdss_buffer);
	}

	/* Unset the previous overlay now that we have a new pipe */
	if (active_mdss_buffer) {
		if (active_mdss_buffer != mdss_buffer)
			unset_overlay(mfd, active_mdss_buffer);
		/* Free the previous buffer if we're done with it */
		fb_quickdraw_put_buffer(&active_mdss_buffer->buffer);
	}
	active_mdss_buffer = mdss_buffer;

	/* If we had errors earlier, we need to cleanup */
	if (ret) {
		pr_err("%s: error setting up overlay, cleanup\n", __func__);
		fb_quickdraw_unlock_buffer(buffer);
		fb_quickdraw_put_buffer(buffer);
		active_mdss_buffer = NULL;
		goto exit;
	}

	memset(&prim_commit, 0, sizeof(struct mdp_display_commit));
	prim_commit.flags = MDP_DISPLAY_COMMIT_OVERLAY;
	prim_commit.wait_for_finish = 1;
	prim_commit.roi.x = x;
	prim_commit.roi.y = y;
	prim_commit.roi.w = w;
	prim_commit.roi.h = h;

	mdss_fb_pan_display_ex(mfd->fbi, &prim_commit);

exit:
	pr_debug("%s- (ret: %d)\n", __func__, ret);

	return ret;
}

static int mdss_quickdraw_erase(void *data, int x1, int y1, int x2, int y2)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)(data);
	struct fb_quickdraw_buffer_data buffer_data;
	struct fb_quickdraw_buffer *buffer = NULL;
	int w = x2 - x1;
	int h = y2 - y1;
	int ret;

	pr_debug("%s+\n", __func__);

	if (x1 < 0 || y1 < 0 || w <= 0 || h <= 0 ||
		(x1 + w) > mfd->panel_info->xres ||
		(y1 + h) > mfd->panel_info->yres) {
		pr_err("%s: Invalid coordinates [x1:%d y1:%d x2:%d y2:%d]\n",
			__func__, x1, y1, x2, y2);
		ret = -EINVAL;
		goto exit;
	}

	/* Allocate the special erase buffer */
	memset(&buffer_data, 0, sizeof(struct fb_quickdraw_buffer_data));
	buffer = mdss_quickdraw_alloc_buffer(data, &buffer_data);
	if (!buffer) {
		pr_err("%s: Unable to allocate erase buffer\n", __func__);
		ret = -EINVAL;
		goto exit;
	}

	buffer->data.x = x1;
	buffer->data.y = y1;
	buffer->data.w = w;
	buffer->data.h = h;

	ret = mdss_quickdraw_execute(data, buffer, COORD_NO_OVERRIDE,
		COORD_NO_OVERRIDE);

	/* release our reference so that when the display is done with this
	   buffer it gets cleaned up immediately */
	fb_quickdraw_put_buffer(buffer);

exit:
	pr_debug("%s- (ret: %d)\n", __func__, ret);

	return ret;
}

static int mdss_quickdraw_cleanup(void *data)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)(data);
	int ret = 0;

	pr_debug("%s+\n", __func__);

	/* Make sure dsi link is idle */
	mdss_dsi_cmd_mdp_busy_wait(mfd);

	/* Free the last used buffer */
	if (active_mdss_buffer) {
		unset_overlay(mfd, active_mdss_buffer);
		fb_quickdraw_unlock_buffer(&active_mdss_buffer->buffer);
		fb_quickdraw_put_buffer(&active_mdss_buffer->buffer);
	}
	active_mdss_buffer = NULL;

	mdss_mdp_overlay_cleanup(mfd);

	mfd->quickdraw_fb_cleanup(mfd);

	mfd->quickdraw_in_progress = 0;

	pr_debug("%s- (ret: %d)\n", __func__, ret);

	return ret;
}

static struct fb_quickdraw_ops mdss_quickdraw_ops = {
	.prepare = mdss_quickdraw_prepare,
	.execute = mdss_quickdraw_execute,
	.erase   = mdss_quickdraw_erase,
	.cleanup = mdss_quickdraw_cleanup,
	.alloc_buffer = mdss_quickdraw_alloc_buffer,
	.delete_buffer = mdss_quickdraw_delete_buffer,
};

void mdss_quickdraw_register(struct msm_fb_data_type *mfd)
{
	BUG_ON(!mfd);

	mdss_quickdraw_ops.data = (void *)mfd;
	fb_quickdraw_register_ops(&mdss_quickdraw_ops);
}
