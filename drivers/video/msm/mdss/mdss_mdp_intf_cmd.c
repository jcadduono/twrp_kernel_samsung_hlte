/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>

#include "mdss_mdp.h"
#include "mdss_panel.h"
#include "mdss_debug.h"
#include "mdss_fb.h"
#include "mdss_mdp_trace.h"

#define VSYNC_EXPIRE_TICK 6

#define MAX_SESSIONS 2

/* wait for at most 2 vsync for lowest refresh rate (24hz) */
#define KOFF_TIMEOUT msecs_to_jiffies(84)

#define STOP_TIMEOUT(hz) msecs_to_jiffies((1000 / hz) * (VSYNC_EXPIRE_TICK + 2))
#define ULPS_ENTER_TIME msecs_to_jiffies(100)

/*
 * STOP_TIMEOUT need to wait for cmd stop depends on fps
 * if the command panel support 60fps the timeout value
 * generated using 16ms(1frame). If that support 15fps the timeout value
 * generated by 40ms(1frame)
 */
#define STOP_TIMEOUT_FOR_ALPM msecs_to_jiffies(40 * (VSYNC_EXPIRE_TICK + 2))

struct mdss_mdp_cmd_ctx {
	struct mdss_mdp_ctl *ctl;
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	u32 panel_ndx;
#endif
	u32 pp_num;
	u8 ref_cnt;
	struct completion pp_comp;
	struct completion stop_comp;
	struct list_head vsync_handlers;
	int panel_on;
	int koff_cnt;
	int clk_enabled;
	int vsync_enabled;
	int rdptr_enabled;
	struct mutex clk_mtx;
	spinlock_t clk_lock;
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQHD_PT_PANEL)
	spinlock_t te_lock;
#endif
	struct work_struct clk_work;
	struct delayed_work ulps_work;
	struct work_struct pp_done_work;
	atomic_t pp_done_cnt;

	/* te config */
	u8 tear_check;
	u16 height;	/* panel height */
	u16 vporch;	/* vertical porches */
	u16 start_threshold;
	u32 vclk_line;	/* vsync clock per line */
	struct mdss_panel_recovery recovery;
	bool ulps;
};

struct mdss_mdp_cmd_ctx mdss_mdp_cmd_ctx_list[MAX_SESSIONS];
extern char board_rev;
int get_lcd_attached(void);

static inline u32 mdss_mdp_cmd_line_count(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_mixer *mixer;
	u32 cnt = 0xffff;	/* init it to an invalid value */
	u32 init;
	u32 height;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (!mixer) {
		mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_RIGHT);
		if (!mixer) {
			mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
			goto exit;
		}
	}

	init = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_VSYNC_INIT_VAL) & 0xffff;

	height = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_HEIGHT) & 0xffff;

	if (height < init) {
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
		goto exit;
	}

	cnt = mdss_mdp_pingpong_read
		(mixer, MDSS_MDP_REG_PP_INT_COUNT_VAL) & 0xffff;

	if (cnt < init)		/* wrap around happened at height */
		cnt += (height - init);
	else
		cnt -= init;

	mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);

	pr_debug("cnt=%d init=%d height=%d\n", cnt, init, height);
exit:
	return cnt;
}


static int mdss_mdp_cmd_tearcheck_cfg(struct mdss_mdp_ctl *ctl,
				      struct mdss_mdp_mixer *mixer)
{
	struct mdss_mdp_pp_tear_check *te;
	struct mdss_panel_info *pinfo;
	u32 vsync_clk_speed_hz, total_lines, vclks_line, cfg;

	if (IS_ERR_OR_NULL(ctl->panel_data)) {
		pr_err("no panel data\n");
		return -ENODEV;
	}

	pinfo = &ctl->panel_data->panel_info;
	te = &ctl->panel_data->panel_info.te;

	mdss_mdp_vsync_clk_enable(1);

	vsync_clk_speed_hz =
		mdss_mdp_get_clk_rate(MDSS_CLK_MDP_VSYNC);

	total_lines = mdss_panel_get_vtotal(pinfo);

	total_lines *= pinfo->mipi.frame_rate;

	vclks_line = (total_lines) ? vsync_clk_speed_hz / total_lines : 0;

	cfg = BIT(19);
	if (pinfo->mipi.hw_vsync_mode)
		cfg |= BIT(20);

