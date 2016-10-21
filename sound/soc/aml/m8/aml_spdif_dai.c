/*
	Amlogic S/PDIF(HDMI)  Soc dai driver
	author:jian.xu@amlogic.com
*/
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/ioctl.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/soundcard.h>
#include <linux/timer.h>
#include <linux/debugfs.h>
#include <linux/major.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <mach/am_regs.h>
#include <mach/pinmux.h>
#include "aml_audio_hw.h"
#include "aml_spdif_dai.h"
#include "aml_i2s.h"
#include <linux/of.h>

/* #define DEBUG_ALSA_SPDIF_DAI */
#define ALSA_PRINT(fmt, args...)	printk(KERN_INFO "[aml-spdif-dai::%s] " fmt, __func__, ##args)
#ifdef DEBUG_ALSA_SPDIF_DAI
#define ALSA_DEBUG(fmt, args...) 	printk(KERN_INFO "[aml-spdif-dai] " fmt, ##args)
#define ALSA_TRACE()     			printk("[aml-spdif-dai] enter func %s,line %d\n", __func__, __LINE__)
#else
#define ALSA_DEBUG(fmt, args...)
#define ALSA_TRACE()
#endif
static unsigned last_iec_clock = -1;
extern int aout_notifier_call_chain(unsigned long val, void *v);
/* static  unsigned  playback_substream_handle = 0 ; */
extern unsigned int IEC958_mode_codec;
static int iec958buf[32 + 16];
void aml_spdif_play(void)
{
#if 0
	_aiu_958_raw_setting_t set;
	_aiu_958_channel_status_t chstat;
	struct snd_pcm_substream substream;
	struct snd_pcm_runtime runtime;
	substream.runtime = &runtime;
	runtime.rate = 48000;
	runtime.format = SNDRV_PCM_FORMAT_S16_LE;
	runtime.channels = 2;
	runtime.sample_bits = 16;
	memset((void *)(&set), 0, sizeof(set));
	memset((void *)(&chstat), 0, sizeof(chstat));
	set.chan_stat = &chstat;
	set.chan_stat->chstat0_l = 0x0100;
	set.chan_stat->chstat0_r = 0x0100;
	set.chan_stat->chstat1_l = 0X200;
	set.chan_stat->chstat1_r = 0X200;
	audio_hw_958_enable(0);
	if (last_iec_clock != AUDIO_CLK_FREQ_48) {
		ALSA_PRINT("enterd %s,set_clock:%d,sample_rate=%d\n", __func__,
			   last_iec_clock, AUDIO_CLK_FREQ_48);
		last_iec_clock = AUDIO_CLK_FREQ_48;
		audio_set_958_clk(AUDIO_CLK_FREQ_48, AUDIO_CLK_256FS);
	}
	audio_util_set_dac_958_format(AUDIO_ALGOUT_DAC_FORMAT_DSP);
	memset(iec958buf, 0, sizeof(iec958buf));
	audio_set_958outbuf((virt_to_phys(iec958buf) + 63) & (~63), 128, 0);
	audio_set_958_mode(AIU_958_MODE_PCM16, &set);
#if OVERCLOCK == 1 || IEC958_OVERCLOCK == 1
	WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 3, 4, 2);	/* 512fs divide 4 == 128fs */
#else
	WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 1, 4, 2);	/* 256fs divide 2 == 128fs */
#endif
	aout_notifier_call_chain(AOUT_EVENT_IEC_60958_PCM, &substream);
	audio_spdifout_pg_enable(1);
	audio_hw_958_enable(1);
#endif
}

static int aml_dai_spdif_set_sysclk(struct snd_soc_dai *cpu_dai,
				    int clk_id, unsigned int freq, int dir)
{
	ALSA_PRINT("clk_id:%d, freq:%u, dir:%d\n", clk_id, freq, dir);
	return 0;
}

