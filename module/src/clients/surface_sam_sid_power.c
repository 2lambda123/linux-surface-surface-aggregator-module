// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Surface SID Battery/AC Driver.
 * Provides support for the battery and AC on 7th generation Surface devices.
 */

#include <asm/unaligned.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>

#include <linux/surface_aggregator_module.h>
#include "surface_sam_sid_power.h"

#define SPWR_WARN	KERN_WARNING KBUILD_MODNAME ": "
#define SPWR_DEBUG	KERN_DEBUG KBUILD_MODNAME ": "


// TODO: check BIX/BST for unknown/unsupported 0xffffffff entries
// TODO: DPTF (/SAN notifications)?
// TODO: other properties?


static unsigned int cache_time = 1000;
module_param(cache_time, uint, 0644);
MODULE_PARM_DESC(cache_time, "battery state chaching time in milliseconds [default: 1000]");

#define SPWR_AC_BAT_UPDATE_DELAY	msecs_to_jiffies(5000)


/*
 * SAM Interface.
 */

#define SAM_EVENT_PWR_CID_BIX		0x15
#define SAM_EVENT_PWR_CID_BST		0x16
#define SAM_EVENT_PWR_CID_ADAPTER	0x17

#define SAM_BATTERY_STA_OK		0x0f
#define SAM_BATTERY_STA_PRESENT		0x10

#define SAM_BATTERY_STATE_DISCHARGING	0x01
#define SAM_BATTERY_STATE_CHARGING	0x02
#define SAM_BATTERY_STATE_CRITICAL	0x04

#define SAM_BATTERY_POWER_UNIT_MA	1


/* Equivalent to data returned in ACPI _BIX method */
struct spwr_bix {
	u8  revision;
	__le32 power_unit;
	__le32 design_cap;
	__le32 last_full_charge_cap;
	__le32 technology;
	__le32 design_voltage;
	__le32 design_cap_warn;
	__le32 design_cap_low;
	__le32 cycle_count;
	__le32 measurement_accuracy;
	__le32 max_sampling_time;
	__le32 min_sampling_time;
	__le32 max_avg_interval;
	__le32 min_avg_interval;
	__le32 bat_cap_granularity_1;
	__le32 bat_cap_granularity_2;
	u8  model[21];
	u8  serial[11];
	u8  type[5];
	u8  oem_info[21];
} __packed;

/* Equivalent to data returned in ACPI _BST method */
struct spwr_bst {
	__le32 state;
	__le32 present_rate;
	__le32 remaining_cap;
	__le32 present_voltage;
} __packed;

/* DPTF event payload */
struct spwr_event_dptf {
	__le32 pmax;
	__le32 _1;		/* currently unknown */
	__le32 _2;		/* currently unknown */
} __packed;


/* Get battery status (_STA) */
static SSAM_DEFINE_SYNC_REQUEST_MD_R(ssam_bat_get_sta, __le32, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x01,
});

/* Get battery static information (_BIX) */
static SSAM_DEFINE_SYNC_REQUEST_MD_R(ssam_bat_get_bix, struct spwr_bix, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x02,
});

/* Get battery dynamic information (_BST) */
static SSAM_DEFINE_SYNC_REQUEST_MD_R(ssam_bat_get_bst, struct spwr_bst, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x03,
});

/* Set battery trip point (_BTP) */
static SSAM_DEFINE_SYNC_REQUEST_MD_W(ssam_bat_set_btp, __le32, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x04,
});

/* Get platform power soruce for battery (DPTF PSRC) */
static SSAM_DEFINE_SYNC_REQUEST_MD_R(ssam_bat_get_psrc, __le32, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x0d,
});

/* Get maximum platform power for battery (DPTF PMAX) */
__always_unused
static SSAM_DEFINE_SYNC_REQUEST_MD_R(ssam_bat_get_pmax, __le32, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x0b,
});

/* Get adapter rating (DPTF ARTG) */
__always_unused
static SSAM_DEFINE_SYNC_REQUEST_MD_R(ssam_bat_get_artg, __le32, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x0f,
});

