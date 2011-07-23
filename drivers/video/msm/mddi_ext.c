/* Copyright (c) 2008-2009, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Code Aurora Forum nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * Alternatively, provided that this notice is retained in full, this software
 * may be relicensed by the recipient under the terms of the GNU General Public
 * License version 2 ("GPL") and only version 2, in which case the provisions of
 * the GPL apply INSTEAD OF those given above.  If the recipient relicenses the
 * software under the GPL, then the identification text in the MODULE_LICENSE
 * macro must be changed to reflect "GPLv2" instead of "Dual BSD/GPL".  Once a
 * recipient changes the license terms to the GPL, subsequent recipients shall
 * not relicense under alternate licensing terms, including the BSD or dual
 * BSD/GPL terms.  In addition, the following license statement immediately
 * below and between the words START and END shall also then apply when this
 * software is relicensed under the GPL:
 *
 * START
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 and only version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * END
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <mach/hardware.h>
#include <asm/io.h>

#include <asm/system.h>
#include <asm/mach-types.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/clk.h>
#include <mach/clk.h>
#include <linux/platform_device.h>

#include "msm_fb.h"
#include "mddihosti.h"

static int mddi_ext_probe(struct platform_device *pdev);
static int mddi_ext_remove(struct platform_device *pdev);

static int mddi_ext_off(struct platform_device *pdev);
static int mddi_ext_on(struct platform_device *pdev);

static struct platform_device *pdev_list[MSM_FB_MAX_DEV_LIST];
static int pdev_list_cnt;

static int mddi_ext_suspend(struct platform_device *pdev, pm_message_t state);
static int mddi_ext_resume(struct platform_device *pdev);

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mddi_ext_early_suspend(struct early_suspend *h);
static void mddi_ext_early_resume(struct early_suspend *h);
#endif

static struct platform_driver mddi_ext_driver = {
	.probe = mddi_ext_probe,
	.remove = mddi_ext_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
#ifdef CONFIG_PM
	.suspend = mddi_ext_suspend,
	.resume = mddi_ext_resume,
#endif
#endif
	.resume_early = NULL,
	.resume = NULL,
	.shutdown = NULL,
	.driver = {
		   .name = "mddi_ext",
		   },
};

static struct clk *mddi_ext_clk;
static struct clk *mddi_ext_pclk;
static struct mddi_platform_data *mddi_ext_pdata;

extern int int_mddi_ext_flag;

static int mddi_ext_off(struct platform_device *pdev)
{
	int ret = 0;

	ret = panel_next_off(pdev);
	mddi_host_stop_ext_display();

	return ret;
}

static int mddi_ext_on(struct platform_device *pdev)
{
	int ret = 0;
	u32 clk_rate;
	struct msm_fb_data_type *mfd;

	mfd = platform_get_drvdata(pdev);

	clk_rate = mfd->fbi->var.pixclock;
	clk_rate = min(clk_rate, mfd->panel_info.clk_max);

	if (mddi_ext_pdata &&
	    mddi_ext_pdata->mddi_sel_clk &&
	    mddi_ext_pdata->mddi_sel_clk(&clk_rate))
		printk(KERN_ERR
			  "%s: can't select mddi io clk targate rate = %d\n",
			  __func__, clk_rate);

	if (clk_set_min_rate(mddi_ext_clk, clk_rate) < 0)
		printk(KERN_ERR "%s: clk_set_min_rate failed\n",
			__func__);

	mddi_host_start_ext_display();
	ret = panel_next_on(pdev);

	return ret;
}

static int mddi_ext_resource_initialized;

static int mddi_ext_probe(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;
	struct platform_device *mdp_dev = NULL;
	struct msm_fb_panel_data *pdata = NULL;
	int rc;
	resource_size_t size ;
	u32 clk_rate;

	if ((pdev->id == 0) && (pdev->num_resources >= 0)) {
		mddi_ext_pdata = pdev->dev.platform_data;

		size =  resource_size(&pdev->resource[0]);
		msm_emdh_base = ioremap(pdev->resource[0].start, size);

		MSM_FB_INFO("external mddi base address = 0x%x\n",
				pdev->resource[0].start);

		if (unlikely(!msm_emdh_base))
			return -ENOMEM;

		mddi_ext_resource_initialized = 1;
		return 0;
	}

	if (!mddi_ext_resource_initialized)
		return -EPERM;

	mfd = platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	if (pdev_list_cnt >= MSM_FB_MAX_DEV_LIST)
		return -ENOMEM;

	mdp_dev = platform_device_alloc("mdp", pdev->id);
	if (!mdp_dev)
		return -ENOMEM;

	/*
	 * link to the latest pdev
	 */
	mfd->pdev = mdp_dev;
	mfd->dest = DISPLAY_EXT_MDDI;

	/*
	 * alloc panel device data
	 */
	if (platform_device_add_data
	    (mdp_dev, pdev->dev.platform_data,
	     sizeof(struct msm_fb_panel_data))) {
		printk(KERN_ERR "mddi_ext_probe: platform_device_add_data failed!\n");
		platform_device_put(mdp_dev);
		return -ENOMEM;
	}
	/*
	 * data chain
	 */
	pdata = mdp_dev->dev.platform_data;
	pdata->on = mddi_ext_on;
	pdata->off = mddi_ext_off;
	pdata->next = pdev;

	/*
	 * get/set panel specific fb info
	 */
	mfd->panel_info = pdata->panel_info;
	mfd->fb_imgType = MDP_RGB_565;

	clk_rate = mfd->panel_info.clk_max;
	if (mddi_ext_pdata &&
	    mddi_ext_pdata->mddi_sel_clk &&
	    mddi_ext_pdata->mddi_sel_clk(&clk_rate))
			printk(KERN_ERR
			  "%s: can't select mddi io clk targate rate = %d\n",
			  __func__, clk_rate);

	if (clk_set_max_rate(mddi_ext_clk, clk_rate) < 0)
		printk(KERN_ERR "%s: clk_set_max_rate failed\n", __func__);
	mfd->panel_info.clk_rate = mfd->panel_info.clk_min;

	/*
	 * set driver data
	 */
	platform_set_drvdata(mdp_dev, mfd);

	/*
	 * register in mdp driver
	 */
	rc = platform_device_add(mdp_dev);
	if (rc)
		goto mddi_ext_probe_err;

	pdev_list[pdev_list_cnt++] = pdev;

