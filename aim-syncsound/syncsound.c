/*
 * syncsound.c - ALSA Application Interface Module
 *
 * Copyright (C) 2017 Cetitec GmbH
 * Copyright (C) 2015 Microchip Technology Germany II GmbH & Co. KG
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This file is licensed under GPLv2.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/printk.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/ctype.h>
#include "mostcore.h"

#define DRIVER_NAME "syncsound"

#define FCNT_VALUE 5
#define SYNC_BUFFER_DEP(bpf) (4 * (1 << FCNT_VALUE) * (bpf))
#define MAX_PERIOD_SIZE (8192) /* DIM2 restriction */

static struct list_head dev_list;
static struct most_aim aim; /* forward declaration */
static struct snd_card *card;

struct mostcore_channel {
	int channel_id;
	struct most_interface *iface;
	struct most_channel_config *cfg;
};

struct channel {
	struct mostcore_channel rx, tx;
	int syncsound_id;
	struct snd_pcm_substream *substream;
	struct snd_pcm_hardware pcm_hardware;
	struct list_head list;
	unsigned int period_pos;
	unsigned int buffer_pos;
	bool is_stream_running, started;
	int packets_per_xact;
	int buffer_size;

	struct task_struct *playback_task;
	wait_queue_head_t playback_waitq;

	void (*copy_fn)(void *alsa, void *most, unsigned int bytes);
};

static void swap_copy16(u16 *dest, const u16 *source, unsigned int bytes)
{
	unsigned int i = 0;

	while (i < (bytes / 2)) {
		dest[i] = swab16(source[i]);
		i++;
	}
}

static void swap_copy24(u8 *dest, const u8 *source, unsigned int bytes)
{
	unsigned int i = 0;

	while (i < bytes - 2) {
		dest[i] = source[i + 2];
		dest[i + 1] = source[i + 1];
		dest[i + 2] = source[i];
		i += 3;
	}
}

static void swap_copy32(u32 *dest, const u32 *source, unsigned int bytes)
{
	unsigned int i = 0;

	while (i < bytes / 4) {
		dest[i] = swab32(source[i]);
		i++;
	}
}

static void alsa_to_most_memcpy(void *alsa, void *most, unsigned int bytes)
{
	memcpy(most, alsa, bytes);
}

static void alsa_to_most_copy16(void *alsa, void *most, unsigned int bytes)
{
	swap_copy16(most, alsa, bytes);
}

static void alsa_to_most_copy24(void *alsa, void *most, unsigned int bytes)
{
	swap_copy24(most, alsa, bytes);
}

static void alsa_to_most_copy32(void *alsa, void *most, unsigned int bytes)
{
	swap_copy32(most, alsa, bytes);
}

static void most_to_alsa_memcpy(void *alsa, void *most, unsigned int bytes)
{
	memcpy(alsa, most, bytes);
}

static void most_to_alsa_copy16(void *alsa, void *most, unsigned int bytes)
{
	swap_copy16(alsa, most, bytes);
}

static void most_to_alsa_copy24(void *alsa, void *most, unsigned int bytes)
{
	swap_copy24(alsa, most, bytes);
}

static void most_to_alsa_copy32(void *alsa, void *most, unsigned int bytes)
{
	swap_copy32(alsa, most, bytes);
}

static struct channel *get_channel_rx(struct most_interface *iface,
				   int channel_id)
{
	struct channel *channel;

	list_for_each_entry(channel, &dev_list, list)
		if (channel->rx.iface == iface && channel->rx.channel_id == channel_id)
			return channel;
	return NULL;
}

static struct channel *get_channel_tx(struct most_interface *iface,
				   int channel_id)
{
	struct channel *channel;

	list_for_each_entry(channel, &dev_list, list)
		if (channel->tx.iface == iface && channel->tx.channel_id == channel_id)
			return channel;
	return NULL;
}

