/* drivers/input/touchscreen/sec_ts.c
 *
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 * http://www.samsungsemi.com/
 *
 * Core file for Samsung TSC driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/irq.h>
#include <linux/of_gpio.h>
#include <linux/time.h>
#include <linux/completion.h>
#include <linux/wakelock.h>
#include "sec_ts.h"

#include <soc/qcom/scm.h>
enum subsystem {
	TZ = 1,
	APSS = 3
};

#define TZ_BLSP_MODIFY_OWNERSHIP_ID 3

static struct device *sec_ts_dev;
EXPORT_SYMBOL(sec_ts_dev);

#ifdef USE_RESET_DURING_POWER_ON
static void sec_ts_reset_work(struct work_struct *work);
#endif
static void sec_ts_read_nv_work(struct work_struct *work);

#ifdef USE_OPEN_CLOSE
static int sec_ts_input_open(struct input_dev *dev);
static void sec_ts_input_close(struct input_dev *dev);
#endif

static int sec_ts_stop_device(struct sec_ts_data *ts);
static int sec_ts_start_device(struct sec_ts_data *ts);

static int sec_ts_read_information(struct sec_ts_data *ts);
static int sec_ts_set_lowpowermode(struct sec_ts_data *ts, u8 mode);

void sec_ts_delay(unsigned int ms);

u8 lv1cmd;
u8 *read_lv1_buff;
static int lv1_readsize;
static int lv1_readremain;
static int lv1_readoffset;

static ssize_t sec_ts_reg_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size);
static ssize_t sec_ts_regreadsize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size);
static inline ssize_t sec_ts_store_error(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count);
static ssize_t sec_ts_enter_recovery_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size);
static ssize_t sec_ts_regread_show(struct device *dev,
		struct device_attribute *attr, char *buf);
static ssize_t sec_ts_gesture_status_show(struct device *dev,
		struct device_attribute *attr, char *buf);
static inline ssize_t sec_ts_show_error(struct device *dev,
		struct device_attribute *attr, char *buf);

static DEVICE_ATTR(sec_ts_reg, (S_IWUSR | S_IWGRP), NULL, sec_ts_reg_store);
static DEVICE_ATTR(sec_ts_regreadsize, (S_IWUSR | S_IWGRP), NULL, sec_ts_regreadsize_store);
static DEVICE_ATTR(sec_ts_enter_recovery, (S_IWUSR | S_IWGRP), NULL, sec_ts_enter_recovery_store);
static DEVICE_ATTR(sec_ts_regread, S_IRUGO, sec_ts_regread_show, NULL);
static DEVICE_ATTR(sec_ts_gesture_status, S_IRUGO, sec_ts_gesture_status_show, NULL);

static struct attribute *cmd_attributes[] = {
	&dev_attr_sec_ts_reg.attr,
	&dev_attr_sec_ts_regreadsize.attr,
	&dev_attr_sec_ts_enter_recovery.attr,
	&dev_attr_sec_ts_regread.attr,
	&dev_attr_sec_ts_gesture_status.attr,
	NULL,
};

static struct attribute_group cmd_attr_group = {
	.attrs = cmd_attributes,
};

static inline ssize_t sec_ts_show_error(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);

	tsp_debug_err(true, &ts->client->dev, "sec_ts :%s read only function, %s\n", __func__, attr->attr.name );
	return -EPERM;
}

static inline ssize_t sec_ts_store_error(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);

	tsp_debug_err(true, &ts->client->dev, "sec_ts :%s write only function, %s\n", __func__, attr->attr.name);
	return -EPERM;
}

#ifdef CONFIG_SECURE_TOUCH
static int sec_ts_change_pipe_owner(struct sec_ts_data *ts, enum subsystem subsystem)
{
	/* scm call disciptor */
	struct scm_desc desc;
	int ret = 0;

	/* number of arguments */
	desc.arginfo = SCM_ARGS(2);
	/* BLSPID (1 - 12) */
	desc.args[0] = ts->client->adapter->nr - 1;
	/* Owner if TZ or APSS */
	desc.args[1] = subsystem;

	ret = scm_call2(SCM_SIP_FNID(SCM_SVC_TZ, TZ_BLSP_MODIFY_OWNERSHIP_ID), &desc);
	tsp_debug_err(true, &ts->client->dev, "%s: return1: %d\n", __func__, ret);
	if (ret)
		return ret;

	tsp_debug_err(true, &ts->client->dev, "%s: return2: %llu\n", __func__, desc.ret[0]);

	return desc.ret[0];
}

static irqreturn_t sec_ts_irq_thread(int irq, void *ptr);

/**
 * Sysfs attr group for secure touch & interrupt handler for Secure world.
 * @atomic : syncronization for secure_enabled
 * @pm_runtime : set rpm_resume or rpm_ilde
 */

static void secure_touch_notify(struct sec_ts_data *ts)
{
	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);
	sysfs_notify(&ts->input_dev->dev.kobj, NULL, "secure_touch");
}

static irqreturn_t secure_filter_interrupt(struct sec_ts_data *ts)
{
	if (atomic_read(&ts->secure_enabled) == SECURE_TOUCH_ENABLE) {
		if (atomic_cmpxchg(&ts->secure_pending_irqs, 0, 1) == 0) {
			tsp_debug_info(true, &ts->client->dev, "%s: pending irq:%d\n",
						__func__, (int)atomic_read(&ts->secure_pending_irqs));
			secure_touch_notify(ts);
		} else {
			tsp_debug_info(true, &ts->client->dev, "%s: --\n", __func__);
		}

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int secure_touch_clk_prepare_enable(struct sec_ts_data *ts)
{
	int ret;

	if (!ts->core_clk || !ts->iface_clk) {
		tsp_debug_err(true, &ts->client->dev, "%s: error clk\n", __func__);
		return -ENODEV;
	}

	ret = clk_prepare_enable(ts->core_clk);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: failed core clk\n", __func__);
		goto err_core_clk;
	}

	ret = clk_prepare_enable(ts->iface_clk);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: failed iface clk\n", __func__);
		goto err_iface_clk;
	}

	return 0;

err_iface_clk:
	clk_disable_unprepare(ts->core_clk);
err_core_clk:
	return -ENODEV;
}

static void secure_touch_clk_unprepare_disable(struct sec_ts_data *ts)
{
	if (!ts->core_clk || !ts->iface_clk) {
		tsp_debug_err(true, &ts->client->dev, "%s: error clk\n", __func__);
		return;
	}

	clk_disable_unprepare(ts->core_clk);
	clk_disable_unprepare(ts->iface_clk);
}

static ssize_t secure_touch_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%d", atomic_read(&ts->secure_enabled));
}

static ssize_t secure_touch_enable_store(struct device *dev,
			struct device_attribute *addr, const char *buf, size_t count)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);
	int data, ret;

	ret = sscanf(buf, "%d", &data);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: failed to read:%d\n",
					__func__, ret);
		return -EINVAL;
	}

	if (data == 1) {
		/* Enable Secure World */
		if (atomic_read(&ts->secure_enabled) == SECURE_TOUCH_ENABLE) {
			tsp_debug_err(true, &ts->client->dev, "%s: already enabled\n", __func__);
			return -EBUSY;
		}

		synchronize_irq(ts->client->irq);

		/* Fix normal active mode : idle mode is failed to i2c for 1 time  */
		ret = sec_ts_fix_tmode(ts, TOUCH_SYSTEM_MODE_TOUCH, TOUCH_MODE_STATE_TOUCH);
		if (ret < 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: failed to fix tmode\n",
					__func__);
			return -EIO;
		}

		/* Release All Finger */
		sec_ts_unlocked_release_all_finger(ts);

		if (pm_runtime_get_sync(ts->client->adapter->dev.parent) < 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: failed to get pm_runtime\n", __func__);
			return -EIO;
		}

		if (secure_touch_clk_prepare_enable(ts) < 0) {
			pm_runtime_put_sync(ts->client->adapter->dev.parent);
			tsp_debug_err(true, &ts->client->dev, "%s: failed to clk enable\n", __func__);
			return -ENXIO;
		}

		sec_ts_change_pipe_owner(ts, TZ);

		reinit_completion(&ts->secure_powerdown);
		reinit_completion(&ts->secure_interrupt);

		atomic_set(&ts->secure_enabled, 1);
		atomic_set(&ts->secure_pending_irqs, 0);

		tsp_debug_info(true, &ts->client->dev, "%s: secure touch enable\n", __func__);
	} else if (data == 0) {
		/* Disable Secure World */
		if (atomic_read(&ts->secure_enabled) == SECURE_TOUCH_DISABLE) {
			tsp_debug_err(true, &ts->client->dev, "%s: already disabled\n", __func__);
			return count;
		}

		sec_ts_change_pipe_owner(ts, APSS);

		secure_touch_clk_unprepare_disable(ts);
		pm_runtime_put_sync(ts->client->adapter->dev.parent);
		atomic_set(&ts->secure_enabled, 0);
		secure_touch_notify(ts);

		sec_ts_delay(10);

		sec_ts_irq_thread(ts->client->irq, ts);
		complete(&ts->secure_interrupt);
		complete(&ts->secure_powerdown);

		tsp_debug_info(true, &ts->client->dev, "%s: secure touch disable\n", __func__);

		ret = sec_ts_release_tmode(ts);
		if (ret < 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: failed to release tmode\n",
					__func__);
			return -EIO;
		}

	} else {
		tsp_debug_err(true, &ts->client->dev, "%s: unsupport value:%d\n", __func__, data);
		return -EINVAL;
	}

	return count;
}

static ssize_t secure_touch_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);
	int val = 0;

	if (atomic_read(&ts->secure_enabled) == SECURE_TOUCH_DISABLE) {
		tsp_debug_err(true, &ts->client->dev, "%s: disabled\n", __func__);
		return -EBADF;
	}

	if (atomic_cmpxchg(&ts->secure_pending_irqs, -1, 0) == -1) {
		tsp_debug_err(true, &ts->client->dev, "%s: pending irq -1\n", __func__);
		return -EINVAL;
	}

	if (atomic_cmpxchg(&ts->secure_pending_irqs, 1, 0) == 1)
		val = 1;

	tsp_debug_err(true, &ts->client->dev, "%s: pending irq is %d\n",
				__func__, atomic_read(&ts->secure_pending_irqs));

	complete(&ts->secure_interrupt);

	return snprintf(buf, PAGE_SIZE, "%u", val);
}

static ssize_t secure_ownership_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "1");
}

static DEVICE_ATTR(secure_touch_enable, (S_IRUGO | S_IWUSR | S_IWGRP),
				secure_touch_enable_show, secure_touch_enable_store);
static DEVICE_ATTR(secure_touch, S_IRUGO, secure_touch_show, NULL);

static DEVICE_ATTR(secure_ownership, S_IRUGO, secure_ownership_show, NULL);

static struct attribute *secure_attr[] = {
	&dev_attr_secure_touch_enable.attr,
	&dev_attr_secure_touch.attr,
	&dev_attr_secure_ownership.attr,
	NULL,
};

static struct attribute_group secure_attr_group = {
	.attrs = secure_attr,
};


