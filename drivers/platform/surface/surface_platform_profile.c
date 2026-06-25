// SPDX-License-Identifier: GPL-2.0+
/*
 * Surface Platform Profile / Performance Mode driver for Surface System
 * Aggregator Module (thermal and fan subsystem).
 *
 * Copyright (C) 2021-2022 Maximilian Luz <luzmaximilian@gmail.com>
 */

#include <linux/unaligned.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/types.h>

#include <linux/surface_aggregator/device.h>

// Enum for the platform performance profile sent to the TMP module.
enum ssam_tmp_profile {
	SSAM_TMP_PROFILE_NORMAL             = 1,
	SSAM_TMP_PROFILE_BATTERY_SAVER      = 2,
	SSAM_TMP_PROFILE_BETTER_PERFORMANCE = 3,
	SSAM_TMP_PROFILE_BEST_PERFORMANCE   = 4,
};

// Enum for the fan profile sent to the FAN module. This fan profile is
// only sent to the EC if the 'has_fan' property is set. The integers are
// not a typo, they differ from the performance profile indices.
enum ssam_fan_profile {
	SSAM_FAN_PROFILE_NORMAL             = 2,
	SSAM_FAN_PROFILE_BATTERY_SAVER      = 1,
	SSAM_FAN_PROFILE_BETTER_PERFORMANCE = 3,
	SSAM_FAN_PROFILE_BEST_PERFORMANCE   = 4,
};

enum ssam_fan_state {
	SSAM_FAN_STATE_NORMAL  = 1,
	SSAM_FAN_STATE_STANDBY = 2,
};

struct ssam_tmp_profile_info {
	__le32 profile;
	__le16 unknown1;
	__le16 unknown2;
} __packed;

struct ssam_platform_profile_device {
	struct ssam_device *sdev;
	struct device *ppdev;
	struct platform_device *pdev;
	bool has_fan;
};

SSAM_DEFINE_SYNC_REQUEST_CL_R(__ssam_tmp_profile_get, struct ssam_tmp_profile_info, {
	.target_category = SSAM_SSH_TC_TMP,
	.command_id      = 0x02,
});

SSAM_DEFINE_SYNC_REQUEST_CL_W(__ssam_tmp_profile_set, __le32, {
	.target_category = SSAM_SSH_TC_TMP,
	.command_id      = 0x03,
});

SSAM_DEFINE_SYNC_REQUEST_W(__ssam_fan_profile_set, u8, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id = SSAM_SSH_TID_SAM,
	.command_id = 0x0e,
	.instance_id = 0x01,
});

SSAM_DEFINE_SYNC_REQUEST_W(__ssam_fan_state_set, u8, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id = SSAM_SSH_TID_SAM,
	.command_id = 0x0f,
	.instance_id = 0x01,
});

SSAM_DEFINE_SYNC_REQUEST_W(__ssam_fan_tmp_offset_set, __le32, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id = SSAM_SSH_TID_SAM,
	.command_id = 0x0c,
	.instance_id = 0x01,
});

SSAM_DEFINE_SYNC_REQUEST_R(__ssam_fan_tmp_offset_get, __le32, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id = SSAM_SSH_TID_SAM,
	.command_id = 0x0d,
	.instance_id = 0x01,
});

SSAM_DEFINE_SYNC_REQUEST_W(__ssam_fan_base_rpm_set, __le16, {
	.target_category = SSAM_SSH_TC_FAN,
	.target_id = SSAM_SSH_TID_SAM,
	.command_id = 0x0b,
	.instance_id = 0x01,
});

static int ssam_tmp_profile_get(struct ssam_device *sdev, enum ssam_tmp_profile *p)
{
	struct ssam_tmp_profile_info info;
	int status;

	status = ssam_retry(__ssam_tmp_profile_get, sdev, &info);
	if (status < 0)
		return status;

	*p = le32_to_cpu(info.profile);
	return 0;
}