	if (te->refx100)
		vclks_line = vclks_line * pinfo->mipi.frame_rate *
			100 / te->refx100;
	else {
		pr_warn("refx100 cannot be zero! Use 6000 as default\n");
		vclks_line = vclks_line * pinfo->mipi.frame_rate *
			100 / 6000;
	}

	cfg |= vclks_line;

	pr_info("%s: te->tear_check_en = %d, res=%d vclks=%x height=%d init=%d rd=%d start=%d ",
		__func__, te->tear_check_en, pinfo->yres, vclks_line, te->sync_cfg_height,
		 te->vsync_init_val, te->rd_ptr_irq, te->start_pos);
	pr_info("thrd_start =%d thrd_cont=%d\n",
		te->sync_threshold_start, te->sync_threshold_continue);

	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_VSYNC, cfg);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_CONFIG_HEIGHT,
				te->sync_cfg_height);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_VSYNC_INIT_VAL,
				te->vsync_init_val);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_RD_PTR_IRQ,
				te->rd_ptr_irq);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_START_POS,
				te->start_pos);
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_SYNC_THRESH,
				((te->sync_threshold_continue << 16) |
				 te->sync_threshold_start));
	mdss_mdp_pingpong_write(mixer, MDSS_MDP_REG_PP_TEAR_CHECK_EN,
				te->tear_check_en);
	return 0;
}

static int mdss_mdp_cmd_tearcheck_setup(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_mixer *mixer;
	int rc = 0;
	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (mixer) {
		rc = mdss_mdp_cmd_tearcheck_cfg(ctl, mixer);
		if (rc)
			goto err;
	}
	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_RIGHT);
	if (mixer)
		rc = mdss_mdp_cmd_tearcheck_cfg(ctl, mixer);
 err:
	return rc;
}

static inline void mdss_mdp_cmd_clk_on(struct mdss_mdp_cmd_ctx *ctx)
{
	unsigned long flags;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	int rc;

	if (!ctx->panel_on) {
		pr_info("%s: Ignore clock on because the unblank does not finished\n", __func__);
		return;
	}

#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctx->panel_ndx, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	mutex_lock(&ctx->clk_mtx);
	MDSS_XLOG(ctx->pp_num, ctx->koff_cnt, ctx->clk_enabled,
						ctx->rdptr_enabled);
	if (!ctx->clk_enabled) {
		mdss_bus_bandwidth_ctrl(true);

		ctx->clk_enabled = 1;
		if (cancel_delayed_work_sync(&ctx->ulps_work))
			pr_debug("deleted pending ulps work\n");

		rc = mdss_iommu_ctrl(1);
		if (IS_ERR_VALUE(rc))
			pr_err("IOMMU attach failed\n");

		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_ON, false);

		if (ctx->ulps) {
			if (mdss_mdp_cmd_tearcheck_setup(ctx->ctl))
				pr_warn("tearcheck setup failed\n");
			mdss_mdp_ctl_intf_event(ctx->ctl,
				MDSS_EVENT_DSI_ULPS_CTRL, (void *)0);
			ctx->ulps = false;
		}

		mdss_mdp_ctl_intf_event
			(ctx->ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)1);

		mdss_mdp_hist_intr_setup(&mdata->hist_intr, MDSS_IRQ_RESUME);
	}
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!ctx->rdptr_enabled)
		mdss_mdp_irq_enable(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);
	ctx->rdptr_enabled = VSYNC_EXPIRE_TICK;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
	mutex_unlock(&ctx->clk_mtx);
}

static inline void mdss_mdp_cmd_clk_off(struct mdss_mdp_cmd_ctx *ctx)
{
	unsigned long flags;
	struct mdss_data_type *mdata = mdss_mdp_get_mdata();
	int set_clk_off = 0;

#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__,ctx->panel_ndx, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	mutex_lock(&ctx->clk_mtx);
	MDSS_XLOG(ctx->pp_num, ctx->koff_cnt, ctx->clk_enabled,
						ctx->rdptr_enabled);
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!ctx->rdptr_enabled)
		set_clk_off = 1;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if ((ctx->clk_enabled && set_clk_off) || (get_lcd_attached() == 0)) {
		ctx->clk_enabled = 0;
		mdss_mdp_hist_intr_setup(&mdata->hist_intr, MDSS_IRQ_SUSPEND);
		mdss_mdp_ctl_intf_event
			(ctx->ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)0);
		mdss_iommu_ctrl(0);
		mdss_bus_bandwidth_ctrl(false);
		mdss_mdp_clk_ctrl(MDP_BLOCK_POWER_OFF, false);
		if (ctx->panel_on)
			schedule_delayed_work(&ctx->ulps_work, ULPS_ENTER_TIME);
	}
	mutex_unlock(&ctx->clk_mtx);
}
#if defined(DYNAMIC_FPS_USE_TE_CTRL)
int	dynamic_fps_use_te_ctrl_value;
#endif
#if defined(CONFIG_LCD_HMT)
int skip_te_enable = 0;
static unsigned int skip_te = 0;
#endif