static int secure_touch_init(struct sec_ts_data *ts)
{
	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);

	init_completion(&ts->secure_interrupt);
	init_completion(&ts->secure_powerdown);

	ts->core_clk = clk_get(&ts->client->dev, "core_clk");
	if (IS_ERR_OR_NULL(ts->core_clk)) {
		tsp_debug_err(true, &ts->client->dev, "%s: failed to get core_clk: %ld\n",
					__func__, PTR_ERR(ts->core_clk));
		goto err_core_clk;
	}

	ts->iface_clk = clk_get(&ts->client->dev, "iface_clk");
	if (IS_ERR_OR_NULL(ts->iface_clk)) {
		tsp_debug_err(true, &ts->client->dev, "%s: failed to get iface_clk: %ld\n",
					__func__, PTR_ERR(ts->iface_clk));
		goto err_iface_clk;
	}

	return 0;

err_iface_clk:
	clk_put(ts->core_clk);
err_core_clk:
	ts->core_clk = NULL;
	ts->iface_clk = NULL;

	return ENODEV;
}

static void secure_touch_stop(struct sec_ts_data *ts, bool stop)
{
	if (atomic_read(&ts->secure_enabled)) {
		atomic_set(&ts->secure_pending_irqs, -1);
		secure_touch_notify(ts);

		if (stop)
			wait_for_completion_interruptible(&ts->secure_powerdown);

		tsp_debug_info(true, &ts->client->dev, "%s: %d\n", __func__, stop);
	}
}
#endif

int sec_ts_i2c_write(struct sec_ts_data *ts, u8 reg, u8 *data, int len)
{
	u8 buf[I2C_WRITE_BUFFER_SIZE + 1];
	int ret;
	unsigned char retry;
	struct i2c_msg msg;

	if (len > I2C_WRITE_BUFFER_SIZE) {
		tsp_debug_err(true, &ts->client->dev, "sec_ts_i2c_write len is larger than buffer size\n");
		return -1;
	}

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s: POWER_STATUS : OFF\n", __func__);
		goto err;
	}

	buf[0] = reg;
	memcpy(buf + 1, data, len);

	msg.addr = ts->client->addr;
	msg.flags = 0;
	msg.len = len + 1;
	msg.buf = buf;
	mutex_lock(&ts->i2c_mutex);
	for (retry = 0; retry < SEC_TS_I2C_RETRY_CNT; retry++) {
		if ((ret = i2c_transfer(ts->client->adapter, &msg, 1)) == 1)
			break;

		tsp_debug_err(true, &ts->client->dev, "%s: I2C retry %d\n", __func__, retry + 1);
		usleep_range(1 * 1000, 1 * 1000);
	}

	mutex_unlock(&ts->i2c_mutex);

	if (retry == SEC_TS_I2C_RETRY_CNT) {
		tsp_debug_err(true, &ts->client->dev, "%s: I2C write over retry limit\n", __func__);
		ret = -EIO;
#ifdef USE_POR_AFTER_I2C_RETRY
		schedule_delayed_work(&ts->reset_work, msecs_to_jiffies(TOUCH_RESET_DWORK_TIME));
#endif
	}

	if (ret == 1)
		return 0;
err:
	return -EIO;
}

int sec_ts_i2c_read(struct sec_ts_data *ts, u8 reg, u8 *data, int len)
{
	u8 buf[4];
	int ret;
	unsigned char retry;
	struct i2c_msg msg[2];
	int remain = len;

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s: POWER_STATUS : OFF\n", __func__);
		goto err;
	}

	buf[0] = reg;

	msg[0].addr = ts->client->addr;
	msg[0].flags = 0;
	msg[0].len = 1;
	msg[0].buf = buf;

	msg[1].addr = ts->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = data;

	mutex_lock(&ts->i2c_mutex);

	if (len <= ts->i2c_burstmax) {

		for (retry = 0; retry < SEC_TS_I2C_RETRY_CNT; retry++) {
			ret = i2c_transfer(ts->client->adapter, msg, 2);
			if (ret == 2)
				break;
			usleep_range(1 * 1000, 1 * 1000);
		}

	} else {
		/*
		 * I2C read buffer is 256 byte. do not support long buffer over than 256.
		 * So, try to seperate reading data about 256 bytes.
		 */

		for (retry = 0; retry < SEC_TS_I2C_RETRY_CNT; retry++) {
			ret = i2c_transfer(ts->client->adapter, msg, 1);
			if (ret == 1)
				break;
			usleep_range(1 * 1000, 1 * 1000);
		}

		do {
			if (remain > ts->i2c_burstmax)
				msg[1].len = ts->i2c_burstmax;
			else
				msg[1].len = remain;

			remain -= ts->i2c_burstmax;

			for (retry = 0; retry < SEC_TS_I2C_RETRY_CNT; retry++) {
				ret = i2c_transfer(ts->client->adapter, &msg[1], 1);
				if (ret == 1)
					break;
				usleep_range(1 * 1000, 1 * 1000);
			}

			msg[1].buf += msg[1].len;

		} while (remain > 0);

	}

	mutex_unlock(&ts->i2c_mutex);

	if (retry == SEC_TS_I2C_RETRY_CNT) {
		tsp_debug_err(true, &ts->client->dev, "%s: I2C read over retry limit\n", __func__);
		ret = -EIO;
#ifdef USE_POR_AFTER_I2C_RETRY
		schedule_delayed_work(&ts->reset_work, msecs_to_jiffies(TOUCH_RESET_DWORK_TIME));
#endif

	}

	return ret;

err:
	return -EIO;
}

static int sec_ts_i2c_write_burst(struct sec_ts_data *ts, u8 *data, int len)
{
	int ret;
	int retry;

	mutex_lock(&ts->i2c_mutex);
	for (retry = 0; retry < SEC_TS_I2C_RETRY_CNT; retry++) {
		if ((ret = i2c_master_send(ts->client, data, len)) == len)
			break;

		usleep_range(1 * 1000, 1 * 1000);
	}

	mutex_unlock(&ts->i2c_mutex);
	if (retry == SEC_TS_I2C_RETRY_CNT) {
		tsp_debug_err(true, &ts->client->dev, "%s: I2C write over retry limit\n", __func__);
		ret = -EIO;
	}

	return ret;
}

static int sec_ts_i2c_read_bulk(struct sec_ts_data *ts, u8 *data, int len)
{
	int ret;
	unsigned char retry;
	int remain = len;
	struct i2c_msg msg;

	msg.addr = ts->client->addr;
	msg.flags = I2C_M_RD;
	msg.len = len;
	msg.buf = data;

	mutex_lock(&ts->i2c_mutex);

	do {
		if (remain > ts->i2c_burstmax)
			msg.len = ts->i2c_burstmax;
		else
			msg.len = remain;

		remain -= ts->i2c_burstmax;

		for (retry = 0; retry < SEC_TS_I2C_RETRY_CNT; retry++) {
			ret = i2c_transfer(ts->client->adapter, &msg, 1);
			if (ret == 1)
				break;
			usleep_range(1 * 1000, 1 * 1000);
		}

	if (retry == SEC_TS_I2C_RETRY_CNT) {
		tsp_debug_err(true, &ts->client->dev, "%s: I2C read over retry limit\n", __func__);
		ret = -EIO;

		break;
	}
		msg.buf += msg.len;

	} while (remain > 0);

	mutex_unlock(&ts->i2c_mutex);

	if (ret == 1)
		return 0;

	return -EIO;
}

#if defined(CONFIG_TOUCHSCREEN_DUMP_MODE)
#include <linux/qcom/sec_debug.h>
extern struct tsp_dump_callbacks dump_callbacks;
static struct delayed_work * p_ghost_check;

extern void sec_ts_run_rawdata_all(struct sec_ts_data *ts);
static void sec_ts_check_rawdata(struct work_struct *work)
{
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data, ghost_check.work);

	if (ts->tsp_dump_lock == 1) {
		tsp_debug_err(true, &ts->client->dev, "%s, ignored ## already checking..\n", __func__);
		return;
	}
	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s, ignored ## IC is power off\n", __func__);
		return;
	}

	ts->tsp_dump_lock = 1;
	tsp_debug_err(true, &ts->client->dev, "%s, start ##\n", __func__);
	sec_ts_run_rawdata_all((void *)ts);
	msleep(100);

	tsp_debug_err(true, &ts->client->dev, "%s, done ##\n", __func__);
	ts->tsp_dump_lock = 0;

}

static void dump_tsp_log(void)
{
	printk(KERN_ERR "sec_ts %s: start \n", __func__);

#if defined(CONFIG_SAMSUNG_LPM_MODE)
	if (poweroff_charging) {
		printk(KERN_ERR "sec_ts %s, ignored ## lpm charging Mode!!\n", __func__);
		return;
	}
#endif
	if (p_ghost_check == NULL) {
		printk(KERN_ERR "sec_ts %s, ignored ## tsp probe fail!!\n", __func__);
		return;
	}
	schedule_delayed_work(p_ghost_check, msecs_to_jiffies(100));
}
#endif


void sec_ts_delay(unsigned int ms)
{
	if (ms < 20)
		usleep_range(ms * 1000, ms * 1000);
	else
		msleep(ms);
}

int sec_ts_wait_for_ready(struct sec_ts_data *ts, unsigned int ack)
{
	int rc = -1;
	int retry = 0;
	u8 tBuff[SEC_TS_EVENT_BUFF_SIZE];

	while (sec_ts_i2c_read(ts, SEC_TS_READ_ONE_EVENT, tBuff, SEC_TS_EVENT_BUFF_SIZE)) {
		if (tBuff[0] == TYPE_STATUS_EVENT_ACK) {
			if (tBuff[1] == ack) {
				rc = 0;
				break;
			}
		}

		if (retry++ > SEC_TS_WAIT_RETRY_CNT) {
			tsp_debug_err(true, &ts->client->dev, "%s: Time Over\n", __func__);
			break;
		}
		sec_ts_delay(20);
	}

	tsp_debug_info(true, &ts->client->dev,
		"%s: %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X [%d]\n",
		__func__, tBuff[0], tBuff[1], tBuff[2], tBuff[3],
		tBuff[4], tBuff[5], tBuff[6], tBuff[7], retry);

	return rc;
}

int sec_ts_read_calibration_report(struct sec_ts_data *ts)
{
	int ret;
	u8 buf[5] = { 0 };

	buf[0] = SEC_TS_READ_CALIBRATION_REPORT;

	ret = sec_ts_i2c_read(ts, buf[0], &buf[1], 4);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: failed to read, %d\n", __func__, ret);
		return ret;
	}

	tsp_debug_info(true, &ts->client->dev, "%s: count:%d, pass count:%d, fail count:%d, status:0x%X\n",
				__func__, buf[1], buf[2], buf[3], buf[4]);

	return buf[4];
}

