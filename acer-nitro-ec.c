// SPDX-License-Identifier: GPL-2.0
/*
 * Acer Nitro EC fan control driver
 *
 * Exposes CPU/GPU fan speed control and temperature readings via the
 * standard Linux hwmon interface for Acer Nitro AN515/AN517 laptops.
 *
 * Supported models:
 *   AN515-44, AN515-46, AN515-54, AN515-56, AN515-57, AN515-58, AN517-55
 *
 * Once loaded, the following sysfs entries become available under
 * /sys/class/hwmon/hwmonX/:
 *
 *   fan1_input      - CPU fan speed (RPM)
 *   fan2_input      - GPU fan speed (RPM)
 *   pwm1            - CPU fan duty cycle (0-255)
 *   pwm1_enable     - CPU fan mode: 0=turbo, 1=manual, 2=auto
 *   pwm2            - GPU fan duty cycle (0-255)
 *   pwm2_enable     - GPU fan mode: 0=turbo, 1=manual, 2=auto
 *   temp1_input     - CPU temperature (millidegrees Celsius)
 *   temp2_input     - GPU temperature (millidegrees Celsius)
 *   temp3_input     - System temperature (millidegrees Celsius)
 *
 * Logging:
 *   - Load with debug=1 for verbose output:
 *       sudo insmod acer-nitro-ec.ko debug=1
 *   - Or enable dynamic_debug at runtime (no reload needed):
 *       echo "module acer_nitro_ec +p" > /sys/kernel/debug/dynamic_debug/control
 *   - Watch logs:
 *       sudo dmesg -w | grep acer-nitro-ec
 */

#include <asm-generic/errno.h>
#define DRIVER_NAME "acer-nitro-ec"
#define pr_fmt(fmt) DRIVER_NAME ": " fmt

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/bitfield.h>
#include <linux/bitops.hi>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/wmi.h>

/* When debug=1, emit dev_dbg messages as dev_info so they appear in dmesg
 * without needing to change the kernel log level or dynamic_debug config.
 */
static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Enable verbose logging (default: false). "
		 "Can also use: echo 'module acer_nitro_ec +p' > "
		 "/sys/kernel/debug/dynamic_debug/control");

