// SPDX-License-Identifier: GPL-2.0
/* Read-only remote power supplies. No hardware access or power-management actions. */
#include "netdisplay_power.h"
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

struct nd_enum {
	const char *name;
	int value;
};
#define E(prefix, name, text) {text, prefix##name}
static const struct nd_enum statuses[] = {E(POWER_SUPPLY_STATUS_, UNKNOWN, "Unknown"),
					  E(POWER_SUPPLY_STATUS_, CHARGING, "Charging"),
					  E(POWER_SUPPLY_STATUS_, DISCHARGING, "Discharging"),
					  E(POWER_SUPPLY_STATUS_, NOT_CHARGING, "Not charging"),
					  E(POWER_SUPPLY_STATUS_, FULL, "Full"),
					  {NULL, 0}};
static const struct nd_enum health[] = {
    E(POWER_SUPPLY_HEALTH_, UNKNOWN, "Unknown"),
    E(POWER_SUPPLY_HEALTH_, GOOD, "Good"),
    E(POWER_SUPPLY_HEALTH_, OVERHEAT, "Overheat"),
    E(POWER_SUPPLY_HEALTH_, DEAD, "Dead"),
    E(POWER_SUPPLY_HEALTH_, OVERVOLTAGE, "Over voltage"),
    E(POWER_SUPPLY_HEALTH_, UNSPEC_FAILURE, "Unspecified failure"),
    E(POWER_SUPPLY_HEALTH_, COLD, "Cold"),
    E(POWER_SUPPLY_HEALTH_, WATCHDOG_TIMER_EXPIRE, "Watchdog timer expire"),
    E(POWER_SUPPLY_HEALTH_, SAFETY_TIMER_EXPIRE, "Safety timer expire"),
    E(POWER_SUPPLY_HEALTH_, OVERCURRENT, "Over current"),
    E(POWER_SUPPLY_HEALTH_, CALIBRATION_REQUIRED, "Calibration required"),
    E(POWER_SUPPLY_HEALTH_, WARM, "Warm"),
    E(POWER_SUPPLY_HEALTH_, COOL, "Cool"),
    E(POWER_SUPPLY_HEALTH_, HOT, "Hot"),
    {NULL, 0}};
static const struct nd_enum technologies[] = {
    E(POWER_SUPPLY_TECHNOLOGY_, UNKNOWN, "Unknown"), E(POWER_SUPPLY_TECHNOLOGY_, NiMH, "NiMH"),
    E(POWER_SUPPLY_TECHNOLOGY_, LION, "Li-ion"),     E(POWER_SUPPLY_TECHNOLOGY_, LIPO, "Li-poly"),
    E(POWER_SUPPLY_TECHNOLOGY_, LiFe, "LiFe"),	     E(POWER_SUPPLY_TECHNOLOGY_, NiCd, "NiCd"),
    E(POWER_SUPPLY_TECHNOLOGY_, LiMn, "LiMn"),	     {NULL, 0}};
static const struct nd_enum levels[] = {E(POWER_SUPPLY_CAPACITY_LEVEL_, UNKNOWN, "Unknown"),
					E(POWER_SUPPLY_CAPACITY_LEVEL_, CRITICAL, "Critical"),
					E(POWER_SUPPLY_CAPACITY_LEVEL_, LOW, "Low"),
					E(POWER_SUPPLY_CAPACITY_LEVEL_, NORMAL, "Normal"),
					E(POWER_SUPPLY_CAPACITY_LEVEL_, HIGH, "High"),
					E(POWER_SUPPLY_CAPACITY_LEVEL_, FULL, "Full"),
					{NULL, 0}};
