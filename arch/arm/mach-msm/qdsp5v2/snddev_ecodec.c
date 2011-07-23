/* Copyright (c) 2009, Code Aurora Forum. All rights reserved.
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
#include <mach/debug_audio_mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <asm/uaccess.h>
#include <mach/qdsp5v2/snddev_ecodec.h>
#include <mach/qdsp5v2/audio_dev_ctl.h>
#include <mach/qdsp5v2/audio_interct.h>
#include <mach/qdsp5v2/aux_pcm.h>
#include <mach/qdsp5v2/afe.h>

/* Context for each external codec device */
struct snddev_ecodec_state {
	struct snddev_ecodec_data *data;
	u32 sample_rate;
	bool enabled;
};

/* Global state for the driver */
struct snddev_ecodec_drv_state {
	struct mutex dev_lock;
	u32 rx_active; /* ensure one rx device at a time */
	u32 tx_active; /* ensure one tx device at a time */
	struct clk *lpa_core_clk;
	struct clk *ecodec_clk;
};

#define ADSP_CTL 1

static struct snddev_ecodec_drv_state snddev_ecodec_drv;

static int snddev_ecodec_open_rx(struct snddev_ecodec_state *ecodec)
{
	int rc = 0;
	struct snddev_ecodec_drv_state *drv = &snddev_ecodec_drv;
	struct msm_afe_config afe_config;

	MM_INFO(" snddev_ecodec_open_rx\n");

	if (!drv->tx_active) {
		/* request GPIO */
		rc = aux_pcm_gpios_request();
		if (rc) {
			MM_ERR("GPIO enable failed\n");
			goto done;
		}
		/* config clocks */
		clk_enable(drv->lpa_core_clk);

		/* enable ecodec clk */
		clk_enable(drv->ecodec_clk);

		/* let ADSP confiure AUX PCM regs */
		aux_codec_adsp_codec_ctl_en(ADSP_CTL);

		/* let adsp configure pcm path */
		aux_codec_pcm_path_ctl_en(ADSP_CTL);

		/* choose ADSP_A */
		audio_interct_aux_regsel(AUDIO_ADSP_A);
		audio_interct_tpcm_source(AUDIO_ADSP_A);
		audio_interct_rpcm_source(AUDIO_ADSP_A);

		clk_disable(drv->lpa_core_clk);

		/* send AUX_CODEC_CONFIG to AFE */
		rc = afe_config_aux_codec(ecodec->data->conf_pcm_ctl_val,
				ecodec->data->conf_aux_codec_intf,
				ecodec->data->conf_data_format_padding_val);
		if (IS_ERR_VALUE(rc))
			goto error;
	}
	/* send CODEC CONFIG to AFE */
	afe_config.sample_rate = ecodec->sample_rate / 1000;
	afe_config.channel_mode = ecodec->data->channel_mode;
	afe_config.volume = AFE_VOLUME_UNITY;
	rc = afe_enable(AFE_HW_PATH_AUXPCM_RX, &afe_config);
	if (IS_ERR_VALUE(rc)) {
		if (!drv->tx_active) {
			aux_pcm_gpios_free();
			clk_disable(drv->ecodec_clk);
		}
		goto done;
	}

	ecodec->enabled = 1;
	return 0;

error:
	aux_pcm_gpios_free();
	clk_disable(drv->ecodec_clk);
done:
	return rc;
}

static int snddev_ecodec_close_rx(struct snddev_ecodec_state *ecodec)
{
	struct snddev_ecodec_drv_state *drv = &snddev_ecodec_drv;

	/* free GPIO */
	if (!drv->tx_active) {
		aux_pcm_gpios_free();
		clk_disable(drv->ecodec_clk);
	}

	/* disable AFE */
	afe_disable(AFE_HW_PATH_AUXPCM_RX);

	ecodec->enabled = 0;

	return 0;
}