/* Unknown (DPTF PSOC) */
__always_unused
static SSAM_DEFINE_SYNC_REQUEST_MD_R(ssam_bat_get_psoc, __le32, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x0c,
});

/* Unknown (DPTF CHGI/ INT3403 SPPC) */
__always_unused
static SSAM_DEFINE_SYNC_REQUEST_MD_W(ssam_bat_set_chgi, __le32, {
	.target_category = SSAM_SSH_TC_BAT,
	.command_id      = 0x0e,
});


/*
 * Common Power-Subsystem Interface.
 */

struct spwr_battery_device {
	struct platform_device *pdev;
	struct ssam_controller *ctrl;
	const struct ssam_battery_properties *p;

	char name[32];
	struct power_supply *psy;
	struct power_supply_desc psy_desc;

	struct delayed_work update_work;

	struct ssam_event_notifier notif;

	struct mutex lock;
	unsigned long timestamp;

	__le32 sta;
	struct spwr_bix bix;
	struct spwr_bst bst;
	u32 alarm;
};

struct spwr_ac_device {
	struct platform_device *pdev;
	struct ssam_controller *ctrl;

	char name[32];
	struct power_supply *psy;
	struct power_supply_desc psy_desc;

	struct ssam_event_notifier notif;

	struct mutex lock;

	__le32 state;
};