static const struct nd_enum charge_types[] = {E(POWER_SUPPLY_CHARGE_TYPE_, UNKNOWN, "Unknown"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, NONE, "N/A"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, TRICKLE, "Trickle"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, FAST, "Fast"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, STANDARD, "Standard"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, ADAPTIVE, "Adaptive"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, CUSTOM, "Custom"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, LONGLIFE, "Long Life"),
					      E(POWER_SUPPLY_CHARGE_TYPE_, BYPASS, "Bypass"),
					      {NULL, 0}};
static const struct nd_enum types[] = {E(POWER_SUPPLY_TYPE_, UNKNOWN, "Unknown"),
				       E(POWER_SUPPLY_TYPE_, BATTERY, "Battery"),
				       E(POWER_SUPPLY_TYPE_, UPS, "UPS"),
				       E(POWER_SUPPLY_TYPE_, MAINS, "Mains"),
				       E(POWER_SUPPLY_TYPE_, USB, "USB"),
				       E(POWER_SUPPLY_TYPE_, USB_DCP, "USB_DCP"),
				       E(POWER_SUPPLY_TYPE_, USB_CDP, "USB_CDP"),
				       E(POWER_SUPPLY_TYPE_, USB_ACA, "USB_ACA"),
				       E(POWER_SUPPLY_TYPE_, USB_TYPE_C, "USB_C"),
				       E(POWER_SUPPLY_TYPE_, USB_PD, "USB_PD"),
				       E(POWER_SUPPLY_TYPE_, USB_PD_DRP, "USB_PD_DRP"),
				       E(POWER_SUPPLY_TYPE_, APPLE_BRICK_ID, "BrickID"),
				       E(POWER_SUPPLY_TYPE_, WIRELESS, "Wireless"),
				       {NULL, 0}};
#undef E