#define MAX_EVENT_COUNT 128
static void sec_sec_ts_read_event(struct sec_ts_data *ts)
{
	int ret;
	int is_event_remain = 0;
	int t_id;
	int event_id;
	int read_event_count = 0;
	bool print_log = false;
	u8 read_event_buff[SEC_TS_EVENT_BUFF_SIZE];
	struct sec_ts_event_coordinate *p_event_coord;
	struct sec_ts_coordinate *coordinate;

	/* in LPM, waiting blsp block resume */
	if (ts->power_status == SEC_TS_STATE_LPM_SUSPEND) {

		wake_lock_timeout(&ts->wakelock, msecs_to_jiffies(3 * MSEC_PER_SEC));
		/* waiting for blsp block resuming, if not occurs i2c error */
		ret = wait_for_completion_interruptible_timeout(&ts->resume_done, msecs_to_jiffies(1 * MSEC_PER_SEC));
		if (ret == 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: LPM: pm resume is not handled\n", __func__);
			return;
		}

		if (ret < 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: LPM: -ERESTARTSYS if interrupted, %d\n", __func__, ret);
			return;
		}

		tsp_debug_err(true, &ts->client->dev, "%s: run LPM interrupt handler, %d\n", __func__, ret);
		/* run lpm interrupt handler */
	}

	/* repeat READ_ONE_EVENT until buffer is empty(No event) */
	do {

		ret = sec_ts_i2c_read(ts, SEC_TS_READ_ONE_EVENT, read_event_buff, SEC_TS_EVENT_BUFF_SIZE);
		if (ret < 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: i2c read one event failed\n", __func__);
			return;
		}
#if 0
		tsp_debug_info(true, &ts->client->dev,
				"%s: ==== STATUS %x, %x(%X,%X), %x, %x, %x, %x, %x, %x ====\n", __func__,
				read_event_buff[0], read_event_buff[1], (read_event_buff[1] >> 4) & 0x0F,
				read_event_buff[1] & 0x0F, read_event_buff[2],
				read_event_buff[3], read_event_buff[4], read_event_buff[5],
				read_event_buff[6], read_event_buff[7]);
#endif

		if (read_event_count > MAX_EVENT_COUNT) {

			tsp_debug_err(true, &ts->client->dev, "%s: event buffer overflow\n", __func__);

			/* write clear event stack command when read_event_count > MAX_EVENT_COUNT */
			ret = sec_ts_i2c_write(ts, SEC_TS_CMD_CLEAR_EVENT_STACK, NULL, 0);
			if (ret < 0)
				tsp_debug_err(true, &ts->client->dev, "%s: i2c write clear event failed\n", __func__);

			return;
		}

		event_id = read_event_buff[0] >> 6;

		switch (event_id) {
		case SEC_TS_STATUS_EVENT:
			/* tchsta == 0 && ttype == 0 && eid == 0 : buffer empty */
			if (read_event_buff[0] != 0)
				tsp_debug_info(true, &ts->client->dev, "%s: STATUS %x %x %x %x %x %x %x %x (%d)\n", __func__,
						read_event_buff[0], read_event_buff[1], read_event_buff[2],
						read_event_buff[3], read_event_buff[4], read_event_buff[5],
						read_event_buff[6], read_event_buff[7], is_event_remain);

			/* watchdog reset -> send SENSEON command */
			if ((read_event_buff[0] == TYPE_STATUS_EVENT_ACK) &&
				(read_event_buff[1] == SEC_TS_ACK_BOOT_COMPLETE) &&
				(read_event_buff[2] == 0x20)) {

				sec_ts_unlocked_release_all_finger(ts);

				ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SENSE_ON, NULL, 0);
				if (ret < 0)
					tsp_debug_err(true, &ts->client->dev, "%s: fail to write Sense_on\n", __func__);

			}

			if ((read_event_buff[0] == TYPE_STATUS_EVENT_ERR) &&
				(read_event_buff[1] == SEC_TS_ERR_ESD)) {
				tsp_debug_err(true, &ts->client->dev, "%s: ESD detected. run reset\n", __func__);
#ifdef USE_RESET_DURING_POWER_ON
				schedule_work(&ts->reset_work.work);
#endif
			}

			if ((ts->use_sponge) && (read_event_buff[0] == TYPE_STATUS_EVENT_ACK) &&
				(read_event_buff[1] == TYPE_STATUS_CODE_SPONGE)) {
				u8 sponge[3] = { 0 };

				ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SPONGE_READ_PARAM, sponge, 2);
				if (ret < 0)
					tsp_debug_err(true, &ts->client->dev, "%s: fail to read sponge command\n", __func__);

				memset(sponge, 0x00, 3);

				ret = sec_ts_i2c_read_bulk(ts, sponge, 3);
				if (ret < 0)
					tsp_debug_err(true, &ts->client->dev, "%s: fail to read sponge data\n", __func__);

				tsp_debug_info(true, &ts->client->dev, "%s: Sponge, %x, %x, %x\n",
							__func__, sponge[0], sponge[1], sponge[2]);

				/* will be fixed to data structure */
				if (sponge[1] & SEC_TS_MODE_SPONGE_AOD)
					ts->scrub_id = SPONGE_EVENT_TYPE_AOD;

				if (sponge[1] & SEC_TS_MODE_SPONGE_SPAY)
					ts->scrub_id = SPONGE_EVENT_TYPE_SPAY;

				input_report_key(ts->input_dev, KEY_BLACK_UI_GESTURE, 1);
				input_sync(ts->input_dev);
				input_report_key(ts->input_dev, KEY_BLACK_UI_GESTURE, 0);
			}

#ifdef SEC_TS_SUPPORT_5MM_SAR_MODE
			/* sar mode */
			if ((read_event_buff[0] == TYPE_STATUS_EVENT_ACK) && (read_event_buff[1] == TYPE_STATUS_CODE_SAR)) {
				if (read_event_buff[2])
					ts->scrub_id = 101; /*sar on*/
				else
					ts->scrub_id = 100; /*sar off*/

				tsp_debug_err(true, &ts->client->dev, "%s: SAR detected %d\n", __func__, read_event_buff[2]);
				input_report_key(ts->input_dev, KEY_BLACK_UI_GESTURE, 1);
				input_sync(ts->input_dev);
				input_report_key(ts->input_dev, KEY_BLACK_UI_GESTURE, 0);
			}
#endif
			is_event_remain = 0;
			break;

		case SEC_TS_COORDINATE_EVENT:
			if (ts->input_closed) {
				tsp_debug_info(true, &ts->client->dev, "%s: device is closed\n", __func__);				
				is_event_remain = 0;
				break;
			}
			p_event_coord = (struct sec_ts_event_coordinate *)read_event_buff;

			t_id = (p_event_coord->tid - 1);

			if (t_id < MAX_SUPPORT_TOUCH_COUNT + MAX_SUPPORT_HOVER_COUNT) {
				print_log = true;
				coordinate = &ts->coord[t_id];
				coordinate->id = t_id;
				coordinate->action = p_event_coord->tchsta;
				coordinate->x = (p_event_coord->x_11_4 << 4) | (p_event_coord->x_3_0);
				coordinate->y = (p_event_coord->y_11_4 << 4) | (p_event_coord->y_3_0);
				coordinate->z = p_event_coord->z;
				coordinate->ttype = p_event_coord->ttype & MASK_3_BITS;
				coordinate->major = p_event_coord->major;
				coordinate->minor = p_event_coord->minor;

				if (!coordinate->palm && (coordinate->ttype == SEC_TS_TOUCHTYPE_PALM))
					coordinate->palm_count++;

				coordinate->palm = (coordinate->ttype == SEC_TS_TOUCHTYPE_PALM);

#ifdef SEC_TS_SUPPORT_HOVERING
				if ((t_id == SEC_TS_EVENTID_HOVER) && coordinate->ttype == SEC_TS_TOUCHTYPE_PROXIMITY) {
					if (coordinate->action == SEC_TS_COORDINATE_ACTION_RELEASE) {
						input_mt_slot(ts->input_dev, 0);
						input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
						tsp_debug_dbg(true, &ts->client->dev,
								"%s: Hover - Release - tid=%d, touch_count=%d\n",
								__func__, t_id, ts->touch_count);
					} else {
						input_mt_slot(ts->input_dev, 0);
						input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);

						input_report_key(ts->input_dev, BTN_TOUCH, false);
						input_report_key(ts->input_dev, BTN_TOOL_FINGER, true);

						input_report_abs(ts->input_dev, ABS_MT_POSITION_X, coordinate->x);
						input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, coordinate->y);

						if (coordinate->action == SEC_TS_COORDINATE_ACTION_PRESS)
							tsp_debug_dbg(true, &ts->client->dev,
									"%s: Hover - Press - tid=%d, touch_count=%d\n",
									__func__, t_id, ts->touch_count);
						else if (coordinate->action == SEC_TS_COORDINATE_ACTION_MOVE)
							tsp_debug_dbg(true, &ts->client->dev,
									"%s: Hover - Move - tid=%d, touch_count=%d\n",
									__func__, t_id, ts->touch_count);
					}
				}
#endif
				if ((coordinate->ttype == SEC_TS_TOUCHTYPE_NORMAL)
						|| (coordinate->ttype == SEC_TS_TOUCHTYPE_PALM)
						|| (coordinate->ttype == SEC_TS_TOUCHTYPE_GLOVE)) {

					if (coordinate->action == SEC_TS_COORDINATE_ACTION_RELEASE) {

						input_mt_slot(ts->input_dev, t_id);
						input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);

						if (ts->touch_count > 0)
							ts->touch_count--;
						if (ts->touch_count == 0) {
							input_report_key(ts->input_dev, BTN_TOUCH, 0);
							input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
						}

					} else if (coordinate->action == SEC_TS_COORDINATE_ACTION_PRESS) {
						ts->touch_count++;
						input_mt_slot(ts->input_dev, t_id);
						input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 1);
						input_report_key(ts->input_dev, BTN_TOUCH, 1);
						input_report_key(ts->input_dev, BTN_TOOL_FINGER, 1);

						input_report_abs(ts->input_dev, ABS_MT_POSITION_X, coordinate->x);
						input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, coordinate->y);
						input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, coordinate->major);
						input_report_abs(ts->input_dev, ABS_MT_TOUCH_MINOR, coordinate->minor);
						input_report_abs(ts->input_dev, ABS_MT_PALM, coordinate->palm);
#ifdef CONFIG_SEC_FACTORY
						input_report_abs(ts->input_dev, ABS_MT_PRESSURE, coordinate->z);
#endif
					} else if (coordinate->action == SEC_TS_COORDINATE_ACTION_MOVE) {
						if ((coordinate->ttype == SEC_TS_TOUCHTYPE_GLOVE) && !ts->touchkey_glove_mode_status) {
							ts->touchkey_glove_mode_status = true;
							input_report_switch(ts->input_dev, SW_GLOVE, 1);
						}

						input_mt_slot(ts->input_dev, t_id);
						input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 1);
						input_report_key(ts->input_dev, BTN_TOUCH, 1);
						input_report_key(ts->input_dev, BTN_TOOL_FINGER, 1);

						input_report_abs(ts->input_dev, ABS_MT_POSITION_X, coordinate->x);
						input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, coordinate->y);
						input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, coordinate->major);
						input_report_abs(ts->input_dev, ABS_MT_TOUCH_MINOR, coordinate->minor);
						input_report_abs(ts->input_dev, ABS_MT_PALM, coordinate->palm);
#ifdef CONFIG_SEC_FACTORY
						input_report_abs(ts->input_dev, ABS_MT_PRESSURE, coordinate->z);
