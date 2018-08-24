/*
* Copyright (C) 2014 MediaTek Inc.
*
* This program is free software: you can redistribute it and/or modify it under the terms of the
* GNU General Public License version 2 as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
* without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along with this program.
* If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/acpi.h>
#include <linux/thermal.h>
#include <linux/printk.h>
#include <linux/platform_device.h>
#include <mt-plat/aee.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include <asm/uaccess.h>

#include <mach/system.h>
#include <mt-plat/sync_write.h>
#include "mt-plat/mtk_thermal_monitor.h"
#include <mach/mt_thermal.h>


extern struct proc_dir_entry *mtk_thermal_get_proc_drv_therm_dir_entry(void);
#define MTK_TZ_COOLER_MAX 10

/* Zone */
static struct thermal_zone_device *thz_dev;
static unsigned int interval; /* seconds, 0 : no auto polling */
static unsigned int trip_temp[MTK_TZ_COOLER_MAX] = {120000, 110000, 100000, 90000, 80000, 70000, 65000, 60000, 55000, 50000};
static int kernelmode;
static int g_THERMAL_TRIP[MTK_TZ_COOLER_MAX] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int num_trip = 0;
static char g_bind[MTK_TZ_COOLER_MAX][THERMAL_NAME_LENGTH] = {{0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0}, {0} };
#define MTKTSBATTERY_TEMP_CRIT 60000 /* 60.000 degree Celsius */

/* Cooler */
static unsigned int cl_dev_sysrst_state;
static struct thermal_cooling_device *cl_dev_sysrst;

/* Polling */
static int polling_trip_temp1 = 40000;
static int polling_trip_temp2 = 20000;
static int polling_factor1 = 5;
static int polling_factor2 = 10;

static kuid_t uid = KUIDT_INIT(0);
static kgid_t gid = KGIDT_INIT(1000);

/* Logging */
static int mtktsbattery_debug_log;
#define mtktsbattery_dprintk(fmt, args...) \
	do { \
		if (mtktsbattery_debug_log) { \
			pr_debug("Power/Battery_Thermal " fmt, ##args); \
		} \
	} while (0)

static int mtktsbattery_register_thermal(void);
static void mtktsbattery_unregister_thermal(void);
static int mtkts_match(struct thermal_cooling_device *cdev, char bind[MTK_TZ_COOLER_MAX][THERMAL_NAME_LENGTH]);
extern int read_tbat_value(void);

static int get_hw_battery_temp(void)
{
	int ret = 0;
#if defined(CONFIG_POWER_EXT)
	/* EVB */
	ret = -1270;
#else
	/* Phone */
	ret = read_tbat_value();
#endif
	return ret;
}

static int mtktsbattery_get_temp(struct thermal_zone_device *thermal,
				   int *t)
{
	*t = (unsigned long)get_hw_battery_temp() * 1000;

	if ((int) *t >= polling_trip_temp1)
		thermal->polling_delay = interval;
	else if ((int) *t < polling_trip_temp2)
		thermal->polling_delay = interval * polling_factor2;
	else
		thermal->polling_delay = interval * polling_factor1;

	mtktsbattery_dprintk("[mtktsbattery_get_hw_temp] T_Battery, %d\n", *t);
	return 0;
}

static int mtkts_match(struct thermal_cooling_device *cdev, char bind[MTK_TZ_COOLER_MAX][THERMAL_NAME_LENGTH])
{
	int i;

	for (i = 0; i < MTK_TZ_COOLER_MAX; i++)
	{
		if (!strcmp(cdev->type, bind[i]))
		{
			return i;
		}
	}
	return i;
}


static int mtktsbattery_bind(struct thermal_zone_device *thermal,
			struct thermal_cooling_device *cdev)
{
	int table_val = 0;
	table_val = mtkts_match(cdev, g_bind);
	if (table_val >= MTK_TZ_COOLER_MAX)
	{
		return 0;
	}
	else
	{
		mtktsbattery_dprintk("[mtktsbattery_bind] %s\n", cdev->type);
		if (mtk_thermal_zone_bind_cooling_device(thermal, table_val, cdev)) {
			mtktsbattery_dprintk("[mtktsbattery_bind] error binding cooling dev\n");
			return -EINVAL;
		} else {
			mtktsbattery_dprintk("[mtktsbattery_bind] binding OK, %d\n", table_val);
		}
	}
	return 0;
}

static int mtktsbattery_unbind(struct thermal_zone_device *thermal,
			  struct thermal_cooling_device *cdev)
{
	int table_val = 0;

	table_val = mtkts_match(cdev, g_bind);
	if (table_val >= MTK_TZ_COOLER_MAX)
	{
		return 0;
	}
	else
	{
		mtktsbattery_dprintk("[mtktsbattery_unbind] %s\n", cdev->type);
		if (thermal_zone_unbind_cooling_device(thermal, table_val, cdev)) {
			mtktsbattery_dprintk("[mtktsbattery_unbind] error unbinding cooling dev\n");
			return -EINVAL;
		} else {
			mtktsbattery_dprintk("[mtktsbattery_unbind] unbinding OK, %d\n", table_val);
		}
	}
	return 0;
}

static int mtktsbattery_get_mode(struct thermal_zone_device *thermal,
				enum thermal_device_mode *mode)
{
	*mode = (kernelmode) ? THERMAL_DEVICE_ENABLED
				 : THERMAL_DEVICE_DISABLED;
	return 0;
}

static int mtktsbattery_set_mode(struct thermal_zone_device *thermal,
				enum thermal_device_mode mode)
{
	kernelmode = mode;
	return 0;
}

static int mtktsbattery_get_trip_type(struct thermal_zone_device *thermal, int trip,
				 enum thermal_trip_type *type)
{
	*type = g_THERMAL_TRIP[trip];
	return 0;
}

static int mtktsbattery_get_trip_temp(struct thermal_zone_device *thermal, int trip,
				 int *temp)
{
	*temp = trip_temp[trip];
	return 0;
}

static int mtktsbattery_get_crit_temp(struct thermal_zone_device *thermal,
				 int *temperature)
{
	*temperature = MTKTSBATTERY_TEMP_CRIT;
	return 0;
}

/* bind callback functions to thermalzone */
static struct thermal_zone_device_ops mtktsbattery_dev_ops = {
	.bind = mtktsbattery_bind,
	.unbind = mtktsbattery_unbind,
	.get_temp = mtktsbattery_get_temp,
	.get_mode = mtktsbattery_get_mode,
	.set_mode = mtktsbattery_set_mode,
	.get_trip_type = mtktsbattery_get_trip_type,
	.get_trip_temp = mtktsbattery_get_trip_temp,
	.get_crit_temp = mtktsbattery_get_crit_temp,
};


static int sysrst_get_max_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	*state = 1;
	return 0;
}
static int sysrst_get_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long *state)
{
	*state = cl_dev_sysrst_state;
	return 0;
}
static int sysrst_set_cur_state(struct thermal_cooling_device *cdev,
				 unsigned long state)
{
	cl_dev_sysrst_state = state;
	if (cl_dev_sysrst_state == 1)
	{
		pr_debug("Power/battery_Thermal: reset, reset, reset!!!");
/* pr_debug("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@"); */
/* pr_debug("*****************************************"); */
/* pr_debug("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@"); */

		BUG();
		/* arch_reset(0,NULL); */
	}
	return 0;
}

