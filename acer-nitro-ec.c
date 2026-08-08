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

#include <asm-generic/errno-base.h>
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

static int nitro_wmi_get_sys_info()
{

}

static int nitro_read_sensor()
{

}

static acpi_status nitro_set_fan_speed()
{

}

static int nitro_apply_fan_state()
{

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
		return -EOPNOTSUPP;
	}

}

static int nitro_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long val)
{
	struct nitro_data *data = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm || channel >= 2)
		return -EOPNOTSUPP;

	switch (attr) {
		case hwmon_pwm_enable:
			if (val < NITRO_MODE_TURBO || val > NITRO_MODE_AUTO)
				return -EINVAL;

			mutex_lock(&data->lock);
			data->mode[channel] = val;
			ret = nitro_apply_fan_state(dev, data);
			mutex_unlock(&data->lock);
			return ret;

		case hwmon_pwm_input:
			if (val < 0 || val > 255)
				return -EINVAL;

			mutex_lock(&data->lock);
			if (data->mode[channel] != NITRO_MODE_MANUAL) {
				mutex_unlock(&data->lock);
				dev_warn(dev,
					"%s: set pwm%d_enable=1 (manual) before writing pwm%d\n", DRIVER_NAME, channel + 1, channel + 1);
				return -EINVAL;
			}

			data->duty_pct[channel] = (u8)(val * 100 / 255);
			ret = nitro_apply_fan_state(dev, data);
			mutex_unlock(&data->lock);
			return ret;
		default:
			return -EOPNOTSUPP;
	}
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
		HWMON_T_INPUT),  /* temp3 = secondary/system */
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
	struct nitro_data *data;
	u64 supported;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!regs)
		return -ENOMEM;

	mutex_init(&data->lock);
	data->mode[0] = NITRO_MODE_AUTO;
	data->mode[1] = NITRO_MODE_AUTO;

	ret = nitro_wmi_get_sys_info(&pdev->dev,
			ACER_WMID_CMD_GET_SUPPORTED_SENSORS,
			&supported);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to query supported sensors: %d\n", ret);
		return ret;
	}

	data->supported_sensors = FIELD_GET(NITRO_SUPPORTED_SENSORS_MASK, supported);
	if (!data->supported_sensors) {
		dev_err(&pdev->dev, "firmware reports no usable sensors\n");
		return -ENODEV;
	}

	nitro_dbg(&pdev->dev, "supported sensor bitmap: 0x%llx\n", data->supported_sensors);

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

/*
 =============================================================
 = Module init/exit - DMI-based device detection             =
 =============================================================
 * */

static struct platform_device *nitro_pdev;

/*
 * Same allowlist as before: this is a sanity check on top of the WMI
 * GUID check below, not the mechanism used to talk to the hardware
 *anymore (there is no more per-model register map - the WMI method
 *set is uniform across models that impement it )
 * */


static const char * const nitro_supported_models[] = {
	"AN515-44", "AN515-46", "AN515-54", "AN515-56",
	"AN515-57", "AN515-58", "AN517-55", NULL
};

static int __init nitro_ec_init(void)
{
	const char *model;
	int ret, i;
	bool matched = false;

	model = dmi_get_system_info(DMI_PRODUCT_NAME);
	if (!model)
		return -ENODEV;

	for (i = 0; nitro_supported_models[i]; i++) {
		if (strstr(model, nitro_supported_models[i])) {
			matched = true;
			break
		}
	}

	if (!matched) {
		pr_info("unsupported model '%s' - not loading\n", model);
		return -ENODEV;
	}

	if (!wmi_has_guid(WMID_GUID4)) {
		pr_info("model '%s' recognized but gaming WMI interface "
			"(%s) not present - not loading\n", model, WMID_GUID4);
		return -ENODEV;
	}

	pr_info("detected '%s', loading driver\n", model);

	ret = platform_driver_register(&nitro_ec_driver);
	if (ret) {
		pr_err("platform_driver_register failed: %d\n", ret);
		return ret;
	}

	nitro_pdev = platform_device_register_simple(
		DRIVER_NAME, PLATFORM_DEVID_NONE,
		NULL, 0);

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

MODULE_AUTHOR("Qapky Qy");
MODULE_DESCRIPTION("Acer Nitro fan and temperature driver (ACPI-WMI)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("dmi:*:svnAcer:pnNitroAN515*:");
MODULE_ALIAS("dmi:*:svnAcer:pnNitroAN517*:");
MODULE_ALIAS("wmi:7A4DDFE7-5B5D-40B4-8595-4408E0CC7F56");
