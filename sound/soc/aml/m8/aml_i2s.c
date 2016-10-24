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
#include <linux/hrtimer.h>
#include <linux/debugfs.h>
#include <linux/major.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>
#include <linux/amlogic/rt5631.h>

#include <mach/am_regs.h>
#include <mach/pinmux.h>

#include <linux/amlogic/amports/amaudio.h>

#include <mach/mod_gate.h>

#include "aml_i2s.h"
#include "aml_audio_hw.h"

#define USE_HW_TIMER
#ifdef USE_HW_TIMER
#define XRUN_NUM 100 /*1ms*100=100ms timeout*/
#else
#define XRUN_NUM 10 /*10ms*10=100ms timeout*/
#endif

//#define DEBUG_ALSA_PLATFRORM

#define ALSA_PRINT(fmt,args...) printk(KERN_INFO "[aml-i2s::%s]" fmt, __func__, ##args)
#ifdef DEBUG_ALSA_PLATFRORM
#define ALSA_DEBUG(fmt,args...) printk(KERN_INFO "[aml-i2s]" fmt,##args)
#define ALSA_TRACE()        printk("[aml-i2s] enter func %s,line %d\n",__FUNCTION__,__LINE__)
#else
#define ALSA_DEBUG(fmt,args...)
#define ALSA_TRACE()
#endif

unsigned int aml_i2s_playback_start_addr = 0;
unsigned int aml_i2s_playback_phy_start_addr = 0;
unsigned int aml_i2s_capture_start_addr  = 0;
unsigned int aml_i2s_capture_phy_start_addr  = 0;

unsigned int aml_i2s_capture_buf_size = 0;
unsigned int aml_i2s_playback_enable = 1;
unsigned int aml_i2s_alsa_write_addr = 0;

extern int android_left_gain;
extern int android_right_gain;
extern int set_android_gain_enable;
extern unsigned audioin_mode;
extern int amaudio2_enable;

static DEFINE_MUTEX(gate_mutex);
static unsigned audio_gate_status = 0;

EXPORT_SYMBOL(aml_i2s_playback_start_addr);
EXPORT_SYMBOL(aml_i2s_capture_start_addr);
EXPORT_SYMBOL(aml_i2s_capture_buf_size);
EXPORT_SYMBOL(aml_i2s_playback_enable);
EXPORT_SYMBOL(aml_i2s_playback_phy_start_addr);
EXPORT_SYMBOL(aml_i2s_capture_phy_start_addr);
EXPORT_SYMBOL(aml_i2s_alsa_write_addr);

static int trigger_underrun = 0;
void aml_audio_hw_trigger(void)
{
    trigger_underrun = 1;
}
EXPORT_SYMBOL(aml_audio_hw_trigger);

static void aml_i2s_timer_callback(unsigned long data);

/*--------------------------------------------------------------------------*\
 * Hardware definition
\*--------------------------------------------------------------------------*/
/* TODO: These values were taken from the AML platform driver, check
 *   them against real values for AML
 */
static const struct snd_pcm_hardware aml_i2s_hardware = {
    .info           = SNDRV_PCM_INFO_INTERLEAVED |
    SNDRV_PCM_INFO_BLOCK_TRANSFER |
    SNDRV_PCM_INFO_PAUSE,

    .formats        = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE,

    .period_bytes_min   = 256,
    .period_bytes_max   = 4096,
    .periods_min        = 2,
    .periods_max        = 512,
    .buffer_bytes_max   = 512 * 512 * 8,

    .rate_min = 8000,
    .rate_max = 192000,
    .channels_min = 2,
    .channels_max = 8,
    .fifo_size = 0,
};

static const struct snd_pcm_hardware aml_i2s_capture = {
    .info           = SNDRV_PCM_INFO_INTERLEAVED |
    SNDRV_PCM_INFO_BLOCK_TRANSFER |
    SNDRV_PCM_INFO_MMAP |
    SNDRV_PCM_INFO_MMAP_VALID |
    SNDRV_PCM_INFO_PAUSE,

    .formats        = SNDRV_PCM_FMTBIT_S16_LE,
    .period_bytes_min   = 64,
    .period_bytes_max   = 32 * 1024,
    .periods_min        = 2,
    .periods_max        = 1024,
    .buffer_bytes_max   = 64 * 1024,

    .rate_min = 8000,
    .rate_max = 48000,
    .channels_min = 2,
    .channels_max = 8,
    .fifo_size = 0,
};