static int aml_dai_spdif_trigger(struct snd_pcm_substream *substream, int cmd,
				 struct snd_soc_dai *dai)
{
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			ALSA_PRINT("aiu 958 playback enable\n");
			audio_hw_958_enable(1);
		} else {
			ALSA_PRINT("spdif in capture enable\n");
			audio_in_spdif_enable(1);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			ALSA_PRINT("aiu 958 playback disable\n");
			audio_hw_958_enable(0);
		} else {
			ALSA_PRINT("spdif in capture disable\n");
			audio_in_spdif_enable(0);
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/*
special call by the audiodsp,add these code,as there are three cases for 958 s/pdif output
1)NONE-PCM  raw output ,only available when ac3/dts audio,when raw output mode is selected by user.
2)PCM  output for  all audio, when pcm mode is selected by user .
3)PCM  output for audios except ac3/dts,when raw output mode is selected by user
*/
void aml_hw_iec958_init(struct snd_pcm_substream *substream, int samesrc)
{
#if 0
	_aiu_958_raw_setting_t set;
	_aiu_958_channel_status_t chstat;
	unsigned iec958_mode;
	int sample_rate;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	struct snd_pcm_runtime *runtime = substream->runtime;
	if (buf == NULL || runtime == NULL) {
		printk("buf/0x%x runtime/0x%x\n", (unsigned)buf,
			(unsigned)runtime);
		return;
	}

	iec958_mode = AIU_958_MODE_PCM16;
	memset((void *)(&set), 0, sizeof(set));
	memset((void *)(&chstat), 0, sizeof(chstat));
	set.chan_stat = &chstat;
	printk("----aml_hw_iec958_init,runtime->rate=%d,runtime->channels=%d,samesrc=%d--\n", runtime->rate, runtime->channels, samesrc);

	sample_rate = (runtime->rate == 192000 && runtime->channels == 2 && runtime->format == SNDRV_PCM_FORMAT_S16) ? runtime->rate >> 2 : runtime->rate;
	switch (sample_rate) {
	case 192000:
		sample_rate = AUDIO_CLK_FREQ_192;
		break;
	case 176400:
		sample_rate = AUDIO_CLK_FREQ_1764;
		break;
	case 96000:
		sample_rate = AUDIO_CLK_FREQ_96;
		break;
	case 88200:
		sample_rate = AUDIO_CLK_FREQ_882;
		break;
	case 48000:
		sample_rate = AUDIO_CLK_FREQ_48;
		break;
	case 44100:
		sample_rate = AUDIO_CLK_FREQ_441;
		break;
	case 32000:
		sample_rate = AUDIO_CLK_FREQ_32;
		break;
	case 8000:
		sample_rate = AUDIO_CLK_FREQ_8;
		break;
	case 11025:
		sample_rate = AUDIO_CLK_FREQ_11;
		break;
	case 16000:
		sample_rate = AUDIO_CLK_FREQ_16;
		break;
	case 22050:
		sample_rate = AUDIO_CLK_FREQ_22;
		break;
	case 12000:
		sample_rate = AUDIO_CLK_FREQ_12;
		break;
	case 24000:
		sample_rate = AUDIO_CLK_FREQ_22;
		break;
	default:
		sample_rate = AUDIO_CLK_FREQ_441;
		break;
	};

	if (!samesrc) {
		unsigned i2s_mode = AIU_I2S_MODE_PCM16;
		switch (runtime->format) {
		case SNDRV_PCM_FORMAT_S32:
			i2s_mode = AIU_I2S_MODE_PCM32;
			break;
		case SNDRV_PCM_FORMAT_S24:
			i2s_mode = AIU_I2S_MODE_PCM24;
			break;
		case SNDRV_PCM_FORMAT_S16:
			i2s_mode = AIU_I2S_MODE_PCM16;
			break;
		}
		audio_out_i2s_enable(0);
		audio_util_set_dac_i2s_format(AUDIO_ALGOUT_DAC_FORMAT_DSP);
		audio_set_i2s_mode(i2s_mode);
		audio_set_aiubuf(runtime->dma_addr, runtime->dma_bytes, runtime->channels);
	}

	audio_hw_958_enable(0);
	audio_set_958_clk(sample_rate, AUDIO_CLK_256FS);

	/* 958 divisor: 0=no div; 1=div by 2; 2=div by 3; 3=div by 4. */
	if (runtime->rate == 192000 && runtime->channels == 8 && runtime->format == SNDRV_PCM_FORMAT_S16) {
		IEC958_mode_codec = 8; /* TrueHD/DTS-HD MA */
		WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 1, 4, 2);	/* 4x than i2s */
	} else if (runtime->rate == 192000 && runtime->channels == 2 && runtime->format == SNDRV_PCM_FORMAT_S16) {
		IEC958_mode_codec = 4; /* EAC3 */
		WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 0, 4, 2);	/* 4x than i2s */
	} else if (samesrc) {
		IEC958_mode_codec = 0;
		WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 3, 4, 2);	/* 512fs divide 4 == 128fs */
	} else if (runtime->rate == 48000 && runtime->channels == 2 && runtime->format == SNDRV_PCM_FORMAT_S16) {
		IEC958_mode_codec = 2; /* AC3/DTS */
		WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 3, 4, 2);	/* 512fs divide 4 == 128fs */
	} else {
		IEC958_mode_codec = 0;
		WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 3, 4, 2);	/* 512fs divide 4 == 128fs */
	}
	/* enable 958 divider */
	 WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 1, 1, 1);
	audio_util_set_dac_958_format(AUDIO_ALGOUT_DAC_FORMAT_DSP);
	/*clear the same source function as new raw data output */
	WRITE_MPEG_REG_BITS(AIU_I2S_MISC, 0, 3, 1);

	switch (runtime->format) {
	case SNDRV_PCM_FORMAT_S32:
		iec958_mode = AIU_958_MODE_PCM32;
		break;
	case SNDRV_PCM_FORMAT_S24:
		iec958_mode = AIU_958_MODE_PCM24;
		break;
	case SNDRV_PCM_FORMAT_S16:
		iec958_mode = AIU_958_MODE_PCM16;
		break;
	}
	if (IEC958_mode_codec > 0) {
		iec958_mode = AIU_958_MODE_PCM_RAW;
	}

	/* AES1+0 */
	if (iec958_mode == AIU_958_MODE_PCM_RAW) {
		set.chan_stat->chstat0_l = 0x8206;
	} else {
		set.chan_stat->chstat0_l = 0x8204;
	}
	set.chan_stat->chstat0_r = set.chan_stat->chstat0_l;

	/* AES3+2 */
	if (IEC958_mode_codec == 8) {
		set.chan_stat->chstat1_l = 0x0900;
	} else if (runtime->rate == 192000) {
		set.chan_stat->chstat1_l = 0x0e00;
	} else if (runtime->rate == 176400) {
		set.chan_stat->chstat1_l = 0x0c00;
	} else if (runtime->rate == 96000) {
		set.chan_stat->chstat1_l = 0x0a00;
	} else if (runtime->rate == 88200) {
		set.chan_stat->chstat1_l = 0x0800;
	} else if (runtime->rate == 48000) {
		set.chan_stat->chstat1_l = 0x0200;
	} else if (runtime->rate == 44100) {
		set.chan_stat->chstat1_l = 0x0000;
	} else if (runtime->rate == 32000) {
		set.chan_stat->chstat1_l = 0x0300;
	} else {
		set.chan_stat->chstat1_l = 0x0100;
	}
	set.chan_stat->chstat1_r = set.chan_stat->chstat1_l;

	audio_set_958outbuf(buf->addr, snd_pcm_lib_buffer_bytes(substream), 0);
	audio_set_958_mode(iec958_mode, &set);

	/* notify hdmi to set audio type */
	if (IEC958_mode_codec == 8) {
		/* TrueHD/DTS-HD MA */
		aout_notifier_call_chain(AOUT_EVENT_RAWDATA_MAT_MLP, substream);
	} else if (iec958_mode == AIU_958_MODE_PCM_RAW) {
		/* AC3/DTS/EAC3 */
		aout_notifier_call_chain(AOUT_EVENT_RAWDATA_DTS_HD, substream);
	} else {
		/* PCM */
		aout_notifier_call_chain(AOUT_EVENT_IEC_60958_PCM, substream);
	}