static bool copy_data(struct channel *channel, void *mbo_address, uint frames, uint frame_bytes)
{
	struct snd_pcm_runtime *const runtime = channel->substream->runtime;
	//unsigned int const frame_bytes = channel->cfg->subbuffer_size;
	unsigned int const buffer_size = runtime->buffer_size;
	unsigned int fr0;

	/*if (channel->cfg->direction & MOST_CH_RX)
		frames = mbo->processed_length / frame_bytes;
	else
		frames = mbo->buffer_length / frame_bytes;*/
	fr0 = min(buffer_size - channel->buffer_pos, frames);

	channel->copy_fn(runtime->dma_area + channel->buffer_pos * frame_bytes,
			 mbo_address,
			 fr0 * frame_bytes);

	if (frames > fr0) {
		/* wrap around at end of ring buffer */
		channel->copy_fn(runtime->dma_area,
				 mbo_address + fr0 * frame_bytes,
				 (frames - fr0) * frame_bytes);
	}

	channel->buffer_pos += frames;
	if (channel->buffer_pos >= buffer_size)
		channel->buffer_pos -= buffer_size;
	channel->period_pos += frames;
	if (channel->period_pos >= runtime->period_size) {
		channel->period_pos -= runtime->period_size;
		return true;
	}

	return false;
}

static int playback_thread(void *data)
{
	struct channel *const channel = data;

	while (!kthread_should_stop()) {
		struct mbo *mbo = NULL;
		bool period_elapsed = false;

		wait_event_interruptible(
			channel->playback_waitq,
			kthread_should_stop() ||
			(channel->is_stream_running &&
			 (mbo = most_get_mbo(channel->tx.iface,
					     channel->tx.channel_id,
					     &aim))));
		if (!mbo)
			continue;

		if (channel->is_stream_running)
			period_elapsed = copy_data(channel, mbo->virt_address,
						   mbo->buffer_length / channel->tx.cfg->subbuffer_size,
						   channel->tx.cfg->subbuffer_size);
		else
			memset(mbo->virt_address, 0, mbo->buffer_length);

		most_submit_mbo(mbo);
		if (period_elapsed)
			snd_pcm_period_elapsed(channel->substream);
	}

	return 0;
}

static void update_pcm_hw_from_cfg(struct channel *channel,
				   const struct most_channel_config *cfg)
{
	uint buf_size = channel->buffer_size == -1 ? MAX_PERIOD_SIZE : channel->buffer_size;

	channel->pcm_hardware.periods_min = 1;
	channel->pcm_hardware.periods_max = cfg->num_buffers;
	if (channel->buffer_size != -1)
		channel->pcm_hardware.period_bytes_min = buf_size;
	channel->pcm_hardware.period_bytes_max = buf_size;
	channel->pcm_hardware.buffer_bytes_max = buf_size * cfg->num_buffers;
}

static int pcm_open_play(struct snd_pcm_substream *substream)
{
	struct channel *channel = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;

	pr_debug("\n");
	if (!channel->tx.iface)
		return -ENOTCONN;
	channel->substream = substream;
	update_pcm_hw_from_cfg(channel, channel->tx.cfg);
	runtime->hw = channel->pcm_hardware;
	return 0;
}

static int pcm_open_capture(struct snd_pcm_substream *substream)
{
	struct channel *channel = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;

	pr_debug("\n");
	if (!channel->rx.iface)
		return -ENOTCONN;
	channel->substream = substream;
	update_pcm_hw_from_cfg(channel, channel->rx.cfg);
	runtime->hw = channel->pcm_hardware;
	return 0;
}

static int pcm_close_play(struct snd_pcm_substream *substream)
{
	struct channel *channel = substream->private_data;

	pr_debug("\n");
	kthread_stop(channel->playback_task);
	if (channel->started)
		most_stop_channel(channel->tx.iface, channel->tx.channel_id, &aim);
	channel->substream = NULL;
	channel->started = false;
	return 0;
}

static int pcm_close_capture(struct snd_pcm_substream *substream)
{
	struct channel *channel = substream->private_data;

	pr_debug("\n");
	if (channel->started)
		most_stop_channel(channel->rx.iface, channel->rx.channel_id, &aim);
	channel->substream = NULL;
	channel->started = false;
	return 0;
}

