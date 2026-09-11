/*
   "fake" /dev/event0 event handler driver for Android M1-M4
   abuses benevolency of the Android init system to make
   the vibrator and other things to work

   also includes an additional kernel compile flag mode for
   Android M1 and M2 to turn this driver into a input mux
   so that the event handler in these versions actually works

   Copyright (C) 2026 Richard Gracik @ 370network (mailto:morc@370.network)
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/slab.h>

//htc_mfg_test vibration test function
//0 Sooner Test loads the library, that's bad
//E/HTCMFGTest(  439): Loading htc_mfg ...
//D/dalvikvm(  439): LOADING path /android/lib/libhtc_mfg.so 0x409b7c20
//I/dalvikvm(  439): Added shared lib /android/lib/libhtc_mfg.so 0x409b7c20
//E/HTCMFG_JNI(  439): MFG JNI onLoad!!
/*void set_vibrator(int value){
	printk("event0_compat: htc_mfg_test: set_vibrator %d", value);
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
EXPORT_SYMBOL(set_vibrator);
*/

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

#ifdef CONFIG_ANDROID_EVENT0_MUX
static void event0_mux_event(struct input_handle *h, unsigned int type, unsigned int code, int value)
{
    if (h->dev == event0_dev)
        return;

    input_event(event0_dev, type, code, value);
    input_sync(event0_dev);
}

static int event0_mux_connect(struct input_handler *handler, struct input_dev *dev, const struct input_device_id *id)
{
    struct input_handle *h;
    int error;

    if (dev == event0_dev)
        return -ENODEV;

    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h)
        return -ENOMEM;

    h->dev = dev;
    h->handler = handler;
    h->name = "event0_mux";

    error = input_register_handle(h);
    if (error) goto err_free;

    error = input_open_device(h);
    if (error) goto err_unreg;

    pr_info("event0_compat muxing inputs from '%s'\n", dev->name);
    return 0;

err_unreg:
    input_unregister_handle(h);
err_free:
    kfree(h);
    return error;
}

static void event0_mux_disconnect(struct input_handle *h)
{
    input_close_device(h);
    input_unregister_handle(h);
    kfree(h);
}

static const struct input_device_id event0_mux_ids[] = {
    { .driver_info = 1 },
    { },
};
MODULE_DEVICE_TABLE(input, event0_mux_ids);

static struct input_handler event0_mux_handler = {
    .event      = event0_mux_event,
    .connect    = event0_mux_connect,
    .disconnect = event0_mux_disconnect,
    .name       = "event0_mux_handler",
    .id_table   = event0_mux_ids,
};
#endif

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
    bitmap_fill(event0_dev->sndbit, SND_CNT);
    bitmap_fill(event0_dev->relbit, REL_CNT);
    bitmap_fill(event0_dev->mscbit, MSC_CNT);

    event0_dev->event = event0_handle;

    error = input_register_device(event0_dev);
    if (error) {
        input_free_device(event0_dev);
        return error;
    }

#ifdef CONFIG_ANDROID_EVENT0_MUX
    error = input_register_handler(&event0_mux_handler);
    if (error) {
        input_unregister_device(event0_dev);
        return error;
    }
#endif

    pr_info("event0_compat driver init\n");
    return 0;
}

static void __exit event0_compat_exit(void) {
#ifdef CONFIG_ANDROID_EVENT0_MUX
    input_unregister_handler(&event0_mux_handler);
#endif
    input_unregister_device(event0_dev);
}

//initializing alongside the filesystem init
//this is ugly, but I atleast get event0 all the time
fs_initcall(event0_compat_init);
module_exit(event0_compat_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Android M1-M4 event0 handler");
