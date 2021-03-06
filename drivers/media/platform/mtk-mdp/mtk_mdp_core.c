/*
 * Copyright (c) 2015-2016 MediaTek Inc.
 * Author: Houlong Wei <houlong.wei@mediatek.com>
 *         Ming Hsiu Tsai <minghsiu.tsai@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/workqueue.h>
#include <soc/mediatek/smi.h>

#include "mtk_mdp_core.h"
#include "mtk_vpu.h"

int mtk_mdp_dbg_level;
EXPORT_SYMBOL(mtk_mdp_dbg_level);

module_param(mtk_mdp_dbg_level, int, S_IRUGO | S_IWUSR);

static const struct mtk_mdp_fmt mtk_mdp_formats[] = {
	{
		.name		= "YUV 4:2:0 mediatek block mode. 2p, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_MT21,
		.depth		= { 8, 4 },
		.color		= MTK_MDP_YUV420,
		.num_planes	= 2,
		.num_comp	= 2,
		.flags		= MTK_MDP_FMT_FLAG_OUTPUT,
	}, {
		.name		= "YUV 4:2:0 non-contig. 2p, Y/CbCr",
		.pixelformat	= V4L2_PIX_FMT_NV12M,
		.depth		= { 8, 4 },
		.color		= MTK_MDP_YUV420,
		.num_planes	= 2,
		.num_comp	= 2,
		.flags		= MTK_MDP_FMT_FLAG_OUTPUT |
				  MTK_MDP_FMT_FLAG_CAPTURE,
	}, {
		.name		= "YUV 4:2:0 non-contig. 3p, Y/Cb/Cr",
		.pixelformat	= V4L2_PIX_FMT_YUV420M,
		.depth		= { 8, 2, 2 },
		.color		= MTK_MDP_YUV420,
		.num_planes	= 3,
		.num_comp	= 3,
		.flags		= MTK_MDP_FMT_FLAG_OUTPUT |
				  MTK_MDP_FMT_FLAG_CAPTURE,
	}
};

static inline struct mtk_mdp_ctx *ctrl_to_ctx(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct mtk_mdp_ctx, ctrl_handler);
}

const struct mtk_mdp_fmt *mtk_mdp_find_fmt(u32 *pixelformat, u32 index,
					   u32 type)
{
	const struct mtk_mdp_fmt *fmt, *def_fmt = NULL;
	u32 i, flag, num = 0;

	if (index >= ARRAY_SIZE(mtk_mdp_formats))
		return NULL;

	flag = V4L2_TYPE_IS_OUTPUT(type) ? MTK_MDP_FMT_FLAG_OUTPUT :
					   MTK_MDP_FMT_FLAG_CAPTURE;
	for (i = 0; i < ARRAY_SIZE(mtk_mdp_formats); ++i) {
		fmt = &mtk_mdp_formats[i];
		if (!(fmt->flags & flag))
			continue;
		if (pixelformat && fmt->pixelformat == *pixelformat)
			return fmt;
		if (index == num)
			def_fmt = fmt;
		num++;
	}
	return def_fmt;
}

void mtk_mdp_set_frame_size(struct mtk_mdp_frame *frame, int width, int height)
{
	frame->f_width	= width;
	frame->f_height	= height;
	frame->crop.width = width;
	frame->crop.height = height;
	frame->crop.left = 0;
	frame->crop.top = 0;
}

int mtk_mdp_enum_fmt_mplane(struct v4l2_fmtdesc *f, u32 type)
{
	const struct mtk_mdp_fmt *fmt;

	fmt = mtk_mdp_find_fmt(NULL, f->index, type);
	if (!fmt)
		return -EINVAL;

	strlcpy(f->description, fmt->name, sizeof(f->description));
	f->pixelformat = fmt->pixelformat;

	return 0;
}

static void mtk_mdp_bound_align_image(u32 *w, unsigned int wmin,
				      unsigned int wmax, unsigned int walign,
				      u32 *h, unsigned int hmin,
				      unsigned int hmax, unsigned int halign)
{
	int width, height, w_step, h_step;

	width = *w;
	height = *h;
	w_step = 1 << walign;
	h_step = 1 << halign;

	v4l_bound_align_image(w, wmin, wmax, walign, h, hmin, hmax, halign, 0);
	if (*w < width && (*w + w_step) <= wmax)
		*w += w_step;
	if (*h < height && (*h + h_step) <= hmax)
		*h += h_step;
}

int mtk_mdp_try_fmt_mplane(struct mtk_mdp_ctx *ctx, struct v4l2_format *f)
{
	struct mtk_mdp_dev *mdp = ctx->mdp_dev;
	struct mtk_mdp_variant *variant = mdp->variant;
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct mtk_mdp_fmt *fmt;
	u32 max_w, max_h, mod_x, mod_y;
	u32 min_w, min_h, tmp_w, tmp_h;
	int i;

	mtk_mdp_dbg(2, "type:%d, wxh:%dx%d", f->type, pix_mp->width,
		    pix_mp->height);

	fmt = mtk_mdp_find_fmt(&pix_mp->pixelformat, 0, f->type);
	if (!fmt) {
		dev_err(&ctx->mdp_dev->pdev->dev,
			"pixelformat format 0x%X invalid\n",
			pix_mp->pixelformat);
		return -EINVAL;
	}

	if (pix_mp->field == V4L2_FIELD_ANY)
		pix_mp->field = V4L2_FIELD_NONE;
	else if (pix_mp->field != V4L2_FIELD_NONE) {
		dev_err(&ctx->mdp_dev->pdev->dev,
			"Not supported field order %d\n",
			pix_mp->field);
		return -EINVAL;
	}

	max_w = variant->pix_max->target_rot_dis_w;
	max_h = variant->pix_max->target_rot_dis_h;

	mod_x = ffs(variant->pix_align->org_w) - 1;
	if (is_yuv420(fmt->color))
		mod_y = ffs(variant->pix_align->org_h) - 1;
	else
		mod_y = ffs(variant->pix_align->org_h) - 2;

	if (V4L2_TYPE_IS_OUTPUT(f->type)) {
		min_w = variant->pix_min->org_w;
		min_h = variant->pix_min->org_h;
	} else {
		min_w = variant->pix_min->target_rot_dis_w;
		min_h = variant->pix_min->target_rot_dis_h;
	}

	mtk_mdp_dbg(2, "mod x,y:%d,%d, max:%dx%d", mod_x, mod_y, max_w,
		    max_h);
	/*
	 * To check if image size is modified to adjust parameter against
	 * hardware abilities
	 */
	tmp_w = pix_mp->width;
	tmp_h = pix_mp->height;

	mtk_mdp_bound_align_image(&pix_mp->width, min_w, max_w, mod_x,
				  &pix_mp->height, min_h, max_h, mod_y);

	if (tmp_w != pix_mp->width || tmp_h != pix_mp->height)
		mtk_mdp_dbg(1, "size change:%dx%d to %dx%d", tmp_w, tmp_h,
			    pix_mp->width, pix_mp->height);
	pix_mp->num_planes = fmt->num_planes;

	if (pix_mp->width >= 1280) /* HD */
		pix_mp->colorspace = V4L2_COLORSPACE_REC709;
	else /* SD */
		pix_mp->colorspace = V4L2_COLORSPACE_SMPTE170M;

	for (i = 0; i < pix_mp->num_planes; ++i) {
		int bpl = (pix_mp->width * fmt->depth[i]) >> 3;
		int sizeimage = bpl * pix_mp->height;

		pix_mp->plane_fmt[i].bytesperline = bpl;
		if (pix_mp->plane_fmt[i].sizeimage < sizeimage)
			pix_mp->plane_fmt[i].sizeimage = sizeimage;
		mtk_mdp_dbg(2, "[%d] bpl:%d, sizeimage:%d", i, bpl,
			    pix_mp->plane_fmt[i].sizeimage);
	}

	return 0;
}

