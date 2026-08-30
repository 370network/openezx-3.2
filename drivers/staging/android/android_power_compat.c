/*

  android_power_compat.c

  shim driver that mirrors a the /sys/class/power_supply/battery device
  to a /sys/android_power/ deviece for use with android (m1 - m5)

  Copyright Richard Gr   ^mik @ 370network (mailto:morc@370.network)

*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/power_supply.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fsnotify.h>
#include <linux/namei.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/fs.h>
#include <linux/uaccess.h>

// using 100 scale since it makes the most sense
// M1-M5 has them visually in steps of 10
#define ANDROID_BATTERY_SCALE          100
#define ANDROID_BATTERY_LOW_LEVEL      10
#define ANDROID_BATTERY_SHUTDOWN_LEVEL  5
#define POLL_INTERVAL_MS               5000

static struct power_supply *battery;
static struct kobject *android_power_kobj;
extern struct kobject *power_kobj;

static int last_battery_level = -1;
static int last_charging_status = -1;
static struct delayed_work power_poll_work;
static char last_requested_state[32] = "370network\n";

static void android_power_notify_file(const char *filename)
{
	struct path path;
	char full_path[64];
	int err;

	snprintf(full_path, sizeof(full_path), "/sys/android_power/%s", filename);

	err = kern_path(full_path, 0, &path);
	if (err)
		return;

	// sysfs_update_file got removed in linux god knows exactly wen
	// this is a hackish workaround that lets FileObserver.MODIFY in
	// android/server/BatteryService$BatteryObserver know that the
	// /sys/android_power nodes have been updated.
	//	Thanks, I hate it.

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,1,0)
	fsnotify(path.dentry->d_inode, FS_MODIFY, path.dentry->d_inode, 
	         FSNOTIFY_EVENT_INODE, NULL, 0);
	fsnotify_parent(&path, path.dentry, FS_MODIFY);
#else
	fsnotify_modify(path.dentry);
#endif

	path_put(&path);

	//notify for good measure, although it doesn't really change much in this case
	sysfs_notify(android_power_kobj, NULL, filename);
}

static int android_power_get_level(void)
{
	union power_supply_propval voltage_now;
	union power_supply_propval voltage_min;
	union power_supply_propval voltage_max;
	int level;
	int ret;

	if (!battery)
		return -ENODEV;

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_NOW, &voltage_now);
	if (ret) return ret;

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN, &voltage_min);
	if (ret) return ret;

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN, &voltage_max);
	if (ret) return ret;

	if (voltage_max.intval <= voltage_min.intval)
		return -EINVAL;

	level = (voltage_now.intval - voltage_min.intval) * 100;
	level /= voltage_max.intval - voltage_min.intval;

	// clamping
	if (level < 0) level = 0;
	if (level > 100) level = 100;

	return (int)level;
}

static int android_power_get_low_state(int level)
{
	if (level < ANDROID_BATTERY_SHUTDOWN_LEVEL)
		return 2;
	if (level < ANDROID_BATTERY_LOW_LEVEL)
		return 1;
	return 0;
}

static void power_poll_worker(struct work_struct *work)
{
	int current_level;
	union power_supply_propval status;
	int ret;

	if (!battery)
		return;

	current_level = android_power_get_level();
	if (current_level >= 0 && current_level != last_battery_level) {
		last_battery_level = current_level;
		android_power_notify_file("battery_level");
		android_power_notify_file("battery_level_raw");
		android_power_notify_file("battery_level_low");
	}

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_STATUS, &status);
	if (ret == 0 && status.intval != last_charging_status) {
		last_charging_status = status.intval;
		android_power_notify_file("charging_state");
	}

	queue_delayed_work(system_freezable_wq, &power_poll_work, msecs_to_jiffies(POLL_INTERVAL_MS));
}

static ssize_t battery_level_raw_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	int level = android_power_get_level();
	if (level < 0) return level;
	return sprintf(buf, "%d\n", level);
}
static struct kobj_attribute battery_level_raw_attr = __ATTR(battery_level_raw, 0444, battery_level_raw_show, NULL);

static ssize_t battery_level_scale_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "%d\n", ANDROID_BATTERY_SCALE);
}
static struct kobj_attribute battery_level_scale_attr = __ATTR(battery_level_scale, 0444, battery_level_scale_show, NULL);

static ssize_t battery_level_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	int level = android_power_get_level();
	if (level < 0) return level;
	return sprintf(buf, "%d\n", level);
}
static struct kobj_attribute battery_level_attr = __ATTR(battery_level, 0444, battery_level_show, NULL);

static ssize_t battery_level_low_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	int level = android_power_get_level();
	if (level < 0) return level;
	return sprintf(buf, "%d\n", android_power_get_low_state(level));
}
static struct kobj_attribute battery_level_low_attr = __ATTR(battery_level_low, 0444, battery_level_low_show, NULL);

static ssize_t battery_shutdown_level_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "%d\n", ANDROID_BATTERY_SHUTDOWN_LEVEL);
}
static struct kobj_attribute battery_shutdown_level_attr = __ATTR(battery_shutdown_level, 0444, battery_shutdown_level_show, NULL);

static ssize_t charging_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	union power_supply_propval status;
	int ret;
	const char *state;

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_STATUS, &status);
	if (ret) return ret;

	/* original android_power driver states
	const char *state_str[] = {
		[ANDROID_CHARGING_STATE_UNKNOWN] = "Unknown",
		[ANDROID_CHARGING_STATE_DISCHARGE] = "Discharging",
		[ANDROID_CHARGING_STATE_MAINTAIN] = "Maintaining",
		[ANDROID_CHARGING_STATE_SLOW] = "Slow",
		[ANDROID_CHARGING_STATE_NORMAL] = "Normal",
		[ANDROID_CHARGING_STATE_FAST] = "Fast",
		[ANDROID_CHARGING_STATE_OVERHEAT] = "Overheat"
	};*/


	switch (status.intval) {
		case POWER_SUPPLY_STATUS_DISCHARGING:
			state = "Discharging"; break;
		case POWER_SUPPLY_STATUS_CHARGING:
			state = "Normal"; break;
		case POWER_SUPPLY_STATUS_FULL:
		case POWER_SUPPLY_STATUS_NOT_CHARGING:
			state = "Maintaining"; break;
		default:
			state = "Unknown"; break;
	}
	return sprintf(buf, "%s\n", state);
}
static struct kobj_attribute charging_state_attr = __ATTR(charging_state, 0444, charging_state_show, NULL);


