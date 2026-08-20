/*

  backlight_compat.c

  shim driver that mirrors a sysclass backlight device to a leds device
  mirrorring only to led-backlight (m5+) for now...

  Copyright (C) 2026 Richard Gracik @ 370network (mailto:morc@370.network)

*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/leds.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>

#define BACKLIGHT_NAME "pwm-backlight.0"
#define BACKLIGHT_MIN 400
#define BACKLIGHT_MAX 1023

static struct led_classdev backlight_compat_cdev;
static struct backlight_device *hw_bl;

static void backlight_compat_set_brightness(struct led_classdev *led_cdev, enum led_brightness value)
{
	if (hw_bl) {
		int recalc = BACKLIGHT_MIN + ((value * (BACKLIGHT_MAX - BACKLIGHT_MIN)) / 255);
		hw_bl->props.brightness = recalc;
		backlight_update_status(hw_bl);
	}
}

static int __init backlight_compat_init(void)
{
	struct device *parent;
	int ret;

	parent = bus_find_device_by_name(&platform_bus_type, NULL, BACKLIGHT_NAME);
	if (!parent)
		return -ENODEV;

	hw_bl = dev_get_drvdata(parent);
	put_device(parent);

	if (!hw_bl)
		return -ENODEV;

	backlight_compat_cdev.name = "lcd-backlight";
	backlight_compat_cdev.brightness_set = backlight_compat_set_brightness;
	backlight_compat_cdev.max_brightness = 255; //android seems to do a 20-255 range

	ret = led_classdev_register(NULL, &backlight_compat_cdev);
	if (ret)
		return ret;

	return 0;
}

static void __exit backlight_compat_exit(void)
{
	led_classdev_unregister(&backlight_compat_cdev);
}

module_init(backlight_compat_init);
module_exit(backlight_compat_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Android M5+ backlight compat shim");