int mtk_mdp_g_fmt_mplane(struct mtk_mdp_ctx *ctx, struct v4l2_format *f)
{
	struct mtk_mdp_frame *frame;
	struct v4l2_pix_format_mplane *pix_mp;
	int i;

	mtk_mdp_dbg(2, "type:%d", f->type);

	frame = mtk_mdp_ctx_get_frame(ctx, f->type);
	if (IS_ERR(frame))
		return PTR_ERR(frame);

	pix_mp = &f->fmt.pix_mp;

	pix_mp->width		= frame->f_width;
	pix_mp->height		= frame->f_height;
	pix_mp->field		= V4L2_FIELD_NONE;
	pix_mp->pixelformat	= frame->fmt->pixelformat;
	pix_mp->colorspace	= V4L2_COLORSPACE_REC709;
	pix_mp->num_planes	= frame->fmt->num_planes;

	for (i = 0; i < pix_mp->num_planes; ++i) {
		pix_mp->plane_fmt[i].bytesperline = (frame->f_width *
			frame->fmt->depth[i]) / 8;
		pix_mp->plane_fmt[i].sizeimage =
			 pix_mp->plane_fmt[i].bytesperline * frame->f_height;

		mtk_mdp_dbg(2, "[%d] bpl:%d, sizeimage:%d", i,
			    pix_mp->plane_fmt[i].bytesperline,
			    pix_mp->plane_fmt[i].sizeimage);
	}

	return 0;
}

