/*
 * Force feedback support for XBOX ONE S and X gamepads via Bluetooth
 *
 * This driver was developed for a student project at fortiss GmbH in Munich.
 * Copyright (c) 2017 Florian Dollinger <dollinger.florian@gmx.de>
 */

/* TODO:
 * - jstest shows at startup the maximum/minimum value,
 *   not the value that corresponds to the "default" position, why?
 * - https: //www.kernel.org/doc/html/v4.10/process/coding-style.html
 * - a lot of more, search for TODO in the code (you can't overlook xD)
 */

#include <linux/hid.h>
#include <linux/power_supply.h>
#include <linux/input.h>	/* ff_memless(), ... */
#include <linux/module.h>	/* MODULE_*, module_*, ... */
#include <linux/slab.h>		/* kzalloc(), kfree(), ... */
#include <linux/delay.h>	/* mdelay(), ... */
#include "hid-ids.h"		/* VENDOR_ID... */


#define DEBUG

/* Module Information */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Florian Dollinger <dollinger.florian@gmx.de>");
MODULE_DESCRIPTION("Linux Kernel driver for XBOX ONE S and X gamepads (bluetooth), including force-feedback");
MODULE_VERSION("0.1.3");


/* Module Parameters, located at /sys/module/.../parameters */
#ifdef DEBUG
static u8 debug_level;
module_param(debug_level, byte, 0644);
MODULE_PARM_DESC(debug_level, "(u8) Debug information level: 0 (none) to 3+ (most verbose).");
#endif

static bool dpad_to_buttons;
module_param(dpad_to_buttons, bool, 0644);
MODULE_PARM_DESC(dpad_to_buttons, "(bool) Map the DPAD-buttons as BTN_DPAD_UP/RIGHT/DOWN/LEFT instead of as a hat-switch. Restart device to take effect.");


/*
 * Debug Printk
 *
 * Prints a debug message to kernel (dmesg)
 * only if both is true, this is a DEBUG version and the
 * debug_level-parameter is equal or higher than the level
 * specified in hid_dbg_lvl
 */

#define DBG_LVL_NONE 0
#define DBG_LVL_FEW  1
#define DBG_LVL_SOME 2
#define DBG_LVL_ALL  3