static int snddev_ecodec_open_tx(struct snddev_ecodec_state *ecodec)
{
	int rc = 0;
	struct snddev_ecodec_drv_state *drv = &snddev_ecodec_drv;
	struct msm_afe_config afe_config;

	MM_INFO(" snddev_ecodec_open_tx \n");

	/* request GPIO */
	if (!drv->rx_active) {
		rc = aux_pcm_gpios_request();
		if (rc) {
			MM_ERR("GPIO enable failed\n");
			goto done;
		}
		/* config clocks */
		clk_enable(drv->lpa_core_clk);

		/* enable ecodec clk */
		clk_enable(drv->ecodec_clk);

		/* let ADSP confiure AUX PCM regs */
		aux_codec_adsp_codec_ctl_en(ADSP_CTL);

		/* let adsp configure pcm path */
		aux_codec_pcm_path_ctl_en(ADSP_CTL);

		/* choose ADSP_A */
		audio_interct_aux_regsel(AUDIO_ADSP_A);
		audio_interct_tpcm_source(AUDIO_ADSP_A);
		audio_interct_rpcm_source(AUDIO_ADSP_A);

		clk_disable(drv->lpa_core_clk);

		/* send AUX_CODEC_CONFIG to AFE */
		rc = afe_config_aux_codec(ecodec->data->conf_pcm_ctl_val,
			ecodec->data->conf_aux_codec_intf,
			ecodec->data->conf_data_format_padding_val);
		if (IS_ERR_VALUE(rc))
			goto error;
	}
	/* send CODEC CONFIG to AFE */
	afe_config.sample_rate = ecodec->sample_rate / 1000;
	afe_config.channel_mode = ecodec->data->channel_mode;
	afe_config.volume = AFE_VOLUME_UNITY;
	rc = afe_enable(AFE_HW_PATH_AUXPCM_TX, &afe_config);
	if (IS_ERR_VALUE(rc)) {
		if (!drv->rx_active) {
			aux_pcm_gpios_free();
			clk_disable(drv->ecodec_clk);
		}
		goto done;
	}

	ecodec->enabled = 1;
	return 0;

error:
	clk_disable(drv->ecodec_clk);
	aux_pcm_gpios_free();
done:
	return rc;
}

static int snddev_ecodec_close_tx(struct snddev_ecodec_state *ecodec)
{
	struct snddev_ecodec_drv_state *drv = &snddev_ecodec_drv;

	/* free GPIO */
	if (!drv->rx_active) {
		aux_pcm_gpios_free();
		clk_disable(drv->ecodec_clk);
	}

	/* disable AFE */
	afe_disable(AFE_HW_PATH_AUXPCM_TX);

	ecodec->enabled = 0;

	return 0;
}


static int snddev_ecodec_open(struct msm_snddev_info *dev_info)
{
	int rc = 0;
	struct snddev_ecodec_state *ecodec;
	struct snddev_ecodec_drv_state *drv = &snddev_ecodec_drv;

	if (!dev_info) {
		rc = -EINVAL;
		goto error;
	}

	ecodec = dev_info->private_data;

	if (ecodec->data->capability & SNDDEV_CAP_RX) {
		mutex_lock(&drv->dev_lock);
		if (drv->rx_active) {
			mutex_unlock(&drv->dev_lock);
			rc = -EBUSY;
			goto error;
		}
		rc = snddev_ecodec_open_rx(ecodec);
		if (!IS_ERR_VALUE(rc))
			drv->rx_active = 1;
		mutex_unlock(&drv->dev_lock);
	} else {
		mutex_lock(&drv->dev_lock);
		if (drv->tx_active) {
			mutex_unlock(&drv->dev_lock);
			rc = -EBUSY;
			goto error;
		}
		rc = snddev_ecodec_open_tx(ecodec);
		if (!IS_ERR_VALUE(rc))
			drv->tx_active = 1;
		mutex_unlock(&drv->dev_lock);
	}
error:
	return rc;
}

static int snddev_ecodec_close(struct msm_snddev_info *dev_info)
{
	int rc = 0;
	struct snddev_ecodec_state *ecodec;
	struct snddev_ecodec_drv_state *drv = &snddev_ecodec_drv;
	if (!dev_info) {
		rc = -EINVAL;
		goto error;
	}

	ecodec = dev_info->private_data;

	if (ecodec->data->capability & SNDDEV_CAP_RX) {
		mutex_lock(&drv->dev_lock);
		if (!drv->rx_active) {
			mutex_unlock(&drv->dev_lock);
			rc = -EPERM;
			goto error;
		}
		rc = snddev_ecodec_close_rx(ecodec);
		if (!IS_ERR_VALUE(rc))
			drv->rx_active = 0;
		mutex_unlock(&drv->dev_lock);
	} else {
		mutex_lock(&drv->dev_lock);
		if (!drv->tx_active) {
			mutex_unlock(&drv->dev_lock);
			rc = -EPERM;
			goto error;
		}
		rc = snddev_ecodec_close_tx(ecodec);
		if (!IS_ERR_VALUE(rc))
			drv->tx_active = 0;
		mutex_unlock(&drv->dev_lock);
	}

error:
	return rc;
}