struct nd_mapping {
	const char *key;
	enum power_supply_property property;
	const struct nd_enum *values;
	bool string;
};
#define N(key, prop) {#key, POWER_SUPPLY_PROP_##prop, NULL, false}
#define S(key, prop) {#key, POWER_SUPPLY_PROP_##prop, NULL, true}
#define V(key, prop, map) {#key, POWER_SUPPLY_PROP_##prop, map, false}
static const struct nd_mapping mappings[] = {
    V(status, STATUS, statuses),
    V(health, HEALTH, health),
    V(technology, TECHNOLOGY, technologies),
    V(capacity_level, CAPACITY_LEVEL, levels),
    V(charge_type, CHARGE_TYPE, charge_types),
    N(present, PRESENT),
    N(online, ONLINE),
    N(authentic, AUTHENTIC),
    N(cycle_count, CYCLE_COUNT),
    N(voltage_max, VOLTAGE_MAX),
    N(voltage_min, VOLTAGE_MIN),
    N(voltage_max_design, VOLTAGE_MAX_DESIGN),
    N(voltage_min_design, VOLTAGE_MIN_DESIGN),
    N(voltage_now, VOLTAGE_NOW),
    N(voltage_avg, VOLTAGE_AVG),
    N(voltage_ocv, VOLTAGE_OCV),
    N(voltage_boot, VOLTAGE_BOOT),
    N(current_max, CURRENT_MAX),
    N(current_now, CURRENT_NOW),
    N(current_avg, CURRENT_AVG),
    N(current_boot, CURRENT_BOOT),
    N(power_now, POWER_NOW),
    N(power_avg, POWER_AVG),
    N(charge_full_design, CHARGE_FULL_DESIGN),
    N(charge_empty_design, CHARGE_EMPTY_DESIGN),
    N(charge_full, CHARGE_FULL),
    N(charge_empty, CHARGE_EMPTY),
    N(charge_now, CHARGE_NOW),
    N(charge_avg, CHARGE_AVG),
    N(charge_counter, CHARGE_COUNTER),
    N(constant_charge_current, CONSTANT_CHARGE_CURRENT),
    N(constant_charge_current_max, CONSTANT_CHARGE_CURRENT_MAX),
    N(constant_charge_voltage, CONSTANT_CHARGE_VOLTAGE),
    N(constant_charge_voltage_max, CONSTANT_CHARGE_VOLTAGE_MAX),
    N(charge_control_limit, CHARGE_CONTROL_LIMIT),
    N(charge_control_limit_max, CHARGE_CONTROL_LIMIT_MAX),
    N(charge_control_start_threshold, CHARGE_CONTROL_START_THRESHOLD),
    N(charge_control_end_threshold, CHARGE_CONTROL_END_THRESHOLD),
    N(input_current_limit, INPUT_CURRENT_LIMIT),
    N(input_voltage_limit, INPUT_VOLTAGE_LIMIT),
    N(input_power_limit, INPUT_POWER_LIMIT),
    N(energy_full_design, ENERGY_FULL_DESIGN),
    N(energy_empty_design, ENERGY_EMPTY_DESIGN),
    N(energy_full, ENERGY_FULL),
    N(energy_empty, ENERGY_EMPTY),
    N(energy_now, ENERGY_NOW),
    N(energy_avg, ENERGY_AVG),
    N(capacity, CAPACITY),
    N(capacity_alert_min, CAPACITY_ALERT_MIN),
    N(capacity_alert_max, CAPACITY_ALERT_MAX),
    N(capacity_error_margin, CAPACITY_ERROR_MARGIN),
    N(temp, TEMP),
    N(temp_max, TEMP_MAX),
    N(temp_min, TEMP_MIN),
    N(temp_alert_min, TEMP_ALERT_MIN),
    N(temp_alert_max, TEMP_ALERT_MAX),
    N(temp_ambient, TEMP_AMBIENT),
    N(temp_ambient_alert_min, TEMP_AMBIENT_ALERT_MIN),
    N(temp_ambient_alert_max, TEMP_AMBIENT_ALERT_MAX),
    N(time_to_empty_now, TIME_TO_EMPTY_NOW),
    N(time_to_empty_avg, TIME_TO_EMPTY_AVG),
    N(time_to_full_now, TIME_TO_FULL_NOW),
    N(time_to_full_avg, TIME_TO_FULL_AVG),
    N(precharge_current, PRECHARGE_CURRENT),
    N(charge_term_current, CHARGE_TERM_CURRENT),
    S(model_name, MODEL_NAME),
    S(manufacturer, MANUFACTURER),
    S(serial_number, SERIAL_NUMBER),
};
#undef N
#undef S
#undef V

struct nd_raw_attribute {
	struct device_attribute attr;
	unsigned int index;
};
struct nd_supply {
	struct delayed_work expiry;
	struct mutex write_lock;
	struct mutex data_lock;
	struct power_supply *psy;
	struct power_supply_desc desc;
	enum power_supply_property properties[ARRAY_SIZE(mappings) + 1];
	struct nd_power_native data;
	struct nd_raw_attribute raw[ND_POWER_MAX_PROPERTIES];
	struct attribute *raw_attrs[ND_POWER_MAX_PROPERTIES + 1];
	struct attribute_group raw_group;
	const struct attribute_group *groups[2];
};
static atomic_t open_count = ATOMIC_INIT(0);
static struct miscdevice nd_misc;

static const char *lookup(const struct nd_power_native *d, const char *key)
{
	unsigned int i;
	for (i = 0; i < d->count; i++)
		if (!strcmp(key, d->properties[i].key))
			return d->properties[i].value;
	return NULL;
}

static int enum_value(const struct nd_enum *map, const char *value)
{
	for (; map->name; map++)
		if (!strcmp(map->name, value))
			return map->value;
	return -ENODATA;
}