#define nitro_dbg(dev, fmt, ...) do {				\
	if (debug)						\
		dev_info(dev, fmt, ##__VA_ARGS__);		\
	else							\
		dev_dbg(dev, fmt, ##__VA_ARGS__);		\
} while (0)

/* ------------------------------------------------------------------ */
/* ACER-WMI Interface                                                  */
/* ------------------------------------------------------------------ */

/*
 *GUID for the Acer WMI device that exposes sensor readout
 * and fan-control method on Nitro and Predator laptops. Sma GUID used by the
 * acer-wmi driver's interface helpers.
 */

#define WMID_GUID4         "7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56"

#define ACER_WMID_GET_GAMING_SYS_INFO_METHODID 5
#define ACER_WMID_SET_GAMING_FAN_BEHAVIOR_METHODID 14
#define ACER_WMID_SET_GAMING_FAN_SPEED_METHODID 16

#define ACER_WMID_CMD_GET_SUPPORTED_SENSORS 0x0000
#define ACER_WMID_CMD_GET_SENSOR_READING 0x0001

enum nitro_sensor_id {
	NITRO_SENSOR_CPU_TEMP = 0X01,
	NITRO_SENSOR_CPU_FAN_SPEED = 0X02,
	NITRO_SENSOR_SYS_TEMP = 0X03,
	NITRO_SENSOR_GPU_FAN_SPEED = 0X06,
	NITRO_SENSOR_GPU_TEMP = 0X0A,
};

#define NITRO_RETURN_STATUS_MASK  GENMASK_ULL(7, 0)
#define NITRO_SENSOR_INDEX_MASK  GENMASK_ULL(15, 8)
#define NITRO_SENSOR_READING_MASK  GENMASK_ULL(23, 8)
#define NITRO_SUPPORTED_SENSORS_MASK  GENMASK_ULL(39, 24)


/*
 * Fan-behavior payload for SET_FAMING_FAN_BEHAVIOR
 *
 *These are opaque vendor magic number (reverse-engineered from the
  Windows utilitys WMI traffic, not ducmented by acer). They are named
  here for readbility but the values themselves are not something this driver can validate independently
 *
 * */

#define FAN_BEHAVIOR_MAX_BOTH 	0x820009ULL /* turbo: both fans full speed */
#define FAN_BEHAVIOR_AUTO_BOTH 	0x410009ULL /* auto: firmware manages both */
#define FAN_BEHAVIOR_CUSTOM_GPU_ONLY_A 	0x010001ULL /* enter cutom mode, GPU driven */
#define FAN_BEHAVIOR_CUSTOM_GPU_ONLY_B 	0xC00008ULL
#define FAN_BEHAVIOR_CUSTOM_CPU_ONLY_A 	0x400008ULL /* enter custom mode, CPU driven */
#define FAN_BEHAVIOR_CUSTOM_CPU_ONLY_B 0x030001ULL
#define FAN_BEHAVIOR_CUSTOM_MIXED 	0xC30009ULL /* both fans independently set*/

#define FAN_INDEX_CPU 1
#define FAN_INDEX_GPU 4

#define NITRO_MODE_TURBO 0
#define NITRO_MODE_MANUAL 1
#define NITRO_MODE_AUTO 2


/* ------------------------------------------------------------------ */
/* Per-device data                                                      */
/* ------------------------------------------------------------------ */

struct nitro_ec_data {
	struct device 	*hwmon_dev;
	struct mutes 	lock; /* protects mode[]/duty_pct[] + WMI writes */

	u64 		supported_sensors;
	u8 		mode[2];
	u8 		duty_pct[2];
};

/* ------------------------------------------------------------------ */
/* EC helpers                                                           */
/* ------------------------------------------------------------------ */

static acpi_status nitro_wmi_exec(struct device *dev, u32 method_id, u64 in,
		u64 *out)
{
	struct acpi_buffer input = { sizeof(u64), &in };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	u64 tmp = 0;

	status = wmi_evaluate_method(WMID_GUID4, 0, method_id, &input, &result);
	if (ACPI_FAILURE(status)) {
		dev_warn(dev, "WMI method 0x%x failed: %s\n", method_id,
				acpi_format_exception(status));
		return status;
	}

	obj = result.pointer;
	if (obj) {
		if (obj->type == ACPI_TYPE_BUFFER) {
			if (obj->buffer.length == sizeof(u32)))
				tmp = *((u32 *)obj->buffer.pointer);
			else if (obj->buffer.length == sizeof(u64))
				tmp = *((u64 *)obj->buffer.pointer);
		} else if (obj->type == ACPI_TYPE_INTEGER) {
			tmp = (u64)obj->integer.value;
		}
	}

	if (out)
		*out = tmp;

	kfree(result.pointer);
	nitro_dbg(dev, "WMI method ox%x(in=0x%llx) -> 0x%llx\n", method_id, in, tmp);

	return status;
}

static int nitro_ec_read(u8 addr, u8 *val)
{
	int ret = ec_read(addr, val);

	if (ret)
		pr_warn("EC read failed at 0x%02X (err %d)\n", addr, ret);

	return ret;
}

static int nitro_ec_write(u8 addr, u8 val)
{
	int ret = ec_write(addr, val);

	if (ret)
		pr_warn("EC write failed at 0x%02X = 0x%02X (err %d)\n",
			addr, val, ret);

	return ret;
}

/*
 * Read a 16-bit RPM value from two consecutive EC registers.
 * The EC stores the raw fan RPM as a big-endian 16-bit integer.
 */