#endif
}

/*
special call by the audiodsp,add these code,as there are three cases for 958 s/pdif output
1)NONE-PCM  raw output ,only available when ac3/dts audio,when raw output mode is selected by user.
2)PCM  output for  all audio, when pcm mode is selected by user .
3)PCM  output for audios except ac3/dts,when raw output mode is selected by user
*/

void aml_alsa_hw_reprepare(void)
{
	ALSA_TRACE();
	/* M8 disable it */
#if 0
	/* diable 958 module before call initiation */
	audio_hw_958_enable(0);
	if (playback_substream_handle != 0)
		aml_hw_iec958_init((struct snd_pcm_substream *)
				    playback_substream_handle);
#endif
}

static int aml_dai_spdif_startup(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{

	int ret = 0;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct aml_runtime_data *prtd = runtime->private_data;
	audio_stream_t *s;

	ALSA_TRACE();
	if (!prtd) {
		prtd =
		    (struct aml_runtime_data *)
		    kzalloc(sizeof(struct aml_runtime_data), GFP_KERNEL);
		if (prtd == NULL) {
			printk("alloc aml_runtime_data error\n");
			ret = -ENOMEM;
			goto out;
		}
		prtd->substream = substream;
		runtime->private_data = prtd;
	}
	s = &prtd->s;
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		s->device_type = AML_AUDIO_SPDIFOUT;
		audio_spdifout_pg_enable(1);
		/* aml_spdif_play_stop(); */
	} else {
		s->device_type = AML_AUDIO_SPDIFIN;
	}

	return 0;
 out:
	return ret;
}