/*--------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------*\
 * Helper functions
\*--------------------------------------------------------------------------*/
static int aml_i2s_preallocate_dma_buffer(struct snd_pcm *pcm,
        int stream)
{

    struct snd_pcm_substream *substream = pcm->streams[stream].substream;
    struct snd_dma_buffer *buf = &substream->dma_buffer;
    struct snd_dma_buffer *tmp_buf = NULL;
    size_t size = 0;

    tmp_buf = kzalloc(sizeof(struct snd_dma_buffer), GFP_KERNEL);
    if (tmp_buf == NULL) {
        printk("alloc tmp buffer struct error\n");
        return -ENOMEM;
    }
    buf->private_data = tmp_buf;

    ALSA_TRACE();
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        //malloc DMA buffer
        size = aml_i2s_hardware.buffer_bytes_max;
        buf->dev.type = SNDRV_DMA_TYPE_DEV;
        buf->dev.dev = pcm->card->dev;
        /* one size for i2s output, another for 958, and 128 for alignment */
        tmp_buf->bytes = size + 4096;
        tmp_buf->area = dma_alloc_coherent(pcm->card->dev, tmp_buf->bytes,
                                       &tmp_buf->addr, GFP_KERNEL);
        printk("aml-i2s %d:"
               "playback preallocate_dma_buffer: area=%p, addr=%p, bytes=%d\n", stream,
               (void *) tmp_buf->area,
               (void *) tmp_buf->addr,
               tmp_buf->bytes);
        if (!tmp_buf->area) {
            printk("alloc playback DMA buffer error\n");
            kfree(tmp_buf);
            buf->private_data = NULL;
            return -ENOMEM;
        }
        buf->bytes = size;
		buf->addr = (tmp_buf->addr + 63) & (~63);
		ALSA_PRINT("tmp_buf->addr:%d, buf->addr:%d\n", tmp_buf->addr, buf->addr);
		buf->area = tmp_buf->area + (tmp_buf->addr - buf->addr);
		ALSA_PRINT("tmp_buf->area:%p, buf->area:%p\n", tmp_buf->area, buf->area);

    } else {
        //malloc DMA buffer
        size = aml_i2s_capture.buffer_bytes_max;
        buf->dev.type = SNDRV_DMA_TYPE_DEV;
        buf->dev.dev = pcm->card->dev;
        tmp_buf->bytes = size * 2;
        tmp_buf->area = dma_alloc_coherent(pcm->card->dev, tmp_buf->bytes,
                                       &tmp_buf->addr, GFP_KERNEL);
        printk("aml-i2s %d:"
               "capture preallocate_dma_buffer: area=%p, addr=%p, bytes=%d\n", stream,
               (void *) tmp_buf->area,
               (void *) tmp_buf->addr,
               tmp_buf->bytes);
        if (!tmp_buf->area) {
            printk("alloc capture DMA buffer error\n");
            kfree(tmp_buf);
            buf->private_data = NULL;
            return -ENOMEM;
        }
        buf->bytes = size;
		buf->addr = tmp_buf->addr;
		buf->area = tmp_buf->area;
    }

    return 0;

}
/*--------------------------------------------------------------------------*\
 * ISR
\*--------------------------------------------------------------------------*/


/*--------------------------------------------------------------------------*\
 * i2s operations
\*--------------------------------------------------------------------------*/
static int aml_i2s_hw_params(struct snd_pcm_substream *substream,
                             struct snd_pcm_hw_params *params)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct aml_runtime_data *prtd = runtime->private_data;
    //  struct snd_soc_pcm_runtime *rtd = snd_pcm_substream_chip(substream);
    audio_stream_t *s = &prtd->s;

    /* this may get called several times by oss emulation
     * with different params */

    snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);
    //runtime->dma_bytes = params_buffer_bytes(params);
    ALSA_PRINT("runtime dma_bytes %d, buffer_bytes %d, stream type %d\n", runtime->dma_bytes, params_buffer_bytes(params), substream->stream);
    s->I2S_addr = runtime->dma_addr;

    /*
     * Both capture and playback need to reset the last ptr to the start address,
       playback and capture use different address calculate, so we reset the different
       start address to the last ptr
    * */
    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        /* s->last_ptr must initialized as dma buffer's start addr */
        s->last_ptr = runtime->dma_addr;
    } else {

        s->last_ptr = 0;
    }
	s->size = 0;

    return 0;
}

static int aml_i2s_hw_free(struct snd_pcm_substream *substream)
{
    return 0;
}