#endif
					} else {
						tsp_debug_dbg(true, &ts->client->dev,
								"%s: do not support coordinate action(%d)\n", __func__, coordinate->action);
					}
				} else {
					tsp_debug_dbg(true, &ts->client->dev,
							"%s: do not support coordinate type(%d)\n", __func__, coordinate->ttype);
				}
			} else {
				tsp_debug_err(true, &ts->client->dev, "%s: tid(%d) is  out of range\n", __func__, t_id);
			}

			is_event_remain = 1;
			break;

		case SEC_TS_GESTURE_EVENT:
			if (read_event_buff[0] & 0x01) {
				struct sec_ts_gesture_status *p_gesture_status = (struct sec_ts_gesture_status *)read_event_buff;

				if (p_gesture_status->scode == SEC_TS_GESTURE_CODE_SIDE_GESTURE) {
					tsp_debug_info(true, &ts->client->dev, "%s: Side gesture: %x\n",
								__func__, p_gesture_status->gesture);

					if ((read_event_buff[2] >= 13) && (read_event_buff[4] >= 13))
						input_report_key(ts->input_dev, KEY_SIDE_GESTURE_RIGHT, 1);
					else if ((read_event_buff[2] <= 1) && (read_event_buff[4] <= 1))
						input_report_key(ts->input_dev, KEY_SIDE_GESTURE_LEFT, 1);
					else
						tsp_debug_info(true, &ts->client->dev, "%s: ERROR  %x %x %x %x %x %x %x %x\n", __func__,
								read_event_buff[0], read_event_buff[1], read_event_buff[2],
								read_event_buff[3], read_event_buff[4], read_event_buff[5],
								read_event_buff[6], read_event_buff[7]);

					input_sync(ts->input_dev);

					input_report_key(ts->input_dev, KEY_SIDE_GESTURE_LEFT, 0);
					input_report_key(ts->input_dev, KEY_SIDE_GESTURE_RIGHT, 0);

				} else {
					/* gesture->status & 0x1 : wake up event */
					tsp_debug_info(true, &ts->client->dev, "%s: %s\n", __func__,
								p_gesture_status->scode == SEC_TS_GESTURE_CODE_AOD ? "AOD" :
								p_gesture_status->scode == SEC_TS_GESTURE_CODE_SPAY ? "SPAY" : "OTHER");

					/* will be fixed after merge String Liabrary : SPAY or Double Tab */
					if (p_gesture_status->scode == SEC_TS_GESTURE_CODE_AOD)
						ts->scrub_id = SPONGE_EVENT_TYPE_AOD;
					else if (p_gesture_status->scode == SEC_TS_GESTURE_CODE_SPAY)
						ts->scrub_id = SPONGE_EVENT_TYPE_SPAY;

					input_report_key(ts->input_dev, KEY_BLACK_UI_GESTURE, 1);
					input_sync(ts->input_dev);
					input_report_key(ts->input_dev, KEY_BLACK_UI_GESTURE, 0);
				}

				/*
				tsp_debug_info(true, &ts->client->dev, "%s: GESTURE  %x %x %x %x %x %x %x %x\n", __func__,
						read_event_buff[0], read_event_buff[1], read_event_buff[2],
						read_event_buff[3], read_event_buff[4], read_event_buff[5],
						read_event_buff[6], read_event_buff[7]);
				*/

			}
			is_event_remain = 1;
			break;

		default:
			tsp_debug_err(true, &ts->client->dev, "%s: unknown event  %x %x %x %x %x %x\n", __func__,
					read_event_buff[0], read_event_buff[1], read_event_buff[2],
					read_event_buff[3], read_event_buff[4], read_event_buff[5]);

			is_event_remain = 1;
			break;

		}

		if (print_log) {
			if (coordinate->action == SEC_TS_COORDINATE_ACTION_PRESS) {
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
					tsp_debug_info(true, &ts->client->dev,
					"%s: [P] tID:%d, x:%d, y:%d, z:%d, major:%d, minor:%d, tc:%d, type:%X\n",
					__func__, t_id, coordinate->x, coordinate->y, coordinate->z,
					coordinate->major, coordinate->minor, ts->touch_count,
					coordinate->ttype);
#else
					tsp_debug_info(true, &ts->client->dev,
					"%s: [P] tID:%d, tc:%d, type:%X\n",
					__func__, t_id, ts->touch_count, coordinate->ttype);
#endif
				coordinate->action = SEC_TS_COORDINATE_ACTION_MOVE;

			} else if (coordinate->action == SEC_TS_COORDINATE_ACTION_RELEASE) {
#if !defined(CONFIG_SAMSUNG_PRODUCT_SHIP)
					tsp_debug_info(true, &ts->client->dev,
					"%s: [R] tID:%d mc: %d tc:%d lx:%d ly:%d, v:%02X%02X, cal:%X(%X|%X), id(%d,%d), p:%d\n",
						__func__, t_id, coordinate->mcount, ts->touch_count,
					coordinate->x, coordinate->y,
						ts->plat_data->img_version_of_ic[2],
					ts->plat_data->img_version_of_ic[3],
					ts->cal_status, ts->nv, ts->cal_count, ts->tspid_val, ts->tspid2_val, coordinate->palm_count);
#else
				tsp_debug_info(true, &ts->client->dev,
					"%s: [R] tID:%d mc: %d tc:%d, v:%02X%02X, cal:%X(%X|%X), id(%d,%d), p:%d\n",
						__func__, t_id, coordinate->mcount, ts->touch_count,
						ts->plat_data->img_version_of_ic[2],
					ts->plat_data->img_version_of_ic[3],
					ts->cal_status, ts->nv, ts->cal_count, ts->tspid_val, ts->tspid2_val, coordinate->palm_count);
#endif
				coordinate->action = SEC_TS_COORDINATE_ACTION_NONE;
				coordinate->mcount = 0;
				coordinate->palm_count = 0;
			} else if (coordinate->action == SEC_TS_COORDINATE_ACTION_MOVE) {
				coordinate->mcount++;
			}/* else {
				tsp_debug_info(true, &ts->client->dev,
						"%s: undefined status: %X\n",
						__func__, coordinate->action);
			}*/

			print_log = false;
		}
	} while (is_event_remain);

	input_sync(ts->input_dev);

}

static irqreturn_t sec_ts_irq_thread(int irq, void *ptr)
{
	struct sec_ts_data *ts = (struct sec_ts_data *)ptr;

#ifdef CONFIG_SECURE_TOUCH
	if (secure_filter_interrupt(ts) == IRQ_HANDLED) {
		wait_for_completion_interruptible_timeout(&ts->secure_interrupt,
					msecs_to_jiffies(5 * MSEC_PER_SEC));

		tsp_debug_info(true, &ts->client->dev,
					"%s: secure interrupt handled\n", __func__);

		return IRQ_HANDLED;
	}
#endif

	mutex_lock(&ts->eventlock);

	sec_sec_ts_read_event(ts);

	mutex_unlock(&ts->eventlock);

	return IRQ_HANDLED;
}

int get_tsp_status(void)
{
	return 0;
}
EXPORT_SYMBOL(get_tsp_status);

void sec_ts_set_charger(bool enable)
{
	return;
/*
	int ret;
	u8 noise_mode_on[] = {0x01};
	u8 noise_mode_off[] = {0x00};

	if (enable) {
		tsp_debug_info(true, &ts->client->dev, "sec_ts_set_charger : charger CONNECTED!!\n");
		ret = sec_ts_i2c_write(ts, SEC_TS_CMD_NOISE_MODE, noise_mode_on, sizeof(noise_mode_on));
		if (ret < 0)
			tsp_debug_err(true, &ts->client->dev, "sec_ts_set_charger: fail to write NOISE_ON\n");
	} else {
		tsp_debug_info(true, &ts->client->dev, "sec_ts_set_charger : charger DISCONNECTED!!\n");
		ret = sec_ts_i2c_write(ts, SEC_TS_CMD_NOISE_MODE, noise_mode_off, sizeof(noise_mode_off));
		if (ret < 0)
			tsp_debug_err(true, &ts->client->dev, "sec_ts_set_charger: fail to write NOISE_OFF\n");
	}
 */
}
EXPORT_SYMBOL(sec_ts_set_charger);

int sec_ts_glove_mode_enables(struct sec_ts_data *ts, int mode)
{
	int ret;

	if (mode)
		ts->touch_functions = (ts->touch_functions | SEC_TS_BIT_SETFUNC_GLOVE | SEC_TS_BIT_SETFUNC_MUTUAL);
	else
		ts->touch_functions = ((ts->touch_functions & (~SEC_TS_BIT_SETFUNC_GLOVE)) | SEC_TS_BIT_SETFUNC_MUTUAL);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s: pwr off, glove:%d, status:%x\n", __func__,
					mode, ts->touch_functions);
		goto glove_enable_err;
	}

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION, &(ts->touch_functions), 1);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: Failed to send command", __func__);
		goto glove_enable_err;
	}

	tsp_debug_info(true, &ts->client->dev, "%s: glove:%d, status:%x\n", __func__,
		mode, ts->touch_functions);

	return 0;

glove_enable_err:
	return -EIO;
}
EXPORT_SYMBOL(sec_ts_glove_mode_enables);

#ifdef SEC_TS_SUPPORT_HOVERING
int sec_ts_hover_enables(struct sec_ts_data *ts, int enables)
{
	int ret;

	if (enables)
		ts->touch_functions = (ts->touch_functions | SEC_TS_BIT_SETFUNC_HOVER | SEC_TS_BIT_SETFUNC_MUTUAL);
	else
		ts->touch_functions = ((ts->touch_functions & (~SEC_TS_BIT_SETFUNC_HOVER)) | SEC_TS_BIT_SETFUNC_MUTUAL);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s: pwr off, hover:%d, status:%x\n", __func__,
					enables, ts->touch_functions);
		goto hover_enable_err;
	}

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION, &(ts->touch_functions), 1);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: Failed to send command", __func__);
		goto hover_enable_err;
	}

	tsp_debug_info(true, &ts->client->dev, "%s: hover:%d, status:%x\n", __func__,
		enables ,ts->touch_functions);

	return 0;

hover_enable_err:
	return -EIO;
}
EXPORT_SYMBOL(sec_ts_hover_enables);
#endif

int sec_ts_set_cover_type(struct sec_ts_data *ts, bool enable)
{
	int ret;

	tsp_debug_info(true, &ts->client->dev, "%s: %d\n", __func__, ts->cover_type);


	switch (ts->cover_type) {
	case SEC_TS_VIEW_WIRELESS:
	case SEC_TS_VIEW_COVER:
	case SEC_TS_VIEW_WALLET:
	case SEC_TS_FLIP_WALLET:
	case SEC_TS_LED_COVER:
	case SEC_TS_MONTBLANC_COVER:
	case SEC_TS_CLEAR_FLIP_COVER:
	case SEC_TS_QWERTY_KEYBOARD_EUR:
	case SEC_TS_QWERTY_KEYBOARD_KOR:
		ts->cover_cmd = (u8)ts->cover_type;
		break;
	case SEC_TS_CHARGER_COVER:
	case SEC_TS_COVER_NOTHING1:
	case SEC_TS_COVER_NOTHING2:
	default:
		ts->cover_cmd = 0;
		tsp_debug_err(true, &ts->client->dev, "%s: not chage touch state, %d\n",
				__func__, ts->cover_type);
		break;
	}

	if (enable)
		ts->touch_functions = (ts->touch_functions | SEC_TS_BIT_SETFUNC_COVER | SEC_TS_BIT_SETFUNC_MUTUAL);
	else
		ts->touch_functions = ((ts->touch_functions & (~SEC_TS_BIT_SETFUNC_COVER)) | SEC_TS_BIT_SETFUNC_MUTUAL);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s: pwr off, close:%d, status:%x\n", __func__,
					enable, ts->touch_functions);
		goto cover_enable_err;
	}

	if (enable) {
		ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_COVERTYPE, &ts->cover_cmd, 1);
		if (ret < 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: Failed to send covertype command: %d", __func__, ts->cover_cmd);
			goto cover_enable_err;
		}
	}

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION, &(ts->touch_functions), 1);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: Failed to send command", __func__);
		goto cover_enable_err;
	}

	tsp_debug_info(true, &ts->client->dev, "%s: close:%d, status:%x\n", __func__,
		enable, ts->touch_functions);

	return 0;