static enum power_supply_property spwr_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static enum power_supply_property spwr_battery_props_chg[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static enum power_supply_property spwr_battery_props_eng[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};


static int spwr_battery_register(struct spwr_battery_device *bat,
				 struct platform_device *pdev,
				 struct ssam_controller *ctrl,
				 const struct ssam_battery_properties *p);

static void spwr_battery_unregister(struct spwr_battery_device *bat);


static inline bool spwr_battery_present(struct spwr_battery_device *bat)
{
	return le32_to_cpu(bat->sta) & SAM_BATTERY_STA_PRESENT;
}


static inline int spwr_battery_load_sta(struct spwr_battery_device *bat)
{
	return ssam_bat_get_sta(bat->ctrl, bat->p->channel, bat->p->instance,
				&bat->sta);
}

static inline int spwr_battery_load_bix(struct spwr_battery_device *bat)
{
	if (!spwr_battery_present(bat))
		return 0;

	return ssam_bat_get_bix(bat->ctrl, bat->p->channel, bat->p->instance,
				&bat->bix);
}

static inline int spwr_battery_load_bst(struct spwr_battery_device *bat)
{
	if (!spwr_battery_present(bat))
		return 0;

	return ssam_bat_get_bst(bat->ctrl, bat->p->channel, bat->p->instance,
				&bat->bst);
}


static inline int spwr_battery_set_alarm_unlocked(struct spwr_battery_device *bat, u32 value)
{
	__le32 alarm = cpu_to_le32(value);

	bat->alarm = value;
	return ssam_bat_set_btp(bat->ctrl, bat->p->channel, bat->p->instance,
				&alarm);
}

static inline int spwr_battery_set_alarm(struct spwr_battery_device *bat, u32 value)
{
	int status;

	mutex_lock(&bat->lock);
	status = spwr_battery_set_alarm_unlocked(bat, value);
	mutex_unlock(&bat->lock);

	return status;
}

static inline int spwr_battery_update_bst_unlocked(struct spwr_battery_device *bat, bool cached)
{
	unsigned long cache_deadline = bat->timestamp + msecs_to_jiffies(cache_time);
	int status;

	if (cached && bat->timestamp && time_is_after_jiffies(cache_deadline))
		return 0;

	status = spwr_battery_load_sta(bat);
	if (status)
		return status;

	status = spwr_battery_load_bst(bat);
	if (status)
		return status;

	bat->timestamp = jiffies;
	return 0;
}

static int spwr_battery_update_bst(struct spwr_battery_device *bat, bool cached)
{
	int status;

	mutex_lock(&bat->lock);
	status = spwr_battery_update_bst_unlocked(bat, cached);
	mutex_unlock(&bat->lock);

	return status;
}

static inline int spwr_battery_update_bix_unlocked(struct spwr_battery_device *bat)
{
	int status;

	status = spwr_battery_load_sta(bat);
	if (status)
		return status;

	status = spwr_battery_load_bix(bat);
	if (status)
		return status;

	status = spwr_battery_load_bst(bat);
	if (status)
		return status;

	bat->timestamp = jiffies;
	return 0;
}

static int spwr_battery_update_bix(struct spwr_battery_device *bat)
{
	int status;

	mutex_lock(&bat->lock);
	status = spwr_battery_update_bix_unlocked(bat);
	mutex_unlock(&bat->lock);

	return status;
}

static inline int spwr_ac_update_unlocked(struct spwr_ac_device *ac)
{
	return ssam_bat_get_psrc(ac->ctrl, 0x01, 0x01, &ac->state);
}

static int spwr_ac_update(struct spwr_ac_device *ac)
{
	int status;

	mutex_lock(&ac->lock);
	status = spwr_ac_update_unlocked(ac);
	mutex_unlock(&ac->lock);

	return status;
}


static int spwr_battery_recheck(struct spwr_battery_device *bat)
{
	bool present = spwr_battery_present(bat);
	u32 unit = get_unaligned_le32(&bat->bix.power_unit);
	int status;

	status = spwr_battery_update_bix(bat);
	if (status)
		return status;

	// if battery has been attached, (re-)initialize alarm
	if (!present && spwr_battery_present(bat)) {
		u32 cap_warn = get_unaligned_le32(&bat->bix.design_cap_warn);
		status = spwr_battery_set_alarm(bat, cap_warn);
		if (status)
			return status;
	}

	// if the unit has changed, re-add the battery
	if (unit != get_unaligned_le32(&bat->bix.power_unit)) {
		spwr_battery_unregister(bat);
		status = spwr_battery_register(bat, bat->pdev, bat->ctrl, bat->p);
	}

	return status;
}


static inline int spwr_notify_bix(struct spwr_battery_device *bat)
{
	int status;

	status = spwr_battery_recheck(bat);
	if (!status)
		power_supply_changed(bat->psy);

	return status;
}

static inline int spwr_notify_bst(struct spwr_battery_device *bat)
{
	int status;

	status = spwr_battery_update_bst(bat, false);
	if (!status)
		power_supply_changed(bat->psy);

	return status;
}

static inline int spwr_notify_adapter_bat(struct spwr_battery_device *bat)
{
	u32 last_full_cap = get_unaligned_le32(&bat->bix.last_full_charge_cap);
	u32 remaining_cap = get_unaligned_le32(&bat->bst.remaining_cap);

	/*
	 * Handle battery update quirk:
	 * When the battery is fully charged and the adapter is plugged in or
	 * removed, the EC does not send a separate event for the state
	 * (charging/discharging) change. Furthermore it may take some time until
	 * the state is updated on the battery. Schedule an update to solve this.
	 */

	if (remaining_cap >= last_full_cap)
		schedule_delayed_work(&bat->update_work, SPWR_AC_BAT_UPDATE_DELAY);

	return 0;
}

static inline int spwr_notify_adapter_ac(struct spwr_ac_device *ac)
{
	int status;

	status = spwr_ac_update(ac);
	if (!status)
		power_supply_changed(ac->psy);

	return status;
}

static u32 spwr_notify_bat(struct ssam_notifier_block *nb, const struct ssam_event *event)
{
	struct spwr_battery_device *bat = container_of(nb, struct spwr_battery_device, notif.base);
	int status;

	dev_dbg(&bat->pdev->dev, "power event (cid = 0x%02x, iid = %d, chn = %d)\n",
		event->command_id, event->instance_id, event->channel);

	// handled here, needs to be handled for all channels/instances
	if (event->command_id == SAM_EVENT_PWR_CID_ADAPTER) {
		status = spwr_notify_adapter_bat(bat);
		return ssam_notifier_from_errno(status) | SSAM_NOTIF_HANDLED;
	}

	// check for the correct channel and instance ID
	if (event->channel != bat->p->channel)
		return 0;

	if (event->instance_id != bat->p->instance)
		return 0;

	switch (event->command_id) {
	case SAM_EVENT_PWR_CID_BIX:
		status = spwr_notify_bix(bat);
		break;

	case SAM_EVENT_PWR_CID_BST:
		status = spwr_notify_bst(bat);
		break;

	default:
		return 0;
	}

	return ssam_notifier_from_errno(status) | SSAM_NOTIF_HANDLED;
}

static u32 spwr_notify_ac(struct ssam_notifier_block *nb, const struct ssam_event *event)
{
	struct spwr_ac_device *ac = container_of(nb, struct spwr_ac_device, notif.base);
	int status;

	dev_dbg(&ac->pdev->dev, "power event (cid = 0x%02x, iid = %d, chn = %d)\n",
		event->command_id, event->instance_id, event->channel);

	// AC has IID = 0
	if (event->instance_id != 0)
		return 0;

	switch (event->command_id) {
	case SAM_EVENT_PWR_CID_ADAPTER:
		status = spwr_notify_adapter_ac(ac);
		return ssam_notifier_from_errno(status) | SSAM_NOTIF_HANDLED;

	default:
		return 0;
	}
}

static void spwr_battery_update_bst_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct spwr_battery_device *bat = container_of(dwork, struct spwr_battery_device, update_work);
	int status;

	status = spwr_battery_update_bst(bat, false);
	if (!status)
		power_supply_changed(bat->psy);

	if (status)
		dev_err(&bat->pdev->dev, "failed to update battery state: %d\n", status);
}


