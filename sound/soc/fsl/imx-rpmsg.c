// SPDX-License-Identifier: GPL-2.0+
// Copyright 2017-2020 NXP

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include <sound/control.h>
#include <sound/pcm_params.h>
#include <sound/soc-dapm.h>
#include "imx-pcm-rpmsg.h"

struct imx_rpmsg {
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
	unsigned long sysclk;
	bool lpa;
};

static struct dev_pm_ops lpa_pm;

static const struct snd_soc_dapm_widget imx_rpmsg_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone Jack", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_MIC("Mic Jack", NULL),
	SND_SOC_DAPM_MIC("Main MIC", NULL),
};

static int imx_rpmsg_late_probe(struct snd_soc_card *card)
{
	struct imx_rpmsg *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd = list_first_entry(&card->rtd_list,
							   struct snd_soc_pcm_runtime, list);
	struct snd_soc_dai *codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	struct device *dev = card->dev;
	int ret;

	if (data->lpa) {
		struct snd_soc_component *codec_comp;
		struct device_node *codec_np;
		struct device_driver *codec_drv;
		struct device *codec_dev = NULL;

		codec_np = data->dai.codecs->of_node;
		if (codec_np) {
			struct platform_device *codec_pdev;
			struct i2c_client *codec_i2c;

			codec_i2c = of_find_i2c_device_by_node(codec_np);
			if (codec_i2c)
				codec_dev = &codec_i2c->dev;
			if (!codec_dev) {
				codec_pdev = of_find_device_by_node(codec_np);
				if (codec_pdev)
					codec_dev = &codec_pdev->dev;
			}
		}
		if (codec_dev) {
			codec_comp = snd_soc_lookup_component_nolocked(codec_dev, NULL);
			if (codec_comp) {
				int i, num_widgets;
				const char *widgets;
				struct snd_soc_dapm_context *dapm;

				num_widgets = of_property_count_strings(data->card.dev->of_node,
									"ignore-suspend-widgets");
				for (i = 0; i < num_widgets; i++) {
					of_property_read_string_index(data->card.dev->of_node,
								      "ignore-suspend-widgets",
								      i, &widgets);
					dapm = snd_soc_component_get_dapm(codec_comp);
					snd_soc_dapm_ignore_suspend(dapm, widgets);
				}
			}
			codec_drv = codec_dev->driver;
			if (codec_drv->pm) {
				memcpy(&lpa_pm, codec_drv->pm, sizeof(lpa_pm));
				lpa_pm.suspend = NULL;
				lpa_pm.resume = NULL;
				lpa_pm.freeze = NULL;
				lpa_pm.thaw = NULL;
				lpa_pm.poweroff = NULL;
				lpa_pm.restore = NULL;
				codec_drv->pm = &lpa_pm;
			}
			put_device(codec_dev);
		}
	}

	if (!data->sysclk)
		return 0;

	ret = snd_soc_dai_set_sysclk(codec_dai, 0, data->sysclk, SND_SOC_CLOCK_IN);
	if (ret && ret != -ENOTSUPP) {
		dev_err(dev, "failed to set sysclk in %s\n", __func__);
		return ret;
	}

	return 0;
}