cover_enable_err:
	return -EIO;


}
EXPORT_SYMBOL(sec_ts_set_cover_type);

/* for debugging--------------------------------------------------------------------------------------*/
static ssize_t sec_ts_reg_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_info(true, &ts->client->dev, "%s: Power off state\n", __func__);
		return -EIO;
	}

	if (size > 0)
		sec_ts_i2c_write_burst(ts, (u8 *)buf, size);

	tsp_debug_info(true, &ts->client->dev, "sec_ts_reg: 0x%x, 0x%x, size %d\n", buf[0], buf[1], (int)size);
	return size;
}

static ssize_t sec_ts_regread_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);
	int ret;
	int length;
	int remain;
	int offset;

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_info(true, &ts->client->dev, "%s: Power off state\n", __func__);
		return -EIO;
	}

	disable_irq(ts->client->irq);

	read_lv1_buff = kzalloc(lv1_readsize, GFP_KERNEL);
	if (!read_lv1_buff) {
		tsp_debug_err(true, &ts->client->dev, "%s kzalloc failed\n", __func__);
		goto malloc_err;
	}

	mutex_lock(&ts->device_mutex);
	remain = lv1_readsize;
	offset = 0;
	do {
		if (remain >= ts->i2c_burstmax)
			length = ts->i2c_burstmax;
		else
			length = remain;

		if (offset == 0)
			ret = sec_ts_i2c_read(ts, lv1cmd, &read_lv1_buff[offset], length);
		else
			ret = sec_ts_i2c_read_bulk(ts, &read_lv1_buff[offset], length);

		if (ret < 0) {
			tsp_debug_err(true, &ts->client->dev, "%s: i2c read %x command, remain =%d\n", __func__, lv1cmd, remain);
			goto i2c_err;
		}

		remain -= length;
		offset += length;
	} while (remain > 0);

	tsp_debug_info(true, &ts->client->dev, "%s: lv1_readsize = %d\n", __func__, lv1_readsize);
	memcpy(buf, read_lv1_buff + lv1_readoffset, lv1_readsize);

i2c_err:
	kfree(read_lv1_buff);
malloc_err:
	mutex_unlock(&ts->device_mutex);
	lv1_readremain = 0;
	enable_irq(ts->client->irq);

	return lv1_readsize;
}

static ssize_t sec_ts_gesture_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);

	mutex_lock(&ts->device_mutex);
	memcpy(buf, ts->gesture_status, sizeof(ts->gesture_status));
	tsp_debug_info(true, &ts->client->dev,
				"sec_sec_ts_gesture_status_show GESTURE STATUS %x %x %x %x %x %x\n",
				ts->gesture_status[0], ts->gesture_status[1], ts->gesture_status[2],
				ts->gesture_status[3], ts->gesture_status[4], ts->gesture_status[5]);
	mutex_unlock(&ts->device_mutex);

	return sizeof(ts->gesture_status);
}

static ssize_t sec_ts_regreadsize_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	lv1cmd = buf[0];
	lv1_readsize = ((unsigned int)buf[4] << 24) |
			((unsigned int)buf[3] << 16) | ((unsigned int) buf[2] << 8) | ((unsigned int)buf[1] << 0);
	lv1_readoffset = 0;
	lv1_readremain = 0;
	return size;
}

static ssize_t sec_ts_enter_recovery_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t size)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);
	struct sec_ts_plat_data *pdata = ts->plat_data;
	int ret;
	int on;

	sscanf(buf, "%d", &on);

	if (on == 1) {
		disable_irq(ts->client->irq);
		gpio_free(pdata->gpio);

		tsp_debug_info(true, &ts->client->dev, "%s: gpio free\n", __func__);
		if (gpio_is_valid(pdata->gpio)) {
			ret = gpio_request_one(pdata->gpio, GPIOF_OUT_INIT_LOW, "sec,tsp_int");
			tsp_debug_info(true, &ts->client->dev, "%s: gpio request one\n", __func__);
			if (ret < 0) {
				tsp_debug_err(true, &ts->client->dev, "Unable to request tsp_int [%d]: %d\n", pdata->gpio, ret);
			}
		} else {
			tsp_debug_err(true, &ts->client->dev, "Failed to get irq gpio\n");
			return -EINVAL;
		}

		pdata->power(ts->client, false);
		sec_ts_delay(100);
		pdata->power(ts->client, true);
	} else {
		gpio_free(pdata->gpio);

		if (gpio_is_valid(pdata->gpio)) {
			ret = gpio_request_one(pdata->gpio, GPIOF_DIR_IN, "sec,tsp_int");
			if (ret) {
				tsp_debug_err(true, &ts->client->dev, "Unable to request tsp_int [%d]\n", pdata->gpio);
				return -EINVAL;
			}
		} else {
			tsp_debug_err(true, &ts->client->dev, "Failed to get irq gpio\n");
			return -EINVAL;
		}

		pdata->power(ts->client, false);
		sec_ts_delay(500);
		pdata->power(ts->client, true);
		sec_ts_delay(500);

		/* AFE Calibration */
		ret = sec_ts_i2c_write(ts, SEC_TS_CMD_CALIBRATION_AMBIENT, NULL, 0);
		if (ret < 0)
			tsp_debug_err(true, &ts->client->dev, "%s: fail to write AFE_CAL\n", __func__);

		sec_ts_delay(1000);
		enable_irq(ts->client->irq);
	}

	sec_ts_read_information(ts);

	return size;
}

#ifdef SEC_TS_SUPPORT_TA_MODE
static void sec_ts_charger_config(struct sec_ts_data *ts, int status)
{
	int ret;

	if (status == 0x01 || status == 0x03)
		ts->touch_functions = (ts->touch_functions | SEC_TS_BIT_SETFUNC_CHARGER | SEC_TS_BIT_SETFUNC_MUTUAL);
	else
		ts->touch_functions = ((ts->touch_functions & (~SEC_TS_BIT_SETFUNC_CHARGER)) | SEC_TS_BIT_SETFUNC_MUTUAL);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s: pwr off, chg:%d, status:%x\n", __func__,
					enable, ts->touch_functions);
		goto charger_enable_err;
	}

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION, &(ts->touch_functions), 1);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: Failed to send command", __func__);
		goto charger_enable_err;
	}

	tsp_debug_info(true, &ts->client->dev, "%s: chg:%d, status:%x\n", __func__,
				enable, ts->touch_functions);

	return 0;

charger_enable_err:
	return -EIO;

}

static void sec_ts_ta_cb(struct sec_ts_callbacks *cb, int status)
{
	struct sec_ts_data *ts =
		container_of(cb, struct sec_ts_data, callbacks);

	tsp_debug_err(true, &ts->client->dev, "[TSP] %s: status : %x\n", __func__, status);

	ts->ta_status = status;
	/* if do not completed driver loading, ta_cb will not run. */
/*
	if (!rmi4_data->init_done.done) {
		tsp_debug_err(true, &rmi4_data->i2c_client->dev,
				"%s: until driver loading done.\n", __func__);
		return;
	}

	if (rmi4_data->touch_stopped || rmi4_data->doing_reflash) {
		tsp_debug_err(true, &rmi4_data->i2c_client->dev,
				"%s: device is in suspend state or reflash.\n",
				__func__);
		return;
	}
*/

	sec_ts_charger_config(ts, status);
}
#endif

static int sec_ts_raw_device_init(struct sec_ts_data *ts)
{
	int ret;

	sec_ts_dev = device_create(sec_class, NULL, 0, ts, "sec_ts");

	ret = IS_ERR(sec_ts_dev);
	if (ret) {
		tsp_debug_err(true, &ts->client->dev, "%s: fail - device_create\n", __func__);
		return ret;
	}

	ret = sysfs_create_group(&sec_ts_dev->kobj, &cmd_attr_group);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: fail - sysfs_create_group\n", __func__);
		goto err_sysfs;
	}

/*
	ret = sysfs_create_link(&sec_ts_dev->kobj,
			&ts->input_dev->dev.kobj, "input");
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "%s: fail - sysfs_create_link\n", __func__);
		goto err_sysfs;
	}
*/
	return ret;
err_sysfs:
	tsp_debug_err(true, &ts->client->dev, "%s: fail\n", __func__);
	return ret;
}

/* for debugging--------------------------------------------------------------------------------------*/

static int sec_ts_pinctrl_configure(struct sec_ts_data *ts, bool enable)
{
	struct pinctrl_state *state;

	tsp_debug_info(true, &ts->client->dev, "%s: %s\n", __func__, enable ? "ACTIVE" : "SUSPEND");

	if (enable) {
		state = pinctrl_lookup_state(ts->plat_data->pinctrl, "tsp_active");
		if (IS_ERR(ts->plat_data->pins_default))
			tsp_debug_err(true, &ts->client->dev, "could not get active pinstate\n");
	} else {
		state = pinctrl_lookup_state(ts->plat_data->pinctrl, "tsp_suspend");
		if (IS_ERR(ts->plat_data->pins_sleep))
			tsp_debug_err(true, &ts->client->dev, "could not get suspend pinstate\n");
	}

	if (!IS_ERR_OR_NULL(state))
		return pinctrl_select_state(ts->plat_data->pinctrl, state);

	return 0;

}

static int sec_ts_power_ctrl(struct i2c_client *client, bool enable)
{
	struct device *dev = &client->dev;
	int retval = 0;
	static struct regulator *avdd, *vddo;

	dev_err(dev, "%s: %d\n", __func__, enable);

	if (!avdd) {
		avdd = devm_regulator_get(&client->dev, "avdd");

		if (IS_ERR(avdd)) {
			dev_err(dev, "%s: could not get avdd, rc = %ld\n",
					__func__, PTR_ERR(avdd));
			avdd = NULL;
			return -ENODEV;
		}
		retval = regulator_set_voltage(avdd, 3300000, 3300000);
		if (retval)
			dev_err(dev, "%s: unable to set avdd voltage to 3.3V\n",
					__func__);

		dev_err(dev, "%s: 3.3V is enabled %s\n", __func__, regulator_is_enabled(avdd) ? "TRUE" : "FALSE");
	}
	if (!vddo) {
		vddo = devm_regulator_get(&client->dev, "vddo");

		if (IS_ERR(vddo)) {
			dev_err(dev, "%s: could not get vddo, rc = %ld\n",
					__func__, PTR_ERR(vddo));
			vddo = NULL;
			return -ENODEV;
		}
		retval = regulator_set_voltage(vddo, 1800000, 1800000);
		if (retval)
			dev_err(dev, "%s: unable to set vddo voltage to 1.8V\n",
					__func__);
		dev_err(dev, "%s: 1.8V is enabled %s\n", __func__, regulator_is_enabled(vddo) ? "TRUE" : "FALSE");

	}

	if (enable) {
		if (0/*regulator_is_enabled(avdd)*/) {
			dev_err(dev, "%s: avdd is already enabled\n", __func__);
		} else {
			retval = regulator_enable(avdd);
			if (retval) {
				dev_err(dev, "%s: Fail to enable regulator avdd[%d]\n",
						__func__, retval);
			}
			dev_err(dev, "%s: avdd is enabled[OK]\n", __func__);
		}

		if (0/*regulator_is_enabled(vddo)*/) {
			dev_err(dev, "%s: vddo is already enabled\n", __func__);
		} else {
			retval = regulator_enable(vddo);
			if (retval) {
				dev_err(dev, "%s: Fail to enable regulator vddo[%d]\n",
						__func__, retval);
			}
			dev_err(dev, "%s: vddo is enabled[OK]\n", __func__);
		}
	} else {
		if (regulator_is_enabled(vddo)) {
			retval = regulator_disable(vddo);
			if (retval) {
				dev_err(dev, "%s: Fail to disable regulator vddo[%d]\n",
						__func__, retval);

			}
			dev_err(dev, "%s: vddo is disabled[OK]\n", __func__);
		} else {
			dev_err(dev, "%s: vddo is already disabled\n", __func__);
		}

		if (regulator_is_enabled(avdd)) {
			retval = regulator_disable(avdd);
			if (retval) {
				dev_err(dev, "%s: Fail to disable regulator avdd[%d]\n",
						__func__, retval);

			}
			dev_err(dev, "%s: avdd is disabled[OK]\n", __func__);
		} else {
			dev_err(dev, "%s: avdd is already disabled\n", __func__);
		}
	}

	return retval;
}