void mtk_mdp_check_crop_change(u32 tmp_w, u32 tmp_h, u32 *w, u32 *h)
{
	if (tmp_w != *w || tmp_h != *h) {
		mtk_mdp_dbg(1, "size change:%dx%d to %dx%d",
			    *w, *h, tmp_w, tmp_h);

		*w = tmp_w;
		*h = tmp_h;
	}
}

int mtk_mdp_try_crop(struct mtk_mdp_ctx *ctx, struct v4l2_crop *cr)
{
	struct mtk_mdp_frame *f;
	struct mtk_mdp_dev *mdp = ctx->mdp_dev;
	struct mtk_mdp_variant *variant = mdp->variant;
	u32 mod_x = 0, mod_y = 0, tmp_w, tmp_h;
	u32 min_w, min_h, max_w, max_h;

	if (cr->c.top < 0 || cr->c.left < 0) {
		dev_err(&ctx->mdp_dev->pdev->dev,
			"doesn't support negative values for top & left\n");
		return -EINVAL;
	}

	mtk_mdp_dbg(2, "set wxh:%dx%d", cr->c.width, cr->c.height);

	if (cr->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
		f = &ctx->d_frame;
	else if (cr->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
		f = &ctx->s_frame;
	else
		return -EINVAL;

	max_w = f->f_width;
	max_h = f->f_height;
	tmp_w = cr->c.width;
	tmp_h = cr->c.height;

	if (V4L2_TYPE_IS_OUTPUT(cr->type)) {
		if ((is_yuv422(f->fmt->color) && f->fmt->num_comp == 1) ||
		    is_rgb(f->fmt->color))
			min_w = 32;
		else
			min_w = 64;
		if ((is_yuv422(f->fmt->color) && f->fmt->num_comp == 3) ||
		    is_yuv420(f->fmt->color))
			min_h = 32;
		else
			min_h = 16;
	} else {
		if (is_yuv420(f->fmt->color) || is_yuv422(f->fmt->color))
			mod_x = ffs(variant->pix_align->target_w) - 1;
		if (is_yuv420(f->fmt->color))
			mod_y = ffs(variant->pix_align->target_h) - 1;
		if (ctx->ctrls.rotate->val == 90 ||
		    ctx->ctrls.rotate->val == 270) {
			max_w = f->f_height;
			max_h = f->f_width;
			min_w = variant->pix_min->target_rot_en_w;
			min_h = variant->pix_min->target_rot_en_h;
			tmp_w = cr->c.height;
			tmp_h = cr->c.width;
		} else {
			min_w = variant->pix_min->target_rot_dis_w;
			min_h = variant->pix_min->target_rot_dis_h;
		}
	}

	mtk_mdp_dbg(2, "mod x,y:%d,%d, min:%dx%d, tmp:%dx%d", mod_x, mod_y,
		    min_w, min_h, tmp_w, tmp_h);

	v4l_bound_align_image(&tmp_w, min_w, max_w, mod_x,
			      &tmp_h, min_h, max_h, mod_y, 0);

	if (!V4L2_TYPE_IS_OUTPUT(cr->type) &&
		(ctx->ctrls.rotate->val == 90 ||
		ctx->ctrls.rotate->val == 270))
		mtk_mdp_check_crop_change(tmp_h, tmp_w,
					&cr->c.width, &cr->c.height);
	else
		mtk_mdp_check_crop_change(tmp_w, tmp_h,
					&cr->c.width, &cr->c.height);

	/* adjust left/top if cropping rectangle is out of bounds */
	/* Need to add code to algin left value with 2's multiple */
	if (cr->c.left + tmp_w > max_w)
		cr->c.left = max_w - tmp_w;
	if (cr->c.top + tmp_h > max_h)
		cr->c.top = max_h - tmp_h;

	if ((is_yuv420(f->fmt->color) || is_yuv422(f->fmt->color)) &&
		cr->c.left & 1)
			cr->c.left -= 1;

	mtk_mdp_dbg(2, "align l,t,w,h:%d,%d,%d,%d, max:%dx%d",
		    cr->c.left, cr->c.top, cr->c.width,
		    cr->c.height, max_w, max_h);
	return 0;
}

int mtk_mdp_check_scaler_ratio(struct mtk_mdp_variant *var, int sw, int sh,
			       int dw, int dh, int rot)
{
	int tmp_w, tmp_h;

	if (rot == 90 || rot == 270) {
		tmp_w = dh;
		tmp_h = dw;
	} else {
		tmp_w = dw;
		tmp_h = dh;
	}

	if ((sw / tmp_w) > var->h_sc_down_max ||
	    (sh / tmp_h) > var->v_sc_down_max ||
	    (tmp_w / sw) > var->h_sc_up_max ||
	    (tmp_h / sh) > var->v_sc_up_max)
		return -EINVAL;

	return 0;
}

static int __mtk_mdp_s_ctrl(struct mtk_mdp_ctx *ctx, struct v4l2_ctrl *ctrl)
{
	struct mtk_mdp_dev *mdp = ctx->mdp_dev;
	struct mtk_mdp_variant *variant = mdp->variant;
	unsigned int flags = MTK_MDP_DST_FMT | MTK_MDP_SRC_FMT;
	int ret = 0;

	if (ctrl->flags & V4L2_CTRL_FLAG_INACTIVE)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		ctx->hflip = ctrl->val;
		break;
	case V4L2_CID_VFLIP:
		ctx->vflip = ctrl->val;
		break;
	case V4L2_CID_ROTATE:
		if ((ctx->state & flags) == flags) {
			ret = mtk_mdp_check_scaler_ratio(variant,
					ctx->s_frame.crop.width,
					ctx->s_frame.crop.height,
					ctx->d_frame.crop.width,
					ctx->d_frame.crop.height,
					ctx->ctrls.rotate->val);

			if (ret)
				return -EINVAL;
		}

		ctx->rotation = ctrl->val;
		break;
	case V4L2_CID_ALPHA_COMPONENT:
		ctx->d_frame.alpha = ctrl->val;
		break;
	}

	ctx->state |= MTK_MDP_PARAMS;
	return 0;
}

static int mtk_mdp_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct mtk_mdp_ctx *ctx = ctrl_to_ctx(ctrl);
	int ret;

	mutex_lock(&ctx->slock);
	ret = __mtk_mdp_s_ctrl(ctx, ctrl);
	mutex_unlock(&ctx->slock);

	return ret;
}

