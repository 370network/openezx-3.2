/*

  android-vibrator_compat.c

  shim driver that mirrors a LED vibrator device to a platform device
  mirrorring to android-vibrator (m4) and sardine-vibrator (m2 & m3)

  Copyright Richard Gráčik @ 370network (mailto:morc@370.network)

*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/leds.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include "timed_output.h"

#define VIBRATOR_LED_NAME      "ezx::vibrator"
#define VIBRATOR_PLATFORM_NAME "leds-regulator.0"

struct android_vibrator {
	struct led_classdev *led;

	struct timer_list timer;

	int enabled;
	unsigned long expires;

	struct timed_output_dev timed_output;
};

static struct android_vibrator *vibrator;


static int find_vibrator_child(struct device *dev, void *data)
{
	struct android_vibrator *vib = data;
	struct led_classdev *led;

	if (strcmp(dev_name(dev), VIBRATOR_LED_NAME))
		return 0;

	led = dev_get_drvdata(dev);

	if (!led)
		return 0;

	vib->led = led;

	return 1;
}

static int find_vibrator(struct android_vibrator *vib)
{
	struct device *parent;
	int ret;

	parent = bus_find_device_by_name(&platform_bus_type,
					 NULL,
					 VIBRATOR_PLATFORM_NAME);

	if (!parent)
		return -ENODEV;

	ret = device_for_each_child(parent, vib,
				    find_vibrator_child);

	put_device(parent);

	if (ret <= 0 || !vib->led)
		return -ENODEV;

	return 0;
}


static void vibrator_timer(unsigned long data)
{
	struct android_vibrator *vib = (struct android_vibrator *)data;

	led_brightness_set(vib->led, 0);

	vib->enabled = 0;
	vib->expires = 0;
}

static void vibrator_set(long duration)
{
	struct android_vibrator *vib = vibrator;

	if (!vib)
		return;

	/*
	 * Cancel any previous timeout.
	 */
	del_timer_sync(&vib->timer);

	if (duration == 0) {
		//printk(KERN_INFO "android-vibrator: OFF\n");

		led_brightness_set(vib->led, 0);

		vib->enabled = 0;
		vib->expires = 0;

		return;
	}

	if (duration < 0) {
		//printk(KERN_INFO "android-vibrator: ON indefinitely\n");

		led_brightness_set(vib->led, 1);

		vib->enabled = 1;
		vib->expires = 0;

		return;
	}

	//printk(KERN_INFO "android-vibrator: %ldms\n", duration);

	led_brightness_set(vib->led, 1);

	vib->enabled = 1;
	vib->expires = jiffies + msecs_to_jiffies(duration);

	mod_timer(&vib->timer, vib->expires);
}


static ssize_t enable_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	struct android_vibrator *vib = dev_get_drvdata(dev);
	long remaining = 0;

	if (vib->enabled && vib->expires) {
		remaining = jiffies_to_msecs(
			vib->expires - jiffies);

		if (remaining < 0)
			remaining = 0;
	}

	if (vib->enabled && !vib->expires)
		return sprintf(buf, "-1\n");

	return sprintf(buf, "%ld\n", remaining);
}


static ssize_t enable_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf,
			    size_t count)
{
	long duration;
	int ret;

	ret = strict_strtol(buf, 10, &duration);
	if (ret)
		return ret;

	//printk(KERN_INFO "android-vibrator: enable = %ld\n",duration);

	vibrator_set(duration);

	return count;
}


static DEVICE_ATTR(enable, 0644,
		   enable_show,
		   enable_store);


static void timed_output_enable(struct timed_output_dev *dev,
				int value)
{
	vibrator_set(value);
}

static int timed_output_get_time(struct timed_output_dev *dev)
{
	struct android_vibrator *vib = vibrator;
	unsigned long remaining;

	if (!vib || !vib->enabled)
		return 0;

	if (!vib->expires)
		return -1;

	remaining = vib->expires - jiffies;

	return jiffies_to_msecs(remaining);
}


static int vibrator_probe(struct platform_device *pdev)
{
	int ret;

	if (!vibrator) {
		vibrator = kzalloc(sizeof(*vibrator), GFP_KERNEL);
		if (!vibrator)
			return -ENOMEM;

		ret = find_vibrator(vibrator);
		if (ret) {
			kfree(vibrator);
			vibrator = NULL;
			return ret;
		}

		vibrator->enabled = 0;
		vibrator->expires = 0;

		setup_timer(&vibrator->timer,
			    vibrator_timer,
			    (unsigned long)vibrator);

		vibrator->timed_output.name = "vibrator";
		vibrator->timed_output.enable =	timed_output_enable;
		vibrator->timed_output.get_time = timed_output_get_time;

		ret = timed_output_dev_register(
			&vibrator->timed_output);

		if (ret) {
			kfree(vibrator);
			vibrator = NULL;
			return ret;
		}
	}

	platform_set_drvdata(pdev, vibrator);

	ret = device_create_file(&pdev->dev,
				 &dev_attr_enable);

	if (ret) {
		platform_set_drvdata(pdev, NULL);
		return ret;
	}

	printk(KERN_INFO
	       "%s: registered\n",
	       dev_name(&pdev->dev));

	return 0;
}


static int vibrator_remove(struct platform_device *pdev)
{
	struct android_vibrator *vib;

	vib = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev,
			   &dev_attr_enable);

	platform_set_drvdata(pdev, NULL);

	return 0;
}


/*
 * Support both platform device names.
 */
static const struct platform_device_id vibrator_ids[] = {
	{
		.name = "android-vibrator",
	},
	{
		.name = "sardine-vibrator",
	},
	{ }
};

MODULE_DEVICE_TABLE(platform, vibrator_ids);


static struct platform_driver vibrator_driver = {
	.probe    = vibrator_probe,
	.remove   = vibrator_remove,
	.id_table = vibrator_ids,

	.driver = {
		.name  = "android-vibrator",
		.owner = THIS_MODULE,
	},
};


static struct platform_device android_vibrator_device = {
	.name = "android-vibrator",
	.id   = -1,
};

static struct platform_device sardine_vibrator_device = {
	.name = "sardine-vibrator",
	.id   = -1,
};


static int __init vibrator_init(void)
{
	int ret;

	ret = platform_driver_register(&vibrator_driver);
	if (ret)
		return ret;

	ret = platform_device_register(
		&android_vibrator_device);

	if (ret)
		goto err_driver;

	ret = platform_device_register(
		&sardine_vibrator_device);

	if (ret)
		goto err_android;

	return 0;


err_android:
	platform_device_unregister(
		&android_vibrator_device);

err_driver:
	platform_driver_unregister(&vibrator_driver);

	return ret;
}


static void __exit vibrator_exit(void)
{
	platform_device_unregister(&sardine_vibrator_device);

	platform_device_unregister(&android_vibrator_device);

	if (vibrator) {
		del_timer_sync(&vibrator->timer);

		led_brightness_set(vibrator->led,0);

		timed_output_dev_unregister(&vibrator->timed_output);

		kfree(vibrator);
		vibrator = NULL;
	}

	platform_driver_unregister(&vibrator_driver);
}


module_init(vibrator_init);
module_exit(vibrator_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Android vibrator compat shim");