static int aml_i2s_prepare(struct snd_pcm_substream *substream)
{

    struct snd_pcm_runtime *runtime = substream->runtime;
    struct aml_runtime_data *prtd = runtime->private_data;
    audio_stream_t *s = &prtd->s;

    ALSA_TRACE();
    if (s && s->device_type == AML_AUDIO_I2SOUT && trigger_underrun) {
        printk("clear i2s out trigger underrun \n");
        trigger_underrun = 0;
    }
    return 0;
}

#ifdef USE_HW_TIMER
int hw_timer_init = 0;
static irqreturn_t audio_isr_handler(int irq, void *data)
{
	struct aml_runtime_data *prtd = data;
	struct snd_pcm_substream *substream = prtd->substream;
	aml_i2s_timer_callback((unsigned long)substream);
	return IRQ_HANDLED;
}

static irqreturn_t iec958_isr_handler(int irq, void *data)
{
	struct aml_runtime_data *prtd = data;
	struct snd_pcm_substream *substream = prtd->substream;
	ALSA_PRINT("iec958_isr_handler\n");
	return IRQ_HANDLED;
}

static int snd_free_hw_timer_irq(void *data)
{
	free_irq(INT_AI_IEC958, data);
	free_irq(INT_TIMER_D, data);
	return 0;
}

static int snd_request_hw_timer(void *data)
{
	int ret = 0;
	if (hw_timer_init == 0) {
        aml_write_reg32(P_ISA_TIMERD, TIMER_COUNT);
        aml_clr_reg32_mask(P_ISA_TIMER_MUX, ((3 << 6) | (1 << 15) | (1 << 19)));
        aml_set_reg32_mask(P_ISA_TIMER_MUX, ((TIMERD_RESOLUTION << 6)
                                             | (TIMERD_MODE << 15)
                                             | (1 << 19)));
		hw_timer_init = 1;
	}
	ret = request_irq(INT_TIMER_D, audio_isr_handler,
				IRQF_SHARED, "timerd_irq", data);
		if (ret < 0) {
			pr_err("audio hw interrupt register fail\n");
			return -1;
		}
	ret = request_irq(INT_AI_IEC958, iec958_isr_handler,
				IRQF_SHARED, "iec958_irq", data);
		if (ret < 0) {
			pr_err("iec958 interrupt register fail\n");
			return -1;
		}
	return 0;
}

#endif

static void start_timer(struct aml_runtime_data *prtd)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&prtd->timer_lock, flags);
	if (!prtd->active) {
#ifndef USE_HW_TIMER
		prtd->timer.expires = jiffies + 1;
		add_timer(&prtd->timer);
#endif
		prtd->active = 1;
		prtd->xrun_num = 0;
	}
	spin_unlock_irqrestore(&prtd->timer_lock, flags);

}

static void stop_timer(struct aml_runtime_data *prtd)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&prtd->timer_lock, flags);
	if (prtd->active) {
#ifndef USE_HW_TIMER
		del_timer(&prtd->timer);
#endif
		prtd->active = 0;
		prtd->xrun_num = 0;
	}
	spin_unlock_irqrestore(&prtd->timer_lock, flags);
}


static int aml_i2s_trigger(struct snd_pcm_substream *substream, int cmd)
{
    struct snd_pcm_runtime *rtd = substream->runtime;
    struct aml_runtime_data *prtd = rtd->private_data;
    int ret = 0;

    switch (cmd) {
    case SNDRV_PCM_TRIGGER_START:
    case SNDRV_PCM_TRIGGER_RESUME:
    case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		start_timer(prtd);
        break;      /* SNDRV_PCM_TRIGGER_START */
    case SNDRV_PCM_TRIGGER_SUSPEND:
    case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
    case SNDRV_PCM_TRIGGER_STOP:
		stop_timer(prtd);
        break;
    default:
        ret = -EINVAL;
    }

    return ret;
}

static snd_pcm_uframes_t aml_i2s_pointer(
    struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct aml_runtime_data *prtd = runtime->private_data;
    audio_stream_t *s = &prtd->s;

    unsigned int addr, ptr;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        if (s->device_type == AML_AUDIO_I2SOUT) {
            ptr = read_i2s_rd_ptr();
        } else {
            ptr = read_iec958_rd_ptr();
        }
        addr = ptr - s->I2S_addr;
        return bytes_to_frames(runtime, addr);
    } else {
        if (s->device_type == AML_AUDIO_I2SIN) {
            ptr = audio_in_i2s_wr_ptr();
        } else {
            ptr = audio_in_spdif_wr_ptr();
        }
        addr = ptr - s->I2S_addr;
        return bytes_to_frames(runtime, addr) / 2;
    }

    return 0;
}