static void aml_dai_spdif_shutdown(struct snd_pcm_substream *substream,
				   struct snd_soc_dai *dai)
{

	struct snd_pcm_runtime *runtime = substream->runtime;
	/* struct snd_dma_buffer *buf = &substream->dma_buffer; */
	ALSA_TRACE();
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		memset((void *)runtime->dma_area, 0,
		       snd_pcm_lib_buffer_bytes(substream));
		if (IEC958_mode_codec == 6) {
			printk
			    ("[%s %d]8chPCM output:disable aml_spdif_play()\n",
			     __func__, __LINE__);
		} else {
			aml_spdif_play();
		}
		/* audio_spdifout_pg_enable(0); */
	}
}

static int aml_dai_spdif_prepare(struct snd_pcm_substream *substream,
				 struct snd_soc_dai *dai)
{

	/* struct snd_soc_pcm_runtime *rtd = substream->private_data; */
	struct snd_pcm_runtime *runtime = substream->runtime;
	/* struct aml_runtime_data *prtd = runtime->private_data; */
	/* audio_stream_t *s = &prtd->s; */

	ALSA_TRACE();
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ALSA_PRINT("call audio_set_958outbuf\n");
		audio_set_958outbuf(runtime->dma_addr, snd_pcm_lib_buffer_bytes(substream), 0);
	} else {
		audio_in_spdif_set_buf(runtime->dma_addr,
				       runtime->dma_bytes * 2);
		memset((void *)runtime->dma_area, 0, runtime->dma_bytes * 2);
		 {
			int *ppp =
			    (int *)(runtime->dma_area + runtime->dma_bytes * 2 -
				    8);
			ppp[0] = 0x78787878;
			ppp[1] = 0x78787878;
		}
	}

	return 0;
}

static int aml_dai_spdif_hw_params(struct snd_pcm_substream *substream,
				   struct snd_pcm_hw_params *params,
				   struct snd_soc_dai *dai)
{
	_aiu_958_raw_setting_t set;
	_aiu_958_channel_status_t chstat;
	unsigned iec958_mode;
	int sample_rate;