#ifdef CONFIG_HAS_EARLYSUSPEND
	mfd->mddi_ext_early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	mfd->mddi_ext_early_suspend.suspend = mddi_ext_early_suspend;
	mfd->mddi_ext_early_suspend.resume = mddi_ext_early_resume;
	register_early_suspend(&mfd->mddi_ext_early_suspend);
#endif

	return 0;

mddi_ext_probe_err:
	platform_device_put(mdp_dev);
	return rc;
}

static int mddi_ext_is_in_suspend;

static int mddi_ext_suspend(struct platform_device *pdev, pm_message_t state)
{
	if (mddi_ext_is_in_suspend)
		return 0;

	mddi_ext_is_in_suspend = 1;

	if (clk_set_min_rate(mddi_ext_clk, 0) < 0)
		printk(KERN_ERR "%s: clk_set_min_rate failed\n", __func__);

	clk_disable(mddi_ext_clk);
	if (mddi_ext_pclk)
		clk_disable(mddi_ext_pclk);

	disable_irq(INT_MDDI_EXT);

	return 0;
}

static int mddi_ext_resume(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;

	mfd = platform_get_drvdata(pdev);

	if (!mddi_ext_is_in_suspend)
		return 0;

	mddi_ext_is_in_suspend = 0;
	enable_irq(INT_MDDI_EXT);

	clk_enable(mddi_ext_clk);
	if (mddi_ext_pclk)
		clk_enable(mddi_ext_pclk);

	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void mddi_ext_early_suspend(struct early_suspend *h)
{
	pm_message_t state;
	struct msm_fb_data_type *mfd = container_of(h, struct msm_fb_data_type,
							mddi_ext_early_suspend);

	state.event = PM_EVENT_SUSPEND;
	mddi_ext_suspend(mfd->pdev, state);
}

static void mddi_ext_early_resume(struct early_suspend *h)
{
	struct msm_fb_data_type *mfd = container_of(h, struct msm_fb_data_type,
							mddi_ext_early_suspend);
	mddi_ext_resume(mfd->pdev);
}
#endif

static int mddi_ext_remove(struct platform_device *pdev)
{
	iounmap(msm_emdh_base);
	return 0;
}

static int mddi_ext_register_driver(void)
{
	return platform_driver_register(&mddi_ext_driver);
}

static int __init mddi_ext_driver_init(void)
{
	int ret;

	mddi_ext_clk = clk_get(NULL, "emdh_clk");
	if (IS_ERR(mddi_ext_clk)) {
		printk(KERN_ERR "can't find emdh_clk\n");
		return PTR_ERR(mddi_ext_clk);
	}
	clk_enable(mddi_ext_clk);

	mddi_ext_pclk = clk_get(NULL, "emdh_pclk");
	if (IS_ERR(mddi_ext_pclk))
		mddi_ext_pclk = NULL;
	else
		clk_enable(mddi_ext_pclk);

	ret = mddi_ext_register_driver();
	if (ret) {
		clk_disable(mddi_ext_clk);
		clk_put(mddi_ext_clk);
		if (mddi_ext_pclk) {
			clk_disable(mddi_ext_pclk);
			clk_put(mddi_ext_pclk);
		}
		printk(KERN_ERR "mddi_ext_register_driver() failed!\n");
		return ret;
	}
	mddi_init();

	return ret;
}

module_init(mddi_ext_driver_init);