static struct thermal_cooling_device_ops mtktsbattery_cooling_sysrst_ops = {
	.get_max_state = sysrst_get_max_state,
	.get_cur_state = sysrst_get_cur_state,
	.set_cur_state = sysrst_set_cur_state,
};


static int mtktsbattery_read(struct seq_file *m, void *v)
{

	seq_printf(m, "[mtktsbattery_read]\n\
	[trip_temp] = %d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n\
	[trip_type] = %d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n\
	[cool_bind] = %s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n\
	time_ms=%d\n",
		trip_temp[0], trip_temp[1], trip_temp[2], trip_temp[3], trip_temp[4],
		trip_temp[5], trip_temp[6], trip_temp[7], trip_temp[8], trip_temp[9],
		g_THERMAL_TRIP[0], g_THERMAL_TRIP[1], g_THERMAL_TRIP[2], g_THERMAL_TRIP[3], g_THERMAL_TRIP[4],
		g_THERMAL_TRIP[5], g_THERMAL_TRIP[6], g_THERMAL_TRIP[7], g_THERMAL_TRIP[8], g_THERMAL_TRIP[9],
		g_bind[0], g_bind[1], g_bind[2], g_bind[3], g_bind[4], g_bind[5], g_bind[6], g_bind[7], g_bind[8], g_bind[9],
		interval);

	return 0;

}