static inline int spwr_battery_prop_status(struct spwr_battery_device *bat)
{
	u32 state = get_unaligned_le32(&bat->bst.state);
	u32 last_full_cap = get_unaligned_le32(&bat->bix.last_full_charge_cap);
	u32 remaining_cap = get_unaligned_le32(&bat->bst.remaining_cap);
	u32 present_rate = get_unaligned_le32(&bat->bst.present_rate);

	if (state & SAM_BATTERY_STATE_DISCHARGING)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	if (state & SAM_BATTERY_STATE_CHARGING)
		return POWER_SUPPLY_STATUS_CHARGING;

	if (last_full_cap == remaining_cap)
		return POWER_SUPPLY_STATUS_FULL;

	if (present_rate == 0)
		return POWER_SUPPLY_STATUS_NOT_CHARGING;

	return POWER_SUPPLY_STATUS_UNKNOWN;
}

static inline int spwr_battery_prop_technology(struct spwr_battery_device *bat)
{
	if (!strcasecmp("NiCd", bat->bix.type))
		return POWER_SUPPLY_TECHNOLOGY_NiCd;

	if (!strcasecmp("NiMH", bat->bix.type))
		return POWER_SUPPLY_TECHNOLOGY_NiMH;

	if (!strcasecmp("LION", bat->bix.type))
		return POWER_SUPPLY_TECHNOLOGY_LION;

	if (!strncasecmp("LI-ION", bat->bix.type, 6))
		return POWER_SUPPLY_TECHNOLOGY_LION;

	if (!strcasecmp("LiP", bat->bix.type))
		return POWER_SUPPLY_TECHNOLOGY_LIPO;

	return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
}

static inline int spwr_battery_prop_capacity(struct spwr_battery_device *bat)
{
	u32 last_full_cap = get_unaligned_le32(&bat->bix.last_full_charge_cap);
	u32 remaining_cap = get_unaligned_le32(&bat->bst.remaining_cap);

	if (remaining_cap && last_full_cap)
		return remaining_cap * 100 / last_full_cap;
	else
		return 0;
}

static inline int spwr_battery_prop_capacity_level(struct spwr_battery_device *bat)
{
	u32 state = get_unaligned_le32(&bat->bst.state);
	u32 last_full_cap = get_unaligned_le32(&bat->bix.last_full_charge_cap);
	u32 remaining_cap = get_unaligned_le32(&bat->bst.remaining_cap);

	if (state & SAM_BATTERY_STATE_CRITICAL)
		return POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;

	if (remaining_cap >= last_full_cap)
		return POWER_SUPPLY_CAPACITY_LEVEL_FULL;

	if (remaining_cap <= bat->alarm)
		return POWER_SUPPLY_CAPACITY_LEVEL_LOW;

	return POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
}