static int snddev_ecodec_set_freq(struct msm_snddev_info *dev_info, u32 rate)
{
	int rc = 0;

	if (!dev_info) {
		rc = -EINVAL;
		goto error;
	}
	return 8000;

error:
	return rc;
}

static int snddev_ecodec_probe(struct platform_device *pdev)
{
	int rc = 0, i;
	struct snddev_ecodec_data *pdata;
	struct msm_snddev_info *dev_info;
	struct snddev_ecodec_state *ecodec;

	if (!pdev || !pdev->dev.platform_data) {
		printk(KERN_ALERT "Invalid caller \n");
		rc = -1;
		goto error;
	}
	pdata = pdev->dev.platform_data;

	ecodec = kzalloc(sizeof(struct snddev_ecodec_state), GFP_KERNEL);
	if (!ecodec) {
		rc = -ENOMEM;
		goto error;
	}

	dev_info = kzalloc(sizeof(struct msm_snddev_info), GFP_KERNEL);
	if (!dev_info) {
		kfree(ecodec);
		rc = -ENOMEM;
		goto error;
	}

	dev_info->name = pdata->name;
	dev_info->copp_id = pdata->copp_id;
	dev_info->acdb_id = pdata->acdb_id;
	dev_info->private_data = (void *) ecodec;
	dev_info->dev_ops.open = snddev_ecodec_open;
	dev_info->dev_ops.close = snddev_ecodec_close;
	dev_info->dev_ops.set_freq = snddev_ecodec_set_freq;
	dev_info->dev_ops.enable_sidetone = NULL;
	dev_info->capability = pdata->capability;
	dev_info->opened = 0;

	msm_snddev_register(dev_info);
	ecodec->data = pdata;
	ecodec->sample_rate = 8000; /* Default to 8KHz */
	 if (pdata->capability & SNDDEV_CAP_RX) {
		for (i = 0; i < VOC_RX_VOL_ARRAY_NUM; i++) {
			dev_info->max_voc_rx_vol[i] =
				pdata->max_voice_rx_vol[i];
			dev_info->min_voc_rx_vol[i] =
				pdata->min_voice_rx_vol[i];
		}
	}
error:
	return rc;
}

static int snddev_ecodec_remove(struct platform_device *pdev)
{
	return 0;
}

static struct platform_driver snddev_ecodec_driver = {
	.probe = snddev_ecodec_probe,
	.remove = snddev_ecodec_remove,
	.driver = { .name = "msm_snddev_ecodec" }
};

static int __init snddev_ecodec_init(void)
{
	int rc = 0;
	struct snddev_ecodec_drv_state *ecodec_drv = &snddev_ecodec_drv;

	MM_INFO(" snddev_ecodec_init \n");
	rc = platform_driver_register(&snddev_ecodec_driver);
	if (IS_ERR_VALUE(rc))
		goto error_platform_driver;
	ecodec_drv->ecodec_clk = clk_get(NULL, "ecodec_clk");
	if (IS_ERR(ecodec_drv->ecodec_clk))
		goto error_ecodec_clk;
	ecodec_drv->lpa_core_clk = clk_get(NULL, "lpa_core_clk");
	if (IS_ERR(ecodec_drv->lpa_core_clk))
		goto error_lpa_core_clk;


	mutex_init(&ecodec_drv->dev_lock);
	ecodec_drv->rx_active = 0;
	ecodec_drv->tx_active = 0;
	return 0;

error_lpa_core_clk:
	clk_put(ecodec_drv->ecodec_clk);
error_ecodec_clk:
	platform_driver_unregister(&snddev_ecodec_driver);
error_platform_driver:

	MM_ERR("%s: encounter error\n", __func__);
	return -ENODEV;
}

static void __exit snddev_ecodec_exit(void)
{
	struct snddev_ecodec_drv_state *ecodec_drv = &snddev_ecodec_drv;

	platform_driver_unregister(&snddev_ecodec_driver);
	clk_put(ecodec_drv->ecodec_clk);

	return;
}

module_init(snddev_ecodec_init);
module_exit(snddev_ecodec_exit);

MODULE_DESCRIPTION("ECodec Sound Device driver");
MODULE_VERSION("1.0");
MODULE_LICENSE("Dual BSD/GPL");