static void aml_i2s_timer_callback(unsigned long data)
{
    struct snd_pcm_substream *substream = (struct snd_pcm_substream *)data;
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct aml_runtime_data *prtd = NULL;
    audio_stream_t *s = NULL;
	int elapsed = 0;
	unsigned int last_ptr, size = 0;
	unsigned long flags = 0;

	if (runtime == NULL || runtime->private_data == NULL)
		return;

	prtd = runtime->private_data;
	s = &prtd->s;

	if (prtd->active == 0)
		return;

	spin_lock_irqsave(&prtd->timer_lock, flags);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (s->device_type == AML_AUDIO_I2SOUT)
			last_ptr = read_i2s_rd_ptr();
		else
			last_ptr = read_iec958_rd_ptr();
		if (last_ptr < s->last_ptr) {
			size = frames_to_bytes(runtime, runtime->buffer_size) + last_ptr - s->last_ptr;
		} else {
			size = last_ptr - s->last_ptr;
		}
		s->last_ptr = last_ptr;
		s->size += bytes_to_frames(runtime, size);
		//ALSA_PRINT("last_ptr:%u, size:%u, s->size:%u, period_size:%d\n", last_ptr, size, s->size, runtime->period_size);
		if (s->size >= runtime->period_size) {
			s->size %= runtime->period_size;
			elapsed = 1;
		}
	} else {
		if (s->device_type == AML_AUDIO_I2SIN)
			last_ptr = audio_in_i2s_wr_ptr();
		else
			last_ptr = audio_in_spdif_wr_ptr();
		if (last_ptr < s->last_ptr) {
			size =
				runtime->buffer_size + (last_ptr -
						  (s->last_ptr)) / 2;
			prtd->xrun_num = 0;
		} else if (last_ptr == s->last_ptr) {
			if (prtd->xrun_num++ > XRUN_NUM) {
				/*dev_info(substream->pcm->card->dev,
					"alsa capture long time no data, quit xrun!\n");
				*/
				prtd->xrun_num = 0;
				s->size = runtime->period_size;
			}
		} else {
			size = (last_ptr - (s->last_ptr)) / 2;
			prtd->xrun_num = 0;
		}
		s->last_ptr = last_ptr;
		s->size += bytes_to_frames(substream->runtime, size);
		if (s->size >= runtime->period_size) {
			s->size %= runtime->period_size;
			elapsed = 1;
		}
	}

#ifndef USE_HW_TIMER
	mod_timer(&prtd->timer, jiffies + 1);
#endif

	spin_unlock_irqrestore(&prtd->timer_lock, flags);
	if (elapsed) {
		snd_pcm_period_elapsed(substream);
	}
}


static int aml_i2s_open(struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    struct aml_runtime_data *prtd = runtime->private_data;
    struct snd_dma_buffer *buf = &substream->dma_buffer;
    audio_stream_t *s = &prtd->s;
    int ret = 0;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        snd_soc_set_runtime_hwparams(substream, &aml_i2s_hardware);
        if (s->device_type == AML_AUDIO_I2SOUT) {
            aml_i2s_playback_start_addr = (unsigned int)buf->area;
            aml_i2s_playback_phy_start_addr = buf->addr;
        }
    } else {
        snd_soc_set_runtime_hwparams(substream, &aml_i2s_capture);
        if (s->device_type == AML_AUDIO_I2SIN) {
            aml_i2s_capture_start_addr = (unsigned int)buf->area;
            aml_i2s_capture_phy_start_addr = buf->addr;
        }
    }

    /* ensure that peroid bytes is a multiple of 512 bytes */
    ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 512);
    if (ret < 0) {
        printk("set period bytes constraint error\n");
        goto out;
    }

    /* ensure that buffer bytes is a multiple of 512 bytes */
    ret = snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES, 512);
    if (ret < 0) {
        printk("set buffer bytes constraint error\n");
        goto out;
    }

    if (!prtd) {
        prtd = kzalloc(sizeof(struct aml_runtime_data), GFP_KERNEL);
        if (prtd == NULL) {
            ALSA_PRINT("alloc aml_runtime_data error\n");
            ret = -ENOMEM;
            goto out;
        }
        prtd->substream = substream;
        runtime->private_data = prtd;
    }

	spin_lock_init(&prtd->timer_lock);