static void mirror_sys(const char *path, const char *buf, size_t count)
{
	struct file *filp;
	loff_t pos = 0;

	filp = filp_open(path, O_WRONLY, 0);
	if (IS_ERR(filp))
		return;

	mm_segment_t old_fs = get_fs();
	set_fs(KERNEL_DS);
	vfs_write(filp, (char __user *)buf, count, &pos);
	set_fs(old_fs);

	filp_close(filp, NULL);
}

static ssize_t acquire_partial_wake_lock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "\n");
}
static ssize_t acquire_partial_wake_lock_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
	printk("partial screen wakelock attempt: %s\n", buf);
	//mirror_sys("/sys/power/wake_lock", buf, count);
	return count;
}
static struct kobj_attribute acquire_partial_wake_lock_attr = __ATTR(acquire_partial_wake_lock, 0666, acquire_partial_wake_lock_show, acquire_partial_wake_lock_store);

static ssize_t acquire_full_wake_lock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "\n");
}
static ssize_t acquire_full_wake_lock_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
	printk("full wakelock attempt: %s\n", buf);
	mirror_sys("/sys/power/wake_lock", buf, count);
	return count;
}
static struct kobj_attribute acquire_full_wake_lock_attr = __ATTR(acquire_full_wake_lock, 0666, acquire_full_wake_lock_show, acquire_full_wake_lock_store);

static ssize_t release_wake_lock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "\n");
}
static ssize_t release_wake_lock_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
	printk("release wakelock attempt: %s\n", buf);
	mirror_sys("/sys/power/wake_unlock", buf, count);
	return count;
}
static struct kobj_attribute release_wake_lock_attr = __ATTR(release_wake_lock, 0666, release_wake_lock_show, release_wake_lock_store);

static ssize_t request_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "%s", last_requested_state);
}
static ssize_t request_state_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count) {
	snprintf(last_requested_state, sizeof(last_requested_state), "%s", buf);
	printk("attempted to request_state: %s\n", buf);

	if (strstr(buf, "standby")) {
//		mirror_sys("/sys/power/state", "standby\n", 8);
	} else if (strstr(buf, "wake")) {
//		mirror_sys("/sys/power/state", "wake\n", 4);
	}

	return count;
}
static struct kobj_attribute request_state_attr = __ATTR(request_state, 0666, request_state_show, request_state_store);

static struct attribute *android_power_attrs[] = {
	&battery_level_attr.attr,
	&battery_level_low_attr.attr,
	&battery_level_raw_attr.attr,
	&battery_level_scale_attr.attr,
	&battery_shutdown_level_attr.attr,
	&charging_state_attr.attr,
	&acquire_partial_wake_lock_attr.attr,
	&acquire_full_wake_lock_attr.attr,
	&release_wake_lock_attr.attr,
	&request_state_attr.attr,
	NULL,
};

static struct attribute_group android_power_attr_group = {
	.attrs = android_power_attrs,
};

static int __init android_power_init(void)
{
	int ret;

	battery = power_supply_get_by_name("battery");
	if (!battery) {
		pr_err("android_power: power supply 'battery' not found\n");
		return -ENODEV;
	}

	android_power_kobj = kobject_create_and_add("android_power", NULL);
	if (!android_power_kobj) {
		return -ENOMEM;
	}

	ret = sysfs_create_group(android_power_kobj, &android_power_attr_group);
	if (ret) {
		pr_err("android_power_compat: failed to create android_power: %d\n", ret);
		goto err_kobject;
	}

	INIT_DELAYED_WORK(&power_poll_work, power_poll_worker);
	queue_delayed_work(system_freezable_wq, &power_poll_work, 0);

	pr_info("android_power_compat: android_power shim reg\n");
	return 0;

err_kobject:
	kobject_put(android_power_kobj);
	android_power_kobj = NULL;

	return ret;
}

static void __exit android_power_exit(void)
{
	cancel_delayed_work_sync(&power_poll_work);

	if (android_power_kobj) {
		sysfs_remove_group(android_power_kobj, &android_power_attr_group);
		kobject_put(android_power_kobj);
		android_power_kobj = NULL;
	}

	pr_info("android_power_compat: removed\n");
}

module_init(android_power_init);
module_exit(android_power_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard Gracik - Morc | 370network");
MODULE_DESCRIPTION("Legacy Android power compat shim");