	ALSA_PRINT("rate:%d, channels:%d, format:%d\n", params_rate(params), params_channels(params), params_format(params));

	iec958_mode = AIU_958_MODE_PCM16;
	memset((void *)(&set), 0, sizeof(set));
	memset((void *)(&chstat), 0, sizeof(chstat));
	set.chan_stat = &chstat;

	switch (params_rate(params)) {
	case 192000:
		sample_rate = AUDIO_CLK_FREQ_192;
		break;
	case 176400:
		sample_rate = AUDIO_CLK_FREQ_1764;
		break;
	case 96000:
		sample_rate = AUDIO_CLK_FREQ_96;
		break;
	case 88200:
		sample_rate = AUDIO_CLK_FREQ_882;
		break;
	case 48000:
		sample_rate = AUDIO_CLK_FREQ_48;
		break;
	case 44100:
		sample_rate = AUDIO_CLK_FREQ_441;
		break;
	case 32000:
		sample_rate = AUDIO_CLK_FREQ_32;
		break;
	case 8000:
		sample_rate = AUDIO_CLK_FREQ_8;
		break;
	case 11025:
		sample_rate = AUDIO_CLK_FREQ_11;
		break;
	case 16000:
		sample_rate = AUDIO_CLK_FREQ_16;
		break;
	case 22050:
		sample_rate = AUDIO_CLK_FREQ_22;
		break;
	case 12000:
		sample_rate = AUDIO_CLK_FREQ_12;
		break;
	case 24000:
		sample_rate = AUDIO_CLK_FREQ_22;
		break;
	default:
		sample_rate = AUDIO_CLK_FREQ_441;
		break;
	};

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S32:
		iec958_mode = AIU_958_MODE_PCM32;
		break;
	case SNDRV_PCM_FORMAT_S24:
		iec958_mode = AIU_958_MODE_PCM24;
		break;
	case SNDRV_PCM_FORMAT_S16:
		iec958_mode = AIU_958_MODE_PCM16;
		break;
	}

	/* AES1+0 */
	if (iec958_mode == AIU_958_MODE_PCM_RAW) {
		set.chan_stat->chstat0_l = 0x8206;
	} else {
		set.chan_stat->chstat0_l = 0x8204;
	}
	set.chan_stat->chstat0_r = set.chan_stat->chstat0_l;

	/* AES3+2 */
	if (IEC958_mode_codec == 8) {
		set.chan_stat->chstat1_l = 0x0900;
	} else if (runtime->rate == 192000) {
		set.chan_stat->chstat1_l = 0x0e00;
	} else if (runtime->rate == 176400) {
		set.chan_stat->chstat1_l = 0x0c00;
	} else if (runtime->rate == 96000) {
		set.chan_stat->chstat1_l = 0x0a00;
	} else if (runtime->rate == 88200) {
		set.chan_stat->chstat1_l = 0x0800;
	} else if (runtime->rate == 48000) {
		set.chan_stat->chstat1_l = 0x0200;
	} else if (runtime->rate == 44100) {
		set.chan_stat->chstat1_l = 0x0000;
	} else if (runtime->rate == 32000) {
		set.chan_stat->chstat1_l = 0x0300;
	} else {
		set.chan_stat->chstat1_l = 0x0100;
	}
	set.chan_stat->chstat1_r = set.chan_stat->chstat1_l;

	audio_set_958_clk(sample_rate, AUDIO_CLK_256FS, params_channels(params));

    WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 0, 12, 1);// 958 divisor more, if true, divided by 2, 4, 6, 8
	WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 3, 4, 2);	/* 512fs divide 4 == 128fs */
    WRITE_MPEG_REG_BITS(AIU_CLK_CTRL, 1, 1, 1);// enable 958 clock

	audio_set_958_mode(iec958_mode, &set);

	return 0;
}

#ifdef CONFIG_PM
static int aml_dai_spdif_suspend(struct snd_soc_dai *dai)
{
	ALSA_TRACE();
/* aml_spdif_play_stop(); */
	return 0;
}