#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQHD_PT_PANEL)
int te;
int te_cnt;
int te_set_done;
struct completion te_check_comp;
int get_lcd_ldi_info(void);
#endif

static void mdss_mdp_cmd_readptr_done(void *arg)
{
	struct mdss_mdp_ctl *ctl = arg;
	struct mdss_mdp_cmd_ctx *ctx = ctl->priv_data;
	struct mdss_mdp_vsync_handler *tmp;
	ktime_t vsync_time;

#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQHD_PT_PANEL)
	static ktime_t vsync_time1;
	static ktime_t vsync_time2;
	static int i = 0;
	static int time1 = 0, time2 = 0;
#endif
	static long long vsync[2];
	long long duration = 16000;
	static int index;
	static int add_value = 1;
//	pr_err("mdss_mdp_cmd_readptr_done\n");
#if defined(DYNAMIC_FPS_USE_TE_CTRL)
	if(dynamic_fps_use_te_ctrl)
	{
		if(dynamic_fps_use_te_ctrl_value)
		{
			dynamic_fps_use_te_ctrl_value = 0;
			return;
		}
		dynamic_fps_use_te_ctrl_value = 1;
	}
#endif

	if (!ctx) {
		pr_err("invalid ctx\n");
		return;
	}

#if defined(CONFIG_LCD_HMT)
	if (skip_te_enable) {
		if (skip_te) {
			pr_debug("%s : Skip TE Signal \n",__func__);
			skip_te = 0;
			return;
		}
		skip_te = 1;
	}
#endif

	ATRACE_BEGIN(__func__);
	vsync_time = ktime_get();
	vsync[index] = ktime_to_us(vsync_time);

	index += add_value;
	add_value *= -1;

	if (vsync[0] && vsync[1])
		duration = vsync[index + add_value] - vsync[index];
	ctl->vsync_cnt++;
	MDSS_XLOG(0xFFFF, ctl->num, ctx->koff_cnt, ctx->clk_enabled,
				ctx->rdptr_enabled, duration);
 
	if (duration <= 8000 || duration >= 22000)
		pr_err("[DEBUG]%s:time : %lld, duration : %lld\n",
				__func__, vsync[index + add_value], duration);

#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQHD_PT_PANEL)
	if (get_lcd_ldi_info()) {
		if (te_set_done == TE_SET_START) {

			pr_debug("%s : TE_SET_START...",__func__);

			if (i % 2 == 0) {
				vsync_time1 = ktime_get();
				time1 = (int)ktime_to_us(vsync_time1);
				te = time1 && time2 ? time1 - time2 : 0;
				pr_debug("[%s] : ktime = %d\n",__func__, te);
			} else {
				vsync_time2 = ktime_get();
				time2 = (int)ktime_to_us(vsync_time2);
				te = time1 && time2 ? time2 - time1 : 0;
				pr_debug("[%s] : ktime = %d\n",__func__, te);
			}
			i++;

			pr_debug("[%s] TE = %d\n",__func__, te);

			spin_lock(&ctx->te_lock);
			te_cnt++;
			if (te_cnt >= 2) { // check TE using only two signal..
				pr_debug(">>>> te_check_comp COMPLETE (%d) <<<< \n", te_cnt);
				complete(&te_check_comp);
			}
			spin_unlock(&ctx->te_lock);
		} else {
			pr_debug("%s : not TE_SET_START...",__func__);
		}
	}
#endif

#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__,ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x88888);
#endif

	spin_lock(&ctx->clk_lock);
	list_for_each_entry(tmp, &ctx->vsync_handlers, list) {
		if (tmp->enabled && !tmp->cmd_post_flush)
			tmp->vsync_handler(ctl, vsync_time);
	}

	if (!ctx->vsync_enabled) {
		if (ctx->rdptr_enabled)
			ctx->rdptr_enabled--;
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQHD_PT_PANEL)
				if (get_lcd_ldi_info())
					if (!(te_set_done == TE_SET_DONE || te_set_done == TE_SET_FAIL))
					{
						pr_info("now restoring TE/ rdptr_enabled++\n");
						if (ctx->rdptr_enabled == 0)
							ctx->rdptr_enabled++;
					}						
#endif
		/* keep clk on during kickoff */
		if (ctx->rdptr_enabled == 0 && ctx->koff_cnt)
			ctx->rdptr_enabled++;
	}

	if (ctx->rdptr_enabled == 0) {
		mdss_mdp_irq_disable_nosync
			(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);
		complete(&ctx->stop_comp);
		schedule_work(&ctx->clk_work);
		index = 0;
		add_value = 1;
		vsync[0] = vsync[1] = 0;
	}

	ATRACE_END(__func__);
	spin_unlock(&ctx->clk_lock);
}