static int spwr_ac_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct spwr_ac_device *ac = power_supply_get_drvdata(psy);
	int status;

	mutex_lock(&ac->lock);

	status = spwr_ac_update_unlocked(ac);
	if (status)
		goto out;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = le32_to_cpu(ac->state) == 1;
		break;

	default:
		status = -EINVAL;
		goto out;
	}

out:
	mutex_unlock(&ac->lock);
	return status;
}

static int spwr_battery_get_property(struct power_supply *psy,
				     enum power_supply_property psp,
				     union power_supply_propval *val)
{
	struct spwr_battery_device *bat = power_supply_get_drvdata(psy);
	int status;

	mutex_lock(&bat->lock);

	status = spwr_battery_update_bst_unlocked(bat, true);
	if (status)
		goto out;

	// abort if battery is not present
	if (!spwr_battery_present(bat) && psp != POWER_SUPPLY_PROP_PRESENT) {
		status = -ENODEV;
		goto out;
	}

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = spwr_battery_prop_status(bat);
		break;

	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = spwr_battery_present(bat);
		break;

	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = spwr_battery_prop_technology(bat);
		break;

	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = get_unaligned_le32(&bat->bix.cycle_count);
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_MIN_DESIGN:
		val->intval = get_unaligned_le32(&bat->bix.design_voltage)
			      * 1000;
		break;

	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = get_unaligned_le32(&bat->bst.present_voltage)
			      * 1000;
		break;

	case POWER_SUPPLY_PROP_CURRENT_NOW:
	case POWER_SUPPLY_PROP_POWER_NOW:
		val->intval = get_unaligned_le32(&bat->bst.present_rate) * 1000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		val->intval = get_unaligned_le32(&bat->bix.design_cap) * 1000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_FULL:
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		val->intval = get_unaligned_le32(&bat->bix.last_full_charge_cap)
			      * 1000;
		break;

	case POWER_SUPPLY_PROP_CHARGE_NOW:
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		val->intval = get_unaligned_le32(&bat->bst.remaining_cap)
			      * 1000;
		break;

	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = spwr_battery_prop_capacity(bat);
		break;

	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = spwr_battery_prop_capacity_level(bat);
		break;

	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = bat->bix.model;
		break;

	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = bat->bix.oem_info;
		break;

	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = bat->bix.serial;
		break;

	default:
		status = -EINVAL;
		goto out;
	}

out:
	mutex_unlock(&bat->lock);
	return status;
}


static ssize_t spwr_battery_alarm_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct spwr_battery_device *bat = power_supply_get_drvdata(psy);

	return sprintf(buf, "%d\n", bat->alarm * 1000);
}

static ssize_t spwr_battery_alarm_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct spwr_battery_device *bat = power_supply_get_drvdata(psy);
	unsigned long value;
	int status;

	status = kstrtoul(buf, 0, &value);
	if (status)
		return status;

	if (!spwr_battery_present(bat))
		return -ENODEV;

	status = spwr_battery_set_alarm(bat, value / 1000);
	if (status)
		return status;

	return count;
}

static const struct device_attribute alarm_attr = {
	.attr = {.name = "alarm", .mode = 0644},
	.show = spwr_battery_alarm_show,
	.store = spwr_battery_alarm_store,
};


static int spwr_ac_register(struct spwr_ac_device *ac,
			    struct platform_device *pdev,
			    struct ssam_controller *ctrl)
{
	struct power_supply_config psy_cfg = {};
	__le32 sta;
	int status;

	// make sure the device is there and functioning properly
	status = ssam_bat_get_sta(ctrl, 0x01, 0x01, &sta);
	if (status)
		return status;

	if ((le32_to_cpu(sta) & SAM_BATTERY_STA_OK) != SAM_BATTERY_STA_OK)
		return -ENODEV;

	psy_cfg.drv_data = ac;

	ac->pdev = pdev;
	ac->ctrl = ctrl;
	mutex_init(&ac->lock);

	snprintf(ac->name, ARRAY_SIZE(ac->name), "ADP0");

	ac->psy_desc.name = ac->name;
	ac->psy_desc.type = POWER_SUPPLY_TYPE_MAINS;
	ac->psy_desc.properties = spwr_ac_props;
	ac->psy_desc.num_properties = ARRAY_SIZE(spwr_ac_props);
	ac->psy_desc.get_property = spwr_ac_get_property;