#ifdef DEBUG
#define hid_dbg_lvl(lvl, fmt_hdev, fmt_str, ...) \
	do { \
		if (debug_level >= lvl) hid_printk(KERN_DEBUG, pr_fmt(fmt_hdev), pr_fmt(fmt_str), ##__VA_ARGS__); \
	} while (0)
#define dbg_hex_dump_lvl(lvl, fmt_prefix, data, size) \
	do { \
		if (debug_level >= lvl) print_hex_dump(KERN_DEBUG, pr_fmt(fmt_prefix), DUMP_PREFIX_NONE, 32, 1, data, size, false); \
	} while (0)
#else
#define hid_dbg_lvl(lvl, fmt_hdev, fmt_str, ...) \
		no_printk(KERN_DEBUG pr_fmt(fmt_str), ##__VA_ARGS__)
#define dbg_hex_dump_lvl(lvl, fmt_prefix, data, size) \
		no_printk(KERN_DEBUG pr_fmt(fmt_prefix))
#endif


/*
 * FF Output Report
 *
 * This is the structure for the rumble output report. For more information
 * about this structure please take a look in the hid-report description.
 * Please notice that the structs are __packed, therefore there is no "padding"
 * between the elements (they behave more like an array).
 *
 * TODO:
 * Use sth. which is aware of the endianess, i.e. __le16 and __le16_to_cpu()
 */

#define FF_ENABLE_RMBL_LEFT  0x02
#define FF_ENABLE_RMBL_RIGHT 0x01

struct ff_data {
	u8 enable_actuators;
	u8 reserved[2];
	u8 magnitude_left;
	u8 magnitude_right;
	u8 duration;
	u8 start_delay;
	u8 loop_count;
} __packed;

struct ff_report {
	u8 report_id;
	struct ff_data ff;
} __packed;

/* static variables are zeroed => empty initialization struct */
static const struct ff_data ff_clear;


/*
 * Device Data
 *
 * We attach information to hdev, which is therefore nearly globally accessible
 * via hid_get_drvdata(hdev). It is attached to the hid_device via
 * hid_set_drvdata(hdev) at the probing function.
 */

enum report_type {
	UNKNOWN,
	LINUX,
	WINDOWS
};

struct xpadneo_devdata {
	/* mutual exclusion */
	spinlock_t lock;

	/* devices */
	struct hid_device *hdev;
	struct input_dev *idev;

	/* report types */
	enum report_type report_descriptor;
	enum report_type report_behaviour;

	/* battery information */
	struct power_supply *batt;
	struct power_supply_desc battery_desc;
	u8 cable_state;
	u8 capacity_level;
};


/*
 * Force Feedback Callback
 *
 * This function is called by the Input Subsystem.
 * The effect data is set in userspace and sent to the driver via ioctl.
 */

static int xpadneo_ff_play(struct input_dev *dev, void *data,
	struct ff_effect *effect)
{
	/* Q: where is drvdata set to hid_device?
	 * A: hid_hw_start (called in probe)
	 *    -> hid_connect -> hidinput_connect
	 *    -> hidinput_allocate (sets drvdata to hid_device)
	 */
	struct hid_device *hdev = input_get_drvdata(dev);
	struct ff_report ff_package;
	u16 weak, strong;

	/* we have to _copy_ the effect values, otherwise we cannot print them
	 * (kernel oops: unable to handle kernel paging request)
	 */
	weak = effect->u.rumble.weak_magnitude;
	strong = effect->u.rumble.strong_magnitude;

	hid_dbg_lvl(DBG_LVL_FEW, hdev,
		"playing effect: strong: %#04x, weak: %#04x\n", strong, weak
	);


	ff_package.ff                  = ff_clear;
	ff_package.report_id           = 0x03;
	ff_package.ff.enable_actuators = FF_ENABLE_RMBL_RIGHT | FF_ENABLE_RMBL_LEFT;
	ff_package.ff.magnitude_right  = (u8)((weak & 0xFF00) >> 8);
	ff_package.ff.magnitude_left   = (u8)((strong & 0xFF00) >> 8);

	/* It is up to the Input-Subsystem to start and stop the effect as needed.
	 * All WE need to do is to play the effect at least 32767 ms long.
	 * Take a look here:
	 * https://stackoverflow.com/questions/48034091/ff-replay-substructure-in-ff-effect-empty/48043342#48043342
	 * We therefore simply play the effect as long as possible, which is
	 * 2, 55s * 255 = 650, 25s ~ = 10min
	 */
	ff_package.ff.duration   = 0xFF;
	ff_package.ff.loop_count = 0xFF;
	hid_hw_output_report(hdev, (u8 *)&ff_package, sizeof(ff_package));

	return 0;
}


/*
 * Device (Gamepad) Initialization
 */

static int xpadneo_initDevice(struct hid_device *hdev)
{
	int error;

	/* Create handle to the input device which is assigned to the hid device
	 * TODO: replace by get_drvdata
	 */
	struct hid_input *hidinput = list_entry(hdev->inputs.next, struct hid_input, list);
	struct input_dev *idev      = hidinput->input;

	struct ff_report ff_package;

	/* TODO: outsource that */

	/* 'HELLO' FROM THE OTHER SIDE */
	ff_package.ff                  = ff_clear;
	ff_package.report_id           = 0x03;
	ff_package.ff.enable_actuators = FF_ENABLE_RMBL_RIGHT;
	ff_package.ff.magnitude_right  = 0x99;
	ff_package.ff.duration         = 50;
	hid_hw_output_report(hdev, (u8 *)&ff_package, sizeof(ff_package));

	mdelay(500);

	ff_package.ff                  = ff_clear;
	ff_package.report_id           = 0x03;
	ff_package.ff.enable_actuators = FF_ENABLE_RMBL_LEFT;
	ff_package.ff.magnitude_left   = 0x99;
	ff_package.ff.duration         = 50;
	hid_hw_output_report(hdev, (u8 *)&ff_package, sizeof(ff_package));


	/* Init Input System for Force Feedback (FF) */
	input_set_capability(idev, EV_FF, FF_RUMBLE);
	error = input_ff_create_memless(idev, NULL, xpadneo_ff_play);
	if (error)
		return error;


	/*
	 * Set default values, otherwise tools which depend on the joystick
	 * subsystem, report arbitrary values until the first real event
	 */
	input_report_abs(idev, ABS_X, 32768);
	input_report_abs(idev, ABS_Y, 32768);
	input_report_abs(idev, ABS_Z, 0);
	input_report_abs(idev, ABS_RX, 32768);
	input_report_abs(idev, ABS_RY, 32768);
	input_report_abs(idev, ABS_RZ, 0);
	input_report_key(idev, BTN_A, 0);
	input_report_key(idev, BTN_B, 0);
	input_report_key(idev, BTN_X, 0);
	input_report_key(idev, BTN_Y, 0);
	input_report_key(idev, BTN_TR, 0);
	input_report_key(idev, BTN_TL, 0);
	input_report_key(idev, BTN_THUMBL, 0);
	input_report_key(idev, BTN_THUMBR, 0);
	input_report_key(idev, BTN_START, 0);
	input_report_key(idev, BTN_MODE, 0);
	input_report_key(idev, ABS_HAT0X, 0);
	input_report_key(idev, ABS_HAT0Y, 0);
	input_sync(idev);

	/* TODO: - do not hardcode codes and values but
	 *         keep them in the mapping structures
	 *       - maybe initDevice isn't the right place
	 */

	return 0;
}


/* Callback function which return the available properties to userspace */
static int battery_get_property(struct power_supply *ps,
	enum power_supply_property property, union power_supply_propval *val)
{
	struct xpadneo_devdata *xdata = power_supply_get_drvdata(ps);
	unsigned long flags;
	u8 capacity_level, cable_state;

	spin_lock_irqsave(&xdata->lock, flags);
	capacity_level = xdata->capacity_level;
	cable_state    = xdata->cable_state;
	spin_unlock_irqrestore(&xdata->lock, flags);

	switch (property) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
		val->intval = capacity_level;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		switch (cable_state) {
		/* We use "FULL" as an indicator that the GP is plugged in */
		case 1: val->intval = POWER_SUPPLY_STATUS_FULL; break;
		case 0: val->intval = POWER_SUPPLY_STATUS_DISCHARGING; break;
		}
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int xpadneo_initBatt(struct hid_device *hdev)
{
	int ret;
	struct xpadneo_devdata *xdata = hid_get_drvdata(hdev);

	static enum power_supply_property battery_props[] = {
		/* is a power supply available? always true */
		POWER_SUPPLY_PROP_PRESENT,
		/* critical, low, normal, high, full */
		POWER_SUPPLY_PROP_CAPACITY_LEVEL,
		/* powers a specific device */
		POWER_SUPPLY_PROP_SCOPE,
		/* charging (full, plugged), not_charging */
		POWER_SUPPLY_PROP_STATUS
	};

	struct power_supply_config ps_config = {
		/* pass the xpadneo_data to the get_property function */
		.drv_data = xdata
	};


	xdata->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;

	/* Set up power supply */

	xdata->battery_desc.name = kasprintf(GFP_KERNEL,
					     "xpadneo_batt_%pMR", hdev->phys);
	if (!xdata->battery_desc.name)
		return -ENOMEM;
	xdata->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;

	/* Which properties of the battery are accessible? */
	xdata->battery_desc.properties = battery_props;
	xdata->battery_desc.num_properties = ARRAY_SIZE(battery_props);

	/*
	 * We have to offer a function which returns the current
	 * property values we defined above. Make sure that
	 * the get_property functions covers all properties above.
	 */
	xdata->battery_desc.get_property = battery_get_property;

	/* Advanced power management emulation */
	xdata->battery_desc.use_for_apm = 0;

	/* Register power supply for our gamepad device */
	xdata->batt = power_supply_register(&hdev->dev, &xdata->battery_desc, &ps_config);
	if (IS_ERR(xdata->batt)) {
		ret = PTR_ERR(xdata->batt);
		hid_err(hdev, "Unable to register battery device\n");
		goto err_free;
	}
	power_supply_powers(xdata->batt, &hdev->dev);


	hid_dbg_lvl(DBG_LVL_SOME, hdev, "power supply registered\n");

err_free:
	kfree(xdata->battery_desc.name);
	xdata->battery_desc.name = NULL;
	return ret;
}


enum mapping_behaviour {
	MAP_IGNORE, /* Completely ignore this field */
	MAP_AUTO,   /* Do not really map it, let hid-core decide */
	MAP_STATIC  /* Map to the values given */
};

struct input_ev {
	/* Map to which input event (EV_KEY, EV_ABS, ...)? */
	u8 event_type;
	/* Map to which input code (BTN_A, ABS_X, ...)? */
	u16 input_code;
};

u8 map_hid_to_input_windows(struct hid_usage *usage, struct input_ev *map_to)
{

	/*
	 * Windows report-descriptor (307 byte):
	 *
	 * 05 01 09 05 a1 01 85 01 09 01 a1 00 09 30 09 31 15 00 27 ff
	 * ff 00 00 95 02 75 10 81 02 c0 09 01 a1 00 09 33 09 34 15 00
	 * 27 ff ff 00 00 95 02 75 10 81 02 c0 05 01 09 32 15 00 26 ff
	 * 03 95 01 75 0a 81 02 15 00 25 00 75 06 95 01 81 03 05 01 09
	 * 35 15 00 26 ff 03 95 01 75 0a 81 02 15 00 25 00 75 06 95 01
	 * 81 03 05 01 09 39 15 01 25 08 35 00 46 3b 01 66 14 00 75 04
	 * 95 01 81 42 75 04 95 01 15 00 25 00 35 00 45 00 65 00 81 03
	 * 05 09 19 01 29 0a 15 00 25 01 75 01 95 0a 81 02 15 00 25 00
	 * 75 06 95 01 81 03 05 01 09 80 85 02 a1 00 09 85 15 00 25 01
	 * 95 01 75 01 81 02 15 00 25 00 75 07 95 01 81 03 c0 05 0f 09
	 * 21 85 03 a1 02 09 97 15 00 25 01 75 04 95 01 91 02 15 00 25
	 * 00 75 04 95 01 91 03 09 70 15 00 25 64 75 08 95 04 91 02 09
	 * 50 66 01 10 55 0e 15 00 26 ff 00 75 08 95 01 91 02 09 a7 15
	 * 00 26 ff 00 75 08 95 01 91 02 65 00 55 00 09 7c 15 00 26 ff
	 * 00 75 08 95 01 91 02 c0 85 04 05 06 09 20 15 00 26 ff 00 75
	 * 08 95 01 81 02 c0 00
	 */

	unsigned int hid_usage = usage->hid & HID_USAGE;
	unsigned int hid_usage_page = usage->hid & HID_USAGE_PAGE;

	switch (hid_usage_page) {
	case HID_UP_BUTTON:
		switch (hid_usage) {
		case 0x01: *map_to = (struct input_ev){EV_KEY, BTN_A};      return MAP_STATIC;
		case 0x02: *map_to = (struct input_ev){EV_KEY, BTN_B};      return MAP_STATIC;
		case 0x03: *map_to = (struct input_ev){EV_KEY, BTN_X};      return MAP_STATIC;
		case 0x04: *map_to = (struct input_ev){EV_KEY, BTN_Y};      return MAP_STATIC;
		case 0x05: *map_to = (struct input_ev){EV_KEY, BTN_TL};     return MAP_STATIC;
		case 0x06: *map_to = (struct input_ev){EV_KEY, BTN_TR};     return MAP_STATIC;
		case 0x07: *map_to = (struct input_ev){EV_KEY, BTN_SELECT}; return MAP_STATIC;
		case 0x08: *map_to = (struct input_ev){EV_KEY, BTN_START};  return MAP_STATIC;
		case 0x09: *map_to = (struct input_ev){EV_KEY, BTN_THUMBL}; return MAP_STATIC;
		case 0x0A: *map_to = (struct input_ev){EV_KEY, BTN_THUMBR}; return MAP_STATIC;
		}
	case HID_UP_GENDESK:
		switch (hid_usage) {
		case 0x30: *map_to = (struct input_ev){EV_ABS, ABS_X};    return MAP_STATIC;
		case 0x31: *map_to = (struct input_ev){EV_ABS, ABS_Y};    return MAP_STATIC;
		case 0x32: *map_to = (struct input_ev){EV_ABS, ABS_Z};    return MAP_STATIC;
		case 0x33: *map_to = (struct input_ev){EV_ABS, ABS_RX};   return MAP_STATIC;
		case 0x34: *map_to = (struct input_ev){EV_ABS, ABS_RY};   return MAP_STATIC;
		case 0x35: *map_to = (struct input_ev){EV_ABS, ABS_RZ};   return MAP_STATIC;
		case 0x39: *map_to = (struct input_ev){0, 0};             return MAP_AUTO;
		case 0x85: *map_to = (struct input_ev){EV_KEY, BTN_MODE}; return MAP_STATIC;
		}
	}

	return MAP_IGNORE;
}

u8 map_hid_to_input_linux(struct hid_usage *usage, struct input_ev *map_to)
{

	/*
	 * Linux report-descriptor (335 byte):
	 *
	 * 05 01 09 05 a1 01 85 01 09 01 a1 00 09 30 09 31 15 00 27 ff
	 * ff 00 00 95 02 75 10 81 02 c0 09 01 a1 00 09 32 09 35 15 00
	 * 27 ff ff 00 00 95 02 75 10 81 02 c0 05 02 09 c5 15 00 26 ff
	 * 03 95 01 75 0a 81 02 15 00 25 00 75 06 95 01 81 03 05 02 09
	 * c4 15 00 26 ff 03 95 01 75 0a 81 02 15 00 25 00 75 06 95 01
	 * 81 03 05 01 09 39 15 01 25 08 35 00 46 3b 01 66 14 00 75 04
	 * 95 01 81 42 75 04 95 01 15 00 25 00 35 00 45 00 65 00 81 03
	 * 05 09 19 01 29 0f 15 00 25 01 75 01 95 0f 81 02 15 00 25 00
	 * 75 01 95 01 81 03 05 0c 0a 24 02 15 00 25 01 95 01 75 01 81
	 * 02 15 00 25 00 75 07 95 01 81 03 05 0c 09 01 85 02 a1 01 05
	 * 0c 0a 23 02 15 00 25 01 95 01 75 01 81 02 15 00 25 00 75 07
	 * 95 01 81 03 c0 05 0f 09 21 85 03 a1 02 09 97 15 00 25 01 75
	 * 04 95 01 91 02 15 00 25 00 75 04 95 01 91 03 09 70 15 00 25
	 * 64 75 08 95 04 91 02 09 50 66 01 10 55 0e 15 00 26 ff 00 75
	 * 08 95 01 91 02 09 a7 15 00 26 ff 00 75 08 95 01 91 02 65 00
	 * 55 00 09 7c 15 00 26 ff 00 75 08 95 01 91 02 c0 85 04 05 06
	 * 09 20 15 00 26 ff 00 75 08 95 01 81 02 c0 00
	 */

	unsigned int hid_usage = usage->hid & HID_USAGE;
	unsigned int hid_usage_page = usage->hid & HID_USAGE_PAGE;

	switch (hid_usage_page) {
	case HID_UP_BUTTON:
		switch (hid_usage) {
		case 0x01: *map_to = (struct input_ev){EV_KEY, BTN_A};       return MAP_STATIC;
		case 0x02: *map_to = (struct input_ev){EV_KEY, BTN_B};       return MAP_STATIC;
		case 0x04: *map_to = (struct input_ev){EV_KEY, BTN_X};       return MAP_STATIC;
		case 0x05: *map_to = (struct input_ev){EV_KEY, BTN_Y};       return MAP_STATIC;
		case 0x07: *map_to = (struct input_ev){EV_KEY, BTN_TL};      return MAP_STATIC;
		case 0x08: *map_to = (struct input_ev){EV_KEY, BTN_TR};      return MAP_STATIC;
		case 0x0C: *map_to = (struct input_ev){EV_KEY, BTN_START};   return MAP_STATIC;
		case 0x0E: *map_to = (struct input_ev){EV_KEY, BTN_THUMBL};  return MAP_STATIC;
		case 0x0F: *map_to = (struct input_ev){EV_KEY, BTN_THUMBR};  return MAP_STATIC;
		}
	case HID_UP_CONSUMER:
		switch (hid_usage) {
		case 0x223: *map_to = (struct input_ev){EV_KEY, BTN_MODE};   return MAP_STATIC;
		case 0x224: *map_to = (struct input_ev){EV_KEY, BTN_SELECT}; return MAP_STATIC;
		}
	case HID_UP_GENDESK:
		switch (hid_usage) {
		case 0x30: *map_to = (struct input_ev){EV_ABS, ABS_X};   return MAP_STATIC;
		case 0x31: *map_to = (struct input_ev){EV_ABS, ABS_Y};   return MAP_STATIC;
		case 0x32: *map_to = (struct input_ev){EV_ABS, ABS_RX};  return MAP_STATIC;
		case 0x35: *map_to = (struct input_ev){EV_ABS, ABS_RY};  return MAP_STATIC;
		case 0x39: *map_to = (struct input_ev){0, 0};            return MAP_AUTO;
		}
	case HID_UP_SIMULATION:
		switch (hid_usage) {
		case 0xC4: *map_to = (struct input_ev){EV_ABS, ABS_RZ};  return MAP_STATIC;
		case 0xC5: *map_to = (struct input_ev){EV_ABS, ABS_Z};   return MAP_STATIC;
		}
	}

	return MAP_IGNORE;
}


/*
 * Input Mapping Hook
 *
 * Invoked at input registering before mapping an usage
 * (called once for every hid-usage).
 */

static int xpadneo_mapping(struct hid_device *hdev, struct hid_input *hi,
			   struct hid_field *field, struct hid_usage *usage,
			   unsigned long **bit, int *max)
{
	/* Return values */
	enum {
		RET_MAP_IGNORE = -1,   /* completely ignore this input */
		RET_MAP_AUTO,        /* let hid-core autodetect the mapping */
		RET_MAP_STATIC       /* mapped by hand, no further processing */
	};

	struct input_ev map_to;
	u8 (*perform_mapping)(struct hid_usage *usage, struct input_ev *map_to);
	struct xpadneo_devdata *xdata = hid_get_drvdata(hdev);


	switch (xdata->report_descriptor) {
	case LINUX:   perform_mapping = map_hid_to_input_linux; break;
	case WINDOWS: perform_mapping = map_hid_to_input_windows; break;
	default:      return RET_MAP_AUTO;
	}


	switch (perform_mapping(usage, &map_to)) {
	case MAP_AUTO:
		hid_dbg_lvl(DBG_LVL_FEW, hdev,
		"UP: 0x%04X, USG: 0x%04X -> automatically\n",
		usage->hid & HID_USAGE_PAGE, usage->hid & HID_USAGE);

		return RET_MAP_AUTO;

	case MAP_IGNORE:
		hid_dbg_lvl(DBG_LVL_FEW, hdev,
		"UP: 0x%04X, USG: 0x%04X -> ignored\n",
		usage->hid & HID_USAGE_PAGE, usage->hid & HID_USAGE);

		return RET_MAP_IGNORE;

	case MAP_STATIC:
		hid_dbg_lvl(DBG_LVL_FEW, hdev,
		"UP: 0x%04X, USG: 0x%04X -> EV: 0x%03X, INP: 0x%03X\n",
		usage->hid & HID_USAGE_PAGE, usage->hid & HID_USAGE,
		map_to.event_type, map_to.input_code);

		hid_map_usage_clear(hi, usage, bit, max,
					map_to.event_type, map_to.input_code);
		return RET_MAP_STATIC;

	}

	/* Something went wrong, ignore this field */
	return RET_MAP_IGNORE;
}


/*
 * Report Descriptor Fixup Hook
 */

static u8 *xpadneo_report_fixup(struct hid_device *hdev, u8 *rdesc,
				unsigned int *rsize)
{
	hid_dbg_lvl(DBG_LVL_SOME, hdev, "REPORT (DESCRIPTOR) FIXUP HOOK, called before report descriptor parsing\n");
	dbg_hex_dump_lvl(DBG_LVL_FEW, "xpadneo: report-descriptor: ", rdesc, *rsize);

	return rdesc;
}


static void parse_raw_event_battery(struct hid_device *hdev, u8 *data,
				    int reportsize)
{
	struct xpadneo_devdata *xdata = hid_get_drvdata(hdev);

	/*
	 * Behaviour on AA Mignon Batteries:
	 *
	 * 0x80 - Cable plugged in, Battery status unknown
	 * 0x84 - Critical Battery Level, Rumble deactivated
	 * 0x85 - Low to Medium Batt. Level
	 * 0x86 - Normal to High Batt. Level
	 * 0x87 - High to Full Batt. Level
	 */

	xdata->cable_state = data[1] == 0x80 ? 1 : 0;
	hid_dbg_lvl(DBG_LVL_ALL, hdev, "data[1]: %X, cable-state: %d\n", data[1], xdata->cable_state);

	switch (data[1]) {
	case 0x80:
		xdata->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
		break;
	case 0x84:
		xdata->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_CRITICAL;
		break;
	case 0x85:
		xdata->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
		break;
	case 0x86:
		xdata->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
		break;
	case 0x87:
		xdata->capacity_level = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
		break;
	}

	power_supply_changed(xdata->batt);
}

static void check_report_behaviour(struct hid_device *hdev, u8 *data,
				   int reportsize)
{
	struct xpadneo_devdata *xdata = hid_get_drvdata(hdev);

	/*
	 * The length of the first input report with an ID of 0x01
	 * reveals which report-type the controller is actually
	 * sending (windows: 16, or linux: 17).
	 */
	if (xdata->report_behaviour == UNKNOWN) {
		switch (reportsize) {
		case 16: xdata->report_behaviour = WINDOWS; break;
		case 17: xdata->report_behaviour = LINUX;   break;
		default: xdata->report_behaviour = UNKNOWN; break;
		}
	}

	/* TODO:
	 * The best solution would be to replace the report descriptor
	 * in case that the wrong reports are sent. Unfortunately I
	 * don't know yet how one can replace the descriptor _after_
	 * the report_fixup hook by hand. I fix it the other way
	 * (translate the report/event) until I found a better solution.
	 */
}

/*
 * HID Raw Event Hook
 */

int xpadneo_raw_event(struct hid_device *hdev, struct hid_report *report,
		      u8 *data, int reportsize)
{
	hid_dbg_lvl(DBG_LVL_SOME, hdev, "RAW EVENT HOOK, called before parsing a report\n");

	dbg_hex_dump_lvl(DBG_LVL_ALL, "xpadneo: raw_event: ", data, reportsize);
	hid_dbg_lvl(DBG_LVL_ALL, hdev, "report->size: %d\n", (report->size)/8);
	hid_dbg_lvl(DBG_LVL_ALL, hdev, "data size (w.o. id): %d\n", reportsize-1);


	switch (report->id) {
	case 01: check_report_behaviour(hdev, data, reportsize); break;
	case 04: parse_raw_event_battery(hdev, data, reportsize); return 1;  /* stop processing */
	}

	/* Continue processing */
	return 0;
}


void xpadneo_report(struct hid_device *hdev, struct hid_report *report)
{
	hid_dbg_lvl(DBG_LVL_SOME, hdev, "REPORT HOOK, called right after parsing a report\n");
}


/*
 * Input Configured Hook
 *
 * We have to fix up the key-bitmap, because there is
 * no DPAD_UP, _RIGHT, _DOWN, _LEFT on the device by default
 *
 * TODO:
 * Furthermore we can set the idev value in xpadneo_data
 */

static int xpadneo_input_configured(struct hid_device *hdev,
							struct hid_input *hi)
{
	struct input_dev *input = hi->input;

	hid_dbg_lvl(DBG_LVL_SOME, hdev, "INPUT CONFIGURED HOOK, invoked just before the device is registered\n");


	/* TODO: outsource that */

	/* Add BTN_DPAD_* to the key-bitmap, since they where not originally
	 * mentioned in the report-description.
	 *
	 * This is necessary to set them later in xpadneo_event
	 * by input_report_key(). Otherwise, no event would be generated
	 * (since it would look like the key doesn't even exist)
	 *
	 * TODO:
	 * - Those buttons are still shown as (null) in jstest
	 * - We should also send out ABS_HAT0X/Y events as mentioned on the
	 *   official HID usage tables (p.34).
	 */
	if (dpad_to_buttons) {
		__set_bit(BTN_DPAD_UP, input->keybit);
		__set_bit(BTN_DPAD_RIGHT, input->keybit);
		__set_bit(BTN_DPAD_DOWN, input->keybit);
		__set_bit(BTN_DPAD_LEFT, input->keybit);

		__clear_bit(ABS_HAT0X, input->absbit);
		__clear_bit(ABS_HAT0Y, input->absbit); /* TODO: necessary? */
	}

	/* In addition to adding new keys to the key-bitmap, we may also
	 * want to remove the old (original) axis from the absolues-bitmap.
	 *
	 * TODO:
	 * Maybe we want both, our custom and the original mapping.
	 * If we decide so, remember that 0x39 is a hat switch on the official
	 * usage tables, but not at the input subsystem, so be sure to use the
	 * right constant!
	 *
	 * Either let hid-core decide itself or input_ev it to ABS_HAT0X/Y by hand:
	 * #define ABS_HAT0X	0x10
	 * #define ABS_HAT0Y	0x11
	 *
	 * Q: I don't know why the usage number does not fit the official
	 *    usage page numbers, however...
	 * A: because there is an difference between hid->usage, which is
	 *    the HID_USAGE_PAGE && HID_USAGE (!), and hid->code, which is
	 *    the internal input-representation as defined in
	 *    input-event-codes.h
	 *
	 * take a look at the following website for the original mapping:
	 * https://elixir.free-electrons.com/linux/v4.4/source/drivers/hid/hid-input.c#L604
	 */

	return 0;
}


/*
 * Event Hook
 *
 * This hook is called whenever an event occurs that is listed on
 * xpadneo_driver.usage_table (which is NULL in our case, therefore it is
 * invoked on every event).
 *
 * We use this hook to attach some more events to our D-pad, as a result
 * our D-pad is reported to Input as both, four buttons AND a hat-switch.
 *
 * Before we can send additional input events, we have to enable
 * the corresponding keys in xpadneo_input_configured.
 */

int xpadneo_event(struct hid_device *hdev, struct hid_field *field,
		  struct hid_usage *usage, __s32 value)
{
	/* Return Codes */
	enum {
		EV_CONT_PROCESSING, /* Let the hid-core autodetect the event */
		EV_STOP_PROCESSING  /* Stop further processing */
	};

	struct xpadneo_devdata *xdata = hid_get_drvdata(hdev);

	/* TODO: use hid_get_drvdata instead */
	struct hid_input *hidinput = list_entry(hdev->inputs.next, struct hid_input, list);
	struct input_dev *idev     = hidinput->input;


	/* TODO:
	 * This is the workaround for the wrong report (Windows report but
	 * Linux descriptor). We would prefer to fixup the descriptor, but we
	 * cannot fix it anymore at the time we recognize the wrong behaviour,
	 * hence we will fire the input events by hand.
	 */

	hid_dbg_lvl(DBG_LVL_SOME, hdev, "desc: %d, beh: %d\n",
			xdata->report_descriptor, xdata->report_behaviour);

	if (xdata->report_behaviour == WINDOWS
					&& xdata->report_descriptor == LINUX) {

		/*
		 * we fix all buttons by hand. You may think that we
		 * could do that by using the windows_map too, but it is more
		 * like an coincidence that this would work in this special case:
		 * It would only, because HID_UP_BUTTONS has no special names
		 * for the HID_USAGE's, therefore the first button stays 0x01
		 * on both reports (windows and linux) - so it is a 1: 1 mapping.
		 * But this is not true in general (i.e. not for other USAGE_PAGES)
		 */

		/* TODO: outsource that part*/

		if ((usage->hid & HID_USAGE_PAGE) == HID_UP_BUTTON) {
			switch (usage->hid & HID_USAGE) {
			case 0x01: input_report_key(idev, BTN_A, value); break;
			case 0x02: input_report_key(idev, BTN_B, value); break;
			case 0x03: input_report_key(idev, BTN_X, value); break;
			case 0x04: input_report_key(idev, BTN_Y, value); break;
			case 0x05: input_report_key(idev, BTN_TL, value); break;
			case 0x06: input_report_key(idev, BTN_TR, value); break;
			case 0x07: input_report_key(idev, BTN_SELECT, value); break;
			case 0x08: input_report_key(idev, BTN_START, value); break;
			case 0x09: input_report_key(idev, BTN_THUMBL, value); break;
			case 0x0A: input_report_key(idev, BTN_THUMBR, value); break;
			}

			hid_dbg_lvl(DBG_LVL_SOME, hdev, "hid-upage: %02x, hid-usage: %02x fixed\n", (usage->hid & HID_USAGE_PAGE), (usage->hid & HID_USAGE));
			return EV_STOP_PROCESSING;
		}
	}

	/* Yep, this is the D-pad event */
	if ((usage->hid & HID_USAGE) == 0x39) {

		/* TODO: outsource that part*/

		/*
		 * You can press UP and RIGHT, RIGHT and DOWN, ... together!
		 *
		 * # value  U R D L
		 * ----------------
		 * 0 0000   0 0 0 0   U = ((value >= 1) && (value <= 2)) || (value == 8)
		 * 1 0001   1 0 0 0   R = (value >= 2) && (value <= 4)
		 * 2 0010   1 1 0 0   D = (value >= 4) && (value <= 6)
		 * 3 0011   0 1 0 0   L = (value >= 6) && (value <= 8)
		 * 4 0100   0 1 1 0
		 * 5 0101   0 0 1 0
		 * 6 0110   0 0 1 1
		 * 7 0111   0 0 0 1
		 * 8 1000   1 0 0 1
		 */

		input_report_key(idev, BTN_DPAD_UP,
			(((value >= 1) && (value <= 2)) || (value == 8)));
		input_report_key(idev, BTN_DPAD_RIGHT, ((value >= 2) && (value <= 4)));
		input_report_key(idev, BTN_DPAD_DOWN, ((value >= 4) && (value <= 6)));
		input_report_key(idev, BTN_DPAD_LEFT, ((value >= 6) && (value <= 8)));

		/*
		 * It is perfectly fine to send those events even if
		 * dpad_to_buttons is false because the keymap decides if the
		 * event is really sent or not. It is also easier to send them
		 * anyway, because dpad_to_buttons may change if the controller
		 * is connected. This way the behaviour does not change until
		 * the controller is reconnected.
		 */
	}

	hid_dbg_lvl(DBG_LVL_SOME, hdev, "hid-upage: %02x, hid-usage: %02x, input-code: %02x, value: %02x\n", (usage->hid & HID_USAGE_PAGE), (usage->hid & HID_USAGE), usage->code, value);

	return EV_CONT_PROCESSING;
}


/* Device Probe and Remove Hook */

static int xpadneo_probe_device(struct hid_device *hdev,
				const struct hid_device_id *id)
{
	int ret;
	struct xpadneo_devdata *xdata;

	hid_dbg_lvl(DBG_LVL_FEW, hdev, "probing device: %s\n", hdev->name);


	/*
	 * Create a "nearly globally" accessible data structure (accessible
	 * through hid_get_drvdata) the structure is freed automatically as
	 * soon as hdev->dev is removed, since we use the devm_ derivate.
	 */
	xdata = devm_kzalloc(&hdev->dev, sizeof(*xdata), GFP_KERNEL);
	if (xdata == NULL)
		return -ENOMEM;

	xdata->hdev = hdev;

	/* Unknown until first report with ID 01 arrives (see raw_event) */
	xdata->report_behaviour = UNKNOWN;

	switch (hdev->dev_rsize) {
	case 307: xdata->report_descriptor = WINDOWS; break;
	case 335: xdata->report_descriptor = LINUX;   break;
	default:  xdata->report_descriptor = UNKNOWN; break;
	}

	hid_set_drvdata(hdev, xdata);


	/* Parse the raw report (includes a call to report_fixup) */
	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		goto return_error;
	}

	/* Debug Output*/
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "hdev:\n");
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* raw dev_rdesc: (see above)\n");
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* raw dev_rsize: %u\n", hdev->dev_rsize);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* bus: 0x%04X\n", hdev->bus);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* report group: %u\n", hdev->group);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* vendor: 0x%08X\n", hdev->vendor);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* version: 0x%08X\n", hdev->version);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* product: 0x%08X\n", hdev->product);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* country: %u\n", hdev->country);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "* driverdata: %lu\n", id->driver_data);

	/* We start our hardware without FF, we will add it afterwards by hand
	 * HID_CONNECT_DEFAULT = (HID_CONNECT_HIDINPUT | HID_CONNECT_HIDRAW
	 *                        | HID_CONNECT_HIDDEV | HID_CONNECT_FF)
	 * Our Input Device is created automatically since we defined
	 * HID_CONNECT_HIDINPUT as one of the flags.
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		goto return_error;
	}

	/* Call the device initialization routines */
	xpadneo_initDevice(hdev);
	xpadneo_initBatt(hdev);


	/* Everything is fine */
	return 0;

