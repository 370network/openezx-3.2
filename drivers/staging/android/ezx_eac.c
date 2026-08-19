/*
   /dev/eac audio driver for the EZX platform
   abuses /dev/dsp and /dev/controlC0 to make audio work
   on the MING

   Copyright Richard Gracik @ 370network (mailto:morc@370.network)

   Based on
** drivers/misc/goldfish_audio.c
**
** Copyright (C) 2007 Google, Inc.
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
*/

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include <sound/asound.h>
#include <linux/soundcard.h>

MODULE_AUTHOR("Google, Inc. & 370network");
MODULE_DESCRIPTION("Android EZX Audio Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

static long custom_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -ENOTTY;
	mm_segment_t oldfs = get_fs();

	set_fs(KERNEL_DS);

	if (file && file->f_op && file->f_op->unlocked_ioctl)
		ret = file->f_op->unlocked_ioctl(file, cmd, arg);

	set_fs(oldfs);
	return ret;
}

static void control(struct file *ctl, const char *name, int val)
{
	struct snd_ctl_elem_value *ev;

	ev = kzalloc(sizeof(*ev), GFP_KERNEL);
	if (!ev) 
		return;

	ev->id.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
	strncpy(ev->id.name, name, sizeof(ev->id.name) - 1);

	ev->value.integer.value[0] = val;
	ev->value.integer.value[1] = val;
	ev->value.enumerated.item[0] = (unsigned int)val;

	custom_ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, (unsigned long)ev);
	kfree(ev);
}

static int eac_audio_open(struct inode *inode, struct file *file)
{
	struct file *ctl_file;
	struct file *dsp_file;

	ctl_file = filp_open("/dev/controlC0", O_RDWR, 0);
	if (!IS_ERR(ctl_file)) {
		control(ctl_file, "Master Playback Volume", 13); //volume 0-15
		control(ctl_file, "Output Mixer AL Switch", 1); //left headphone channel
		control(ctl_file, "Output Mixer AR Switch", 1); //right headphone channel
		control(ctl_file, "Output Mixer A2 Switch", 1); //loudspeaker output
		control(ctl_file, "Downmixer", 3); //downmixing for the loudspeaker, for now
		filp_close(ctl_file, NULL);
	}

	dsp_file = filp_open("/dev/dsp", O_WRONLY, 0);
	if (IS_ERR(dsp_file)) {
		pr_err("eac_audio failed opening /dev/dsp\n");
		return PTR_ERR(dsp_file);
	}


	int fmt = AFMT_S16_LE;
	int chan = 2;
	int bits = 44100;

	custom_ioctl(dsp_file, SNDCTL_DSP_SETFMT, (unsigned long)&fmt);
	custom_ioctl(dsp_file, SNDCTL_DSP_CHANNELS, (unsigned long)&chan);
	custom_ioctl(dsp_file, SNDCTL_DSP_SPEED, (unsigned long)&bits);

	file->private_data = dsp_file;
	return 0;
}

static ssize_t eac_audio_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct file *dsp_file = file->private_data;

	if (!dsp_file || !dsp_file->f_op->write)
		return -ENODEV;

	return vfs_write(dsp_file, buf, count, &dsp_file->f_pos);
}

static ssize_t eac_audio_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	//no passing of the mic into Android for now
	return -ENODEV;
}

static int eac_audio_release(struct inode *inode, struct file *file)
{
	struct file *dsp_file = file->private_data;

	if (dsp_file)
		filp_close(dsp_file, NULL);

	return 0;
}

static long eac_audio_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	//ignore ioctls flowing into /dev/eac for now
	//AudioHardwareHTC uses them to switch modes

	/* temporary workaround, until we switch to the ALSA API */
	if (cmd == 315)
		return -1;
	else
		return 0;
}

/* file operations for /dev/eac */
static struct file_operations eac_audio_fops = {
	.owner          = THIS_MODULE,
	.open           = eac_audio_open,
	.read           = eac_audio_read,
	.write          = eac_audio_write,
	.release        = eac_audio_release,
	.unlocked_ioctl = eac_audio_ioctl,

};

static struct miscdevice eac_audio_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "eac",
	.fops  = &eac_audio_fops,
};

static int __init eac_audio_init(void)
{
	printk("eac_audio_init\n");
	return misc_register(&eac_audio_misc);
}

static void __exit eac_audio_exit(void)
{
	misc_deregister(&eac_audio_misc);
}

//original goldfish audio did a platform device
//this one is ommited for now.

module_init(eac_audio_init);
module_exit(eac_audio_exit);
