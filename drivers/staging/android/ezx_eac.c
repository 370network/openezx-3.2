/*
   /dev/eac audio driver for the EZX platform
   abuses /dev/dsp and /dev/controlC0 to make audio work
   on the MING

   Copyright (C) 2026 Richard Gracik @ 370network (mailto:morc@370.network)

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

//AudioHardwareHTC known ioctl names
#define IOCTL_BT_SCO_AUDIO_PATH_CONTROL 300
#define IOCTL_SET_SPEAKERPHONE          311

struct eac_audio_files {
    struct file *ctl_file;
    struct file *dsp_file;
};

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
	struct eac_audio_files *files;

	printk("eac_audio attempt to open /dev/eac\n");

	files = kzalloc(sizeof(*files), GFP_KERNEL);
	if (!files)
		return -ENOMEM;

	files->ctl_file = filp_open("/dev/controlC0", O_RDWR, 0);
	if (IS_ERR(files->ctl_file)) {
                pr_warn("eac_audio failed opening /dev/controlC0, we are possibly already opened by AudioFlinger\n");
                files->ctl_file = NULL;
        } else {
		control(files->ctl_file, "Master Playback Volume", 10); //volume 0-15 (13 distorts a lot already)
		control(files->ctl_file, "Output Mixer AL Switch", 1); //left headphone channel
		control(files->ctl_file, "Output Mixer AR Switch", 1); //right headphone channel
		control(files->ctl_file, "Output Mixer A1 Switch", 0); //earpiece output
		control(files->ctl_file, "Output Mixer A2 Switch", 1); //loudspeaker output
		control(files->ctl_file, "Downmixer", 3);	//2->1ch -6db - downmixing for the loudspeaker
							//maybe regular 2->1ch is enough, needd more testing
							//plus disable it for headphones, otherwise you turn mono
		//filp_close(files->ctl_file, NULL);
	}

	files->dsp_file = filp_open("/dev/dsp", O_WRONLY, 0);
	if (IS_ERR(files->dsp_file)) {
		pr_warn("eac_audio failed opening /dev/dsp, we are possibly already opened by AudioFlinger\n");
		files->dsp_file = NULL;
	} else {
		int fmt = AFMT_S16_LE;
		int chan = 2;
		int bits = 44100;

		custom_ioctl(files->dsp_file, SNDCTL_DSP_SETFMT, (unsigned long)&fmt);
		custom_ioctl(files->dsp_file, SNDCTL_DSP_CHANNELS, (unsigned long)&chan);
		custom_ioctl(files->dsp_file, SNDCTL_DSP_SPEED, (unsigned long)&bits);
	}

	file->private_data = files;
	return 0;
}

static ssize_t eac_audio_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct eac_audio_files *files = file->private_data;

	if (!files->dsp_file || !files->dsp_file->f_op->write)
		return -ENODEV;

	return vfs_write(files->dsp_file, buf, count, &files->dsp_file->f_pos);
}

static ssize_t eac_audio_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	//no passing of the mic into Android for now
	return -ENODEV;
}

static int eac_audio_release(struct inode *inode, struct file *file)
{
	struct eac_audio_files *files = file->private_data;

	if (files->dsp_file)
		filp_close(files->dsp_file, NULL);

	if (files->ctl_file)
		filp_close(files->ctl_file, NULL);

	kfree(files);
	file->private_data = NULL;

	return 0;
}

static long eac_audio_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct eac_audio_files *files = file->private_data;

	printk("eac_audio ioctl cmd %d\n", cmd);

	int value;
	if (get_user(value, (int __user *)arg)) {
		return -EFAULT;
	}

	switch (cmd) {
		case 303: //(libhardware) android::AudioDriver::setVolume
		case 304: //(libhardware) android::AudioDriver::getVolume - specific stream type
		case 305: //(libhardware) android::AudioDriver::setStreamType
		case 307: //(libaudioflinger) AudioHardwareHTC::setVoiceValue &
			//(libhardware) android::AudioDriver::setVolume - master (?)
		case 308: //(libhardware) android::AudioDriver::getVolume - master (?)
		case 309: //(libhardware) android::AudioDriver::muteMicrophone
		case 310: //(libhardware) android::AudioDriver::isMicrophoneMuted
		case 312: //(libhardware) android::AudioDriver::isSpeakerphoneOn - get state
		case 317: //(libhardware) android::AudioDriver::setSampleRate
			printk("eac_audio stub cmd %d value %d\n", cmd, value);
			return 0;

		case 313: //(libhardware) android::AudioDriver::stayAwake
			printk("eac_audio was asked to stay awake for suspend: value %d - we ignore you :D\n", value);
			return 0;

		case IOCTL_BT_SCO_AUDIO_PATH_CONTROL:	//300 | (libaudioflinger) AudioHardwareHTC::enableBluetooth &
							//(libhardware) android::AudioDriver::bluetooth &
							//libhardware) android::AudioDriver::speakerphone - disable BT
		case 315: //(libaudioflinger) AudioHardwareHTC::open
			return -1;

		case IOCTL_SET_SPEAKERPHONE:	//311 | (libaudioflinger) AudioHardwareHTC::enableSpeaker &
						//(libhardware) android::AudioDriver::speakerphone
			printk("eac_audio set speakerphone %d\n", value);
			control(files->ctl_file, "Output Mixer A1 Switch", !value); //earpiece output
			control(files->ctl_file, "Output Mixer A2 Switch", value); //loudspeaker output
			break;
		default:
			return 0;
	}
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