	ac->psy = power_supply_register(&ac->pdev->dev, &ac->psy_desc, &psy_cfg);
	if (IS_ERR(ac->psy)) {
		status = PTR_ERR(ac->psy);
		goto err_psy;
	}

	ac->notif.base.priority = 1;
	ac->notif.base.fn = spwr_notify_ac;
	ac->notif.event.reg = SSAM_EVENT_REGISTRY_SAM;
	ac->notif.event.id.target_category = SSAM_SSH_TC_BAT;
	ac->notif.event.id.instance = 0;
	ac->notif.event.flags = SSAM_EVENT_SEQUENCED;

	status = ssam_notifier_register(ctrl, &ac->notif);
	if (status)
		goto err_notif;

	return 0;

err_notif:
	power_supply_unregister(ac->psy);
err_psy:
	mutex_destroy(&ac->lock);
	return status;
}

static int spwr_ac_unregister(struct spwr_ac_device *ac)
{
	ssam_notifier_unregister(ac->ctrl, &ac->notif);
	power_supply_unregister(ac->psy);
	mutex_destroy(&ac->lock);
	return 0;
}

static int spwr_battery_register(struct spwr_battery_device *bat,
				 struct platform_device *pdev,
				 struct ssam_controller *ctrl,
				 const struct ssam_battery_properties *p)
{
	struct power_supply_config psy_cfg = {};
	__le32 sta;
	int status;

	bat->pdev = pdev;
	bat->ctrl = ctrl;
	bat->p = p;

	// make sure the device is there and functioning properly
	status = ssam_bat_get_sta(ctrl, bat->p->channel, bat->p->instance, &sta);
	if (status)
		return status;

	if ((le32_to_cpu(sta) & SAM_BATTERY_STA_OK) != SAM_BATTERY_STA_OK)
		return -ENODEV;

	status = spwr_battery_update_bix_unlocked(bat);
	if (status)
		return status;

	if (spwr_battery_present(bat)) {
		u32 cap_warn = get_unaligned_le32(&bat->bix.design_cap_warn);
		status = spwr_battery_set_alarm_unlocked(bat, cap_warn);
		if (status)
			return status;
	}

	snprintf(bat->name, ARRAY_SIZE(bat->name), "BAT%d", bat->p->num);
	bat->psy_desc.name = bat->name;
	bat->psy_desc.type = POWER_SUPPLY_TYPE_BATTERY;

	if (get_unaligned_le32(&bat->bix.power_unit) == SAM_BATTERY_POWER_UNIT_MA) {
		bat->psy_desc.properties = spwr_battery_props_chg;
		bat->psy_desc.num_properties = ARRAY_SIZE(spwr_battery_props_chg);
	} else {
		bat->psy_desc.properties = spwr_battery_props_eng;
		bat->psy_desc.num_properties = ARRAY_SIZE(spwr_battery_props_eng);
	}

	bat->psy_desc.get_property = spwr_battery_get_property;

	mutex_init(&bat->lock);
	psy_cfg.drv_data = bat;

	INIT_DELAYED_WORK(&bat->update_work, spwr_battery_update_bst_workfn);

	bat->psy = power_supply_register(&bat->pdev->dev, &bat->psy_desc, &psy_cfg);
	if (IS_ERR(bat->psy)) {
		status = PTR_ERR(bat->psy);
		goto err_psy;
	}

	bat->notif.base.priority = 1;
	bat->notif.base.fn = spwr_notify_bat;
	bat->notif.event.reg = p->registry;
	bat->notif.event.id.target_category = SSAM_SSH_TC_BAT;
	bat->notif.event.id.instance = 0;
	bat->notif.event.flags = SSAM_EVENT_SEQUENCED;

	status = ssam_notifier_register(ctrl, &bat->notif);
	if (status)
		goto err_notif;

	status = device_create_file(&bat->psy->dev, &alarm_attr);
	if (status)
		goto err_file;

	return 0;

err_file:
	ssam_notifier_unregister(ctrl, &bat->notif);
err_notif:
	power_supply_unregister(bat->psy);
err_psy:
	mutex_destroy(&bat->lock);
	return status;
}