#ifndef USE_HW_TIMER
	init_timer(&prtd->timer);
    prtd->timer.function = &aml_i2s_timer_callback;
    prtd->timer.data = (unsigned long)substream;
#else
    ret = snd_request_hw_timer(prtd);
    if (ret < 0) {
        ALSA_PRINT("request audio hw timer failed \n");
        goto out;
    }
#endif

out:
    return ret;
}

static int aml_i2s_close(struct snd_pcm_substream *substream)
{
	struct aml_runtime_data *prtd = substream->runtime->private_data;

#ifdef USE_HW_TIMER
	snd_free_hw_timer_irq(prtd);
#endif
	kfree(prtd);
	prtd = NULL;

	return 0;
}

extern unsigned int IEC958_mode_codec;
extern void audio_out_i2s_enable(unsigned flag);
extern void aml_hw_iec958_init(struct snd_pcm_substream *substream);
extern void audio_hw_958_enable(unsigned flag);

static int aml_i2s_copy_playback(struct snd_pcm_runtime *runtime, int channel,
                                 snd_pcm_uframes_t pos,
                                 void __user *buf, snd_pcm_uframes_t count, struct snd_pcm_substream *substream)
{
    int res = 0;
    int n;
    char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, pos);
    struct aml_runtime_data *prtd = runtime->private_data;
    struct snd_dma_buffer *buffer = &substream->dma_buffer;
    struct aml_audio_buffer *tmp_buf = buffer->private_data;
    void *ubuf = tmp_buf->buffer_start;
    n = frames_to_bytes(runtime, count);
    if (n > tmp_buf->buffer_size) {
	    printk("FATAL_ERR:UserData/%d > buffer_size/%d\n",
				n, tmp_buf->buffer_size);
		return -EFAULT;
	}
    res = copy_from_user(ubuf, buf, n);
    if (res) {
        return -EFAULT;
    }
    if (access_ok(VERIFY_READ, buf, n)) {
        if (runtime->format == SNDRV_PCM_FORMAT_S16_LE) {
			ALSA_PRINT("16: channel:%d, pos:%d, count:%d\n", channel, frames_to_bytes(runtime, pos), frames_to_bytes(runtime, count));
			memcpy(hwbuf, ubuf, n);
        } else if (runtime->format == SNDRV_PCM_FORMAT_S24_LE) {
			ALSA_PRINT("24: channel:%d, pos:%d, count:%d\n", channel, frames_to_bytes(runtime, pos), frames_to_bytes(runtime, count));
			memcpy(hwbuf, ubuf, n);
        } else if (runtime->format == SNDRV_PCM_FORMAT_S32_LE) {
			ALSA_PRINT("32: channel:%d, pos:%d, count:%d\n", channel, frames_to_bytes(runtime, pos), frames_to_bytes(runtime, count));
			memcpy(hwbuf, ubuf, n);
        }
    } else {
        res = -EFAULT;
    }
    return res;
}

static unsigned int aml_get_in_wr_ptr(void)
{
    return (audio_in_i2s_wr_ptr() - aml_i2s_capture_phy_start_addr);
}