static ssize_t mtktsbattery_write(struct file *file, const char __user *buffer, size_t count,
				  loff_t *data)
{
	int len = 0, time_msec = 0;
	int trip[MTK_TZ_COOLER_MAX] = {0};
	int t_type[MTK_TZ_COOLER_MAX] = {0};
	int i;
	char bind[MTK_TZ_COOLER_MAX][THERMAL_NAME_LENGTH];
	char desc[512];


	len = (count < (sizeof(desc) - 1)) ? count : (sizeof(desc) - 1);
	if (copy_from_user(desc, buffer, len))
	{
		return 0;
	}
	desc[len] = '\0';

	if (sscanf(desc, "%d %d %d %19s %d %d %19s %d %d %19s %d %d %19s %d %d %19s %d %d %19s %d %d %19s %d %d %19s %d %d %19s %d %d %19s %d",
				&num_trip,
				&trip[0], &t_type[0], bind[0], &trip[1], &t_type[1], bind[1],
				&trip[2], &t_type[2], bind[2], &trip[3], &t_type[3], bind[3],
				&trip[4], &t_type[4], bind[4], &trip[5], &t_type[5], bind[5],
				&trip[6], &t_type[6], bind[6], &trip[7], &t_type[7], bind[7],
				&trip[8], &t_type[8], bind[8], &trip[9], &t_type[9], bind[9],
				&time_msec) == 32)
	{
		mtktsbattery_dprintk("[mtktsbattery_write] unregister_thermal\n");
		mtktsbattery_unregister_thermal();

		for (i = 0; i < MTK_TZ_COOLER_MAX; i++)
		{
			g_THERMAL_TRIP[i] = t_type[i];
			memcpy(g_bind[i], bind[i], THERMAL_NAME_LENGTH);
			trip_temp[i] = trip[i];
		}
		interval = time_msec;

		mtktsbattery_dprintk("[mtktsbattery_write] [trip_type]=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
				g_THERMAL_TRIP[0], g_THERMAL_TRIP[1], g_THERMAL_TRIP[2], g_THERMAL_TRIP[3], g_THERMAL_TRIP[4],
				g_THERMAL_TRIP[5], g_THERMAL_TRIP[6], g_THERMAL_TRIP[7], g_THERMAL_TRIP[8], g_THERMAL_TRIP[9]);

		mtktsbattery_dprintk("[mtktsbattery_write] [cool_bind]=%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
				g_bind[0], g_bind[1], g_bind[2], g_bind[3], g_bind[4], g_bind[5], g_bind[6], g_bind[7], g_bind[8], g_bind[9]);

		mtktsbattery_dprintk("[mtktsbattery_write] [trip_temp]==%d,%d,%d,%d,%d,%d,%d,%d,%d,%d, [time_ms]=%d\n",
				trip_temp[0], trip_temp[1], trip_temp[2], trip_temp[3], trip_temp[4],
				trip_temp[5], trip_temp[6], trip_temp[7], trip_temp[8], trip_temp[9], interval);

		mtktsbattery_dprintk("[mtktsbattery_write] register_thermal\n");
		mtktsbattery_register_thermal();

		return count;
	}
	else
	{
		mtktsbattery_dprintk("[mtktsbattery_write] bad argument\n");
	}

	return -EINVAL;
}


static int  mtktsbattery_register_cooler(void)
{
	/* cooling devices */
	cl_dev_sysrst = mtk_thermal_cooling_device_register("mtktsbattery-sysrst", NULL,
		&mtktsbattery_cooling_sysrst_ops);
	return 0;
}

static int mtktsbattery_register_thermal(void)
{
	mtktsbattery_dprintk("[mtktsbattery_register_thermal]\n");

	/* trips : trip 0~1 */
	thz_dev = mtk_thermal_zone_device_register("mtktsbattery", num_trip, NULL,
		&mtktsbattery_dev_ops, 0, 0, 0, interval);

	return 0;
}

static void mtktsbattery_unregister_cooler(void)
{
	if (cl_dev_sysrst) {
		mtk_thermal_cooling_device_unregister(cl_dev_sysrst);
		cl_dev_sysrst = NULL;
	}
}

static void mtktsbattery_unregister_thermal(void)
{
	mtktsbattery_dprintk("[mtktsbattery_unregister_thermal]\n");

	if (thz_dev) {
		mtk_thermal_zone_device_unregister(thz_dev);
		thz_dev = NULL;
	}
}

static int mtkts_battery_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtktsbattery_read, NULL);
}

static const struct file_operations mtkts_battery_fops = {
	.owner = THIS_MODULE,
	.open = mtkts_battery_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.write = mtktsbattery_write,
	.release = single_release,
};



static int __init mtktsbattery_init(void)
{
	int err = 0;
	struct proc_dir_entry *entry = NULL;
	struct proc_dir_entry *mtktsbattery_dir = NULL;

	mtktsbattery_dprintk("[mtktsbattery_init]\n");

	err = mtktsbattery_register_cooler();
	if (err)
		return err;

	err = mtktsbattery_register_thermal();
	if (err)
		goto err_unreg;

	mtktsbattery_dir = mtk_thermal_get_proc_drv_therm_dir_entry();
	if (!mtktsbattery_dir) {
		mtktsbattery_dprintk("%s mkdir /proc/driver/thermal failed\n", __func__);
	}

	entry = proc_create("tzbattery", S_IRUGO | S_IWUSR | S_IWGRP, mtktsbattery_dir, &mtkts_battery_fops);
	if (entry)
		proc_set_user(entry, uid, gid);

	return 0;

err_unreg:
	mtktsbattery_unregister_cooler();
	return err;
}

static void __exit mtktsbattery_exit(void)
{
	mtktsbattery_dprintk("[mtktsbattery_exit]\n");
	mtktsbattery_unregister_thermal();
	mtktsbattery_unregister_cooler();
}
module_init(mtktsbattery_init);
module_exit(mtktsbattery_exit);