static int ssam_tmp_profile_set(struct ssam_device *sdev, enum ssam_tmp_profile p)
{
	const __le32 profile_le = cpu_to_le32(p);

	return ssam_retry(__ssam_tmp_profile_set, sdev, &profile_le);
}

static int ssam_fan_profile_set(struct ssam_device *sdev, enum ssam_fan_profile p)
{
	const u8 profile = p;

	return ssam_retry(__ssam_fan_profile_set, sdev->ctrl, &profile);
}

static int ssam_fan_state_set(struct ssam_device *sdev, enum ssam_fan_state s)
{
	const u8 state = s;

	return ssam_retry(__ssam_fan_state_set, sdev->ctrl, &state);
}

static int ssam_fan_tmp_offset_set(struct ssam_device *sdev, const u32 o)
{
	const __le32 offset = cpu_to_le32(o);

	return ssam_retry(__ssam_fan_tmp_offset_set, sdev->ctrl, &offset);
}

static int ssam_fan_tmp_offset_get(struct ssam_device *sdev, u32 *o)
{
	__le32 offset;
	int status;

	status = ssam_retry(__ssam_fan_tmp_offset_get, sdev->ctrl, &offset);
	if (status < 0)
		return status;

	*o = le32_to_cpu(offset);
	return 0;
}

static int ssam_fan_base_rpm_set(struct ssam_device *sdev, const u16 r)
{
	const __le16 rpm = cpu_to_le16(r);

	return ssam_retry(__ssam_fan_base_rpm_set, sdev->ctrl, &rpm);
}

static int convert_ssam_tmp_to_profile(struct ssam_device *sdev, enum ssam_tmp_profile p)
{
	switch (p) {
	case SSAM_TMP_PROFILE_NORMAL:
		return PLATFORM_PROFILE_BALANCED;

	case SSAM_TMP_PROFILE_BATTERY_SAVER:
		return PLATFORM_PROFILE_LOW_POWER;

	case SSAM_TMP_PROFILE_BETTER_PERFORMANCE:
		return PLATFORM_PROFILE_BALANCED_PERFORMANCE;

	case SSAM_TMP_PROFILE_BEST_PERFORMANCE:
		return PLATFORM_PROFILE_PERFORMANCE;

	default:
		dev_err(&sdev->dev, "invalid performance profile: %d", p);
		return -EINVAL;
	}
}


static int convert_profile_to_ssam_tmp(struct ssam_device *sdev, enum platform_profile_option p)
{
	switch (p) {
	case PLATFORM_PROFILE_LOW_POWER:
		return SSAM_TMP_PROFILE_BATTERY_SAVER;

	case PLATFORM_PROFILE_BALANCED:
		return SSAM_TMP_PROFILE_NORMAL;

	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		return SSAM_TMP_PROFILE_BETTER_PERFORMANCE;

	case PLATFORM_PROFILE_PERFORMANCE:
		return SSAM_TMP_PROFILE_BEST_PERFORMANCE;

	default:
		/* This should have already been caught by platform_profile_store(). */
		WARN(true, "unsupported platform profile");
		return -EOPNOTSUPP;
	}
}

static int convert_profile_to_ssam_fan(struct ssam_device *sdev, enum platform_profile_option p)
{
	switch (p) {
	case PLATFORM_PROFILE_LOW_POWER:
		return SSAM_FAN_PROFILE_BATTERY_SAVER;

	case PLATFORM_PROFILE_BALANCED:
		return SSAM_FAN_PROFILE_NORMAL;

	case PLATFORM_PROFILE_BALANCED_PERFORMANCE:
		return SSAM_FAN_PROFILE_BETTER_PERFORMANCE;

	case PLATFORM_PROFILE_PERFORMANCE:
		return SSAM_FAN_PROFILE_BEST_PERFORMANCE;

	default:
		/* This should have already been caught by platform_profile_store(). */
		WARN(true, "unsupported platform profile");
		return -EOPNOTSUPP;
	}
}

static int ssam_platform_profile_get(struct device *dev,
				     enum platform_profile_option *profile)
{
	struct ssam_platform_profile_device *tpd;
	enum ssam_tmp_profile tp;
	int status;