static int sec_ts_parse_dt(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sec_ts_plat_data *pdata = dev->platform_data;
	struct device_node *np = dev->of_node;
	u32 coords[2], lines[2];
	u32 temp;
	int ret = 0;
	int id = 0;
	int count = 0;

	pdata->gpio = of_get_named_gpio(np, "sec,irq_gpio", 0);
	if (gpio_is_valid(pdata->gpio)) {
		ret = gpio_request_one(pdata->gpio, GPIOF_DIR_IN, "sec,tsp_int");
		if (ret) {
			tsp_debug_err(true, &client->dev, "Unable to request tsp_int [%d]\n", pdata->gpio);
			return -EINVAL;
		}
	} else {
		tsp_debug_err(true, &client->dev, "Failed to get irq gpio\n");
		return -EINVAL;
	}

	client->irq = gpio_to_irq(pdata->gpio);

	if (of_property_read_u32(np, "sec,i2c-burstmax", &pdata->i2c_burstmax)) {
		tsp_debug_err(true, &client->dev, "Failed to get irq_type property\n");
		return -EINVAL;
	}

	if (of_property_read_u32_array(np, "sec,max_coords", coords, 2)) {
		tsp_debug_err(true, &client->dev, "Failed to get max_coords property\n");
		return -EINVAL;
	}
	pdata->max_x = coords[0] - 1;
	pdata->max_y = coords[1] - 1;

	of_property_read_u32(np, "sec,grip_area", &pdata->grip_area);

	if (of_property_read_u32_array(np, "sec,num_lines", lines, 2)) {
		tsp_debug_info(true, &client->dev, "skipped to get num_lines property\n");
	} else {
		pdata->num_rx = lines[0];
		pdata->num_tx = lines[1];
		tsp_debug_info(true, &client->dev, "num_of[rx,tx]: [%d,%d]\n",
				pdata->num_rx, pdata->num_tx);
	}

	pdata->tspid = of_get_named_gpio(np, "sec,tsp-id", 0);
	pdata->tspid2 = of_get_named_gpio(np, "sec,tsp-id2", 0);

	if (gpio_is_valid(pdata->tspid) && gpio_is_valid(pdata->tspid2)) {
		if (gpio_request(pdata->tspid, "tspid") < 0)
			tsp_debug_err(true, &client->dev, "can not request tspid gpios\n");

		if (gpio_request(pdata->tspid2, "tspid2") < 0)
			tsp_debug_err(true, &client->dev, "can not request tspid2 gpios\n");

		/* tsp_id : 0 STM, 1 LSI
		 * tsp_id2 : 0 ALPS, 1 ILSIN(or DONGWOO) */
		if (gpio_get_value(pdata->tspid) == 0) {
			tsp_debug_err(true, &client->dev, "%s: is not sec_ts\n", __func__);
			return -ECONNREFUSED;
		}

		if (gpio_get_value(pdata->tspid)) {
			if (gpio_get_value(pdata->tspid2))
				id = 1;
			else
				id = 0;
		}
	}

	count = of_property_count_strings(np, "sec,firmware_name");
	if (count <= 0) {
		pdata->firmware_name = NULL;
	} else {
		if (count < (id + 1))
			id = 0;

		of_property_read_string_index(np, "sec,firmware_name", id, &pdata->firmware_name);
	}
	if (of_property_read_string(np, "sec,tsp-project", &pdata->project_name))
		tsp_debug_info(true, &client->dev, "skipped to get project_name property\n");
	if (of_property_read_string(np, "sec,tsp-model", &pdata->model_name))
		tsp_debug_info(true, &client->dev, "skipped to get model_name property\n");

	temp = get_lcd_attached("GET");
	if (temp == 0xFFFFFF) {
		tsp_debug_err(true, &client->dev, "%s: lcd is not attached\n", __func__);
		return -1;
	}

	pdata->panel_revision = ((temp >> 8) & 0xFF) >> 4;
	pdata->power = sec_ts_power_ctrl;

	if (of_property_read_u32(np, "sec,bringup", &pdata->bringup) < 0)
		pdata->bringup = 0;

	tsp_debug_err(true, &client->dev, "%s: i2c buffer limit: %d, lcd_id:%06X, bringup:%d, FW:%s(%d), id:%d,%d, grip:%d\n",
			__func__, pdata->i2c_burstmax, temp, pdata->bringup, pdata->firmware_name, count, pdata->tspid, pdata->tspid2, pdata->grip_area);

	return ret;
}

static int sec_ts_read_information(struct sec_ts_data *ts)
{
	unsigned char data[13] = { 0 };
	int ret;

	memset(data, 0x0, 3);
	ret = sec_ts_i2c_read(ts, SEC_TS_READ_DEVICE_ID, data, 3);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev,
					"%s: failed to read device id(%d)\n",
					__func__, ret);
		return ret;
	}

	tsp_debug_info(true, &ts->client->dev,
				"%s: %X, %X, %X\n",
				__func__, data[0], data[1], data[2]);

	memset(data, 0x0, 9);
	ret = sec_ts_i2c_read(ts, SEC_TS_READ_SUB_ID, data, 13);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev,
					"%s: failed to read sub id(%d)\n",
					__func__, ret);
		return ret;
	}

	tsp_debug_info(true, &ts->client->dev,
				"%s: AP/BL(%X), DEV1:%X, DEV2:%X, nTX:%X, nRX:%X, rY:%d, rX:%d\n",
				__func__, data[0], data[1], data[2], data[3], data[4],
				(data[5] << 8) | data[6], (data[7] << 8) | data[8]);

	/* Set X,Y Resolution from IC information. */
	if (((data[5] << 8) | data[6]) > 0)
		ts->plat_data->max_y = ((data[5] << 8) | data[6]) - 1;

	if (((data[7] << 8) | data[8]) > 0)
		ts->plat_data->max_x = ((data[7] << 8) | data[8]) - 1;

	ts->tx_count = data[3];
	ts->rx_count = data[4];

	data[0] = 0;
	ret = sec_ts_i2c_read(ts, SEC_TS_READ_BOOT_STATUS, data, 1);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev,
					"%s: failed to read sub id(%d)\n",
					__func__, ret);
		return ret;
	}

	tsp_debug_info(true, &ts->client->dev,
				"%s: STATUS : %X\n",
				__func__, data[0]);

	return ret;
}

#ifdef SEC_TS_SUPPORT_SPONGELIB
int sec_ts_set_custom_library(struct sec_ts_data *ts)
{
	u8 data[3] = { 0 };
	int ret;

	tsp_debug_err(true, &ts->client->dev, "%s: Sponge (%d)\n",
				__func__, ts->lowpower_mode);

	data[2] = ((ts->lowpower_mode) & 0xF);

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SPONGE_WRITE_PARAM, &data[0], 3);
	if (ret < 0)
		tsp_debug_err(true, &ts->client->dev, "%s: Failed to Sponge\n", __func__);

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SPONGE_NOTIFY_PACKET, NULL, 0);
	if (ret < 0)
		tsp_debug_err(true, &ts->client->dev, "%s: Failed to send NOTIFY SPONGE\n", __func__);

	return ret;
}

int sec_ts_check_custom_library(struct sec_ts_data *ts)
{
	u8 data[10] = { 0 };
	int ret;

	ret = ts->sec_ts_i2c_read(ts, SEC_TS_CMD_SPONGE_GET_INFO, &data[0], 10);

	tsp_debug_info(true, &ts->client->dev,
				"%s: (%d) %c%c%c%c, || %02X, %02X, %02X, %02X, || %02X, %02X\n",
				__func__, ret, data[0], data[1], data[2], data[3], data[4],
				data[5], data[6], data[7], data[8], data[9]);

	/* compare model name with device tree */
	ret = strncmp(data, ts->plat_data->model_name, 4);
	if (ret == 0)
		ts->use_sponge = true;
	else
		ts->use_sponge = false;

	tsp_debug_info(true, &ts->client->dev, "%s: use %s\n",
				__func__, ts->use_sponge ? "SPONGE" : "VENDOR");

	return ret;
}
#endif

static int sec_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct sec_ts_data *ts;
	struct sec_ts_plat_data *pdata;
	static char sec_ts_phys[64] = { 0 };
	int ret = 0;

	if (tsp_init_done) {
		pr_err("%s: tsp already init done\n", __func__);
		return -ENODEV;
	}

	tsp_debug_info(true, &client->dev, "%s\n", __func__);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		tsp_debug_err(true, &client->dev, "%s : EIO err!\n", __func__);
		return -EIO;
	}

	/* power enable */
	sec_ts_power_ctrl(client, true);

	/* parse dt */
	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
				sizeof(struct sec_ts_plat_data), GFP_KERNEL);

		if (!pdata) {
			tsp_debug_err(true, &client->dev, "Failed to allocate platform data\n");
			goto error_allocate_mem;
		}

		client->dev.platform_data = pdata;

		ret = sec_ts_parse_dt(client);
		if (ret) {
			tsp_debug_err(true, &client->dev, "Failed to parse dt\n");
			goto error_allocate_mem;
		}
	} else {
		pdata = client->dev.platform_data;
	}

	if (!pdata) {
		tsp_debug_err(true, &client->dev, "No platform data found\n");
		goto error_allocate_mem;
	}
	if (!pdata->power) {
		tsp_debug_err(true, &client->dev, "No power contorl found\n");
		goto error_allocate_mem;
	}

	pdata->pinctrl = devm_pinctrl_get(&client->dev);
	if (IS_ERR(pdata->pinctrl))
		tsp_debug_err(true, &client->dev, "could not get pinctrl\n");

	ts = kzalloc(sizeof(struct sec_ts_data), GFP_KERNEL);
	if (!ts) {
		tsp_debug_err(true, &client->dev, "%s: Failed to alloc mem for info\n", __func__);
		goto error_allocate_mem;
	}

	ts->client = client;
	ts->plat_data = pdata;
	ts->crc_addr = 0x0001FE00;
	ts->fw_addr = 0x00002000;
	ts->para_addr = 0x18000;
	ts->sec_ts_i2c_read = sec_ts_i2c_read;
	ts->sec_ts_i2c_write = sec_ts_i2c_write;
	ts->sec_ts_i2c_write_burst = sec_ts_i2c_write_burst;
	ts->sec_ts_i2c_read_bulk = sec_ts_i2c_read_bulk;
	ts->i2c_burstmax = pdata->i2c_burstmax;