static void set_most_config(const struct channel *channel,
			    struct most_channel_config *cfg,
			    const struct snd_pcm_hw_params *hw_params)
{
	int width = snd_pcm_format_physical_width(params_format(hw_params));

	if (channel->packets_per_xact != -1)
		cfg->packets_per_xact = channel->packets_per_xact;
	cfg->subbuffer_size = width * params_channels(hw_params) / BITS_PER_BYTE;
	cfg->buffer_size = params_period_bytes(hw_params);
}

static int pcm_hw_params(struct snd_pcm_substream *substream,
			 struct snd_pcm_hw_params *hw_params)
{
	struct channel *channel = substream->private_data;

	if ((params_channels(hw_params) > channel->pcm_hardware.channels_max) ||
	    (params_channels(hw_params) < channel->pcm_hardware.channels_min)) {
		pr_err("Requested number of channels not supported.\n");
		return -EINVAL;
	}
	return snd_pcm_lib_alloc_vmalloc_buffer(substream, params_buffer_bytes(hw_params));
}

static int pcm_hw_params_play(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *hw_params)
{
	struct channel *channel = substream->private_data;
	int err;

	pr_debug("\n");
	err = pcm_hw_params(substream, hw_params);
	if (err < 0)
		return err;
	set_most_config(channel, channel->tx.cfg, hw_params);
	return 0;
}

static int pcm_hw_params_capture(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *hw_params)
{
	struct channel *channel = substream->private_data;
	int err;

	pr_debug("\n");
	err = pcm_hw_params(substream, hw_params);
	if (err < 0)
		return err;
	set_most_config(channel, channel->rx.cfg, hw_params);
	pr_debug("channels %d, buffer bytes %d, period bytes %d, frame size %d, sample size %d\n",
		 params_channels(hw_params),
		 params_buffer_bytes(hw_params),
		 params_period_bytes(hw_params),
		 channel->rx.cfg->subbuffer_size,
		 snd_pcm_format_physical_width(params_format(hw_params)));
	return 0;
}

static int pcm_hw_free(struct snd_pcm_substream *substream)
{
	pr_debug("\n");
	return snd_pcm_lib_free_vmalloc_buffer(substream);
}

static int pcm_prepare_play(struct snd_pcm_substream *substream)
{
	int err;
	struct channel *channel = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int width = snd_pcm_format_physical_width(runtime->format);

	pr_debug("\n");
	channel->copy_fn = NULL;
	if (snd_pcm_format_big_endian(runtime->format) || width == 8)
		channel->copy_fn = alsa_to_most_memcpy;
	else if (width == 16)
		channel->copy_fn = alsa_to_most_copy16;
	else if (width == 24)
		channel->copy_fn = alsa_to_most_copy24;
	else if (width == 32)
		channel->copy_fn = alsa_to_most_copy32;
	if (!channel->copy_fn) {
		pr_err("unsupported format\n");
		return -EINVAL;
	}
	channel->period_pos = 0;
	channel->buffer_pos = 0;
	channel->playback_task = kthread_run(playback_thread, channel, "most");
	if (IS_ERR(channel->playback_task)) {
		err = PTR_ERR(channel->playback_task);
		pr_debug("Couldn't start thread: %d\n", err);
		return -ENOMEM;
	}
	err = most_start_channel(channel->tx.iface, channel->tx.channel_id, &aim);
	if (err) {
		pr_debug("most_start_channel() failed: %d\n", err);
		kthread_stop(channel->playback_task);
		return -EBUSY;
	}
	channel->started = true;
	return 0;
}

static int pcm_prepare_capture(struct snd_pcm_substream *substream)
{
	struct channel *channel = substream->private_data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	int width = snd_pcm_format_physical_width(runtime->format);

	pr_debug("\n");
	channel->copy_fn = NULL;
	if (snd_pcm_format_big_endian(runtime->format) || width == 8)
		channel->copy_fn = most_to_alsa_memcpy;
	else if (width == 16)
		channel->copy_fn = most_to_alsa_copy16;
	else if (width == 24)
		channel->copy_fn = most_to_alsa_copy24;
	else if (width == 32)
		channel->copy_fn = most_to_alsa_copy32;
	if (!channel->copy_fn) {
		pr_err("unsupported format\n");
		return -EINVAL;
	}
	channel->period_pos = 0;
	channel->buffer_pos = 0;
	if (most_start_channel(channel->rx.iface, channel->rx.channel_id, &aim)) {
		pr_err("most_start_channel() failed!\n");
		return -EBUSY;
	}
	channel->started = true;
	return 0;
}

