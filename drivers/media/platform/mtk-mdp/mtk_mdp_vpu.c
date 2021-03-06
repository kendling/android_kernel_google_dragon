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

#include "mtk_mdp_core.h"
#include "mtk_mdp_vpu.h"
#include "mtk_vpu.h"


static inline struct mtk_mdp_ctx *vpu_to_ctx(struct mtk_mdp_vpu *vpu)
{
	return container_of(vpu, struct mtk_mdp_ctx, vpu);
}

static void mtk_mdp_vpu_handle_init_ack(struct mdp_ipi_comm_ack *msg)
{
	struct mtk_mdp_vpu *vpu = (struct mtk_mdp_vpu *)msg->ap_inst;

	/* mapping VPU address to kernel virtual address */
	vpu->vsi = (struct mdp_process_vsi *)
			vpu_mapping_dm_addr(vpu->pdev, msg->vpu_inst_addr);
	vpu->inst_addr = msg->vpu_inst_addr;
}

static void mtk_mdp_vpu_ipi_handler(void *data, unsigned int len, void *priv)
{
	unsigned int msg_id = *(unsigned int *)data;
	struct mdp_ipi_comm_ack *msg = (struct mdp_ipi_comm_ack *)data;
	struct mtk_mdp_vpu *vpu = (struct mtk_mdp_vpu *)msg->ap_inst;

	vpu->failure = msg->status;
	if (msg->status == MDP_IPI_MSG_STATUS_OK) {
		struct mtk_mdp_ctx *ctx;

		switch (msg_id) {
		case VPU_MDP_INIT_ACK:
			mtk_mdp_vpu_handle_init_ack(data);
			break;
		case VPU_MDP_DEINIT_ACK:
		case VPU_MDP_PROCESS_ACK:
			break;
		default:
			ctx = vpu_to_ctx(vpu);
			dev_err(&ctx->mdp_dev->pdev->dev,
				"handle unknown ipi msg %x\n",
				msg_id);
			break;
		}
	}
}

int mtk_mdp_vpu_register(struct platform_device *pdev)
{
	struct mtk_mdp_dev *mdp = platform_get_drvdata(pdev);
	int err;

	err = vpu_ipi_register(mdp->vpu_dev, IPI_MDP,
			       mtk_mdp_vpu_ipi_handler, "mdp_vpu", NULL);
	if (err != 0) {
		dev_err(&mdp->pdev->dev,
			"vpu_ipi_registration fail status=%d\n",
			err);
		return -EBUSY;
	}

	return 0;
}

static int mtk_mdp_vpu_send_msg(void *msg, int len, struct mtk_mdp_vpu *vpu,
				int id)
{
	struct mtk_mdp_ctx *ctx = vpu_to_ctx(vpu);
	int err;

	mutex_lock(&ctx->mdp_dev->vpulock);
	err = vpu_ipi_send(vpu->pdev, (enum ipi_id)id, msg, len);
	if (err != 0) {
		mutex_unlock(&ctx->mdp_dev->vpulock);
		dev_err(&ctx->mdp_dev->pdev->dev,
			"vpu_ipi_send fail status %d\n", err);
		return -EBUSY;
	}

	mutex_unlock(&ctx->mdp_dev->vpulock);

	return MDP_IPI_MSG_STATUS_OK;
}

static int mtk_mdp_vpu_send_ap_ipi(struct mtk_mdp_vpu *vpu, uint32_t msg_id)
{
	int err;
	struct mdp_ipi_comm msg;

	msg.msg_id = msg_id;
	msg.ipi_id = IPI_MDP;
	msg.vpu_inst_addr = vpu->inst_addr;
	msg.ap_inst = (uint64_t)vpu;
	err = mtk_mdp_vpu_send_msg((void *)&msg, sizeof(msg), vpu, IPI_MDP);
	if (!err && vpu->failure != MDP_IPI_MSG_STATUS_OK)
		err = vpu->failure;

	return err;
}

int mtk_mdp_vpu_init(struct mtk_mdp_vpu *vpu)
{
	int err;
	struct mdp_ipi_init msg;
	struct mtk_mdp_ctx *ctx = vpu_to_ctx(vpu);

	memset(vpu, 0, sizeof(*vpu));
	vpu->pdev = ctx->mdp_dev->vpu_dev;

	msg.msg_id = AP_MDP_INIT;
	msg.ipi_id = IPI_MDP;
	msg.ap_inst = (uint64_t)vpu;
	err = mtk_mdp_vpu_send_msg((void *)&msg, sizeof(msg), vpu, IPI_MDP);
	if (!err && vpu->failure != MDP_IPI_MSG_STATUS_OK)
		err = vpu->failure;

	return err;
}

int mtk_mdp_vpu_deinit(struct mtk_mdp_vpu *vpu)
{
	return mtk_mdp_vpu_send_ap_ipi(vpu, AP_MDP_DEINIT);
}

int mtk_mdp_vpu_process(struct mtk_mdp_vpu *vpu)
{
	return mtk_mdp_vpu_send_ap_ipi(vpu, AP_MDP_PROCESS);
}