static void mdss_mdp_cmd_underflow_recovery(void *data)
{
	struct mdss_mdp_cmd_ctx *ctx = data;
	unsigned long flags;

	if (!data) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	if (!ctx->ctl)
		return;
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (ctx->koff_cnt) {
		mdss_mdp_ctl_reset(ctx->ctl);
		pr_debug("%s: intf_num=%d\n", __func__,
					ctx->ctl->intf_num);
		ctx->koff_cnt--;
		mdss_mdp_irq_disable_nosync(MDSS_MDP_IRQ_PING_PONG_COMP,
						ctx->pp_num);
		complete_all(&ctx->pp_comp);
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
}
#if 0
static void mdss_mdp_cmd_pingpong_recovery(struct mdss_mdp_cmd_ctx *ctx)
{
	unsigned long flags;

	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	if (!ctx->ctl)
		return;
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (ctx->koff_cnt) {
		mdss_mdp_ctl_reset(ctx->ctl);
		pr_debug("%s: intf_num=%d\n", __func__,
					ctx->ctl->intf_num);
		ctx->koff_cnt--;
		mdss_mdp_irq_disable_nosync(MDSS_MDP_IRQ_PING_PONG_COMP,
						ctx->pp_num);
		complete_all(&ctx->pp_comp);
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
}
#endif

static void mdss_mdp_cmd_pingpong_done(void *arg)
{
	struct mdss_mdp_ctl *ctl = arg;
	struct mdss_mdp_cmd_ctx *ctx = ctl->priv_data;
	struct mdss_mdp_vsync_handler *tmp;
	ktime_t vsync_time;

#if defined (CONFIG_FB_MSM_MDSS_DBG_SEQ_TICK)
	mdss_dbg_tick_save(PP_DONE);
#endif
	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	mdss_mdp_ctl_perf_set_transaction_status(ctl,
		PERF_HW_MDP_STATE, PERF_STATUS_DONE);

	spin_lock(&ctx->clk_lock);
	list_for_each_entry(tmp, &ctx->vsync_handlers, list) {
		if (tmp->enabled && tmp->cmd_post_flush)
			tmp->vsync_handler(ctl, vsync_time);
	}
	mdss_mdp_irq_disable_nosync(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num);

	complete_all(&ctx->pp_comp);
	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
					ctx->rdptr_enabled);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, ctl->roi_bkup.w, ctl->roi_bkup.h);
#endif

	if (ctx->koff_cnt) {
		atomic_inc(&ctx->pp_done_cnt);
		schedule_work(&ctx->pp_done_work);
		ctx->koff_cnt--;
		if (ctx->koff_cnt) {
			pr_err("%s: too many kickoffs=%d!\n", __func__,
			       ctx->koff_cnt);
			ctx->koff_cnt = 0;
		}
	} else
		pr_err("%s: should not have pingpong interrupt!\n", __func__);

	trace_mdp_cmd_pingpong_done(ctl, ctx->pp_num, ctx->koff_cnt);
	pr_debug("%s: ctl_num=%d intf_num=%d ctx=%d kcnt=%d\n", __func__,
		ctl->num, ctl->intf_num, ctx->pp_num, ctx->koff_cnt);

	spin_unlock(&ctx->clk_lock);
}