#ifdef USE_RESET_DURING_POWER_ON
	INIT_DELAYED_WORK(&ts->reset_work, sec_ts_reset_work);
#endif
	INIT_DELAYED_WORK(&ts->work_read_nv, sec_ts_read_nv_work);

	i2c_set_clientdata(client, ts);

	if (gpio_is_valid(ts->plat_data->tspid) && gpio_is_valid(ts->plat_data->tspid2)) {
		ts->tspid_val = gpio_get_value(ts->plat_data->tspid);
		ts->tspid2_val = gpio_get_value(ts->plat_data->tspid2);
	} else {
		ts->tspid_val = 0;
		ts->tspid2_val = 0;
	}

	ts->input_dev = input_allocate_device();
	if (!ts->input_dev) {
		tsp_debug_err(true, &ts->client->dev, "%s: allocate device err!\n", __func__);
		ret = -ENOMEM;
		goto err_allocate_device;
	}

	ts->input_dev->name = "sec_touchscreen";
	snprintf(sec_ts_phys, sizeof(sec_ts_phys), "%s/input1",
			ts->input_dev->name);
	ts->input_dev->phys = sec_ts_phys;
	ts->input_dev->id.bustype = BUS_I2C;
	ts->input_dev->dev.parent = &client->dev;
	ts->touch_count = 0;
	ts->sec_ts_i2c_write = sec_ts_i2c_write;
	ts->sec_ts_i2c_read = sec_ts_i2c_read;

	mutex_init(&ts->lock);
	mutex_init(&ts->device_mutex);
	mutex_init(&ts->i2c_mutex);
	mutex_init(&ts->eventlock);

	wake_lock_init(&ts->wakelock, WAKE_LOCK_SUSPEND, "tsp_wakelock");
	init_completion(&ts->resume_done);

#ifdef USE_OPEN_CLOSE
	ts->input_dev->open = sec_ts_input_open;
	ts->input_dev->close = sec_ts_input_close;
#endif

	tsp_debug_err(true, &client->dev, "%s init resource\n", __func__);

	sec_ts_pinctrl_configure(ts, true);

	sec_ts_delay(70);
	ts->power_status = SEC_TS_STATE_POWER_ON;

	sec_ts_wait_for_ready(ts, SEC_TS_ACK_BOOT_COMPLETE);

	tsp_debug_err(true, &client->dev, "%s power enable\n", __func__);

#ifdef SEC_TS_FW_UPDATE_ON_PROBE
	ret = sec_ts_firmware_update_on_probe(ts);
	if (ret < 0)
		goto err_init;
#else
	tsp_debug_info(true, &ts->client->dev, "%s: fw update on probe disabled!\n", __func__);
#endif

	ret = sec_ts_read_information(ts);
	if (ret < 0)
		goto err_init;

	/* Sense_on */
	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SENSE_ON, NULL, 0);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "sec_ts_probe: fail to write Sense_on\n");
		goto err_init;
	}

	ts->pFrame = kzalloc(ts->tx_count * ts->rx_count * 2, GFP_KERNEL);
	if (!ts->pFrame) {
		tsp_debug_err(true, &ts->client->dev, "%s: allocate pFrame err!\n", __func__);
		ret = -ENOMEM;
		goto err_allocate_frame;
	}

#ifdef CONFIG_TOUCHSCREN_SEC_TS_GLOVEMODE
	input_set_capability(ts->input_dev, EV_SW, SW_GLOVE);
#endif
	set_bit(EV_SYN, ts->input_dev->evbit);
	set_bit(EV_KEY, ts->input_dev->evbit);
	set_bit(EV_ABS, ts->input_dev->evbit);
	set_bit(BTN_TOUCH, ts->input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, ts->input_dev->keybit);
	set_bit(KEY_BLACK_UI_GESTURE, ts->input_dev->keybit);
#ifdef SEC_TS_SUPPORT_TOUCH_KEY
	if (ts->plat_data->support_mskey) {
		int i;

		for (i = 0 ; i < ts->plat_data->num_touchkey ; i++)
			set_bit(ts->plat_data->touchkey[i].keycode, ts->input_dev->keybit);

		set_bit(EV_LED, ts->input_dev->evbit);
		set_bit(LED_MISC, ts->input_dev->ledbit);
	}
#endif
	set_bit(KEY_SIDE_GESTURE, ts->input_dev->keybit);
	set_bit(KEY_SIDE_GESTURE_RIGHT, ts->input_dev->keybit);
	set_bit(KEY_SIDE_GESTURE_LEFT, ts->input_dev->keybit);
#ifdef INPUT_PROP_DIRECT
	set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);
#endif

	ts->input_dev->evbit[0] = BIT_MASK(EV_ABS) | BIT_MASK(EV_KEY);
	set_bit(INPUT_PROP_DIRECT, ts->input_dev->propbit);

	input_mt_init_slots(ts->input_dev, MAX_SUPPORT_TOUCH_COUNT, INPUT_MT_DIRECT);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X, 0, ts->plat_data->max_x, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y, 0, ts->plat_data->max_y, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_PALM, 0, 1, 0, 0);
#ifdef CONFIG_SEC_FACTORY
	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);
#endif
#ifdef SEC_TS_SUPPORT_GRIP_EVENT
	input_set_abs_params(ts->input_dev, ABS_MT_GRIP, 0, 1, 0, 0);
#endif
	input_set_drvdata(ts->input_dev, ts);
	i2c_set_clientdata(client, ts);

	ret = input_register_device(ts->input_dev);
	if (ret) {
		tsp_debug_err(true, &ts->client->dev, "%s: Unable to register %s input device\n", __func__, ts->input_dev->name);
		goto err_input_register_device;
	}

	tsp_debug_info(true, &ts->client->dev, "sec_ts_probe request_irq = %d\n" , client->irq);

	ret = request_threaded_irq(client->irq, NULL, sec_ts_irq_thread,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT, SEC_TS_I2C_NAME, ts);
	if (ret < 0) {
		tsp_debug_err(true, &ts->client->dev, "sec_ts_probe: Unable to request threaded irq\n");
		goto err_irq;
	}

#ifdef SEC_TS_SUPPORT_TA_MODE
	ts->callbacks.inform_charger = sec_ts_ta_cb;
	if (ts->plat_data->register_cb)
		ts->plat_data->register_cb(&ts->callbacks);
#endif

	sec_ts_raw_device_init(ts);
	sec_ts_fn_init(ts);

#ifdef CONFIG_SECURE_TOUCH
	if (sysfs_create_group(&ts->input_dev->dev.kobj, &secure_attr_group) < 0)
		tsp_debug_err(true, &ts->client->dev, "%s: do not make secure group\n", __func__);
	else
		secure_touch_init(ts);
#endif

	device_init_wakeup(&client->dev, true);

	schedule_delayed_work(&ts->work_read_nv, msecs_to_jiffies(100));

#ifdef SEC_TS_SUPPORT_SPONGELIB
	sec_ts_check_custom_library(ts);
#endif

#if defined(CONFIG_TOUCHSCREEN_DUMP_MODE)
		dump_callbacks.inform_dump = dump_tsp_log;
		INIT_DELAYED_WORK(&ts->ghost_check, sec_ts_check_rawdata);
		p_ghost_check = &ts->ghost_check;
#endif

	tsp_init_done = true;

	return 0;

err_irq:
	input_unregister_device(ts->input_dev);
	ts->input_dev = NULL;
err_input_register_device:
	if (ts->input_dev)
		input_free_device(ts->input_dev);
	kfree(ts->pFrame);
err_allocate_frame:
err_init:
	wake_lock_destroy(&ts->wakelock);
err_allocate_device:
	kfree(ts);

error_allocate_mem:
	if (gpio_is_valid(pdata->gpio))
		gpio_free(pdata->gpio);
	if (gpio_is_valid(pdata->tspid))
		gpio_free(pdata->tspid);
	if (gpio_is_valid(pdata->tspid2))
		gpio_free(pdata->tspid2);

	sec_ts_power_ctrl(client, false);
	if (ret == -ECONNREFUSED)
		sec_ts_delay(100);
	ret = -ENODEV;
#ifdef CONFIG_TOUCHSCREEN_DUMP_MODE
	p_ghost_check = NULL;
#endif

	return ret;
}

void sec_ts_unlocked_release_all_finger(struct sec_ts_data *ts)
{
	int i;

	for (i = 0; i < MAX_SUPPORT_TOUCH_COUNT; i++) {
		input_mt_slot(ts->input_dev, i);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);

		if ((ts->coord[i].action == SEC_TS_COORDINATE_ACTION_PRESS) ||
 			(ts->coord[i].action == SEC_TS_COORDINATE_ACTION_MOVE)) {

 			ts->coord[i].action = SEC_TS_COORDINATE_ACTION_RELEASE;
			tsp_debug_info(true, &ts->client->dev,
					"%s: [RA] tID:%d mc: %d tc:%d, v:%02X%02X, cal:%X(%X|%X), id(%d,%d), p:%d\n",
					__func__, i, ts->coord[i].mcount, ts->touch_count,
					ts->plat_data->img_version_of_ic[2],
					ts->plat_data->img_version_of_ic[3],
					ts->cal_status, ts->nv, ts->cal_count, ts->tspid_val,
					ts->tspid2_val, ts->coord[i].palm_count);
 		}

		ts->coord[i].mcount = 0;
		ts->coord[i].palm_count = 0;

	}

	input_mt_slot(ts->input_dev, 0);

	input_report_key(ts->input_dev, BTN_TOUCH, false);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, false);
	input_report_switch(ts->input_dev, SW_GLOVE, false);
	ts->touchkey_glove_mode_status = false;
	ts->touch_count = 0;

	input_report_key(ts->input_dev, KEY_SIDE_GESTURE_LEFT, 0);
	input_report_key(ts->input_dev, KEY_SIDE_GESTURE_RIGHT, 0);

	input_sync(ts->input_dev);

}

void sec_ts_locked_release_all_finger(struct sec_ts_data *ts)
{
	int i;

	mutex_lock(&ts->eventlock);

	for (i = 0; i < MAX_SUPPORT_TOUCH_COUNT; i++) {
		input_mt_slot(ts->input_dev, i);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);

		if ((ts->coord[i].action == SEC_TS_COORDINATE_ACTION_PRESS) ||
 			(ts->coord[i].action == SEC_TS_COORDINATE_ACTION_MOVE)) {

 			ts->coord[i].action = SEC_TS_COORDINATE_ACTION_RELEASE;
			tsp_debug_info(true, &ts->client->dev,
					"%s: [RA] tID:%d mc: %d tc:%d, v:%02X%02X, cal:%X(%X|%X), id(%d,%d), p:%d\n",
					__func__, i, ts->coord[i].mcount, ts->touch_count,
					ts->plat_data->img_version_of_ic[2],
					ts->plat_data->img_version_of_ic[3],
					ts->cal_status, ts->nv, ts->cal_count, ts->tspid_val,
					ts->tspid2_val, ts->coord[i].palm_count);
 		}

		ts->coord[i].mcount = 0;
		ts->coord[i].palm_count = 0;

	}

	input_mt_slot(ts->input_dev, 0);

	input_report_key(ts->input_dev, BTN_TOUCH, false);
	input_report_key(ts->input_dev, BTN_TOOL_FINGER, false);
	input_report_switch(ts->input_dev, SW_GLOVE, false);
	ts->touchkey_glove_mode_status = false;
	ts->touch_count = 0;

	input_report_key(ts->input_dev, KEY_SIDE_GESTURE_LEFT, 0);
	input_report_key(ts->input_dev, KEY_SIDE_GESTURE_RIGHT, 0);

	input_sync(ts->input_dev);

	mutex_unlock(&ts->eventlock);

}