	tpd = dev_get_drvdata(dev);

	status = ssam_tmp_profile_get(tpd->sdev, &tp);
	if (status)
		return status;

	status = convert_ssam_tmp_to_profile(tpd->sdev, tp);
	if (status < 0)
		return status;

	*profile = status;
	return 0;
}

static int ssam_platform_profile_set(struct device *dev,
				     enum platform_profile_option profile)
{
	struct ssam_platform_profile_device *tpd;
	int tp;

	tpd = dev_get_drvdata(dev);

	tp = convert_profile_to_ssam_tmp(tpd->sdev, profile);
	if (tp < 0)
		return tp;

	tp = ssam_tmp_profile_set(tpd->sdev, tp);
	if (tp < 0)
		return tp;

	if (tpd->has_fan) {
		tp = convert_profile_to_ssam_fan(tpd->sdev, profile);
		if (tp < 0)
			return tp;
		tp = ssam_fan_profile_set(tpd->sdev, tp);
	}

	return tp;
}

static int ssam_platform_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_BALANCED_PERFORMANCE, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static const struct platform_profile_ops ssam_platform_profile_ops = {
	.probe = ssam_platform_profile_probe,
	.profile_get = ssam_platform_profile_get,
	.profile_set = ssam_platform_profile_set,
};

static inline u32 fixed_milli_to_float(const u32 fixed)
{
	u32 sign = (fixed < 0) ? 0x80000000 : 0x00000000;

	u32 abs_fixed = (fixed < 0) ? (u32)(-fixed) : (u32)fixed;

	int lz = __builtin_clz(abs_fixed);
	int msb = (int)(sizeof(abs_fixed) * 8) - 1 - lz;

	u32 normalized = abs_fixed << lz;

	u32 aligned_mantissa = (u64)normalized * 4398046511ULL >> 32;

	int unbiased_exponent = 127 + msb - 10;

	if (aligned_mantissa < normalized) {
		aligned_mantissa >>= 1;
		unbiased_exponent += 1;
	}

	u32 exponent = (u32)unbiased_exponent << 23;

	u32 mantissa = (aligned_mantissa >> 8) & 0x007FFFFF;

	return sign | exponent | mantissa;
}

static inline u32 float_to_fixed_milli(const u32 input)
{
	u32 fixed = ((input & 0x007FFFFF) | 0x00800000) << 8;

	fixed = (u64)fixed * 4194304000ULL >> 32;

	u32 exponent = (input >> 23) & 0xFF;

	int msb = exponent - 127 + 10;
	if (msb < 0)
		return 0;

	int lz = (int)(sizeof(input) * 8) - 1 - msb;

	fixed = fixed >> lz;

	if (input >> 31)
		fixed = ((fixed ^ 0xFFFFFFFF) + 1) | 0x80000000;

	return fixed;
}

static ssize_t fan_temp_offset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ssam_platform_profile_device *tpd;
	u32 degrees_float;

	tpd = dev_get_drvdata(dev);

	ssam_fan_tmp_offset_get(tpd->sdev, &degrees_float);

	const long int millidegrees = float_to_fixed_milli(degrees_float);

	return sysfs_emit(buf, "%ld\n", millidegrees);
}

static ssize_t fan_temp_offset_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct ssam_platform_profile_device *tpd;
	int status;
	long int millidegrees;

	tpd = dev_get_drvdata(dev);

	if (kstrtol(buf, 10, &millidegrees) < 0)
		return -EINVAL;

	const u32 degrees_float = fixed_milli_to_float(millidegrees);

	status = ssam_fan_tmp_offset_set(tpd->sdev, degrees_float);
	if (status < 0)
		return status;

	return count;
}

static ssize_t fan_base_rpm_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct ssam_platform_profile_device *tpd;
	int status;
	long int rpm;

	tpd = dev_get_drvdata(dev);

	if (kstrtol(buf, 10, &rpm) < 0)
		return -EINVAL;

	status = ssam_fan_base_rpm_set(tpd->sdev, rpm);
	if (status < 0)
		return status;

	return count;
}