static void pingpong_done_work(struct work_struct *work)
{
	struct mdss_mdp_cmd_ctx *ctx =
		container_of(work, typeof(*ctx), pp_done_work);

	if (ctx->ctl) {
		while (atomic_add_unless(&ctx->pp_done_cnt, -1, 0))
			mdss_mdp_ctl_notify(ctx->ctl, MDP_NOTIFY_FRAME_DONE);

#if !defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_FHD_FA2_PT_PANEL)
		mdss_mdp_ctl_perf_release_bw(ctx->ctl);
#endif
	}
}

static void clk_ctrl_work(struct work_struct *work)
{
	struct mdss_mdp_cmd_ctx *ctx =
		container_of(work, typeof(*ctx), clk_work);

	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	mdss_mdp_cmd_clk_off(ctx);
}

static void __mdss_mdp_cmd_ulps_work(struct work_struct *work)
{
	struct delayed_work *dw = to_delayed_work(work);
	struct mdss_mdp_cmd_ctx *ctx =
		container_of(dw, struct mdss_mdp_cmd_ctx, ulps_work);

	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return;
	}

	if (!ctx->panel_on) {
		pr_err("Panel is off. skipping ULPS configuration\n");
		return;
	}

	if (!mdss_mdp_ctl_intf_event(ctx->ctl, MDSS_EVENT_DSI_ULPS_CTRL,
		(void *)1)) {
		ctx->ulps = true;
		ctx->ctl->play_cnt = 0;
		mdss_mdp_footswitch_ctrl_ulps(0, &ctx->ctl->mfd->pdev->dev);
	}
}

static int mdss_mdp_cmd_add_vsync_handler(struct mdss_mdp_ctl *ctl,
		struct mdss_mdp_vsync_handler *handle)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	bool enable_rdptr = false;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return -ENODEV;
	}
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
		xlog(__func__, ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif

	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
					ctx->rdptr_enabled);

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (!handle->enabled) {
		handle->enabled = true;
		list_add(&handle->list, &ctx->vsync_handlers);

		enable_rdptr = !handle->cmd_post_flush;
		if (enable_rdptr)
			ctx->vsync_enabled++;
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	if (enable_rdptr)
		mdss_mdp_cmd_clk_on(ctx);

	return 0;
}

static int mdss_mdp_cmd_remove_vsync_handler(struct mdss_mdp_ctl *ctl,
		struct mdss_mdp_vsync_handler *handle)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("%s: invalid ctx\n", __func__);
		return -ENODEV;
	}

	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
				ctx->rdptr_enabled, 0x88888);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
		xlog(__func__, ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x88888);
#endif

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (handle->enabled) {
		handle->enabled = false;
		list_del_init(&handle->list);

		if (!handle->cmd_post_flush) {
			if (ctx->vsync_enabled)
				ctx->vsync_enabled--;
			else
				WARN(1, "unbalanced vsync disable");
		}
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
	return 0;
}

int mdss_mdp_cmd_reconfigure_splash_done(struct mdss_mdp_ctl *ctl, bool handoff)
{
	struct mdss_panel_data *pdata;
	int ret = 0;

	pdata = ctl->panel_data;

	pdata->panel_info.cont_splash_enabled = 0;
#if !defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQXGA_S6TNMR7_PT_PANEL)
	mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_CLK_CTRL, (void *)0);
#endif
	return ret;
}

void mdp5_dump_regs(void)
{
	int i, z, start, len;
	int offsets[] = {0x0};
	int length[] = {19776};

	printk("%s: =============MDSS Reg DUMP==============\n", __func__);
	for (i = 0; i < sizeof(offsets) / sizeof(int); i++) {
		start = offsets[i];
		len = length[i];
		printk("-------- Address %05x: -------\n", start);
		for (z = 0; z < len; z++) {
			if ((z & 3) == 0)
				printk("%05x:", start + (z * 4));
			printk(" %08x", MDSS_MDP_REG_READ(start + (z * 4)));
			if ((z & 3) == 3)
				printk("\n");
		}
		if ((z & 3) != 0)
			printk("\n");
	}
	printk("%s: ============= END ==============\n", __func__);
}

static int mdss_mdp_cmd_wait4pingpong(struct mdss_mdp_ctl *ctl, void *arg)
{
	struct mdss_mdp_cmd_ctx *ctx;
	struct mdss_panel_data *pdata;
	unsigned long flags;
	int need_wait = 0;
	int rc = 0;
	static int recovery_cnt;

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	pdata = ctl->panel_data;

	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (ctx->koff_cnt > 0)
		need_wait = 1;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, ctl->roi_bkup.w, ctl->roi_bkup.h);