static int pcm_trigger(struct snd_pcm_substream *substream, int cmd)
{
	struct channel *channel = substream->private_data;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		pr_debug("start\n");
		channel->is_stream_running = true;
		wake_up_interruptible(&channel->playback_waitq);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
		pr_debug("stop\n");
		channel->is_stream_running = false;
		return 0;

	default:
		pr_info("pcm_trigger(), invalid\n");
		return -EINVAL;
	}
	return 0;
}

static snd_pcm_uframes_t pcm_pointer(struct snd_pcm_substream *substream)
{
	struct channel *channel = substream->private_data;

	return channel->buffer_pos;
}

static const struct snd_pcm_ops play_ops = {
	.open       = pcm_open_play,
	.close      = pcm_close_play,
	.ioctl      = snd_pcm_lib_ioctl,
	.hw_params  = pcm_hw_params_play,
	.hw_free    = pcm_hw_free,
	.prepare    = pcm_prepare_play,
	.trigger    = pcm_trigger,
	.pointer    = pcm_pointer,
	.page       = snd_pcm_lib_get_vmalloc_page,
	.mmap       = snd_pcm_lib_mmap_vmalloc,
};
static const struct snd_pcm_ops capture_ops = {
	.open       = pcm_open_capture,
	.close      = pcm_close_capture,
	.ioctl      = snd_pcm_lib_ioctl,
	.hw_params  = pcm_hw_params_capture,
	.hw_free    = pcm_hw_free,
	.prepare    = pcm_prepare_capture,
	.trigger    = pcm_trigger,
	.pointer    = pcm_pointer,
	.page       = snd_pcm_lib_get_vmalloc_page,
	.mmap       = snd_pcm_lib_mmap_vmalloc,
};

static int probe_channel(struct most_interface *iface, int channel_id,
			 struct most_channel_config *cfg,
			 struct kobject *parent, char *args)
{
	int syncsound_id;
	struct channel *channel;

	if (!iface)
		return -EINVAL;
	if (cfg->data_type != MOST_CH_SYNC) {
		pr_err("Incompatible channel type\n");
		return -EINVAL;
	}
	if (cfg->direction == MOST_CH_RX)
		channel = get_channel_rx(iface, channel_id);
	else if (cfg->direction == MOST_CH_TX)
		channel = get_channel_tx(iface, channel_id);
	if (channel) {
		pr_err("channel (%s:%d) is already linked to MLB_SYNC%d\n",
		       iface->description, channel_id, channel->syncsound_id);
		return -EINVAL;
	}
	while (*args && !isdigit(*args))
		++args;
	if (!*args)
		return -EINVAL;
	if (kstrtoint(args, 0, &syncsound_id))
		return -EINVAL;
	list_for_each_entry(channel, &dev_list, list)
		if (channel->syncsound_id == syncsound_id)
			break;
	if (&channel->list == &dev_list)
		return -ENOENT;
	if (cfg->direction == MOST_CH_RX) {
		channel->rx.iface = iface;
		channel->rx.channel_id = channel_id;
		channel->rx.cfg = cfg;
	} else {
		channel->tx.iface = iface;
		channel->tx.channel_id = channel_id;
		channel->tx.cfg = cfg;
	}
	if (channel->packets_per_xact == -1)
		channel->packets_per_xact = cfg->packets_per_xact;
	/*
	 * buffer_size is not taken from the mostcore configuration
	 * to keep the syncsound buffer size calculation by default.
	 */
	return 0;
}