return_error:
	return ret;
}


static void xpadneo_remove_device(struct hid_device *hdev)
{
	struct xpadneo_devdata *xdata = hid_get_drvdata(hdev);

	hid_hw_close(hdev);

	/* Clean up */

	/* TODO:
	 * if (!sc->battery_desc.name)
	 *	return;
	 */
	power_supply_unregister(xdata->batt);
	/* TODO: kfree(sc->battery_desc.name); */
	/* TODO: sc->battery_desc.name = NULL; */

	hid_hw_stop(hdev);


	hid_dbg_lvl(DBG_LVL_FEW, hdev, "goodbye %s\n", hdev->name);
}


/*
 * Device ID Structure, define all supported devices here
 */

static const struct hid_device_id xpadneo_devices[] = {

	/*
	 * The ProductID is somehow related to the Firmware Version,
	 * but it somehow changed back from 0x02FD (newer fw) to 0x02E0 (older)
	 * and vice versa on one controller here.
	 *
	 * Unfortunately you cannot tell from product id how the gamepad really
	 * behaves on reports, since the newer firmware supports both mappings
	 * (the one which is standard in linux and the old one, which is still
	 * used in windows).
	 */

	/* XBOX ONE S / X */
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_MICROSOFT, 0x02FD) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_MICROSOFT, 0x02E0) },
	/* SENTINEL VALUE, indicates the end*/
	{ }
};