#endif

	ctl->roi_bkup.w = ctl->width;
	ctl->roi_bkup.h = ctl->height;

	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
			ctx->rdptr_enabled, ctl->roi_bkup.w,
			ctl->roi_bkup.h);

	pr_debug("%s: need_wait=%d  intf_num=%d ctx=%p\n",
			__func__, need_wait, ctl->intf_num, ctx);

	if (need_wait) {
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQXGA_S6E3HA1_PT_PANEL)
		if (!board_rev)
			rc = wait_for_completion_timeout(
				&ctx->pp_comp, msecs_to_jiffies(20));
		else
#endif
		rc = wait_for_completion_timeout(
				&ctx->pp_comp, msecs_to_jiffies(1000));
		trace_mdp_cmd_wait_pingpong(ctl->num, ctx->koff_cnt);

		if (rc <= 0) {
			WARN(1, "cmd kickoff timed out (rc = %d, recovery_cnt = %d) ctl=%d\n",
						rc, ++recovery_cnt, ctl->num);
			mdss_dsi_debug_check_te(pdata);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
			dumpreg();
			mdp5_dump_regs();
			mdss_mdp_debug_bus();
			xlog_dump();
#if 0
			mdss_mdp_cmd_pingpong_recovery(ctx);
#else
			panic("Pingpong Timeout");
#endif
#endif
			rc = -EPERM;
			mdss_mdp_ctl_notify(ctl, MDP_NOTIFY_FRAME_TIMEOUT);
		} else {
			rc = 0;
		}
	}
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__,ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, rc);
#endif

	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
					ctx->rdptr_enabled, rc);
	return rc;
}

static int mdss_mdp_cmd_set_partial_roi(struct mdss_mdp_ctl *ctl)
{
	int rc = 0;
	if (ctl->roi.w && ctl->roi.h && ctl->roi_changed &&
			ctl->panel_data->panel_info.partial_update_enabled) {
		ctl->panel_data->panel_info.roi_x = ctl->roi.x;
		ctl->panel_data->panel_info.roi_y = ctl->roi.y;
		ctl->panel_data->panel_info.roi_w = ctl->roi.w;
		ctl->panel_data->panel_info.roi_h = ctl->roi.h;

		rc = mdss_mdp_ctl_intf_event(ctl,
				MDSS_EVENT_ENABLE_PARTIAL_UPDATE, NULL);
	}
	return rc;
}

int mdss_mdp_cmd_kickoff(struct mdss_mdp_ctl *ctl, void *arg)
{
	struct mdss_mdp_cmd_ctx *ctx;
	unsigned long flags;
	int rc;

	ATRACE_BEGIN(__func__);
	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	if (get_lcd_attached() == 0) {
		pr_err("%s : lcd is not attached..\n",__func__);
		return -ENODEV;
	}
	mdss_mdp_ctl_perf_set_transaction_status(ctl,
		PERF_HW_MDP_STATE, PERF_STATUS_BUSY);

	pr_debug("%s:+\n", __func__);

	if (ctx->panel_on == 0) {
		rc = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_UNBLANK, NULL);
		WARN(rc, "intf %d unblank error (%d)\n", ctl->intf_num, rc);

		ctx->panel_on++;

		rc = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_ON, NULL);
		WARN(rc, "intf %d panel on error (%d)\n", ctl->intf_num, rc);

		mdss_mdp_ctl_intf_event(ctl,
				MDSS_EVENT_REGISTER_RECOVERY_HANDLER,
				(void *)&ctx->recovery);
	}

	MDSS_XLOG(ctl->num, ctl->roi.x, ctl->roi.y, ctl->roi.w,
						ctl->roi.h);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctl->num, ctl->roi.x, ctl->roi.y, ctl->roi.w, ctl->roi.h, 0x1234);
#endif

	spin_lock_irqsave(&ctx->clk_lock, flags);
	ctx->koff_cnt++;
	spin_unlock_irqrestore(&ctx->clk_lock, flags);
	trace_mdp_cmd_kickoff(ctl->num, ctx->koff_cnt);

	mdss_mdp_cmd_clk_on(ctx);

	mdss_mdp_cmd_set_partial_roi(ctl);

	/*
	 * tx dcs command if had any
	 */
	mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_DSI_CMDLIST_KOFF, NULL);
	INIT_COMPLETION(ctx->pp_comp);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctl->num,  ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	mdss_mdp_irq_enable(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num);
	mdss_mdp_ctl_write(ctl, MDSS_MDP_REG_CTL_START, 1);
	mdss_mdp_ctl_perf_set_transaction_status(ctl,
	PERF_SW_COMMIT_STATE, PERF_STATUS_DONE);
	mb();
	MDSS_XLOG(ctl->num,  ctx->koff_cnt, ctx->clk_enabled,
						ctx->rdptr_enabled);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	{
		void mdss_mdp_mixer_read(void);
		mdss_mdp_mixer_read();
	}