static const struct v4l2_ctrl_ops mtk_mdp_ctrl_ops = {
	.s_ctrl = mtk_mdp_s_ctrl,
};

int mtk_mdp_ctrls_create(struct mtk_mdp_ctx *ctx)
{
	if (ctx->ctrls_rdy) {
		dev_err(&ctx->mdp_dev->pdev->dev,
			"Control handler of this context was created already\n");
		return 0;
	}

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, MTK_MDP_MAX_CTRL_NUM);

	ctx->ctrls.rotate = v4l2_ctrl_new_std(&ctx->ctrl_handler,
			&mtk_mdp_ctrl_ops, V4L2_CID_ROTATE, 0, 270, 90, 0);
	ctx->ctrls.hflip = v4l2_ctrl_new_std(&ctx->ctrl_handler,
					     &mtk_mdp_ctrl_ops,
					     V4L2_CID_HFLIP,
					     0, 1, 1, 0);
	ctx->ctrls.vflip = v4l2_ctrl_new_std(&ctx->ctrl_handler,
					     &mtk_mdp_ctrl_ops,
					     V4L2_CID_VFLIP,
					     0, 1, 1, 0);
	ctx->ctrls.global_alpha = v4l2_ctrl_new_std(&ctx->ctrl_handler,
						    &mtk_mdp_ctrl_ops,
						    V4L2_CID_ALPHA_COMPONENT,
						    0, 255, 1, 0);
	ctx->ctrls_rdy = ctx->ctrl_handler.error == 0;

	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;

		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		dev_err(&ctx->mdp_dev->pdev->dev,
			"Failed to create G-Scaler control handlers\n");
		return err;
	}

	return 0;
}