static int disconnect_channel(struct most_interface *iface, int channel_id)
{
	bool is_tx = true;
	struct channel *channel;

	channel = get_channel_rx(iface, channel_id);
	if (channel)
		is_tx = false;
	else
		channel = get_channel_tx(iface, channel_id);
	if (!channel) {
		pr_err("sound_disconnect_channel(), invalid channel %d\n",
		       channel_id);
		return -EINVAL;
	}
	if (is_tx) {
		channel->tx.iface = NULL;
		channel->tx.channel_id = 0;
		channel->tx.cfg = NULL;
	} else {
		channel->rx.iface = NULL;
		channel->rx.channel_id = 0;
		channel->rx.cfg = NULL;
	}
	return 0;
}

static int rx_completion(struct mbo *mbo)
{
	struct channel *channel = get_channel_rx(mbo->ifp, mbo->hdm_channel_id);
	bool period_elapsed = false;

	if (!channel) {
		pr_debug("invalid channel %d\n", mbo->hdm_channel_id);
		return -EINVAL;
	}

	if (channel->is_stream_running)
		period_elapsed = copy_data(channel, mbo->virt_address,
					   mbo->processed_length / channel->rx.cfg->subbuffer_size,
					   channel->rx.cfg->subbuffer_size);
	most_put_mbo(mbo);

	if (period_elapsed)
		snd_pcm_period_elapsed(channel->substream);
	return 0;
}

static int tx_completion(struct most_interface *iface, int channel_id)
{
	struct channel *channel = get_channel_tx(iface, channel_id);

	if (!channel) {
		pr_debug("invalid channel %d\n", channel_id);
		return -EINVAL;
	}

	wake_up_interruptible(&channel->playback_waitq);
	return 0;
}

/**
 * Initialization of the struct most_aim
 */
static struct most_aim aim = {
	.name = DRIVER_NAME,
	.probe_channel = probe_channel,
	.disconnect_channel = disconnect_channel,
	.rx_completion = rx_completion,
	.tx_completion = tx_completion,
};

extern u32 syncsound_get_num_devices(void); /* aim-mlb150 */

static __initconst const struct snd_pcm_hardware most_hardware = {
	.info = SNDRV_PCM_INFO_MMAP          |
		SNDRV_PCM_INFO_MMAP_VALID    |
		SNDRV_PCM_INFO_BATCH         |
		SNDRV_PCM_INFO_INTERLEAVED   |
		SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.buffer_bytes_max = 128 * MAX_PERIOD_SIZE,
	.period_bytes_min = 128,
	.period_bytes_max = MAX_PERIOD_SIZE, /* buffer_size */
	.periods_min = 1,
	.periods_max = 128, /* num_buffers */
	.channels_min = 1,
	.channels_max = 6,
	.formats = SNDRV_PCM_FMTBIT_S16_BE  |
		   SNDRV_PCM_FMTBIT_S16_LE  |
		   SNDRV_PCM_FMTBIT_S24_3BE |
		   SNDRV_PCM_FMTBIT_S24_3LE,
};

struct syncsound_attr {
	char name[32];
	struct channel *channel;
	struct device_attribute dev;
};
static ssize_t packets_per_xact_store(struct device *dev, struct device_attribute *dev_attr,
				      const char *buf, size_t count)
{
	struct syncsound_attr *attr = container_of(dev_attr, struct syncsound_attr, dev);
	int ret = kstrtoint(buf, 0, &attr->channel->packets_per_xact);

	return ret ? ret : count;
}
static ssize_t packets_per_xact_show(struct device *dev,
				     struct device_attribute *dev_attr,
				     char *buf)
{
	struct syncsound_attr *attr = container_of(dev_attr, struct syncsound_attr, dev);

	return sprintf(buf, "%d ", attr->channel->packets_per_xact);
}
static ssize_t buffer_size_store(struct device *dev, struct device_attribute *dev_attr,
				      const char *buf, size_t count)
{
	struct syncsound_attr *attr = container_of(dev_attr, struct syncsound_attr, dev);
	int ret = kstrtoint(buf, 0, &attr->channel->buffer_size);

	return ret ? ret : count;
}
static ssize_t buffer_size_show(struct device *dev,
				     struct device_attribute *dev_attr,
				     char *buf)
{
	struct syncsound_attr *attr = container_of(dev_attr, struct syncsound_attr, dev);

	return sprintf(buf, "%d ", attr->channel->buffer_size);
}