static DEVICE_ATTR_RW(fan_temp_offset);
static DEVICE_ATTR_WO(fan_base_rpm);

static struct attribute *pdev_attrs[] = {
	&dev_attr_fan_temp_offset.attr,
	&dev_attr_fan_base_rpm.attr,
	NULL,
};

ATTRIBUTE_GROUPS(pdev);

static void surface_platform_profile_unregister_pdev(void *data)
{
	struct platform_device *pdev = data;

	platform_device_unregister(pdev);
}

static int surface_platform_profile_probe(struct ssam_device *sdev)
{
	struct ssam_platform_profile_device *tpd;
	int ret;

	tpd = devm_kzalloc(&sdev->dev, sizeof(*tpd), GFP_KERNEL);
	if (!tpd)
		return -ENOMEM;

	tpd->sdev = sdev;
	ssam_device_set_drvdata(sdev, tpd);

	tpd->has_fan = device_property_read_bool(&sdev->dev, "has_fan");

	tpd->ppdev = devm_platform_profile_register(&sdev->dev, "Surface Platform Profile",
						    tpd, &ssam_platform_profile_ops);

	if (IS_ERR(tpd->ppdev))
		return PTR_ERR(tpd->ppdev);

	if (tpd->has_fan) {

		tpd->pdev = platform_device_alloc("surface_fan_profile", PLATFORM_DEVID_NONE);

		if (!tpd->pdev)
			return -ENOMEM;

		tpd->pdev->dev.parent = &sdev->dev;
		tpd->pdev->dev.groups = pdev_groups;

		ret = platform_device_add(tpd->pdev);
		if (ret) {
			platform_device_put(tpd->pdev);
			return ret;
		}

		dev_set_drvdata(&tpd->pdev->dev, tpd);

		ret = devm_add_action_or_reset(&sdev->dev, surface_platform_profile_unregister_pdev, tpd->pdev);
		if (ret)
			return ret;

	}

	return 0;
}

#ifdef CONFIG_PM_SLEEP

static int surface_platform_profile_pm_prepare(struct device *dev)
{
	struct ssam_platform_profile_device *tpd;
	int status;

	tpd = dev_get_drvdata(dev);

	if (tpd->has_fan) {
		status = ssam_fan_state_set(tpd->sdev, SSAM_FAN_STATE_STANDBY);
		if (status)
			dev_err(&tpd->sdev->dev, "pm: fan standby notification failed");
	}

	return status;
}

static void surface_platform_profile_pm_complete(struct device *dev)
{
	struct ssam_platform_profile_device *tpd;
	int status;

	tpd = dev_get_drvdata(dev);

	if (tpd->has_fan) {
		status = ssam_fan_state_set(tpd->sdev, SSAM_FAN_STATE_NORMAL);
		if (status)
			dev_err(&tpd->sdev->dev, "pm: fan resume notification failed");
	}
}

static const struct dev_pm_ops surface_platform_profile_pm_ops = {
	.prepare  = surface_platform_profile_pm_prepare,
	.complete = surface_platform_profile_pm_complete,
};

#else /* CONFIG_PM_SLEEP */

static const struct dev_pm_ops surface_platform_profile_pm_ops = { };

#endif /* CONFIG_PM_SLEEP */

static const struct ssam_device_id ssam_platform_profile_match[] = {
	{ SSAM_SDEV(TMP, SAM, 0x00, 0x01) },
	{ },
};
MODULE_DEVICE_TABLE(ssam, ssam_platform_profile_match);

static struct ssam_device_driver surface_platform_profile = {
	.probe = surface_platform_profile_probe,
	.match_table = ssam_platform_profile_match,
	.driver = {
		.name = "surface_platform_profile",
		.pm = &surface_platform_profile_pm_ops,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};
module_ssam_device_driver(surface_platform_profile);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Platform Profile Support for Surface System Aggregator Module");
MODULE_LICENSE("GPL");