void mtk_mdp_ctrls_delete(struct mtk_mdp_ctx *ctx)
{
	if (ctx->ctrls_rdy) {
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		ctx->ctrls_rdy = false;
	}
}

/* The color format (num_comp, num_planes) must be already configured. */
int mtk_mdp_prepare_addr(struct mtk_mdp_ctx *ctx, struct vb2_buffer *vb,
			 struct mtk_mdp_frame *frame, struct mtk_mdp_addr *addr)
{
	u32 pix_size;

	if ((vb == NULL) || (frame == NULL))
		return -EINVAL;

	pix_size = frame->f_width * frame->f_height;

	mtk_mdp_dbg(3, "planes:%d, comp:%d, size:%d",
		    frame->fmt->num_planes, frame->fmt->num_comp, pix_size);

	addr->y = vb2_dma_contig_plane_dma_addr(vb, 0);

	if (frame->fmt->num_planes == 1) {
		switch (frame->fmt->num_comp) {
		case 1:
			addr->cb = 0;
			addr->cr = 0;
			break;
		case 2:
			/* decompose Y into Y/Cb */
			addr->cb = (dma_addr_t)(addr->y + pix_size);
			addr->cr = 0;
			break;
		case 3:
			/* decompose Y into Y/Cb/Cr */
			addr->cb = (dma_addr_t)(addr->y + pix_size);
			if (MTK_MDP_YUV420 == frame->fmt->color)
				addr->cr = (dma_addr_t)(addr->cb
						+ (pix_size >> 2));
			else /* 422 */
				addr->cr = (dma_addr_t)(addr->cb
						+ (pix_size >> 1));
			break;
		default:
			dev_err(&ctx->mdp_dev->pdev->dev,
				"Invalid the number of color planes\n");
			return -EINVAL;
		}
	} else {
		if (frame->fmt->num_planes >= 2)
			addr->cb = vb2_dma_contig_plane_dma_addr(vb, 1);

		if (frame->fmt->num_planes == 3)
			addr->cr = vb2_dma_contig_plane_dma_addr(vb, 2);
	}

	mtk_mdp_dbg(3, "addr y,cb,cr:%p,%p,%p",
		    (void *)addr->y, (void *)addr->cb, (void *)addr->cr);