static DEVICE_ATTR_RW(packets_per_xact);
static DEVICE_ATTR_RW(buffer_size);
static const struct device_attribute *syncsound_attrs[] = {
	&dev_attr_packets_per_xact,
	&dev_attr_buffer_size,
};
static struct attribute *dev_attrs[SNDRV_CARDS * ARRAY_SIZE(syncsound_attrs) + 1];
static struct attribute_group dev_attr_group = {
	.name = "syncsound",
	.attrs = dev_attrs,
};
static void __init init_channel_attrs(int syncsound_id, struct syncsound_attr *attr, struct channel *channel)
{
	struct syncsound_attr *t;
	uint i, pos;

	pos = syncsound_id * ARRAY_SIZE(syncsound_attrs);
	attr += pos;
	for (i = 0, t = attr; i < ARRAY_SIZE(syncsound_attrs); ++i, ++t) {
		t->channel = channel;
		t->dev = *syncsound_attrs[i];
		snprintf(t->name, sizeof(t->name), "%s%d",
			 t->dev.attr.name, syncsound_id);
		t->dev.attr.name = t->name;
		dev_attrs[pos + (t - attr)] = &t->dev.attr;
	}
}

static int __init mod_init(void)
{
	int ret, i;
	uint max_pcms = min_t(uint, syncsound_get_num_devices(), SNDRV_CARDS);
	struct channel *channels;
	struct syncsound_attr *attr;

	pr_info("init()\n");
	INIT_LIST_HEAD(&dev_list);
	ret = snd_card_new(NULL, -1, NULL, THIS_MODULE,
			   max_pcms * sizeof(*channels) +
			   max_pcms * sizeof(*attr) *
			   ARRAY_SIZE(syncsound_attrs),
			   &card);
	if (ret)
		return ret;
	channels = card->private_data;
	attr = (void *)&channels[max_pcms];
	for (i = 0; i < max_pcms; ++i) {
		struct snd_pcm *pcm;
		struct channel *channel = &channels[i];

		channel->rx.cfg = NULL;
		channel->rx.iface = NULL;
		channel->rx.channel_id = 0;
		channel->tx.cfg = NULL;
		channel->tx.iface = NULL;
		channel->tx.channel_id = 0;
		channel->syncsound_id = i;
		channel->packets_per_xact = 255;
		channel->buffer_size = -1;
		channel->pcm_hardware = most_hardware;
		init_waitqueue_head(&channel->playback_waitq);
		strlcpy(card->driver, "MLB_Sync_Driver", sizeof(card->driver));
		strlcpy(card->shortname, "MLB_Sync_Audio", sizeof(card->shortname));
		strlcpy(card->longname, "Virtual soundcard over MLB synchronous channels", sizeof(card->longname));
		ret = snd_pcm_new(card, card->driver, i, 1, 1, &pcm);
		if (ret)
			goto err_free_card;
		init_channel_attrs(i, attr, channel);
		snprintf(pcm->name, sizeof(pcm->name), "MLB_SYNC%d", i);
		pcm->private_data = channel;
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &play_ops);
		snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &capture_ops);
		list_add_tail(&channel->list, &dev_list);
	}
	ret = snd_card_add_dev_attr(card, &dev_attr_group);
	if (ret)
		goto err_free_card;
	ret = snd_card_register(card);
	if (ret)
		goto err_free_card;
	return most_register_aim(&aim);
err_free_card:
	snd_card_free(card);
	pr_debug("ret %d\n", ret);
	return ret;
}

static void __exit mod_exit(void)
{
	pr_info("exit()\n");
	snd_card_free(card);
	most_deregister_aim(&aim);
}

module_init(mod_init);
module_exit(mod_exit);

MODULE_AUTHOR("Cetitec GmbH <support@cetitec.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ALSA AIM (syncsound interface) for mostcore");