static int aml_i2s_copy_capture(struct snd_pcm_runtime *runtime, int channel,
                                snd_pcm_uframes_t pos,
                                void __user *buf, snd_pcm_uframes_t count, struct snd_pcm_substream *substream)
{
    unsigned int *tfrom, *left, *right;
    unsigned short *to;
    int res = 0, n = 0, i = 0, j = 0, size = 0;
    unsigned int t1, t2;
    unsigned char r_shift = 8;
    struct aml_runtime_data *prtd = runtime->private_data;
    audio_stream_t *s = &prtd->s;
    char *hwbuf = runtime->dma_area + frames_to_bytes(runtime, pos) * 2;
    struct snd_dma_buffer *buffer = &substream->dma_buffer;
    struct aml_audio_buffer *tmp_buf = buffer->private_data;
    void *ubuf = tmp_buf->buffer_start;
    if (s->device_type == AML_AUDIO_I2SIN) {
        unsigned int buffersize = (unsigned int)runtime->buffer_size * 8; //framesize*8
        unsigned int hw_ptr = aml_get_in_wr_ptr();
        unsigned int alsa_read_ptr = frames_to_bytes(runtime, pos) * 2;
        size = (buffersize + hw_ptr - alsa_read_ptr) % buffersize;
    }
    if (s->device_type == AML_AUDIO_SPDIFIN) { //spdif in
        r_shift = 12;
    }
    to = (unsigned short *)ubuf;//tmp buf;
    tfrom = (unsigned int *)hwbuf;  // 32bit buffer
    n = frames_to_bytes(runtime, count);
    // for low latency amaudio2 mode, check the hw ptr overrun  state machine
    if (size < 2 * n && s->device_type == AML_AUDIO_I2SIN && amaudio2_enable) {
        printk("Alsa ptr is too close to HW ptr, Reset ALSA!\n");
        return -EPIPE;
    }
    if (access_ok(VERIFY_WRITE, buf, frames_to_bytes(runtime, count))) {
        if (runtime->channels == 2) {
            left = tfrom;
            right = tfrom + 8;
            if (pos % 8) {
                printk("audio data unligned\n");
            }
            if ((n * 2) % 64) {
                printk("audio data unaligned 64 bytes\n");
            }
            for (j = 0; j < n * 2 ; j += 64) {
                for (i = 0; i < 8; i++) {
                    t1 = (*left++);
                    t2 = (*right++);
                    *to++ = (unsigned short)((t1 >> r_shift) & 0xffff);
                    *to++ = (unsigned short)((t2 >> r_shift) & 0xffff);
                }
                left += 8;
                right += 8;
            }
        }
    }
    res = copy_to_user(buf, ubuf, n);
    return res;
}

static int aml_i2s_copy(struct snd_pcm_substream *substream, int channel,
                        snd_pcm_uframes_t pos,
                        void __user *buf, snd_pcm_uframes_t count)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    int ret = 0;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
        ret = aml_i2s_copy_playback(runtime, channel, pos, buf, count, substream);
    } else {
        ret = aml_i2s_copy_capture(runtime, channel, pos, buf, count, substream);
    }
    return ret;
}

int aml_i2s_silence(struct snd_pcm_substream *substream, int channel,
                    snd_pcm_uframes_t pos, snd_pcm_uframes_t count)
{
    char* ppos;
    int n;
    struct snd_pcm_runtime *runtime = substream->runtime;
    ALSA_TRACE();

    n = frames_to_bytes(runtime, count);
    ppos = runtime->dma_area + frames_to_bytes(runtime, pos);
    memset(ppos, 0, n);
    return 0;
}

static struct snd_pcm_ops aml_i2s_ops = {
    .open       = aml_i2s_open,
    .close      = aml_i2s_close,
    .ioctl      = snd_pcm_lib_ioctl,
    .hw_params  = aml_i2s_hw_params,
    .hw_free    = aml_i2s_hw_free,
    .prepare    = aml_i2s_prepare,
    .trigger    = aml_i2s_trigger,
    .pointer    = aml_i2s_pointer,
    //.copy       = aml_i2s_copy,
    //.silence    =   aml_i2s_silence,
};


/*--------------------------------------------------------------------------*\
 * ASoC platform driver
\*--------------------------------------------------------------------------*/
static u64 aml_i2s_dmamask = 0xffffffff;

static int aml_i2s_new(struct snd_soc_pcm_runtime *rtd)
{
    int ret = 0;
    struct snd_soc_card *card = rtd->card;
    struct snd_pcm *pcm = rtd->pcm ;
    ALSA_TRACE();
    if (!card->dev->dma_mask) {
        card->dev->dma_mask = &aml_i2s_dmamask;
    }
    if (!card->dev->coherent_dma_mask) {
        card->dev->coherent_dma_mask = 0xffffffff;
    }

    if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
        ret = aml_i2s_preallocate_dma_buffer(pcm,
                                             SNDRV_PCM_STREAM_PLAYBACK);
        if (ret) {
            goto out;
        }
    }

    if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
        pr_debug("aml-i2s:"
                 "Allocating i2s capture DMA buffer\n");
        ret = aml_i2s_preallocate_dma_buffer(pcm,
                                             SNDRV_PCM_STREAM_CAPTURE);
        if (ret) {
            goto out;
        }
    }

out:
    return ret;
}

static void aml_i2s_free_dma_buffers(struct snd_pcm *pcm)
{
    struct snd_pcm_substream *substream;
    struct snd_dma_buffer *buf;
    struct snd_dma_buffer *tmp_buf;
    int stream;
    ALSA_TRACE();
    for (stream = 0; stream < 2; stream++) {
        substream = pcm->streams[stream].substream;
        if (!substream) {
            continue;
        }

        buf = &substream->dma_buffer;
        if (!buf->area) {
            continue;
        }
        tmp_buf = buf->private_data;
		if (tmp_buf) {
			dma_free_coherent(pcm->card->dev, tmp_buf->bytes,
							  tmp_buf->area, tmp_buf->addr);
            kfree(tmp_buf);
		}
        buf->area = NULL;
        buf->private_data = NULL;
    }
}