	return 0;
}

int mtk_mdp_process_done(void *priv, int vb_state)
{
	struct mtk_mdp_dev *mdp = priv;
	struct mtk_mdp_ctx *ctx;

	ctx = v4l2_m2m_get_curr_priv(mdp->m2m_dev);
	if (!ctx)
		return 0;
	mutex_lock(&ctx->slock);

	if (test_and_clear_bit(MTK_MDP_M2M_PEND, &mdp->state)) {
		if (test_and_clear_bit(MTK_MDP_M2M_SUSPENDING, &mdp->state)) {
			set_bit(MTK_MDP_M2M_SUSPENDED, &mdp->state);
			wake_up(&mdp->irq_queue);
			goto done_unlock;
		}

		mutex_unlock(&ctx->slock);
		mtk_mdp_m2m_job_finish(ctx, vb_state);

		/* wake_up job_abort, stop_streaming */
		if (ctx->state & MTK_MDP_CTX_STOP_REQ) {
			ctx->state &= ~MTK_MDP_CTX_STOP_REQ;
			wake_up(&mdp->irq_queue);
		}
		return 0;
	}

done_unlock:
	mutex_unlock(&ctx->slock);
	return 0;
}

static struct mtk_mdp_pix_max mtk_mdp_size_max = {
	.org_scaler_bypass_w	= 4096,
	.org_scaler_bypass_h	= 4096,
	.org_scaler_input_w	= 4096,
	.org_scaler_input_h	= 4096,
	.real_rot_dis_w		= 4096,
	.real_rot_dis_h		= 4096,
	.real_rot_en_w		= 4096,
	.real_rot_en_h		= 4096,
	.target_rot_dis_w	= 4096,
	.target_rot_dis_h	= 4096,
	.target_rot_en_w	= 4096,
	.target_rot_en_h	= 4096,
};

static struct mtk_mdp_pix_min mtk_mdp_size_min = {
	.org_w			= 16,
	.org_h			= 16,
	.real_w			= 16,
	.real_h			= 16,
	.target_rot_dis_w	= 16,
	.target_rot_dis_h	= 16,
	.target_rot_en_w	= 16,
	.target_rot_en_h	= 16,
};

static struct mtk_mdp_pix_align mtk_mdp_size_align = {
	.org_h			= 16,
	.org_w			= 16,
	.offset_h		= 2,
	.real_w			= 16,
	.real_h			= 16,
	.target_w		= 2,
	.target_h		= 2,
};

static struct mtk_mdp_variant mtk_mdp_default_variant = {
	.pix_max		= &mtk_mdp_size_max,
	.pix_min		= &mtk_mdp_size_min,
	.pix_align		= &mtk_mdp_size_align,
	.in_buf_cnt		= 32,
	.out_buf_cnt		= 32,
	.h_sc_up_max		= 32,
	.v_sc_up_max		= 32,
	.h_sc_down_max		= 32,
	.v_sc_down_max		= 128,
};

static const struct of_device_id mtk_mdp_comp_dt_ids[] = {
	{
		.compatible = "mediatek,mt8173-mdp-rdma",
		.data = (void *)MTK_MDP_RDMA
	}, {
		.compatible = "mediatek,mt8173-mdp-rsz",
		.data = (void *)MTK_MDP_RSZ
	}, {
		.compatible = "mediatek,mt8173-mdp-wdma",
		.data = (void *)MTK_MDP_WDMA
	}, {
		.compatible = "mediatek,mt8173-mdp-wrot",
		.data = (void *)MTK_MDP_WROT
	}
};

static const struct of_device_id mtk_mdp_of_ids[] = {
	{ .compatible = "mediatek,mt8173-mdp", },
	{ },
};
MODULE_DEVICE_TABLE(of, mtk_mdp_of_ids);

static void mtk_mdp_clock_on(struct mtk_mdp_dev *mdp)
{
	struct device *dev = &mdp->pdev->dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(mdp->comp); i++)
		mtk_mdp_comp_clock_on(dev, mdp->comp[i]);
}