static struct hid_driver xpadneo_driver = {
	/* The name of the driver */
	.name = "xpadneo",

	/* Which devices is this driver for */
	.id_table = xpadneo_devices,

	/* Hooked as the input device is configured (before it is registered)
	 * we need that because we do not configure the input-device ourself
	 * but leave it up to hid_hw_start()
	 */
	.input_configured = xpadneo_input_configured,

	/* Invoked on input registering before mapping an usage */
	.input_mapping = xpadneo_mapping,

	/* If usage in usage_table, this hook is called */
	.event = xpadneo_event,

	/* Called before report descriptor parsing (NULL means nop) */
	.report_fixup = xpadneo_report_fixup,

	/* Called when a new device is inserted */
	.probe = xpadneo_probe_device,

	/* Called when a device is removed */
	.remove = xpadneo_remove_device,

	/* If report in report_table, this hook is called */
	.raw_event = xpadneo_raw_event,

	.report = xpadneo_report
};

MODULE_DEVICE_TABLE(hid, xpadneo_devices);



/*
 * Module Init and Exit
 *
 * We may replace init and remove by module_hid_driver(xpadneo_driver)
 * in future versions, as long as there is nothing special in these two
 * functions (but registering and unregistering the driver). Up to now it is
 * more useful for us to not "oversimplify" the whole driver-registering thing.
 *
 * Caution: do not use both! (module_hid_driver and hid_(un)register_driver)
 */

static int __init xpadneo_initModule(void)
{
	pr_info("%s: hello there!\n", xpadneo_driver.name);
	return hid_register_driver(&xpadneo_driver);
}

static void __exit xpadneo_exitModule(void)
{
	hid_unregister_driver(&xpadneo_driver);
	pr_info("%s: goodbye!\n", xpadneo_driver.name);
}

/*
 * Tell the driver system which functions to call at initialization and
 * removal of the module
 */
module_init(xpadneo_initModule);
module_exit(xpadneo_exitModule);