#ifdef CONFIG_PM
static int aml_i2s_suspend(struct snd_soc_dai *dai)
{
    struct snd_pcm_runtime *runtime = dai->runtime;
    struct aml_runtime_data *prtd;
    struct aml_i2s_dma_params *params;
    if (!runtime) {
        return 0;
    }

    prtd = runtime->private_data;
    params = prtd->params;

    /* disable the PDC and save the PDC registers */
    // TODO
    printk("aml i2s suspend\n");

    return 0;
}

static int aml_i2s_resume(struct snd_soc_dai *dai)
{
    struct snd_pcm_runtime *runtime = dai->runtime;
    struct aml_runtime_data *prtd;
    struct aml_i2s_dma_params *params;
    if (!runtime) {
        return 0;
    }

    prtd = runtime->private_data;
    params = prtd->params;

    /* restore the PDC registers and enable the PDC */
    // TODO
    printk("aml i2s resume\n");
    return 0;
}
#else
#define aml_i2s_suspend NULL
#define aml_i2s_resume  NULL
#endif

#ifdef CONFIG_DEBUG_FS

static struct dentry *debugfs_root;
static struct dentry *debugfs_regs;
static struct dentry *debugfs_mems;

static int regs_open_file(struct inode *inode, struct file *file)
{
    return 0;
}

/**
 *  cat regs
 */
static ssize_t regs_read_file(struct file *file, char __user *user_buf,
                              size_t count, loff_t *ppos)
{
    ssize_t ret;
    char *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!buf) {
        return -ENOMEM;
    }

    ret = sprintf(buf, "Usage: \n"
                  "	echo base reg val >regs\t(set the register)\n"
                  "	echo base reg >regs\t(show the register)\n"
                  "	base -> c(cbus), x(aix), p(apb), h(ahb) \n"
                 );

    if (ret >= 0) {
        ret = simple_read_from_buffer(user_buf, count, ppos, buf, ret);
    }
    kfree(buf);

    return ret;
}

static int read_regs(char base, int reg)
{
    int val = 0;
    switch (base) {
    case 'c':
        val = READ_CBUS_REG(reg);
        break;
    case 'x':
        val = READ_AXI_REG(reg);
        break;
    case 'p':
        val = READ_APB_REG(reg);
        break;
    case 'h':
        //val = READ_AHB_REG(reg);
        break;
    default:
        break;
    };
    printk("\tReg %x = %x\n", reg, val);
    return val;
}

static void write_regs(char base, int reg, int val)
{
    switch (base) {
    case 'c':
        WRITE_CBUS_REG(reg, val);
        break;
    case 'x':
        WRITE_AXI_REG(reg, val);
        break;
    case 'p':
        WRITE_APB_REG(reg, val);
        break;
    case 'h':
        //WRITE_AHB_REG(reg, val);
        break;
    default:
        break;
    };
    printk("Write reg:%x = %x\n", reg, val);
}
static ssize_t regs_write_file(struct file *file,
                               const char __user *user_buf, size_t count, loff_t *ppos)
{
    char buf[32];
    int buf_size = 0;
    char *start = buf;
    unsigned long reg, value;
    char base;

    buf_size = min(count, (sizeof(buf) - 1));

    if (copy_from_user(buf, user_buf, buf_size)) {
        return -EFAULT;
    }
    buf[buf_size] = 0;
    while (*start == ' ') {
        start++;
    }

    base = *start;
    start ++;
    if (!(base == 'c' || base == 'x' || base == 'p' || base == 'h')) {
        return -EINVAL;
    }

    while (*start == ' ') {
        start++;
    }

    reg = simple_strtoul(start, &start, 16);

    while (*start == ' ') {
        start++;
    }

    if (strict_strtoul(start, 16, &value)) {
        read_regs(base, reg);
        return -EINVAL;
    }

    write_regs(base, reg, value);

    return buf_size;
}

static const struct file_operations regs_fops = {
    .open = regs_open_file,
    .read = regs_read_file,
    .write = regs_write_file,
};

