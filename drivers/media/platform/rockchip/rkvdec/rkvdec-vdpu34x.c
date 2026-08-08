// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VDPU34x video decoder helpers
 *
 * The RK3566/RK3568 VDPU34x needs a small H.264 decode before every real
 * H.264 job. It switches the hardware cleanly between CAVLC and CABAC and
 * works around the cabac/cavlc state retention erratum described by the
 * Rockchip BSP driver.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/mm.h>

#include "rkvdec.h"
#include "rkvdec-cabac.h"
#include "rkvdec-vdpu381-regs.h"

#define VDPU34X_WORKAROUND_BUF_SIZE	(2 * PAGE_SIZE)
#define VDPU34X_FIX_RPS_OFFSET		128
#define VDPU34X_FIX_PPS_OFFSET		256
#define VDPU34X_FIX_RLC_OFFSET		384
#define VDPU34X_FIX_OUTPUT_OFFSET	512
#define VDPU34X_FIX_COLMV_OFFSET		640

/* Minimal 16x16 IDR picture and its hardware SPS/PPS packet. */
static const u8 vdpu34x_h264_fix_data[384] = {
	0x00, 0x00, 0x01, 0x65, 0x88, 0x81, 0x00, 0x9f,
	0xfe, 0x6f, 0x5f, 0x32, 0xc5, 0x42, 0x54, 0x26,
	0x81, 0xd5, 0xe9, 0x71, 0x10,

	[VDPU34X_FIX_PPS_OFFSET] =
	0xff, 0x3f, 0x80, 0x14, 0x40, 0x00, 0x04, 0x40,
	0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xff, 0x3f, 0x42,
};

int rkvdec_vdpu34x_workaround_init(struct rkvdec_core *core)
{
	u8 *data;

	core->workaround_size = VDPU34X_WORKAROUND_BUF_SIZE;
	core->workaround_cpu = dmam_alloc_coherent(core->dev,
						   core->workaround_size,
						   &core->workaround_dma,
						   GFP_KERNEL);
	if (!core->workaround_cpu)
		return -ENOMEM;

	data = core->workaround_cpu;
	memcpy(data, vdpu34x_h264_fix_data, sizeof(vdpu34x_h264_fix_data));
	memcpy(data + PAGE_SIZE, rkvdec_h264_cabac_table,
	       sizeof(rkvdec_h264_cabac_table));

	return 0;
}

static void vdpu34x_clear_workaround_regs(struct rkvdec_core *core)
{
	u32 offset;

	for (offset = 0x20; offset <= 0x1c0; offset += sizeof(u32))
		writel_relaxed(0, core->regs + offset);

	for (offset = 0x200; offset <= 0x238; offset += sizeof(u32))
		writel_relaxed(0, core->regs + offset);

	for (offset = 0x280; offset <= 0x314; offset += sizeof(u32))
		writel_relaxed(0, core->regs + offset);

	writel_relaxed(0, core->regs + VDPU381_REG_STA_INT);
}

void rkvdec_vdpu34x_h264_workaround(struct rkvdec_ctx *ctx)
{
	struct rkvdec_core *core = ctx->core;
	u32 dma = lower_32_bits(core->workaround_dma);
	u32 offset, status;
	int ret;

	if (!(ctx->dev->variant->quirks & RKVDEC_QUIRK_VDPU34X_H264_CABAC))
		return;

	vdpu34x_clear_workaround_regs(core);

	/* Clear all three internal decoder caches. */
	writel_relaxed(1, core->cache + 0x10);
	writel_relaxed(1, core->cache + 0x50);
	writel_relaxed(1, core->cache + 0x90);

	/* Common registers for the minimal H.264 job. IRQ is disabled. */
	writel_relaxed(0x00000001, core->regs + 0x024);
	writel_relaxed(0x00000072, core->regs + 0x02c);
	writel_relaxed(0x00000102, core->regs + 0x030);
	writel_relaxed(0x01048201, core->regs + 0x034);
	writel_relaxed(0x00000001, core->regs + 0x03c);
	writel_relaxed(0x00000030, core->regs + 0x040);
	writel_relaxed(0x00003fff, core->regs + 0x044);
	writel_relaxed(0x00000001, core->regs + 0x048);
	writel_relaxed(0x00000001, core->regs + 0x04c);
	writel_relaxed(0x00000010, core->regs + 0x050);
	writel_relaxed(0x00000006, core->regs + 0x054);
	writel_relaxed(0xffffdfff, core->regs + 0x060);
	writel_relaxed(0x3ffbfbff, core->regs + 0x064);
	writel_relaxed(0x800fffff, core->regs + 0x068);
	writel_relaxed(0x000000ff, core->regs + 0x080);

	/* Stream, RLC, output and COLMV addresses. */
	writel_relaxed(dma, core->regs + 0x200);
	writel_relaxed(dma + VDPU34X_FIX_RLC_OFFSET, core->regs + 0x204);
	writel_relaxed(dma + VDPU34X_FIX_OUTPUT_OFFSET, core->regs + 0x208);
	writel_relaxed(dma + VDPU34X_FIX_COLMV_OFFSET, core->regs + 0x20c);

	/* RCB scratch addresses used by the fake picture. */
	for (offset = 0x214; offset <= 0x220; offset += sizeof(u32))
		writel_relaxed(dma + 0x1c0, core->regs + offset);
	for (offset = 0x224; offset <= 0x228; offset += sizeof(u32))
		writel_relaxed(dma + 0x340, core->regs + offset);
	for (offset = 0x22c; offset <= 0x238; offset += sizeof(u32))
		writel_relaxed(dma, core->regs + offset);

	writel_relaxed(dma + VDPU34X_FIX_PPS_OFFSET, core->regs + 0x280);
	writel_relaxed(dma + VDPU34X_FIX_RPS_OFFSET, core->regs + 0x288);
	for (offset = 0x28c; offset <= 0x30c; offset += sizeof(u32))
		writel_relaxed(dma + VDPU34X_FIX_COLMV_OFFSET,
			       core->regs + offset);
	writel_relaxed(dma + PAGE_SIZE, core->regs + 0x310);

	/* Complete this short job synchronously before programming the real one. */
	wmb();
	writel(VDPU381_DEC_E_BIT, core->regs + VDPU381_REG_DEC_E);
	ret = readl_poll_timeout_atomic(core->regs + VDPU381_REG_STA_INT,
					status, (status & 0x106) == 0x106,
					2, 60);
	if (ret)
		dev_warn_ratelimited(core->dev,
				     "H.264 CABAC workaround timed out (status %#x)\n",
				     status);

	writel(0, core->regs + VDPU381_REG_STA_INT);
}