static int aml_dai_spdif_resume(struct snd_soc_dai *dai)
{
	ALSA_TRACE();
/* aml_spdif_play(); */
	return 0;
}
#else
#define aml_spdif_suspend NULL
#define aml_spdif_resume NULL
#endif

static int aml_dai_spdif_digital_mute(struct snd_soc_dai *dai, int mute)
{
	ALSA_PRINT("mute:%d\n", mute);
	return 0;
}

static int aml_dai_spdif_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	ALSA_PRINT("mute:%d, stream:%d\n", mute, stream);
	return 0;
}

static struct snd_soc_dai_ops spdif_dai_ops = {
	.set_sysclk = aml_dai_spdif_set_sysclk,
	.digital_mute = aml_dai_spdif_digital_mute,
	.mute_stream = aml_dai_spdif_mute_stream,
	.trigger = aml_dai_spdif_trigger,
	.prepare = aml_dai_spdif_prepare,
	.hw_params = aml_dai_spdif_hw_params,
	.shutdown = aml_dai_spdif_shutdown,
	.startup = aml_dai_spdif_startup,
};

static struct snd_soc_dai_driver aml_spdif_dai[] = {
	{
	 .name = "aml-spdif-dai",
	 .playback = {
		      .stream_name = "S/PDIF Playback",
		      .channels_min = 1,
		      .channels_max = 8,
		      .rates = (SNDRV_PCM_RATE_32000 |
				SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000 |
				SNDRV_PCM_RATE_88200 |
				SNDRV_PCM_RATE_96000 |
				SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000),
		      .formats =
		      (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE |
		       SNDRV_PCM_FMTBIT_S32_LE),},
	 .capture = {
		     .stream_name = "S/PDIF Capture",
		     .channels_min = 1,
		     .channels_max = 8,
		     .rates = (SNDRV_PCM_RATE_32000 |
			       SNDRV_PCM_RATE_44100 |
			       SNDRV_PCM_RATE_48000 | SNDRV_PCM_RATE_96000),
		     .formats = SNDRV_PCM_FMTBIT_S16_LE,},
	 .ops = &spdif_dai_ops,
	 .suspend = aml_dai_spdif_suspend,
	 .resume = aml_dai_spdif_resume,
	 }
};

static const struct snd_soc_component_driver aml_component = { .name =
	    "aml-spdif-dai",
};

static int aml_dai_spdif_probe(struct platform_device *pdev)
{
	ALSA_PRINT("aml_spdif_probe\n");
	return snd_soc_register_component(&pdev->dev, &aml_component,
					  aml_spdif_dai,
					  ARRAY_SIZE(aml_spdif_dai));
}

static int aml_dai_spdif_remove(struct platform_device *pdev)
{
	snd_soc_unregister_component(&pdev->dev);
	return 0;
}

#ifdef CONFIG_USE_OF
static const struct of_device_id amlogic_spdif_dai_dt_match[] = {
	    {.compatible = "amlogic,aml-spdif-dai",
	     },
{},
};
#else
#define amlogic_spdif_dai_dt_match NULL
#endif

static struct platform_driver aml_spdif_dai_driver = {
	.probe = aml_dai_spdif_probe,
	.remove = aml_dai_spdif_remove,
	.driver = {
		   .name = "aml-spdif-dai",
		   .owner = THIS_MODULE,
		   .of_match_table = amlogic_spdif_dai_dt_match,
		   },
};

static int __init aml_dai_spdif_init(void)
{
	ALSA_PRINT("enter aml_dai_spdif_init\n");
	aml_spdif_play();
	return platform_driver_register(&aml_spdif_dai_driver);
}

module_init(aml_dai_spdif_init);

static void __exit aml_dai_spdif_exit(void)
{
	platform_driver_unregister(&aml_spdif_dai_driver);
}

module_exit(aml_dai_spdif_exit);

MODULE_AUTHOR("jian.xu, <jian.xu@amlogic.com>");
MODULE_DESCRIPTION("Amlogic S/PDIF<HDMI> Controller Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:aml-spdif");