static void mtk_mdp_clock_off(struct mtk_mdp_dev *mdp)
{
	struct device *dev = &mdp->pdev->dev;
	int i;

	for (i = 0; i < ARRAY_SIZE(mdp->comp); i++)
		mtk_mdp_comp_clock_off(dev, mdp->comp[i]);
}

static void mtk_wdt_worker(struct work_struct *work)
{
	struct mtk_mdp_dev *mdp =
			container_of(work, struct mtk_mdp_dev, wdt_work);
	struct mtk_mdp_ctx *ctx;

	mtk_mdp_dbg(0, "Watchdog timeout");

	list_for_each_entry(ctx, &mdp->ctx_list, list) {
		mtk_mdp_dbg(0, "[%d] Change as state error", ctx->id);
		mtk_mdp_ctx_error(ctx);
	}
}

static void mtk_mdp_reset_handler(void *priv)
{
	struct mtk_mdp_dev *mdp = priv;

	queue_work(mdp->wdt_wq, &mdp->wdt_work);
}

static int mtk_mdp_probe(struct platform_device *pdev)
{
	struct mtk_mdp_dev *mdp;
	struct device *dev = &pdev->dev;
	struct device_node *node;
	int i, ret = 0;

	mdp = devm_kzalloc(dev, sizeof(*mdp), GFP_KERNEL);
	if (!mdp)
		return -ENOMEM;

	mdp->id = pdev->id;
	mdp->variant = &mtk_mdp_default_variant;
	mdp->pdev = pdev;
	INIT_LIST_HEAD(&mdp->ctx_list);

	init_waitqueue_head(&mdp->irq_queue);
	mutex_init(&mdp->lock);
	mutex_init(&mdp->vpulock);

	/* Iterate over sibling MDP function blocks */
	for_each_child_of_node(dev->of_node->parent, node) {
		const struct of_device_id *of_id;
		enum mtk_mdp_comp_type comp_type;
		int comp_id;
		struct mtk_mdp_comp *comp;

		of_id = of_match_node(mtk_mdp_comp_dt_ids, node);
		if (!of_id)
			continue;

		if (!of_device_is_available(node)) {
			dev_err(dev, "Skipping disabled component %s\n",
				node->full_name);
			continue;
		}

		comp_type = (enum mtk_mdp_comp_type)of_id->data;
		comp_id = mtk_mdp_comp_get_id(node, comp_type);
		if (comp_id < 0) {
			dev_warn(dev, "Skipping unknown component %s\n",
				 node->full_name);
			continue;
		}

		comp = devm_kzalloc(dev, sizeof(*comp), GFP_KERNEL);
		if (!comp) {
			ret = -ENOMEM;
			goto err_comp;
		}
		mdp->comp[comp_id] = comp;

		ret = mtk_mdp_comp_init(dev, node, comp, comp_id);
		if (ret)
			goto err_comp;
	}

	mdp->workqueue = create_singlethread_workqueue(MTK_MDP_MODULE_NAME);
	if (!mdp->workqueue) {
		dev_err(&pdev->dev, "unable to alloc workqueue\n");
		ret = -ENOMEM;
		goto err_alloc_workqueue;
	}

	mdp->wdt_wq = create_singlethread_workqueue("mdp_wdt_wq");
	if (!mdp->wdt_wq) {
		dev_err(&pdev->dev, "unable to alloc wdt workqueue\n");
		ret = -ENOMEM;
		goto err_alloc_wdt_wq;
	}
	INIT_WORK(&mdp->wdt_work, mtk_wdt_worker);

	ret = v4l2_device_register(dev, &mdp->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register v4l2 device\n");
		ret = -EINVAL;
		goto err_dev_register;
	}

	ret = mtk_mdp_register_m2m_device(mdp);
	if (ret) {
		v4l2_err(&mdp->v4l2_dev, "Failed to init mem2mem device\n");
		goto err_m2m_register;
	}

	mdp->vpu_dev = vpu_get_plat_device(pdev);
	vpu_wdt_reg_handler(mdp->vpu_dev, mtk_mdp_reset_handler, mdp,
			    VPU_RST_MDP);

	platform_set_drvdata(pdev, mdp);

	mdp->alloc_ctx = vb2_dma_contig_init_ctx(dev);
	if (IS_ERR(mdp->alloc_ctx)) {
		ret = PTR_ERR(mdp->alloc_ctx);
		goto err_alloc_ctx;
	}

	pm_runtime_enable(dev);
	dev_dbg(dev, "mdp-%d registered successfully\n", mdp->id);

	return 0;

err_alloc_ctx:
	mtk_mdp_unregister_m2m_device(mdp);

err_m2m_register:
	v4l2_device_unregister(&mdp->v4l2_dev);

err_dev_register:
	destroy_workqueue(mdp->wdt_wq);

err_alloc_wdt_wq:
	destroy_workqueue(mdp->workqueue);

err_alloc_workqueue:

err_comp:
	for (i = 0; i < ARRAY_SIZE(mdp->comp); i++)
		mtk_mdp_comp_deinit(dev, mdp->comp[i]);

	dev_dbg(dev, "err %d\n", ret);
	return ret;
}