static int nitro_read_fan_rpm(struct device *dev,
			      const struct nitro_ec_regs *regs, int channel,
			      long *rpm)
{
	u8 hi, lo;
	int ret;

	if (channel == 0) {
		ret = nitro_ec_read(regs->cpu_fan_rpm_hi, &hi);
		if (ret)
			return ret;
		ret = nitro_ec_read(regs->cpu_fan_rpm_lo, &lo);
	} else {
		ret = nitro_ec_read(regs->gpu_fan_rpm_hi, &hi);
		if (ret)
			return ret;
		ret = nitro_ec_read(regs->gpu_fan_rpm_lo, &lo);
	}

	if (ret)
		return ret;

	/*
	 * The EC naming is misleading: the register called "HIGH BITS" (0x13/0x15)
	 * holds the low byte of RPM, and "LOW BITS" (0x14/0x16) holds the high byte.
	 * Confirmed by cross-referencing with the Linux-NitroSense Python implementation:
	 *   cpufanspeed = cpufanspeedLowBits << 8 | cpufanspeedHighBits
	 */
	*rpm = (long)(((u16)lo << 8) | hi);

	nitro_dbg(dev, "%s fan RPM: reg_hi(0x%02X)=0x%02X reg_lo(0x%02X)=0x%02X -> %ld RPM\n",
		  channel == 0 ? "CPU" : "GPU",
		  channel == 0 ? regs->cpu_fan_rpm_hi : regs->gpu_fan_rpm_hi, hi,
		  channel == 0 ? regs->cpu_fan_rpm_lo : regs->gpu_fan_rpm_lo, lo,
		  *rpm);

	return 0;
}

/* ------------------------------------------------------------------ */
/* hwmon callbacks                                                      */
/* ------------------------------------------------------------------ */

static const enum nitro_sensor_id nitro_temp_sensor[] = {
	[0]=NITRO_SENSOR_CPU_TEMP, [1]=NITRO_SENSOR_GPU_TEMP, [2]=NITRO_SENSOR_SYS_TEMP,
};

static const enum nitro_sensor_id nitro_fan_sensor[] = {
	[0]=NITRO_SENSOR_CPU_FAN_SPEED, [1]=NITRO_SENSOR_GPU_FAN_SPEED,
};

static umode_t nitro_hwmon_is_visible(const void *drvdata,
		enum hwmon_sensor_types type, u32 attr, int channel)
{
	const struct nitro_data *data = drvdata;
	enum nitro_sensor_id sensor;

	switch (type) {
		case hwmon_fan:
			if (channel >= ARRAY_SIZE(nitro_fan_sensor))
				return 0;
			sensor = nitro_fan_sensor[channel];
			return (data->supported_sensors & BIT(sensor - 1)) ? 0444 : 0;
		case hwmon_pwm:
			if (channel >= ARRAY_SIZE(nitro_fan_sensor))
				return 0;
			sensor = nitro_fan_sensor[channel];
			return (data->supported_sensors & BIT(sensor - 1)) ? 0644 : 0;
		case hwmon_temp:
			if (channel >= ARRAY_SIZE(nitro_temp_sensor))
				return 0;
			sensor = nitro_temp_sensor[channel];
			return (data->supported_sensors & BIT(sensor - 1)) ? 0444 : 0;
		default:
			return 0;
	}
};