#endif
	ATRACE_END(__func__);
	pr_debug("%s : -- \n", __func__);

	return 0;
}

int mdss_mdp_cmd_stop(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_cmd_ctx *ctx;
	struct mdss_panel_info *pinfo = &ctl->panel_data->panel_info;
	unsigned long flags;
	struct mdss_mdp_vsync_handler *tmp, *handle;
	int need_wait = 0;
	int ret = 0;
	u8 timeout_status = 0;
	int hz;

	pr_debug("%s:+\n", __func__);

	if (get_lcd_attached() == 0) {
		pr_err("%s : lcd is not attached..\n",__func__);
		return 0;
	}

	ctx = (struct mdss_mdp_cmd_ctx *) ctl->priv_data;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	list_for_each_entry_safe(handle, tmp, &ctx->vsync_handlers, list)
		mdss_mdp_cmd_remove_vsync_handler(ctl, handle);
	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
				ctx->rdptr_enabled, XLOG_FUNC_ENTRY);

#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x11111);
#endif
	spin_lock_irqsave(&ctx->clk_lock, flags);
	if (ctx->rdptr_enabled) {
		INIT_COMPLETION(ctx->stop_comp);
		need_wait = 1;
	}
	spin_unlock_irqrestore(&ctx->clk_lock, flags);

	hz = mdss_panel_get_framerate(&ctl->panel_data->panel_info);

	if (need_wait) {
		if (pinfo->alpm_event && pinfo->alpm_event(CHECK_CURRENT_STATUS))
			timeout_status = wait_for_completion_timeout(&ctx->stop_comp,\
							STOP_TIMEOUT_FOR_ALPM);
		else
			timeout_status = wait_for_completion_timeout(&ctx->stop_comp,\
						STOP_TIMEOUT(hz)); /*msecs_to_jiffies(1000));*/ //STOP_TIMEOUT(16 * 4 frames) -> 1000
		if (timeout_status <= 0) {
			WARN(1, "stop cmd time out\n");
			if (IS_ERR_OR_NULL(ctl->panel_data)) {
				pr_err("no panel data\n");
			} else {
				pinfo = &ctl->panel_data->panel_info;

#if defined(CONFIG_MACH_KLTE_CUDUOS) || defined(CONFIG_MACH_H3G_CHN_OPEN) || defined(CONFIG_MACH_H3G_CHN_CMCC) || defined(CONFIG_MACH_HLTE_CHN_CMCC) || defined(CONFIG_MACH_HLTE_CHN_TDOPEN) 
				mdss_mdp_irq_disable
					(MDSS_MDP_IRQ_PING_PONG_RD_PTR,
							ctx->pp_num);
				ctx->rdptr_enabled = 0;
#else
				if (pinfo->panel_dead) {
					mdss_mdp_irq_disable
						(MDSS_MDP_IRQ_PING_PONG_RD_PTR,
								ctx->pp_num);
					ctx->rdptr_enabled = 0;
				}
#endif
			}
		}
	}
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQXGA_S6E3HA1_PT_PANEL)
	if (!board_rev) {
		mdss_mdp_irq_disable(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num);
		if (ctx->rdptr_enabled)
			ctx->rdptr_enabled = 0;
	}
