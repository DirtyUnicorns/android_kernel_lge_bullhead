/*
 * Screen wake timeout
 * Copyright (C) 2014 flar2 <asegaert@gmail.com>
 * 
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/fb.h> 
#include <linux/qpnp/power-on.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/init.h>
#include <linux/wake_gestures.h>
#include <linux/alarmtimer.h>

#ifdef CONFIG_WAKE_GESTURES
#define ANDROID_TOUCH_DECLARED
#endif

#define WAKE_TIMEOUT_MAJOR_VERSION	1
#define WAKE_TIMEOUT_MINOR_VERSION	1
#define WAKEFUNC "wakefunc"
#define PWRKEY_DUR		60

static struct input_dev * wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static struct notifier_block wfnotif; 
static long long wake_timeout = 0;
static struct alarm wakefunc_rtc;
static bool wakefunc_triggered = false;

static void wake_presspwr(struct work_struct * wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
                return;
	input_event(wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(wake_pwrdev, EV_SYN, 0, 0);
	msleep(PWRKEY_DUR);
	input_event(wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(wake_pwrdev, EV_SYN, 0, 0);

	msleep(PWRKEY_DUR * 6);
	wakefunc_triggered = true;

	input_event(wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(wake_pwrdev, EV_SYN, 0, 0);
	msleep(PWRKEY_DUR);
	input_event(wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(wake_pwrdev, EV_SYN, 0, 0);

	pwrkey_pressed = true;

	msleep(PWRKEY_DUR);
        mutex_unlock(&pwrkeyworklock);
	
	return;
}
static DECLARE_WORK(wake_presspwr_work, wake_presspwr);

void timeout_pwrtrigger(void) {
	schedule_work(&wake_presspwr_work);
	return;
}

static void wakefunc_rtc_start(void)
{
	ktime_t wakeup_time;
	ktime_t curr_time = { .tv64 = 0 };

	if (!dt2w_switch && !s2w_switch)
		return;

	wakefunc_triggered = false;
	wakeup_time = ktime_add_us(curr_time,
			(wake_timeout * 1000LL * 60000LL));
	alarm_start_relative(&wakefunc_rtc, wakeup_time);

	pr_debug("%s: Current Time: %ld, Alarm set to: %ld\n",
			WAKEFUNC,
			ktime_to_timeval(curr_time).tv_sec,
			ktime_to_timeval(wakeup_time).tv_sec);
		
	pr_debug("%s: Timeout started for %llu minutes\n", WAKEFUNC,
			wake_timeout);
}

static void wakefunc_rtc_cancel(void)
{
	int ret;

	wakefunc_triggered = false;
	ret = alarm_cancel(&wakefunc_rtc);
	if (ret)
		pr_debug("%s: Timeout canceled\n", WAKEFUNC);
	else
		pr_debug("%s: Nothing to cancel\n",
				WAKEFUNC);
}


static enum alarmtimer_restart wakefunc_rtc_callback(struct alarm *al, ktime_t now)
{
	struct timeval ts;
	ts = ktime_to_timeval(now);

	timeout_pwrtrigger();
	
	pr_debug("%s: Time of alarm expiry: %ld\n", WAKEFUNC,
			ts.tv_sec);

	return ALARMTIMER_NORESTART;
}


//sysfs
static ssize_t show_wake_timeout(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%lld\n", wake_timeout);
}

static ssize_t store_wake_timeout(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long long input;
	int ret;

	ret = sscanf(buf, "%llu", &input);

	if (ret != 1) {
		return -EINVAL;
	}

	wake_timeout = input;

	return count;
}

static DEVICE_ATTR(wake_timeout, (S_IWUSR|S_IRUGO),
	show_wake_timeout, store_wake_timeout);


#ifdef ANDROID_TOUCH_DECLARED
extern struct kobject *android_touch_kobj;
#else
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);
#endif

static int fb_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;

	if (evdata && evdata->data && event == FB_EVENT_BLANK) {
		blank = evdata->data;
		switch (*blank) {
			case FB_BLANK_UNBLANK:
				//display on
				wakefunc_rtc_cancel();
				break;
			case FB_BLANK_POWERDOWN:
			case FB_BLANK_HSYNC_SUSPEND:
			case FB_BLANK_VSYNC_SUSPEND:
			case FB_BLANK_NORMAL:
				//display off
				if (pwrkey_pressed == false && wakefunc_triggered == false && wake_timeout > 0) {
					wakefunc_rtc_start();
				}
				break;
		}
	}

	return NOTIFY_OK;
}

static int __init wake_timeout_init(void)
{
	int rc;

	pr_info("wake_timeout version %d.%d\n",
		 WAKE_TIMEOUT_MAJOR_VERSION,
		 WAKE_TIMEOUT_MINOR_VERSION);

	alarm_init(&wakefunc_rtc, ALARM_REALTIME,
			wakefunc_rtc_callback);

	wake_pwrdev = input_allocate_device();
	if (!wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(wake_pwrdev, EV_KEY, KEY_POWER);
	wake_pwrdev->name = "wakefunc_pwrkey";
	wake_pwrdev->phys = "wakefunc_pwrkey/input0";

	rc = input_register_device(wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

#ifndef ANDROID_TOUCH_DECLARED
	android_touch_kobj = kobject_create_and_add("android_touch", NULL) ;
	if (android_touch_kobj == NULL) {
		pr_warn("%s: android_touch_kobj create_and_add failed\n", __func__);
	}
#endif
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_wake_timeout.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for wake_timeout\n", __func__);
	}

	wfnotif.notifier_call = fb_notifier_callback;
	if (fb_register_client(&wfnotif)) {
		rc = -EINVAL;
		goto err_alloc_dev;
	}	

err_input_dev:
	input_free_device(wake_pwrdev);
err_alloc_dev:
	pr_info(WAKEFUNC"%s: done\n", __func__);

	return 0;
}


static void __exit wake_timeout_exit(void)
{

	alarm_cancel(&wakefunc_rtc);
#ifndef ANDROID_TOUCH_DECLARED
	kobject_del(android_touch_kobj);
#endif
	fb_unregister_client(&wfnotif);
	input_unregister_device(wake_pwrdev);
	input_free_device(wake_pwrdev);

	return;
}

MODULE_AUTHOR("flar2 <asegaert@gmail.com>");
MODULE_DESCRIPTION("'wake_timeout' - Disable screen wake functions after timeout");
MODULE_LICENSE("GPL v2");

module_init(wake_timeout_init);
module_exit(wake_timeout_exit);