static int nitro_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long *val)
{
	struct nitro_data *data = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_fan:
		if(channel >= ARRAY_SIZE(nitro_fan_sensor))
			return -EOPNOTSUPP;
		return nitro_read_sensor(dev, nitro_fan_sensor[channel], false, val);

	case hwmon_temp:
		if (channel >= ARRAY_SIZE(nitro_temp_sensor))
			return -EOPNOTSUPP;
		return nitro_read_sensor(dev, nitro_temp_sensor[channel], true, val);
	case hwmon_pwm:
		if (channel >= 2)
			return -EOPNOTSUPP;
		switch (attr) {
			case hwmon_pwm_input:
				/* Cached last-written value */
				mutex_lock(&data->lock);
				*val = (long)data->duty_pct[channel] * 255 / 100;
				mutex_unlock(&data->lock);
				return 0;
			case hwmon_pwm_enable:
				mutex_lock(&data->lock);
				*val = data->mode[channel];
				return 0;
			default:
				return -EOPNOTSUPP;
		}
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int nitro_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct nitro_ec_data *data = dev_get_drvdata(dev);
	const struct nitro_ec_regs *regs = data->regs;
	u8 ec_val;

	switch (type) {

	case hwmon_pwm:
		switch (attr) {

		case hwmon_pwm_input:
			if (val < 0 || val > 255)
				return -EINVAL;
			/* Scale 0-255 → 0-100 */
			ec_val = (u8)(val * 100 / 255);
			dev_info(dev, "%s fan speed set: hwmon=%ld -> EC=%u%%\n",
				 channel == 0 ? "CPU" : "GPU", val, ec_val);
			return nitro_ec_write(channel == 0
					      ? regs->cpu_fan_speed_ctrl
					      : regs->gpu_fan_speed_ctrl,
					      ec_val);

		case hwmon_pwm_enable: {
			/*
			 * 0 = turbo (full speed, firmware-forced)
			 * 1 = manual (controlled via pwm attribute)
			 * 2 = automatic (firmware manages speed)
			 */
			static const char * const mode_names[] = {
				"turbo", "manual", "auto"
			};
			if (val < 0 || val > 2)
				return -EINVAL;
			if (channel == 0) {
				if (val == 0)
					ec_val = CPU_TURBO_MODE;
				else if (val == 1)
					ec_val = CPU_MANUAL_MODE;
				else
					ec_val = CPU_AUTO_MODE;
				dev_info(dev,
					 "CPU fan mode set: %s (EC=0x%02X)\n",
					 mode_names[val], ec_val);
				return nitro_ec_write(regs->cpu_fan_mode_ctrl,
						      ec_val);
			} else {
				if (val == 0)
					ec_val = GPU_TURBO_MODE;
				else if (val == 1)
					ec_val = GPU_MANUAL_MODE;
				else
					ec_val = GPU_AUTO_MODE;
				dev_info(dev,
					 "GPU fan mode set: %s (EC=0x%02X)\n",
					 mode_names[val], ec_val);
				return nitro_ec_write(regs->gpu_fan_mode_ctrl,
						      ec_val);
			}
		}
		}
		break;

	default:
		break;
	}

	return -EOPNOTSUPP;
}

/* ------------------------------------------------------------------ */
/* hwmon chip descriptor                                                */
/* ------------------------------------------------------------------ */

static const struct hwmon_channel_info * const nitro_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
		HWMON_F_INPUT,   /* fan1 = CPU */
		HWMON_F_INPUT),  /* fan2 = GPU */
	HWMON_CHANNEL_INFO(pwm,
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE,  /* pwm1 = CPU */
		HWMON_PWM_INPUT | HWMON_PWM_ENABLE), /* pwm2 = GPU */
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT,   /* temp1 = CPU */
		HWMON_T_INPUT,   /* temp2 = GPU */
		HWMON_T_INPUT),  /* temp3 = system */
	NULL
};

static const struct hwmon_ops nitro_hwmon_ops = {
	.is_visible = nitro_hwmon_is_visible,
	.read       = nitro_hwmon_read,
	.write      = nitro_hwmon_write,
};

static const struct hwmon_chip_info nitro_chip_info = {
	.ops  = &nitro_hwmon_ops,
	.info = nitro_hwmon_info,
};

/* ------------------------------------------------------------------ */
/* Platform driver                                                      */
/* ------------------------------------------------------------------ */