static int mtk_mdp_remove(struct platform_device *pdev)
{
	struct mtk_mdp_dev *mdp = platform_get_drvdata(pdev);
	int i;

	pm_runtime_disable(&pdev->dev);
	mtk_mdp_unregister_m2m_device(mdp);
	v4l2_device_unregister(&mdp->v4l2_dev);

	vb2_dma_contig_cleanup_ctx(mdp->alloc_ctx);
	flush_workqueue(mdp->workqueue);
	destroy_workqueue(mdp->workqueue);

	for (i = 0; i < ARRAY_SIZE(mdp->comp); i++)
		mtk_mdp_comp_deinit(&pdev->dev, mdp->comp[i]);

	dev_dbg(&pdev->dev, "%s driver unloaded\n", pdev->name);
	return 0;
}

#if defined(CONFIG_PM_RUNTIME) || defined(CONFIG_PM_SLEEP)
static int mtk_mdp_pm_suspend(struct device *dev)
{
	struct mtk_mdp_dev *mdp = dev_get_drvdata(dev);

	mtk_mdp_clock_off(mdp);

	return 0;
}

static int mtk_mdp_pm_resume(struct device *dev)
{
	struct mtk_mdp_dev *mdp = dev_get_drvdata(dev);

	mtk_mdp_clock_on(mdp);

	return 0;
}
#endif /* CONFIG_PM_RUNTIME || CONFIG_PM_SLEEP */

#ifdef CONFIG_PM_SLEEP
static int mtk_mdp_suspend(struct device *dev)
{
	if (pm_runtime_suspended(dev))
		return 0;

	return mtk_mdp_pm_suspend(dev);
}

static int mtk_mdp_resume(struct device *dev)
{
	if (pm_runtime_suspended(dev))
		return 0;

	return mtk_mdp_pm_resume(dev);
}
#endif /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops mtk_mdp_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(mtk_mdp_suspend, mtk_mdp_resume)
	SET_RUNTIME_PM_OPS(mtk_mdp_pm_suspend, mtk_mdp_pm_resume, NULL)
};

static struct platform_driver mtk_mdp_driver = {
	.probe		= mtk_mdp_probe,
	.remove		= mtk_mdp_remove,
	.driver = {
		.name	= MTK_MDP_MODULE_NAME,
		.owner	= THIS_MODULE,
		.pm	= &mtk_mdp_pm_ops,
		.of_match_table = mtk_mdp_of_ids,
	}
};

module_platform_driver(mtk_mdp_driver);

MODULE_AUTHOR("Houlong Wei <houlong.wei@mediatek.com>");
MODULE_DESCRIPTION("Mediatek image processor driver");
MODULE_LICENSE("GPL v2");
