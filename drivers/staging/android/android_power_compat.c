/*

  android_power_compat.c

  shim driver that mirrors the /sys/class/power_supply/battery device
  to /sys/android_power/ nodes for use with Android M1 - M5
  it also includes a mostly custom reimplementation of the full/partial
  wakelock system for use with Android M1 - 0.9 systems but with using
  the regular linux wakelock APIs 

  Copyright (C) 2026 Richard Gráčik @ 370network (mailto:morc@370.network)
  
  Based on drivers/android/power.c available from the M3/M5 and
  Android 1.0 kernel source drops
**
** Copyright (C) 2005-2008 Google, Inc.
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
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/earlysuspend.h>
#include <linux/suspend.h>
#include <linux/wakelock.h>
#include <linux/mutex.h>

static struct power_supply *battery;
static struct kobject *android_power_kobj;
static struct delayed_work power_poll_work;

static DEFINE_MUTEX(g_early_suspend_lock);

static int g_active_full_wake_locks;
static int g_active_partial_wake_locks;
static enum {
	USER_AWAKE,
	USER_NOTIFICATION,
	USER_SLEEP
} g_user_suspend_state;

static ktime_t g_user_suspend_state_changed;
static ktime_t g_auto_off_timeout;
static ktime_t g_last_user_activity;
static ktime_t g_last_notification;
static ktime_t g_notification_timeout;

static int g_max_user_lockouts = 16;

struct g_user_wake_locks {
	enum {
		USER_WAKE_LOCK_INACTIVE,
		USER_WAKE_LOCK_PARTIAL,
		USER_WAKE_LOCK_FULL
	} state;

	struct wake_lock wl;

	char name_buffer[32];
};
static struct g_user_wake_locks *g_user_wake_locks;

// M1-M5 has them visually in steps of 10
// using 100 scale since it makes the most sense
static int g_battery_level = -1;
static int g_battery_level_scale = 100;
static int g_battery_low_level = 10;
static int g_battery_shutdown_level = 5;
static int g_charging_state = -1;
#define POLL_INTERVAL_MS                5000 //battery state refresh interval

//extern import from earlysuspend
extern void request_suspend_state(suspend_state_t state);


static void android_power_notify_file(const char *filename) {
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

static int android_power_get_level(void) {
	union power_supply_propval voltage_now;
	union power_supply_propval voltage_min;
	union power_supply_propval voltage_max;
	int level, ret;

	if (!battery) return -ENODEV;

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_NOW, &voltage_now);
	if (ret) return ret;

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN, &voltage_min);
	if (ret) return ret;

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN, &voltage_max);
	if (ret) return ret;

	if (voltage_max.intval <= voltage_min.intval) return -EINVAL;

	level = (voltage_now.intval - voltage_min.intval) * 100;
	level /= voltage_max.intval - voltage_min.intval;

	// clamping
	if (level < 0) level = 0;
	if (level > 100) level = 100;

	return (int)level;
}

static int android_power_get_low_state(int level) {
	if (level < g_battery_shutdown_level) return 2;
	if (level < g_battery_low_level) return 1;
	return 0;
}

static void power_poll_worker(struct work_struct *work) {
	int current_level;
	union power_supply_propval status;
	int ret;

	if (!battery) return;

	current_level = android_power_get_level();
	if (current_level >= 0 && current_level != g_battery_level) {
		g_battery_level = current_level;
		android_power_notify_file("battery_level");
		android_power_notify_file("battery_level_raw");
		android_power_notify_file("battery_level_low");
	}

	ret = battery->get_property(battery, POWER_SUPPLY_PROP_STATUS, &status);
	if (ret == 0 && status.intval != g_charging_state) {
		g_charging_state = status.intval;
		android_power_notify_file("charging_state");
	}

	queue_delayed_work(system_freezable_wq, &power_poll_work, msecs_to_jiffies(POLL_INTERVAL_MS));
}

static int parse_wake_lock_name(const char *buf, size_t n, char *name, size_t name_size) {
	size_t len;

	if (!buf || !name)
		return -EINVAL;

	if (!n)
		return -EINVAL;

	len = n;

	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == '\0'))
		len--;

	if (!len)
		return -EINVAL;

	if (len >= name_size)
		return -EOVERFLOW;

	memcpy(name, buf, len);
	name[len] = '\0';

	return 0;
}

static int lookup_wake_lock_name_locked(const char *buf, size_t n, int allocate, char *parsed_name) {
	int i;
	int free_index = -1;
	int inactive_index = -1;
	int ret;

	ret = parse_wake_lock_name( buf, n, parsed_name, sizeof(g_user_wake_locks[0].name_buffer));

	if (ret)
		return ret;

	for (i = 0; i < g_max_user_lockouts; i++) {
		if (g_user_wake_locks[i].name_buffer[0] != '\0') {
			if (!strcmp( g_user_wake_locks[i].name_buffer, parsed_name))
				return i;
			if (g_user_wake_locks[i].state == USER_WAKE_LOCK_INACTIVE && inactive_index < 0)
				inactive_index = i;
		} else if (free_index < 0) {
			free_index = i;
		}
	}

	if (!allocate)
		return -ENOENT;

	if (free_index < 0)
		free_index = inactive_index;

	if (free_index < 0)
		return -ENOSPC;

	strcpy(g_user_wake_locks[free_index].name_buffer, parsed_name);
	g_user_wake_locks[free_index].state = USER_WAKE_LOCK_INACTIVE;
	printk("android_power_compat: slot %d for '%s'\n", free_index, parsed_name);

	return free_index;
}

//basically the same for request_state
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	ssize_t ret;
	mutex_lock(&g_early_suspend_lock);

	//hopefully properly returning the full and partial wakelock counts
	ret = sprintf(buf, "%d-%d-%d\n", g_user_suspend_state, g_active_full_wake_locks == 0, g_active_partial_wake_locks == 0);
	mutex_unlock(&g_early_suspend_lock);
	return ret;
}

//cloned to request_state during M5 kernel changes
static ssize_t state_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	if (!strncmp(buf, "standby", 7)) {
		mutex_lock(&g_early_suspend_lock);
		g_user_suspend_state = USER_SLEEP;
		printk("android_power_compat STANDBY\n");
		mutex_unlock(&g_early_suspend_lock);
		request_suspend_state(PM_SUSPEND_STANDBY);
		return n;
	}
	if (!strncmp(buf, "wake", 4)) {
		mutex_lock(&g_early_suspend_lock);
		g_user_suspend_state = USER_AWAKE;
		printk("android_power_compat WAKE\n");
		mutex_unlock(&g_early_suspend_lock);
		request_suspend_state(PM_SUSPEND_ON);
		return n;
	}
	printk("android_power_compat state_store: invalid argument\n");
	return -EINVAL;
}
static struct kobj_attribute state_attr = __ATTR(state, 0666, state_show, state_store);

static ssize_t show_ktime_t(char *buf, ktime_t *ptr){
	ssize_t ret;

	mutex_lock(&g_early_suspend_lock);
	ret = sprintf(buf, "%lld\n", ktime_to_ns(*ptr));
	mutex_unlock(&g_early_suspend_lock);
	return ret;
}

static ssize_t auto_off_timeout_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	ssize_t ret;

	mutex_lock(&g_early_suspend_lock);
	ret = sprintf(buf, "%ld\n", (long)ktime_to_ms(g_auto_off_timeout) / 1000);
	mutex_unlock(&g_early_suspend_lock);
	return ret;
}

static ssize_t auto_off_timeout_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	long seconds;

	if (sscanf(buf, "%ld", &seconds) != 1)
		return -EINVAL;

	if (seconds < 0)
		return -EINVAL;

	mutex_lock(&g_early_suspend_lock);
	g_auto_off_timeout = ktime_set(seconds, 0);
	mutex_unlock(&g_early_suspend_lock);
	return n;
}
static struct kobj_attribute auto_off_timeout_attr = __ATTR(auto_off_timeout, 0666, auto_off_timeout_show, auto_off_timeout_store);

static ssize_t notification_timeout_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	ssize_t ret;

	mutex_lock(&g_early_suspend_lock);
	ret = sprintf(buf, "%ld\n", (long)ktime_to_ms(g_notification_timeout) / 1000);
	mutex_unlock(&g_early_suspend_lock);
	return ret;
}

static ssize_t notification_timeout_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	long seconds;

	if (sscanf(buf, "%ld", &seconds) != 1)
		return -EINVAL;

	if (seconds < 0)
		return -EINVAL;

	mutex_lock(&g_early_suspend_lock);
	g_notification_timeout = ktime_set(seconds, 0);
	mutex_unlock(&g_early_suspend_lock);
	return n;
}
static struct kobj_attribute notification_timeout_attr = __ATTR(notification_timeout, 0666, notification_timeout_show, notification_timeout_store);

static ssize_t last_user_activity_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	return show_ktime_t(buf, &g_last_user_activity);
}

static ssize_t last_user_activity_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	u64 nanoseconds;

	if (sscanf(buf, "%llu", &nanoseconds) != 1)
		return -EINVAL;

	mutex_lock(&g_early_suspend_lock);

	if (nanoseconds == 0)
		g_last_user_activity = ktime_get();
	else
		g_last_user_activity = ns_to_ktime(nanoseconds);

	mutex_unlock(&g_early_suspend_lock);
	return n;
}
static struct kobj_attribute last_user_activity_attr = __ATTR(last_user_activity, 0666, last_user_activity_show, last_user_activity_store);

static ssize_t last_notification_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	return show_ktime_t(buf, &g_last_notification);
}

static ssize_t last_notification_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	u64 nanoseconds;

	if (sscanf(buf, "%llu", &nanoseconds) != 1)
		return -EINVAL;

	mutex_lock(&g_early_suspend_lock);

	if (nanoseconds == 0)
		g_last_notification = ktime_get();
	else
		g_last_notification = ns_to_ktime(nanoseconds);

	mutex_unlock(&g_early_suspend_lock);
	return n;
}
static struct kobj_attribute last_notification_attr = __ATTR(last_notification, 0666, last_notification_show, last_notification_store);

static ssize_t request_sleep_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return show_ktime_t(buf, &g_user_suspend_state_changed);
}

static ssize_t request_sleep_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	u64 nanoseconds;

	if (sscanf(buf, "%llu", &nanoseconds) != 1)
		return -EINVAL;

	mutex_lock(&g_early_suspend_lock);
	g_user_suspend_state_changed =ns_to_ktime(nanoseconds);
	mutex_unlock(&g_early_suspend_lock);
	printk("android_power_compat: request_sleep %llu\n",nanoseconds);

	return n;
}
static struct kobj_attribute request_sleep_attr = __ATTR(request_sleep, 0666, request_sleep_show, request_sleep_store);

static ssize_t battery_level_raw_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	int level = android_power_get_level();
	if (level < 0) return level;
	return sprintf(buf, "%d\n", level);
}
static struct kobj_attribute battery_level_raw_attr = __ATTR(battery_level_raw, 0444, battery_level_raw_show, NULL);

static ssize_t battery_level_scale_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "%d\n", g_battery_level_scale);
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

static ssize_t battery_low_level_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	ssize_t ret;
	mutex_lock(&g_early_suspend_lock);
	ret = sprintf(buf, "%d\n", g_battery_low_level);
	mutex_unlock(&g_early_suspend_lock);
	return ret;
}

static ssize_t battery_low_level_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	int new_val;

	if (sscanf(buf, "%d", &new_val) != 1)
		return -EINVAL;
	if (new_val < 0 || new_val > 100)
		return -EINVAL;

	mutex_lock(&g_early_suspend_lock);
	g_battery_low_level = new_val;
	mutex_unlock(&g_early_suspend_lock);

	android_power_notify_file("battery_level_low");
	return n;
}
static struct kobj_attribute battery_low_level_attr = __ATTR(battery_low_level, 0666, battery_low_level_show, battery_low_level_store);

static ssize_t battery_shutdown_level_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	return sprintf(buf, "%d\n", g_battery_shutdown_level);
}
static struct kobj_attribute battery_shutdown_level_attr = __ATTR(battery_shutdown_level, 0666, battery_shutdown_level_show, NULL);

static ssize_t charging_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
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

static ssize_t acquire_partial_wake_lock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	int i;
	char *s = buf;

	mutex_lock(&g_early_suspend_lock);

	for (i = 0; i < g_max_user_lockouts; i++) {
		if (g_user_wake_locks[i].name_buffer[0] != '\0' && g_user_wake_locks[i].state == USER_WAKE_LOCK_PARTIAL)
			s += sprintf( s, "%s ", g_user_wake_locks[i].name_buffer);
	}

	s += sprintf(s, "\n");
	mutex_unlock(&g_early_suspend_lock);
	return s - buf;
}

static ssize_t acquire_partial_wake_lock_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	int i;
	char name[32];

	mutex_lock(&g_early_suspend_lock);
	i = lookup_wake_lock_name_locked(buf, n, 1, name);

	if (i < 0) {
		mutex_unlock(&g_early_suspend_lock);
		return i;
	}

	if (g_user_wake_locks[i].state == USER_WAKE_LOCK_PARTIAL) {
		printk("android_power_compat: partial %s already active\n", name);
		mutex_unlock(&g_early_suspend_lock);
		return n;
	}

	if (g_user_wake_locks[i].state == USER_WAKE_LOCK_FULL) {
		printk("android_power_compat: not taking partial wakelock %s\n", name);
		mutex_unlock(&g_early_suspend_lock);
		return -EBUSY;
	}

	g_user_wake_locks[i].state = USER_WAKE_LOCK_PARTIAL;
	g_active_partial_wake_locks++;

	//printk("acquire_partial_wake_lock_store: %s, size %d\n", g_user_wake_locks[i].name_buffer, n);
	wake_lock(&g_user_wake_locks[i].wl);
	mutex_unlock(&g_early_suspend_lock);
	return n;
}
static struct kobj_attribute acquire_partial_wake_lock_attr = __ATTR(acquire_partial_wake_lock, 0666, acquire_partial_wake_lock_show, acquire_partial_wake_lock_store);

static ssize_t acquire_full_wake_lock_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	int i;
	char *s = buf;

	mutex_lock(&g_early_suspend_lock);
	for (i = 0; i < g_max_user_lockouts; i++) {
		if (g_user_wake_locks[i].name_buffer[0] != '\0' && g_user_wake_locks[i].state == USER_WAKE_LOCK_FULL)
			s += sprintf( s, "%s ", g_user_wake_locks[i].name_buffer);
	}
	s += sprintf(s, "\n");

	mutex_unlock(&g_early_suspend_lock);
	return s - buf;
}

static ssize_t acquire_full_wake_lock_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	int i;
	char name[32];

	mutex_lock(&g_early_suspend_lock);

	i = lookup_wake_lock_name_locked( buf, n, 1, name);

	if (i < 0) {
		mutex_unlock(&g_early_suspend_lock);
		return i;
	}

	if (g_user_wake_locks[i].state == USER_WAKE_LOCK_FULL) {
		printk("android_power_compat: full %s already active\n", name);
		mutex_unlock(&g_early_suspend_lock);
		return n;
	}

	if (g_user_wake_locks[i].state == USER_WAKE_LOCK_PARTIAL) {
		printk("android_power_compat: not taking full wakelock %s\n", name);
		mutex_unlock(&g_early_suspend_lock);
		return -EBUSY;
	}

	g_user_wake_locks[i].state = USER_WAKE_LOCK_FULL;
	g_active_full_wake_locks++;

	//printk("acquire_full_wake_lock_store: %s, size %d\n", g_user_wake_locks[i].name_buffer, n);
	wake_lock(&g_user_wake_locks[i].wl);
	mutex_unlock(&g_early_suspend_lock);
	return n;
}
static struct kobj_attribute acquire_full_wake_lock_attr = __ATTR(acquire_full_wake_lock, 0666, acquire_full_wake_lock_show, acquire_full_wake_lock_store);

static ssize_t release_wake_lock_show( struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	int i;
	char *s = buf;

	mutex_lock(&g_early_suspend_lock);
	for (i = 0; i < g_max_user_lockouts; i++) {
		if (g_user_wake_locks[i].name_buffer[0] != '\0' && g_user_wake_locks[i].state == USER_WAKE_LOCK_INACTIVE)
			s += sprintf( s, "%s ", g_user_wake_locks[i].name_buffer);
	}
	s += sprintf(s, "\n");

	mutex_unlock(&g_early_suspend_lock);
	return s - buf;
}

static ssize_t release_wake_lock_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n) {
	int i;
	char name[32];
	int was_full;
	int was_partial;
	int enter_standby = 0;

	mutex_lock(&g_early_suspend_lock);

	i = lookup_wake_lock_name_locked(buf, n, 0, name);

	if (i < 0) {
		mutex_unlock(&g_early_suspend_lock);
		return i;
	}

	if (g_user_wake_locks[i].state == USER_WAKE_LOCK_INACTIVE) {
		printk("android_power_compat: %s already inactive\n", name);
		mutex_unlock(&g_early_suspend_lock);
		return -EINVAL;
	}

	was_full = (g_user_wake_locks[i].state == USER_WAKE_LOCK_FULL);
	was_partial = (g_user_wake_locks[i].state == USER_WAKE_LOCK_PARTIAL);

	g_user_wake_locks[i].state = USER_WAKE_LOCK_INACTIVE;

	if (was_full) {
		if (g_active_full_wake_locks > 0)
			g_active_full_wake_locks--;

		if (g_active_full_wake_locks == 0 && g_user_suspend_state == USER_NOTIFICATION) {
			g_user_suspend_state = USER_SLEEP;
			enter_standby = 1;
		}

		printk("android_power_compat full wakelock %s release\n", name);
	} else if (was_partial) {
		if (g_active_partial_wake_locks > 0)
			g_active_partial_wake_locks--;
	}

	wake_unlock(&g_user_wake_locks[i].wl);
	//printk("release_wake_lock_store: %s, size %d\n", g_user_wake_locks[i].name_buffer, n);
	mutex_unlock(&g_early_suspend_lock);
	if (enter_standby) {
		printk("android_power_compat: entering standby\n");
		request_suspend_state(PM_SUSPEND_STANDBY);
	}
	return n;
}
static struct kobj_attribute release_wake_lock_attr =__ATTR(release_wake_lock, 0666, release_wake_lock_show, release_wake_lock_store);

static ssize_t request_state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
	ssize_t ret;

	mutex_lock(&g_early_suspend_lock);
	if(g_user_suspend_state == USER_AWAKE)
		ret = sprintf(buf, "wake\n");
	else if(g_user_suspend_state == USER_NOTIFICATION)
		ret = sprintf(buf, "standby (w/full wake lock)\n");
	else
		ret = sprintf(buf, "standby\n");
	mutex_unlock(&g_early_suspend_lock);
	return ret;
}

static ssize_t request_state_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t n){
	return state_store(kobj, attr, buf, n);
}
static struct kobj_attribute request_state_attr = __ATTR(request_state, 0666, request_state_show, request_state_store);

static struct attribute *android_power_attrs[] = {
	&state_attr.attr, //M1 - 1.0
	&request_state_attr.attr, //M1 - 1.0

	&acquire_full_wake_lock_attr.attr, //M1 - 1.0
	&acquire_partial_wake_lock_attr.attr, //M1 - 1.0
	&release_wake_lock_attr.attr, //M1 - 1.0

	&auto_off_timeout_attr.attr, //M1 - M3
	&notification_timeout_attr.attr, //M1 - M3

	&last_user_activity_attr.attr, //M1 - M3
	&last_notification_attr.attr, //M1 - M3
	&request_sleep_attr.attr, //M1 - M3

	&battery_level_attr.attr, //M1 - M5
	&battery_level_low_attr.attr, //M1 - M5
	&battery_level_raw_attr.attr, //M1 - M5
	&battery_level_scale_attr.attr, //M1 - M5
	&battery_low_level_attr.attr, //M1 - 1.0
	&battery_shutdown_level_attr.attr, //M1 - M5
	&charging_state_attr.attr, //M1 - M5
	NULL,
};

static struct attribute_group android_power_attr_group = {
	.attrs = android_power_attrs,
};

static int __init android_power_init(void){
	int ret;
	int i;

	battery = power_supply_get_by_name("battery");
	if (!battery) {
		pr_err("android_power_compat: power supply 'battery' not found\n");
		return -ENODEV;
	}

	g_user_wake_locks = kcalloc(g_max_user_lockouts, sizeof(*g_user_wake_locks), GFP_KERNEL);
	if (!g_user_wake_locks)
		return -ENOMEM;

	//init wakelock memory
	for (i = 0; i < g_max_user_lockouts; i++) {
		memset(&g_user_wake_locks[i],0,sizeof(g_user_wake_locks[i]));
		wake_lock_init(&g_user_wake_locks[i].wl,WAKE_LOCK_SUSPEND,g_user_wake_locks[i].name_buffer);
		g_user_wake_locks[i].state = USER_WAKE_LOCK_INACTIVE;
	}

	g_user_suspend_state = USER_AWAKE;

	g_active_full_wake_locks = 0;
	g_active_partial_wake_locks = 0;

	g_auto_off_timeout = ktime_set(0, 0);
	g_notification_timeout = ktime_set(0, 0);
	g_last_user_activity = ktime_get();
	g_last_notification = ktime_get();
	g_user_suspend_state_changed = ktime_get();

	android_power_kobj = kobject_create_and_add("android_power", NULL);
	if (!android_power_kobj) {
		ret = -ENOMEM;
		goto err_wakelocks;
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

err_wakelocks:
	for (i = 0; i < g_max_user_lockouts; i++)
		wake_lock_destroy(&g_user_wake_locks[i].wl);

	return ret;
}

static void __exit android_power_exit(void){
	int i;

	cancel_delayed_work_sync(&power_poll_work);

	//clear wakelocks
	for (i = 0; i < g_max_user_lockouts; i++) {
		if (wake_lock_active(&g_user_wake_locks[i].wl))
			wake_unlock(&g_user_wake_locks[i].wl);

		wake_lock_destroy(&g_user_wake_locks[i].wl);
	}

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
MODULE_AUTHOR("Richard Gráčik - Morc | 370network");
MODULE_DESCRIPTION("Legacy Android power compat shim");