static int nitro_ec_probe(struct platform_device *pdev)
{
	const struct nitro_ec_regs *regs;
	struct nitro_ec_data *data;

	regs = dev_get_platdata(&pdev->dev);
	if (!regs) {
		dev_err(&pdev->dev, "no platform data — aborting probe\n");
		return -ENODEV;
	}

	dev_dbg(&pdev->dev, "EC register map:\n"
		"  CPU fan mode=0x%02X speed=0x%02X rpm=0x%02X/0x%02X\n"
		"  GPU fan mode=0x%02X speed=0x%02X rpm=0x%02X/0x%02X\n"
		"  Temps: cpu=0x%02X gpu=0x%02X sys=0x%02X\n",
		regs->cpu_fan_mode_ctrl, regs->cpu_fan_speed_ctrl,
		regs->cpu_fan_rpm_hi,    regs->cpu_fan_rpm_lo,
		regs->gpu_fan_mode_ctrl, regs->gpu_fan_speed_ctrl,
		regs->gpu_fan_rpm_hi,    regs->gpu_fan_rpm_lo,
		regs->cpu_temp, regs->gpu_temp, regs->sys_temp);

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->regs = regs;
	platform_set_drvdata(pdev, data);

	data->hwmon_dev = devm_hwmon_device_register_with_info(
		&pdev->dev, "acer_nitro_ec", data,
		&nitro_chip_info, NULL);

	if (IS_ERR(data->hwmon_dev)) {
		dev_err(&pdev->dev, "hwmon registration failed: %ld\n",
			PTR_ERR(data->hwmon_dev));
		return PTR_ERR(data->hwmon_dev);
	}

	dev_info(&pdev->dev, "hwmon interface registered at %s\n",
		 dev_name(data->hwmon_dev));
	if (debug)
		dev_info(&pdev->dev, "verbose logging enabled\n");

	return 0;
}

static struct platform_driver nitro_ec_driver = {
	.probe  = nitro_ec_probe,
	.driver = {
		.name = DRIVER_NAME,
	},
};

/* ------------------------------------------------------------------ */
/* Module init/exit — DMI-based device detection                       */
/* ------------------------------------------------------------------ */

static struct platform_device *nitro_pdev;

static int __init nitro_ec_init(void)
{
	const struct nitro_ec_regs *regs = NULL;
	const char *model;
	int ret;

	model = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (!model)
		return -ENODEV;

	if (strstr(model, "AN515-44"))
		regs = &regs_an515_44;
	else if (strstr(model, "AN515-46") ||
		 strstr(model, "AN515-54") ||
		 strstr(model, "AN515-56") ||
		 strstr(model, "AN515-57") ||
		 strstr(model, "AN515-58") ||
		 strstr(model, "AN517-55"))
		regs = &regs_an515_46;

	if (!regs) {
		pr_info("unsupported model '%s' — not loading\n", model);
		return -ENODEV;
	}

	pr_info("detected '%s', loading driver\n", model);

	ret = platform_driver_register(&nitro_ec_driver);
	if (ret) {
		pr_err("platform_driver_register failed: %d\n", ret);
		return ret;
	}

	nitro_pdev = platform_device_register_data(
		NULL, DRIVER_NAME, PLATFORM_DEVID_NONE,
		regs, sizeof(*regs));

	if (IS_ERR(nitro_pdev)) {
		pr_err("platform_device_register failed: %ld\n",
		       PTR_ERR(nitro_pdev));
		platform_driver_unregister(&nitro_ec_driver);
		return PTR_ERR(nitro_pdev);
	}

	return 0;
}

static void __exit nitro_ec_exit(void)
{
	pr_info("unloading driver\n");
	platform_device_unregister(nitro_pdev);
	platform_driver_unregister(&nitro_ec_driver);
}

module_init(nitro_ec_init);
module_exit(nitro_ec_exit);

MODULE_AUTHOR("Linux-NitroSense contributors");
MODULE_DESCRIPTION("Acer Nitro AN515/AN517 EC fan control driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("dmi:*:svnAcer:pnNitroAN515*:");
MODULE_ALIAS("dmi:*:svnAcer:pnNitroAN517*:");