#ifdef USE_RESET_DURING_POWER_ON
static void sec_ts_reset_work(struct work_struct *work)
{
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
							reset_work.work);

	u8 temp_lpm = 0;
	u8 temp_status = 0;

	if (!tsp_init_done) {
		tsp_debug_err(true, &ts->client->dev, "%s: is not done, return\n", __func__);
		return;
	}

	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);

	temp_lpm = ts->lowpower_mode;
	temp_status = ts->power_status;

	ts->lowpower_mode = 0;
	
	sec_ts_stop_device(ts);

	sec_ts_delay(30);

	sec_ts_start_device(ts);

	ts->lowpower_mode = temp_lpm;
	if ((ts->lowpower_mode) && (temp_status < SEC_TS_STATE_POWER_ON))
		sec_ts_input_close(ts->input_dev);

}
#endif

static void sec_ts_read_nv_work(struct work_struct *work)
{
	struct sec_ts_data *ts = container_of(work, struct sec_ts_data,
							work_read_nv.work);

	ts->nv = get_tsp_nvm_data(ts, SEC_TS_NVM_OFFSET_FAC_RESULT);
	ts->cal_count = get_tsp_nvm_data(ts, SEC_TS_NVM_OFFSET_CAL_COUNT);

	tsp_debug_info(true, &ts->client->dev, "%s: fac_nv:%02X, cal_nv:%02X\n", __func__, ts->nv, ts->cal_count);
}

static int sec_ts_set_lowpowermode(struct sec_ts_data *ts, u8 mode)
{
	int ret;
	u8 data;

	tsp_debug_err(true, &ts->client->dev, "%s: %s(%X)\n", __func__,
			mode == TO_LOWPOWER_MODE ? "ENTER" : "EXIT", ts->lowpower_mode);

	if (mode) {
		if (ts->use_sponge)
			sec_ts_set_custom_library(ts);

		data = ((ts->lowpower_mode >> 4) & 0xF);
		ret = sec_ts_i2c_write(ts, SEC_TS_CMD_GESTURE_MODE, &data, 1);
		if (ret < 0)
			tsp_debug_err(true, &ts->client->dev, "%s: Failed to set\n", __func__);
	}

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_POWER_MODE, &mode, 1);
	if (ret < 0)
		tsp_debug_err(true, &ts->client->dev,
				"%s: failed\n", __func__);

	sec_ts_locked_release_all_finger(ts);

	if (device_may_wakeup(&ts->client->dev)) {
		if (mode)
			enable_irq_wake(ts->client->irq);
		else
			disable_irq_wake(ts->client->irq);
	}

	ts->lowpower_status = mode;

	return ret;
}

#ifdef USE_OPEN_CLOSE
static int sec_ts_input_open(struct input_dev *dev)
{
	struct sec_ts_data *ts = input_get_drvdata(dev);
	int ret;

	ts->input_closed = false;

	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);
#ifdef CONFIG_SECURE_TOUCH
	secure_touch_stop(ts, 0);
#endif

	if (ts->lowpower_status) {
#ifdef USE_RESET_EXIT_LPM
		schedule_delayed_work(&ts->reset_work, msecs_to_jiffies(TOUCH_RESET_DWORK_TIME));
#else
		sec_ts_set_lowpowermode(ts, TO_TOUCH_MODE);
#endif
	} else {
		ret = sec_ts_start_device(ts);
		if (ret < 0)
			tsp_debug_err(true, &ts->client->dev, "%s: Failed to start device\n", __func__);
	}

	return 0;
}

static void sec_ts_input_close(struct input_dev *dev)
{
	struct sec_ts_data *ts = input_get_drvdata(dev);

	ts->input_closed = true;

	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);
#ifdef CONFIG_SECURE_TOUCH
	secure_touch_stop(ts, 1);
#endif
#ifdef USE_RESET_DURING_POWER_ON
	cancel_delayed_work(&ts->reset_work);
#endif

	if (ts->lowpower_mode) {
		sec_ts_set_lowpowermode(ts, TO_LOWPOWER_MODE);

		ts->power_status = SEC_TS_STATE_LPM_RESUME;
	} else {
		sec_ts_stop_device(ts);
	}

}
#endif

static int sec_ts_remove(struct i2c_client *client)
{
	struct sec_ts_data *ts = i2c_get_clientdata(client);

	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);

#ifdef USE_RESET_DURING_POWER_ON
	cancel_delayed_work(&ts->reset_work);
#endif
	sec_ts_fn_remove(ts);

	free_irq(client->irq, ts);

#ifdef CONFIG_TOUCHSCREEN_DUMP_MODE
	p_ghost_check = NULL;
#endif
	device_init_wakeup(&client->dev, false);
	wake_lock_destroy(&ts->wakelock);

	input_mt_destroy_slots(ts->input_dev);
	input_unregister_device(ts->input_dev);

	ts->input_dev = NULL;
	ts->plat_data->power(ts->client, false);

	kfree(ts);
	return 0;
}

static void sec_ts_shutdown(struct i2c_client *client)
{
	struct sec_ts_data *ts = i2c_get_clientdata(client);

	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);

	sec_ts_remove(client);
}

static int sec_ts_stop_device(struct sec_ts_data *ts)
{
	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);

	mutex_lock(&ts->device_mutex);

	if (ts->power_status == SEC_TS_STATE_POWER_OFF) {
		tsp_debug_err(true, &ts->client->dev, "%s: already power off\n", __func__);
		goto out;
	}

	ts->power_status = SEC_TS_STATE_POWER_OFF;

	disable_irq(ts->client->irq);
	sec_ts_locked_release_all_finger(ts);

	ts->plat_data->power(ts->client, false);

	if (ts->plat_data->enable_sync)
		ts->plat_data->enable_sync(false);

	sec_ts_pinctrl_configure(ts, false);

out:
	mutex_unlock(&ts->device_mutex);
	return 0;
}

static int sec_ts_start_device(struct sec_ts_data *ts)
{
	int ret;

	tsp_debug_info(true, &ts->client->dev, "%s\n", __func__);

	sec_ts_pinctrl_configure(ts, true);

	mutex_lock(&ts->device_mutex);

	if (ts->power_status == SEC_TS_STATE_POWER_ON) {
		tsp_debug_err(true, &ts->client->dev, "%s: already power on\n", __func__);
		goto out;
	}

	sec_ts_locked_release_all_finger(ts);

	ts->plat_data->power(ts->client, true);
	sec_ts_delay(70);
	ts->power_status = SEC_TS_STATE_POWER_ON;
	sec_ts_wait_for_ready(ts, SEC_TS_ACK_BOOT_COMPLETE);

	if (ts->plat_data->enable_sync)
		ts->plat_data->enable_sync(true);

#ifdef SEC_TS_SUPPORT_TA_MODE
	if (ts->ta_status)
		sec_ts_charger_config(ts, ts->ta_status);
#endif

	if (ts->flip_enable) {
		ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_COVERTYPE, &ts->cover_cmd, 1);

		ts->touch_functions = ts->touch_functions | SEC_TS_BIT_SETFUNC_COVER | SEC_TS_BIT_SETFUNC_MUTUAL;
		tsp_debug_info(true, &ts->client->dev,
				"%s: cover cmd write type:%d, mode:%x, ret:%d", __func__, ts->touch_functions, ts->cover_cmd, ret);
	} else {
		ts->touch_functions = ((ts->touch_functions & (~SEC_TS_BIT_SETFUNC_COVER)) | SEC_TS_BIT_SETFUNC_MUTUAL);
	}

	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SET_TOUCHFUNCTION, &ts->touch_functions, 1);
	if (ret < 0)
		tsp_debug_err(true, &ts->client->dev,
				"%s: Failed to send glove_mode command", __func__);

	/* Sense_on */
	ret = sec_ts_i2c_write(ts, SEC_TS_CMD_SENSE_ON, NULL, 0);
	if (ret < 0)
		tsp_debug_err(true, &ts->client->dev, "sec_ts_probe: fail to write Sense_on\n");

	enable_irq(ts->client->irq);

out:
	mutex_unlock(&ts->device_mutex);
	return 0;
}

#ifdef CONFIG_PM
static int sec_ts_pm_suspend(struct device *dev)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);
/*
	mutex_lock(&ts->input_dev->mutex);
	if (ts->input_dev->users)
		sec_ts_stop_device(ts);
	mutex_unlock(&ts->input_dev->mutex);
*/

	if (ts->lowpower_mode) {
		ts->power_status = SEC_TS_STATE_LPM_SUSPEND;
		reinit_completion(&ts->resume_done);
	}

	return 0;
}

static int sec_ts_pm_resume(struct device *dev)
{
	struct sec_ts_data *ts = dev_get_drvdata(dev);
/*
	mutex_lock(&ts->input_dev->mutex);
	if (ts->input_dev->users)
		sec_ts_start_device(ts);
	mutex_unlock(&ts->input_dev->mutex);
*/

	if (ts->lowpower_mode) {
		ts->power_status = SEC_TS_STATE_LPM_RESUME;
		complete_all(&ts->resume_done);
	}

	return 0;
}
#endif

static const struct i2c_device_id sec_ts_id[] = {
	{ SEC_TS_I2C_NAME, 0 },
	{ },
};

#ifdef CONFIG_PM
static const struct dev_pm_ops sec_ts_dev_pm_ops = {
	.suspend = sec_ts_pm_suspend,
	.resume = sec_ts_pm_resume,
};
#endif

#ifdef CONFIG_OF
static struct of_device_id sec_ts_match_table[] = {
	{ .compatible = "sec,sec_ts",},
	{ },
};
#else
#define sec_ts_match_table NULL
#endif

static struct i2c_driver sec_ts_driver = {
	.probe		= sec_ts_probe,
	.remove		= sec_ts_remove,
	.shutdown	= sec_ts_shutdown,
	.id_table	= sec_ts_id,
	.driver = {
		.owner    = THIS_MODULE,
		.name	= SEC_TS_I2C_NAME,
#ifdef CONFIG_OF
		.of_match_table = sec_ts_match_table,
#endif
#ifdef CONFIG_PM
		.pm = &sec_ts_dev_pm_ops,
#endif
	},
};

static int __init sec_ts_init(void)
{
	pr_err("%s\n", __func__);

#ifdef CONFIG_SAMSUNG_LPM_MODE
	if (poweroff_charging) {
		pr_err("%s : Do not load driver due to : lpm %d\n",
				__func__, poweroff_charging);
		return -ENODEV;
	}
#endif
	return i2c_add_driver(&sec_ts_driver);
}

static void __exit sec_ts_exit(void)
{
	i2c_del_driver(&sec_ts_driver);
}

MODULE_AUTHOR("Hyobae, Ahn<hyobae.ahn@samsung.com>");
MODULE_DESCRIPTION("Samsung Electronics TouchScreen driver");
MODULE_LICENSE("GPL");

module_init(sec_ts_init);
module_exit(sec_ts_exit);
