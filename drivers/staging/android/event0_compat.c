/*
   "fake" /dev/event0 event handler driver for Android M1-M4
   abuses benevolency of the Android init system to make
   the vibrator and other things to work

   Copyright (C) 2026 Richard Gracik @ 370network (mailto:morc@370.network)
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/fs.h>
#include <asm/uaccess.h>

static struct input_dev *event0_dev;

static int event0_handle(struct input_dev *dev, unsigned int type, unsigned int code, int value) {
    //printk("event0_compat type %d code %d state %d\n", type, code, value);

    switch(type){
	case EV_LED: //M1 - M4 LCD backlight
	        pr_info("event0_compat backlight code %u state %d\n", code, value);
		if (code == 8){ //assuming that 8 is lcd-backlight, 4 could be keyboard/button-backlight
			const char *state = value ? "255\n" : "20\n";
			struct file *backlight = filp_open("/sys/class/leds/lcd-backlight/brightness", O_WRONLY, 0);
                	mm_segment_t filesystem;

                	if (!IS_ERR(backlight)) {
                	        filesystem = get_fs();
                	        set_fs(KERNEL_DS);

                	        vfs_write(backlight, state, strlen(state), &backlight->f_pos);

                	        set_fs(filesystem);
                	        filp_close(backlight, NULL);
                	}
		}
		break;

	case EV_SND: //M1 vibrator
		pr_info("event0_compat vibrator code %u state %d\n", code, value);
		const char *vibrating = value ? "-1\n" : "0\n";
		struct file *vib = filp_open("/sys/devices/platform/android-vibrator/enable", O_WRONLY, 0);
		mm_segment_t filesystem;

		if (!IS_ERR(vib)) {
			filesystem = get_fs();
			set_fs(KERNEL_DS);

			vfs_write(vib, vibrating, strlen(vibrating), &vib->f_pos);

			set_fs(filesystem);
			filp_close(vib, NULL);
		}
    }

    return 0;
}

static int __init event0_compat_init(void) {
    int error;

    event0_dev = input_allocate_device();
    if (!event0_dev)
        return -ENOMEM;

    event0_dev->name = "Android M1-M4 event0 handler";
    event0_dev->id.bustype = BUS_VIRTUAL;

    __set_bit(EV_SYN, event0_dev->evbit);
    __set_bit(EV_KEY, event0_dev->evbit);
    __set_bit(EV_REL, event0_dev->evbit);
    __set_bit(EV_MSC, event0_dev->evbit);
    __set_bit(EV_LED, event0_dev->evbit);
    __set_bit(EV_SND, event0_dev->evbit);

    memset(event0_dev->ledbit, 0xff, sizeof(event0_dev->ledbit));
    bitmap_fill(event0_dev->keybit, KEY_CNT);

    event0_dev->event = event0_handle;

    error = input_register_device(event0_dev);
    if (error) {
        input_free_device(event0_dev);
        return error;
    }

    pr_info("event0_compat driver init\n");
    return 0;
}

static void __exit event0_compat_exit(void) {
    input_unregister_device(event0_dev);
}

module_init(event0_compat_init);
module_exit(event0_compat_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Android M1-M4 event handler");