static int get_property(struct power_supply *psy, enum power_supply_property property,
			union power_supply_propval *value)
{
	struct nd_supply *s = power_supply_get_drvdata(psy);
	unsigned int i;
	int ret = -ENODATA;
	if (property == POWER_SUPPLY_PROP_SCOPE) {
		value->intval = POWER_SUPPLY_SCOPE_DEVICE;
		return 0;
	}
	mutex_lock(&s->data_lock);
	for (i = 0; i < ARRAY_SIZE(mappings); i++) {
		const struct nd_mapping *m = &mappings[i];
		const char *text;
		if (property != m->property)
			continue;
		text = lookup(&s->data, m->key);
		if (!text)
			break;
		if (m->string) {
			/* Identity strings remain immutable for this device's lifetime. */
			value->strval = text;
			ret = 0;
		} else if (m->values) {
			ret = enum_value(m->values, text);
			if (ret >= 0) {
				value->intval = ret;
				ret = 0;
			}
		} else {
			ret = kstrtoint(text, 10, &value->intval);
			if (ret)
				ret = -ENODATA;
		}
		break;
	}
	mutex_unlock(&s->data_lock);
	return ret;
}

static ssize_t raw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct nd_supply *s = power_supply_get_drvdata(psy);
	struct nd_raw_attribute *raw = container_of(attr, struct nd_raw_attribute, attr);
	ssize_t ret;
	mutex_lock(&s->data_lock);
	ret = sysfs_emit(buf, "%s\n", s->data.properties[raw->index].value);
	mutex_unlock(&s->data_lock);
	return ret;
}