static void spwr_battery_unregister(struct spwr_battery_device *bat)
{
	ssam_notifier_unregister(bat->ctrl, &bat->notif);
	cancel_delayed_work_sync(&bat->update_work);
	device_remove_file(&bat->psy->dev, &alarm_attr);
	power_supply_unregister(bat->psy);
	mutex_destroy(&bat->lock);
}


/*
 * Battery Driver.
 */

#ifdef CONFIG_PM_SLEEP
static int surface_sam_sid_battery_resume(struct device *dev)
{
	struct spwr_battery_device *bat;

	bat = dev_get_drvdata(dev);
	return spwr_battery_recheck(bat);
}
#else
#define surface_sam_sid_battery_resume NULL
#endif

SIMPLE_DEV_PM_OPS(surface_sam_sid_battery_pm, NULL, surface_sam_sid_battery_resume);

static int surface_sam_sid_battery_probe(struct platform_device *pdev)
{
	struct spwr_battery_device *bat;
	struct ssam_controller *ctrl;
	int status;

	// link to ec
	status = ssam_client_bind(&pdev->dev, &ctrl);
	if (status)
		return status == -ENXIO ? -EPROBE_DEFER : status;

	bat = devm_kzalloc(&pdev->dev, sizeof(struct spwr_battery_device), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	platform_set_drvdata(pdev, bat);
	return spwr_battery_register(bat, pdev, ctrl, pdev->dev.platform_data);
}

static int surface_sam_sid_battery_remove(struct platform_device *pdev)
{
	struct spwr_battery_device *bat;

	bat = platform_get_drvdata(pdev);
	spwr_battery_unregister(bat);

	return 0;
}

static struct platform_driver surface_sam_sid_battery = {
	.probe = surface_sam_sid_battery_probe,
	.remove = surface_sam_sid_battery_remove,
	.driver = {
		.name = "surface_sam_sid_battery",
		.pm = &surface_sam_sid_battery_pm,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};


/*
 * AC Driver.
 */

static int surface_sam_sid_ac_probe(struct platform_device *pdev)
{
	struct spwr_ac_device *ac;
	struct ssam_controller *ctrl;
	int status;

	// link to ec
	status = ssam_client_bind(&pdev->dev, &ctrl);
	if (status)
		return status == -ENXIO ? -EPROBE_DEFER : status;

	ac = devm_kzalloc(&pdev->dev, sizeof(struct spwr_ac_device), GFP_KERNEL);
	if (!ac)
		return -ENOMEM;

	status = spwr_ac_register(ac, pdev, ctrl);
	if (status)
		return status;

	platform_set_drvdata(pdev, ac);
	return 0;
}

static int surface_sam_sid_ac_remove(struct platform_device *pdev)
{
	struct spwr_ac_device *ac;

	ac = platform_get_drvdata(pdev);
	return spwr_ac_unregister(ac);
}

static struct platform_driver surface_sam_sid_ac = {
	.probe = surface_sam_sid_ac_probe,
	.remove = surface_sam_sid_ac_remove,
	.driver = {
		.name = "surface_sam_sid_ac",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
};


static int __init surface_sam_sid_power_init(void)
{
	int status;

	status = platform_driver_register(&surface_sam_sid_battery);
	if (status)
		return status;

	status = platform_driver_register(&surface_sam_sid_ac);
	if (status) {
		platform_driver_unregister(&surface_sam_sid_battery);
		return status;
	}

	return 0;
}

static void __exit surface_sam_sid_power_exit(void)
{
	platform_driver_unregister(&surface_sam_sid_battery);
	platform_driver_unregister(&surface_sam_sid_ac);
}

module_init(surface_sam_sid_power_init);
module_exit(surface_sam_sid_power_exit);

MODULE_AUTHOR("Maximilian Luz <luzmaximilian@gmail.com>");
MODULE_DESCRIPTION("Surface Battery/AC Driver for 7th Generation Surface Devices");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:surface_sam_sid_ac");
MODULE_ALIAS("platform:surface_sam_sid_battery");