#endif

	if (cancel_work_sync(&ctx->clk_work))
		pr_debug("no pending clk work\n");

	if (cancel_delayed_work_sync(&ctx->ulps_work))
		pr_debug("deleted pending ulps work\n");

	mdss_mdp_ctl_intf_event(ctl,
			MDSS_EVENT_REGISTER_RECOVERY_HANDLER,
			NULL);

	ctx->panel_on = 0;
	mdss_mdp_cmd_clk_off(ctx);

	flush_work(&ctx->pp_done_work);


	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num,
				   NULL, NULL);
	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num,
				   NULL, NULL);

	memset(ctx, 0, sizeof(*ctx));
	ctl->priv_data = NULL;

	if (ctl->num == 0) {
		ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_BLANK, NULL);
		WARN(ret, "intf %d unblank error (%d)\n", ctl->intf_num, ret);

		ret = mdss_mdp_ctl_intf_event(ctl, MDSS_EVENT_PANEL_OFF, NULL);
		WARN(ret, "intf %d unblank error (%d)\n", ctl->intf_num, ret);
	}

	ctl->stop_fnc = NULL;
	ctl->display_fnc = NULL;
	ctl->wait_pingpong = NULL;
	ctl->add_vsync_handler = NULL;
	ctl->remove_vsync_handler = NULL;

	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
				ctx->rdptr_enabled, XLOG_FUNC_EXIT);
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0x222222);
#endif
	pr_debug("%s:-\n", __func__);

	return 0;
}

int mdss_mdp_cmd_start(struct mdss_mdp_ctl *ctl)
{
	struct mdss_mdp_cmd_ctx *ctx;
	struct mdss_mdp_mixer *mixer;
	int i, ret;

	pr_debug("%s:+\n", __func__);

	mixer = mdss_mdp_mixer_get(ctl, MDSS_MDP_MIXER_MUX_LEFT);
	if (!mixer) {
		pr_err("mixer not setup correctly\n");
		return -ENODEV;
	}

	for (i = 0; i < MAX_SESSIONS; i++) {
		ctx = &mdss_mdp_cmd_ctx_list[i];
		if (ctx->ref_cnt == 0) {
			ctx->ref_cnt++;
			break;
		}
	}
	if (i == MAX_SESSIONS) {
		pr_err("too many sessions\n");
		return -ENOMEM;
	}

	ctl->priv_data = ctx;
	if (!ctx) {
		pr_err("invalid ctx\n");
		return -ENODEV;
	}

	ctx->ctl = ctl;
#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	ctx->panel_ndx = ctl->panel_ndx;
#endif
	ctx->pp_num = mixer->num;
	init_completion(&ctx->pp_comp);
	init_completion(&ctx->stop_comp);
	spin_lock_init(&ctx->clk_lock);
#if defined(CONFIG_FB_MSM_MIPI_SAMSUNG_OCTA_CMD_WQHD_PT_PANEL)
	spin_lock_init(&ctx->te_lock);
#endif
	mutex_init(&ctx->clk_mtx);
	INIT_WORK(&ctx->clk_work, clk_ctrl_work);
	INIT_DELAYED_WORK(&ctx->ulps_work, __mdss_mdp_cmd_ulps_work);
	INIT_WORK(&ctx->pp_done_work, pingpong_done_work);
	atomic_set(&ctx->pp_done_cnt, 0);
	INIT_LIST_HEAD(&ctx->vsync_handlers);

	ctx->recovery.fxn = mdss_mdp_cmd_underflow_recovery;
	ctx->recovery.data = ctx;

	pr_debug("%s: ctx=%p num=%d mixer=%d\n", __func__,
				ctx, ctx->pp_num, mixer->num);
	MDSS_XLOG(ctl->num, ctx->koff_cnt, ctx->clk_enabled,
					ctx->rdptr_enabled);

	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_RD_PTR, ctx->pp_num,
				   mdss_mdp_cmd_readptr_done, ctl);

#if defined (CONFIG_FB_MSM_MDSS_DSI_DBG)
	xlog(__func__, ctl->num, ctx->koff_cnt, ctx->clk_enabled, ctx->rdptr_enabled, 0, 0);
#endif
	mdss_mdp_set_intr_callback(MDSS_MDP_IRQ_PING_PONG_COMP, ctx->pp_num,
				   mdss_mdp_cmd_pingpong_done, ctl);

	ret = mdss_mdp_cmd_tearcheck_setup(ctl);

	if (ret) {
		pr_err("tearcheck setup failed\n");
		return ret;
	}

	ctl->stop_fnc = mdss_mdp_cmd_stop;
	ctl->display_fnc = mdss_mdp_cmd_kickoff;
	ctl->wait_pingpong = mdss_mdp_cmd_wait4pingpong;
	ctl->add_vsync_handler = mdss_mdp_cmd_add_vsync_handler;
	ctl->remove_vsync_handler = mdss_mdp_cmd_remove_vsync_handler;
	ctl->read_line_cnt_fnc = mdss_mdp_cmd_line_count;
	pr_debug("%s:-\n", __func__);

	return 0;
}