static int imx_rpmsg_probe(struct platform_device *pdev)
{
	struct snd_soc_dai_link_component *dlc;
	struct snd_soc_dai *cpu_dai;
	struct device_node *np = NULL;
	struct of_phandle_args args;
	const char *platform_name;
	struct imx_rpmsg *data;
	int ret = 0;

	dlc = devm_kzalloc(&pdev->dev, 3 * sizeof(*dlc), GFP_KERNEL);
	if (!dlc)
		return -ENOMEM;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data) {
		ret = -ENOMEM;
		goto fail;
	}

	data->dai.cpus = &dlc[0];
	data->dai.num_cpus = 1;
	data->dai.platforms = &dlc[1];
	data->dai.num_platforms = 1;
	data->dai.codecs = &dlc[2];
	data->dai.num_codecs = 1;

	data->dai.name = "rpmsg hifi";
	data->dai.stream_name = "rpmsg hifi";
	data->dai.dai_fmt = SND_SOC_DAIFMT_I2S |
			    SND_SOC_DAIFMT_NB_NF |
			    SND_SOC_DAIFMT_CBC_CFC;

	/*
	 * i.MX rpmsg sound cards work on codec slave mode. MCLK will be
	 * disabled by CPU DAI driver in hw_free(). Some codec requires MCLK
	 * present at power up/down sequence. So need to set ignore_pmdown_time
	 * to power down codec immediately before MCLK is turned off.
	 */
	data->dai.ignore_pmdown_time = 1;

	data->dai.cpus->dai_name = pdev->dev.platform_data;
	cpu_dai = snd_soc_find_dai(data->dai.cpus);
	if (!cpu_dai) {
		ret = -EPROBE_DEFER;
		goto fail;
	}
	np = cpu_dai->dev->of_node;
	if (!np) {
		dev_err(&pdev->dev, "failed to parse CPU DAI device node\n");
		ret = -ENODEV;
		goto fail;
	}

	ret = of_reserved_mem_device_init_by_idx(&pdev->dev, np, 0);
	if (ret)
		dev_warn(&pdev->dev, "no reserved DMA memory\n");

	/* Optional codec node */
	ret = of_parse_phandle_with_fixed_args(np, "audio-codec", 0, 0, &args);
	if (ret) {
		*data->dai.codecs = snd_soc_dummy_dlc;
	} else {
		struct clk *clk;

		ret = snd_soc_get_dlc(&args, data->dai.codecs);
		if (ret) {
			dev_err(&pdev->dev, "Unable to get codec_dai_name\n");
			goto fail;
		}

		clk = devm_get_clk_from_child(&pdev->dev, args.np, NULL);
		if (!IS_ERR(clk))
			data->sysclk = clk_get_rate(clk);
	}

	if (!of_property_read_string(np, "fsl,rpmsg-channel-name", &platform_name))
		data->dai.platforms->name = platform_name;
	else
		data->dai.platforms->name = "rpmsg-audio-channel";
	data->dai.playback_only = true;
	data->dai.capture_only = true;
	data->card.num_links = 1;
	data->card.dai_link = &data->dai;

	if (of_property_read_bool(np, "fsl,rpmsg-out"))
		data->dai.capture_only = false;

	if (of_property_read_bool(np, "fsl,rpmsg-in"))
		data->dai.playback_only = false;

	if (data->dai.playback_only && data->dai.capture_only) {
		dev_err(&pdev->dev, "no enabled rpmsg DAI link\n");
		ret = -EINVAL;
		goto fail;
	}

	if (of_property_read_bool(np, "fsl,enable-lpa"))
		data->lpa = true;

	data->card.dev = &pdev->dev;
	data->card.owner = THIS_MODULE;
	data->card.dapm_widgets = imx_rpmsg_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_rpmsg_dapm_widgets);
	data->card.late_probe = imx_rpmsg_late_probe;
	/*
	 * Inoder to use common api to get card name and audio routing.
	 * Use parent of_node for this device, revert it after finishing using
	 */
	data->card.dev->of_node = np;

	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret)
		goto fail;

	if (of_property_present(np, "audio-routing")) {
		ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
		if (ret) {
			dev_err(&pdev->dev, "failed to parse audio-routing: %d\n", ret);
			goto fail;
		}
	}

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);
	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "snd_soc_register_card failed\n");
		goto fail;
	}

fail:
	pdev->dev.of_node = NULL;
	return ret;
}

static struct platform_driver imx_rpmsg_driver = {
	.driver = {
		.name = "imx-audio-rpmsg",
		.pm = &snd_soc_pm_ops,
	},
	.probe = imx_rpmsg_probe,
};
module_platform_driver(imx_rpmsg_driver);

MODULE_DESCRIPTION("Freescale SoC Audio RPMSG Machine Driver");
MODULE_AUTHOR("Shengjiu Wang <shengjiu.wang@nxp.com>");
MODULE_ALIAS("platform:imx-audio-rpmsg");
MODULE_LICENSE("GPL v2");