static int mems_open_file(struct inode *inode, struct file *file)
{
    return 0;
}
static ssize_t mems_read_file(struct file *file, char __user *user_buf,
                              size_t count, loff_t *ppos)
{
    ssize_t ret;
    char *buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
    if (!buf) {
        return -ENOMEM;
    }

    ret = sprintf(buf, "Usage: \n"
                  "	echo vmem >mems\t(read 64 bytes from vmem)\n"
                  "	echo vmem val >mems (write int value to vmem\n"
                 );

    if (ret >= 0) {
        ret = simple_read_from_buffer(user_buf, count, ppos, buf, ret);
    }
    kfree(buf);

    return ret;
}

static ssize_t mems_write_file(struct file *file,
                               const char __user *user_buf, size_t count, loff_t *ppos)
{
    char buf[256];
    int buf_size = 0;
    char *start = buf;
    unsigned long mem, value;
    int i = 0;
    unsigned* addr = 0;

    buf_size = min(count, (sizeof(buf) - 1));

    if (copy_from_user(buf, user_buf, buf_size)) {
        return -EFAULT;
    }
    buf[buf_size] = 0;

    while (*start == ' ') {
        start++;
    }

    mem = simple_strtoul(start, &start, 16);

    while (*start == ' ') {
        start++;
    }

    if (strict_strtoul(start, 16, &value)) {
        addr = (unsigned*)mem;
        printk("%p: ", addr);
        for (i = 0; i < 8; i++) {
            printk("%08x, ", addr[i]);
        }
        printk("\n");
        return -EINVAL;
    }
    addr = (unsigned*)mem;
    printk("%p: %08x\n", addr, *addr);
    *addr = value;
    printk("%p: %08x^\n", addr, *addr);

    return buf_size;
}
static const struct file_operations mems_fops = {
    .open = mems_open_file,
    .read = mems_read_file,
    .write = mems_write_file,
};

static void aml_i2s_init_debugfs(void)
{
    debugfs_root = debugfs_create_dir("aml", NULL);
    if (IS_ERR(debugfs_root) || !debugfs_root) {
        printk("aml: Failed to create debugfs directory\n");
        debugfs_root = NULL;
    }

    debugfs_regs = debugfs_create_file("regs", 0644, debugfs_root, NULL, &regs_fops);
    if (!debugfs_regs) {
        printk("aml: Failed to create debugfs file\n");
    }

    debugfs_mems = debugfs_create_file("mems", 0644, debugfs_root, NULL, &mems_fops);
    if (!debugfs_mems) {
        printk("aml: Failed to create debugfs file\n");
    }
}
static void aml_i2s_cleanup_debugfs(void)
{
    debugfs_remove_recursive(debugfs_root);
}
#else
static void aml_i2s_init_debugfs(void)
{
}
static void aml_i2s_cleanup_debugfs(void)
{
}
#endif

struct snd_soc_platform_driver aml_soc_platform = {
    .ops    = &aml_i2s_ops,
    .pcm_new    = aml_i2s_new,
    .pcm_free   = aml_i2s_free_dma_buffers,
    .suspend    = aml_i2s_suspend,
    .resume     = aml_i2s_resume,
};

EXPORT_SYMBOL_GPL(aml_soc_platform);

static int aml_soc_platform_probe(struct platform_device *pdev)
{
    ALSA_TRACE();
    return snd_soc_register_platform(&pdev->dev, &aml_soc_platform);
}

static int aml_soc_platform_remove(struct platform_device *pdev)
{
    snd_soc_unregister_platform(&pdev->dev);
    return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id amlogic_audio_dt_match[] = {
    {
        .compatible = "amlogic,aml-i2s",
    },
    {},
};
#else
#define amlogic_audio_dt_match NULL
#endif

static struct platform_driver aml_i2s_driver = {
    .driver = {
        .name = "aml-i2s",
        .owner = THIS_MODULE,
        .of_match_table = amlogic_audio_dt_match,
    },

    .probe = aml_soc_platform_probe,
    .remove = aml_soc_platform_remove,
};

static int __init aml_alsa_audio_init(void)
{
    aml_i2s_init_debugfs();
    return platform_driver_register(&aml_i2s_driver);
}

static void __exit aml_alsa_audio_exit(void)
{
    aml_i2s_cleanup_debugfs();
    platform_driver_unregister(&aml_i2s_driver);
}

module_init(aml_alsa_audio_init);
module_exit(aml_alsa_audio_exit);

MODULE_AUTHOR("AMLogic, Inc.");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AML audio driver for ALSA");