static bool valid_text(const char *s, size_t size, bool identifier)
{
	size_t i, len = strnlen(s, size);
	if (len == size || (identifier && (!len || !strcmp(s, ".") || !strcmp(s, ".."))))
		return false;
	for (i = 0; i < len; i++) {
		unsigned char c = s[i];
		if (c < 32 || c == 127)
			return false;
		if (identifier && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				    (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
			return false;
	}
	return true;
}

static int validate(const struct nd_power_native *d)
{
	unsigned int i, j;
	if (d->version != ND_POWER_ABI_VERSION || d->count > ND_POWER_MAX_PROPERTIES ||
	    !valid_text(d->name, sizeof(d->name), true) || strncmp(d->name, "netdisplay-", 11))
		return -EINVAL;
	for (i = 0; i < d->count; i++) {
		if (!valid_text(d->properties[i].key, ND_POWER_KEY_SIZE, true) ||
		    !valid_text(d->properties[i].value, ND_POWER_VALUE_SIZE, false))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (!strcmp(d->properties[i].key, d->properties[j].key))
				return -EINVAL;
	}
	return lookup(d, "type") ? 0 : -EINVAL;
}

static bool same_identity(const struct nd_power_native *a, const struct nd_power_native *b)
{
	unsigned int i;
	if (a->count != b->count || strcmp(a->name, b->name))
		return false;
	for (i = 0; i < a->count; i++) {
		const char *key = a->properties[i].key;
		if (strcmp(key, b->properties[i].key))
			return false;
		if ((!strcmp(key, "type") || !strcmp(key, "model_name") ||
		     !strcmp(key, "manufacturer") || !strcmp(key, "serial_number")) &&
		    strcmp(a->properties[i].value, b->properties[i].value))
			return false;
	}
	return true;
}

/* Expire even if userspace hangs with a half-read network message. */
static void expire_supply(struct work_struct *work)
{
	struct nd_supply *s = container_of(to_delayed_work(work), struct nd_supply, expiry);
	mutex_lock(&s->write_lock);
	if (s->psy) {
		power_supply_unregister(s->psy);
		s->psy = NULL;
	}
	mutex_unlock(&s->write_lock);
}

static ssize_t nd_write(struct file *file, const char __user *buf, size_t size, loff_t *offset)
{
	struct nd_supply *s = file->private_data;
	struct nd_power_native *d;
	struct power_supply_config config = {.drv_data = s, .no_wakeup_source = true};
	unsigned int i;
	int ret;
	if (size != sizeof(*d))
		return -EMSGSIZE;
	d = kvmalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	if (copy_from_user(d, buf, sizeof(*d))) {
		kvfree(d);
		return -EFAULT;
	}
	ret = validate(d);
	if (ret) {
		kvfree(d);
		return ret;
	}
	mutex_lock(&s->write_lock);
	if (s->psy && !same_identity(&s->data, d)) {
		ret = -ESTALE;
		goto out;
	}
	mutex_lock(&s->data_lock);
	if (!s->psy)
		memcpy(&s->data, d, sizeof(*d));
	else {
		/* Never modify strings or keys potentially referenced by sysfs readers. */
		for (i = 0; i < d->count; i++)
			if (strcmp(s->data.properties[i].value, d->properties[i].value))
				memcpy(s->data.properties[i].value, d->properties[i].value,
				       ND_POWER_VALUE_SIZE);
	}
	mutex_unlock(&s->data_lock);
	if (!s->psy) {
		int type = enum_value(types, lookup(d, "type"));
		s->desc.name = s->data.name;
		s->desc.type = type < 0 ? POWER_SUPPLY_TYPE_UNKNOWN : type;
		s->desc.get_property = get_property;
		s->desc.no_thermal = true;
		s->desc.properties = s->properties;
		s->desc.num_properties = 0;
		s->properties[s->desc.num_properties++] = POWER_SUPPLY_PROP_SCOPE;
		for (i = 0; i < ARRAY_SIZE(mappings); i++)
			if (lookup(d, mappings[i].key))
				s->properties[s->desc.num_properties++] = mappings[i].property;
		for (i = 0; i < d->count; i++) {
			sysfs_attr_init(&s->raw[i].attr.attr);
			s->raw[i].index = i;
			s->raw[i].attr.attr.name = s->data.properties[i].key;
			s->raw[i].attr.attr.mode = 0444;
			s->raw[i].attr.show = raw_show;
			s->raw_attrs[i] = &s->raw[i].attr.attr;
		}
		s->raw_attrs[d->count] = NULL;
		s->raw_group.name = "remote";
		s->raw_group.attrs = s->raw_attrs;
		s->groups[0] = &s->raw_group;
		config.attr_grp = s->groups;
		s->psy = power_supply_register(nd_misc.this_device, &s->desc, &config);
		if (IS_ERR(s->psy)) {
			ret = PTR_ERR(s->psy);
			s->psy = NULL;
			goto out;
		}
	}
	power_supply_changed(s->psy);
	mod_delayed_work(system_wq, &s->expiry, msecs_to_jiffies(15000));
	ret = size;
out:
	mutex_unlock(&s->write_lock);
	kvfree(d);
	return ret;
}

static int nd_open(struct inode *inode, struct file *file)
{
	struct nd_supply *s;
	if (atomic_inc_return(&open_count) > 512) {
		atomic_dec(&open_count);
		return -EMFILE;
	}
	s = kvzalloc(sizeof(*s), GFP_KERNEL);
	if (!s) {
		atomic_dec(&open_count);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&s->expiry, expire_supply);
	mutex_init(&s->write_lock);
	mutex_init(&s->data_lock);
	file->private_data = s;
	return nonseekable_open(inode, file);
}
static int nd_release(struct inode *inode, struct file *file)
{
	struct nd_supply *s = file->private_data;
	cancel_delayed_work_sync(&s->expiry);
	if (s->psy)
		power_supply_unregister(s->psy);
	kvfree(s);
	atomic_dec(&open_count);
	return 0;
}
static const struct file_operations nd_fops = {
    .owner = THIS_MODULE,
    .open = nd_open,
    .write = nd_write,
    .release = nd_release,
};
static struct miscdevice nd_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "netdisplay-power",
    .fops = &nd_fops,
    .mode = 0600,
};
module_misc_device(nd_misc);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("NetDisplay contributors");
MODULE_DESCRIPTION("Read-only, device-scoped remote NetDisplay batteries and chargers");
MODULE_VERSION("0.1.0");
