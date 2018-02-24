/*  
 * Force feedback support for XBOX ONE S and X gamepads via Bluetooth
 *
 * This driver was developed for a student project at fortiss GmbH in Munich.
 * Copyright (c) 2017 Florian Dollinger <dollinger.florian@gmx.de>
 */

/* TODO:
 * - jstest shows at startup the maximum/minimum value,
 *   not the value that corresponds to the "default" position, why?
 * - https://www.kernel.org/doc/html/v4.10/process/coding-style.html
 * - a lot of more, search for TODO in the code (you can't overlook xD)
 */

#include <linux/hid.h>
#include <linux/input.h>	/* ff_memless(), ... */
#include <linux/module.h>	/* MODULE_*, module_*, ... */
#include <linux/slab.h>		/* kzalloc(), kfree(), ... */
#include <linux/delay.h>	/* mdelay(), ... */
#include "hid-ids.h"


#define DEBUG

/* MODULE INFORMATION */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Florian Dollinger <dollinger.florian@gmx.de>");
MODULE_DESCRIPTION("Linux Kernel driver for XBOX ONE S and X gamepads (bluetooth), including force-feedback");
MODULE_VERSION("0.1.3");


/* MODULE PARAMETER
 * Located at /sys/module//parameters
 */
#ifdef DEBUG
static u8 debug_level = 3;
module_param(debug_level, byte, 0644);
MODULE_PARM_DESC(debug_level, "(u8) Debug information level: 0 (none) to 3+ (maximum).");
#endif

static bool dpad_as_buttons = 0;
module_param(dpad_as_buttons, bool, 0644);
MODULE_PARM_DESC(dpad_as_buttons, "(bool) Map the DPAD-buttons as BTN_DPAD_UP/RIGHT/DOWN/LEFT instead of as a hat-switch. Restart device to take effect.");


/* DEBUG PRINTK
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
		if(debug_level >= lvl) hid_printk(KERN_DEBUG, pr_fmt(fmt_hdev), pr_fmt(fmt_str), ##__VA_ARGS__)
#define dbg_hex_dump_lvl(lvl, fmt_prefix, data, size) \
		if(debug_level >= lvl) print_hex_dump(KERN_DEBUG, pr_fmt(fmt_prefix), DUMP_PREFIX_NONE, 32, 1, data, size, false);
#else
#define hid_dbg_lvl(lvl, fmt_hdev, fmt_str, ...) \
		no_printk(KERN_DEBUG pr_fmt(fmt_str), ##__VA_ARGS__)
#define dbg_hex_dump_lvl(lvl, fmt_prefix, data, size) \
		no_printk(KERN_DEBUG pr_fmt(fmt_prefix))
#endif


/*
 * FF Output Report
 * 
 * This is the structure for the rumble output report.
 * For more information about the structure please take
 * a look in the hid-report description.
 * 
 * Please notice that the structs are __packed,
 * therefore there is no "padding" between the elements
 * (they behave more like an array).
 * 
 * TODO:
 * use a variable which is aware of the endianess!
 * (something like __le16 and __le16_to_cpu(...))
 */

#define FF_ENABLE_RMBL_LEFT     0b10
#define FF_ENABLE_RMBL_RIGHT    0b01

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

/* static variables are always zeroed,
 * we use that to create an empty initialization struct
 */
static const struct ff_data ff_clear;


/*
 * Device Data
 * 
 * This Structure will be nearly globally accessbile via hid_get_drvdata(hdev).
 * We use it therefore to store information about the device centralized.
 * It is attached to the hid_device via hid_set_drvdata(hdev) in the
 * probing function.
 */

struct xpadneo_devdata {
	struct hid_device *hdev;
	struct input_dev *idev;
};


/*
 * FORCE FEEDBACK CALLBACK
 * 
 * This function is called by the Input Subsystem.
 * The effect data is set in userspace and sent to the driver via ioctl.
 */

static int xpadneo_ff_play (struct input_dev *dev, void *data,
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


	ff_package.ff = ff_clear;
	ff_package.report_id = 0x03;
	ff_package.ff.enable_actuators = FF_ENABLE_RMBL_RIGHT | FF_ENABLE_RMBL_LEFT;
	ff_package.ff.magnitude_right = (u8)((weak & 0xFF00) >> 8);
	ff_package.ff.magnitude_left = (u8)((strong & 0xFF00) >> 8);

	/*
	 * It is up to the Input-Subsystem to start and stop the effect as needed.
	 * All WE need to do is to play the effect at least 32767 ms long.
	 * Take a look here:
	 * https://stackoverflow.com/questions/48034091/ff-replay-substructure-in-ff-effect-empty/48043342#48043342
	 * 
	 * We therefore simply play the effect as long as possible
	 * which is (2,55s * 255 = 650,25s = 10min)
	 */
	ff_package.ff.duration = 0xFF;
	ff_package.ff.loop_count = 0xFF;
	hid_hw_output_report(hdev, (u8*)&ff_package, sizeof(ff_package));

	return 0;
}


/*
 * Device (Controller) Initialization
 * 
 * This function is called "by hand" inside the probe-function.
 */

static int xpadneo_initDevice (struct hid_device *hdev)
{
	struct hid_report *report;
	struct list_head *output_report_list
		= &hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	int error;

	/* create handle to the input device which is assigned to the hid device
	 * TODO:
	 * replace by get_drvdata
	 */
	struct hid_input *hidinput = list_entry(hdev->inputs.next, struct hid_input, list);
	struct input_dev *dev = hidinput->input;

	unsigned int i;
	struct ff_report ff_package;


	if (list_empty(output_report_list)) {
		hid_err(hdev, "no output report found\n");
		return -ENODEV;
	}

	if(list_is_singular(output_report_list)) {
		hid_dbg_lvl(DBG_LVL_FEW, hdev, "only one output report\n");
	}

	report = list_entry(output_report_list->next, struct hid_report, list);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "output-report (id %i) contains %i fields:\n", report->id, report->maxfield);

	for(i = 0; i < report->maxfield; i++){
		hid_dbg_lvl(DBG_LVL_FEW, hdev, "\tfield[%u]: usage: %#04x, size: %i, count:%i\n", \
			i, \
			report->field[i]->usage->hid & HID_USAGE, \
			report->field[i]->report_size, \
			report->field[i]->report_count
		);
	}

	/* SEND OUT A 'HELLO' RUMBLE-PACKAGE */
	ff_package.ff = ff_clear;
	ff_package.report_id = 0x03;
	ff_package.ff.enable_actuators = FF_ENABLE_RMBL_RIGHT;
	ff_package.ff.magnitude_right = 0x99;
	ff_package.ff.duration = 50;
	hid_hw_output_report(hdev, (u8*)&ff_package, sizeof(ff_package));

	mdelay(500);

	ff_package.ff = ff_clear;
	ff_package.report_id = 0x03;
	ff_package.ff.enable_actuators = FF_ENABLE_RMBL_LEFT;
	ff_package.ff.magnitude_left = 0x99;
	ff_package.ff.duration = 50;
	hid_hw_output_report(hdev, (u8*)&ff_package, sizeof(ff_package));

	/* INIT FORCE FEEDBACK (FF) */
	input_set_capability(dev, EV_FF, FF_RUMBLE);
	error = input_ff_create_memless(dev, NULL, xpadneo_ff_play);
	if (error) {
		return error;
	}

	return 0;
}


/*
 * General Input Mapping Structure Definition
 * 
 * NOTE:
 * this is still not the most efficient way since
 * 1) a lot of elements store "0" (e.g. if the first entry is 0x30 like in generic_desktop)
 * 2) we have to store max "by hand", which is error-prone
 * 
 * possible solution (problem 1):
 * can we use macros to "translate" each index depending on the time we declared it?
 * like FORWARD(GENDESK, 0x31) gives 0,
 *      FORWARD(GENDESK, 0x35) gives 1, ...
 * and backwards:
 *      BACKWARD_USAGE_PAGE(0) gives GENDESK
 *      BACKWARD_USAGE(0)      gives 0x31
 */

#define MAP_IGNORE  0 /* MAP_IGNORE must be 0, because the static struct is zeroed by default */
#define MAP_AUTO    1 /* do not really input_ev it, let hid-core decide */
#define MAP_STATIC  2 /* input_ev to the values given */

/* The position of an map_entry withing the map_usagepage.input_ev equals the hid usage id. */
struct map_entry {
	u8 map_type;			/* input_ev the hid usage statically (by hand), automatic (let hid-core decide) or ignore it (MAP_STATIC, MAP_AUTO, MAP_IGNORE) */
	u8 map_to_event_type;	/* to which input event should the hid usage be mapped? (EV_KEY, EV_ABS, ...) */
	u16 map_to_input_code;	/* to which input code do you want to input_ev? (BTN_SOUTH, ABS_X, ...) */
};

/* There is one map_usagepage for every hid usage page, like generic-desktop, button, and so on */
struct map_usagepage {
	int max;							/* what's the maximum index in input_ev? */
	const struct map_entry *input_ev;	/* an array of the above declared map_entry */
};

/* one mapping for every device, like Xbox One S and Xbox One X */
struct device_input_mapping {
	const struct map_usagepage generic_desktop;
	const struct map_usagepage button;
	const struct map_usagepage simulation;
	const struct map_usagepage consumer;
	const struct map_usagepage generic_device_control;
};


/*
 * Instantiated Mapping Structures
 */

static const struct device_input_mapping xboxone_s_map = {

	.button = {
		.max = 0x0f,
		.input_ev = (struct map_entry[]) {
			/* [HID_USAGE] = {MAPPING_TYPE, MAP_TO_EVENT_TYPE, MAP_TO_INPUT_CODE} */
			[0x01] = {MAP_STATIC, EV_KEY, BTN_SOUTH},
			[0x02] = {MAP_STATIC, EV_KEY, BTN_EAST},
			[0x04] = {MAP_STATIC, EV_KEY, BTN_NORTH},
			[0x05] = {MAP_STATIC, EV_KEY, BTN_WEST},
			[0x07] = {MAP_STATIC, EV_KEY, BTN_TL},
			[0x08] = {MAP_STATIC, EV_KEY, BTN_TR},
			[0x0C] = {MAP_STATIC, EV_KEY, BTN_START},
			[0x0E] = {MAP_STATIC, EV_KEY, BTN_THUMBL},
			[0x0F] = {MAP_STATIC, EV_KEY, BTN_THUMBR}
			/* All other Bits in the report are just padding! */
		}
	},

	.consumer = {
		.max = 0x224,
		.input_ev = (struct map_entry[]) {
			[0x223] = {MAP_STATIC, EV_KEY, BTN_MODE},
			[0x224] = {MAP_STATIC, EV_KEY, BTN_SELECT},
		}
	},

	.generic_desktop = {
		.max = 0x39,
		.input_ev = (struct map_entry[]) {
			[0x30] = {MAP_STATIC, EV_ABS, ABS_X},     /* left stick X */
			[0x31] = {MAP_STATIC, EV_ABS, ABS_Y},     /* left stick Y */
			[0x32] = {MAP_STATIC, EV_ABS, ABS_RX},    /* right stick X */
			[0x35] = {MAP_STATIC, EV_ABS, ABS_RY},    /* right stick Y */
			[0x39] = {MAP_AUTO, 0, 0},                /* Dpad */

			/* NOTE:
			 * MAP_AUTO means that we let hid-core autodetect
			 * the correct mapping (for Dpad, which is ABS_HAT0X/Y).
			 * But we will add also another usage (BTN_DPAD_*)
			 * in the event/input_configured hooks.
			 */
		}
	},

	.simulation = {
		.max = 0xC5,
		.input_ev = (struct map_entry[]) {
			[0xC4] = {MAP_STATIC, EV_ABS, ABS_RZ},    /* right shoulder trigger */
			[0xC5] = {MAP_STATIC, EV_ABS, ABS_Z},     /* left shoulder trigger */
		}
	}
};

static const struct device_input_mapping xboxone_x_map = {
	
	.button = {
		.max = 0x0a,
		.input_ev = (struct map_entry[]) {
			[0x01] = {MAP_STATIC, EV_KEY, BTN_SOUTH},     /* A */
			[0x02] = {MAP_STATIC, EV_KEY, BTN_EAST},      /* B */
			[0x03] = {MAP_STATIC, EV_KEY, BTN_WEST},      /* X */
			[0x04] = {MAP_STATIC, EV_KEY, BTN_NORTH},     /* Y */
			[0x05] = {MAP_STATIC, EV_KEY, BTN_TL},        /* L1 */
			[0x06] = {MAP_STATIC, EV_KEY, BTN_TR},        /* L2 */
			[0x07] = {MAP_STATIC, EV_KEY, BTN_SELECT},    /* View, Middle Left */
			[0x08] = {MAP_STATIC, EV_KEY, BTN_START},     /* Menu, Middle Right */
			[0x09] = {MAP_STATIC, EV_KEY, BTN_THUMBL},    /* L3, Stick */
			[0x0A] = {MAP_STATIC, EV_KEY, BTN_THUMBR},    /* R3, Stick */
			/* All other Bits in the report are just padding! */
		}
	},

	.generic_desktop = {
		.max = 0x85,
		.input_ev = (struct map_entry[]) {
			[0x30] = {MAP_STATIC, EV_ABS, ABS_X},     /* left stick X */
			[0x31] = {MAP_STATIC, EV_ABS, ABS_Y},     /* left stick Y */
			[0x32] = {MAP_STATIC, EV_ABS, ABS_Z},     /* left shoulder trigger */
			[0x33] = {MAP_STATIC, EV_ABS, ABS_RX},    /* right stick Y */
			[0x34] = {MAP_STATIC, EV_ABS, ABS_RY},    /* right stick X */
			[0x35] = {MAP_STATIC, EV_ABS, ABS_RZ},    /* right shoulder trigger */
			[0x39] = {MAP_AUTO, 0, 0},                /* Dpad */
			[0x85] = {MAP_STATIC, EV_KEY, BTN_MODE}   /* Xbox button */
		}
	}
};

static const struct device_input_mapping xboxone_x2_map = {

	.button = {
		.max = 0x0a,
		.input_ev = (struct map_entry[]) {
			[0x01] = {MAP_STATIC, EV_KEY, BTN_SOUTH},     /* A */
			[0x02] = {MAP_STATIC, EV_KEY, BTN_EAST},      /* B */
			[0x03] = {MAP_STATIC, EV_KEY, BTN_WEST},      /* X */
			[0x04] = {MAP_STATIC, EV_KEY, BTN_NORTH},     /* Y */
			[0x05] = {MAP_STATIC, EV_KEY, BTN_TL},        /* L1 */
			[0x06] = {MAP_STATIC, EV_KEY, BTN_TR},        /* L2 */
			[0x07] = {MAP_STATIC, EV_KEY, BTN_SELECT},    /* View, Middle Left */
			[0x08] = {MAP_STATIC, EV_KEY, BTN_START},     /* Menu, Middle Right */
			[0x09] = {MAP_STATIC, EV_KEY, BTN_THUMBL},    /* L3, Stick */
			[0x0A] = {MAP_STATIC, EV_KEY, BTN_THUMBR},    /* R3, Stick */
			/* All other Bits in the report are just padding! */
		}
	},

	.generic_desktop = {
		.max = 0x85,
		.input_ev = (struct map_entry[]) {
			[0x30] = {MAP_STATIC, EV_ABS, ABS_X},     /* left stick X */
			[0x31] = {MAP_STATIC, EV_ABS, ABS_Y},     /* left stick Y */
			[0x32] = {MAP_STATIC, EV_ABS, ABS_RX},    /* right stick Y */
			[0x35] = {MAP_STATIC, EV_ABS, ABS_RY},    /* right stick X */
			[0x39] = {MAP_AUTO, 0, 0},                /* Dpad */
			[0x85] = {MAP_STATIC, EV_KEY, BTN_MODE}   /* Xbox button */
		}
	},

	.simulation = {
		.max = 0xC5,
		.input_ev = (struct map_entry[]) {
			[0xC4] = {MAP_STATIC, EV_ABS, ABS_RZ},
			[0xC5] = {MAP_STATIC, EV_ABS, ABS_Z},
		}
	},

	.consumer = {
		.max = 0x223,
		.input_ev = (struct map_entry[]) {
			[0x223] = {MAP_STATIC, EV_KEY, BTN_MODE}
		}
	}
};

/*
 * INPUT MAPPING HOOK
 * 
 * Invoked at input registering before mapping an usage
 * (called once for every hid-usage).
 * 
 * We use the above stuctures (xboxone_..._map) to
 * input_ev an input-event-code to every hid-usage.
 */

static int xpadneo_mapping (struct hid_device *hdev, struct hid_input *hi,
			struct hid_field *field, struct hid_usage *usage,
			unsigned long **bit, int *max)
{
	/* return values */
	#define RET_MAP_IGNORE -1 /* completely ignore this input */
	#define RET_MAP_AUTO    0 /* let hid-core autodetect the mapping */
	#define RET_MAP_STATIC  1 /* mapping done by hand, no further processing */

	const struct device_input_mapping* device_map;

	unsigned int uid = usage->hid & HID_USAGE;
	unsigned int upid = usage->hid & HID_USAGE_PAGE;

	struct map_entry m;
	struct map_usagepage mup;


	/* TODO:
	 * use hid_get_drvdata(hdev) and ->quirks like hid-sony
	 * instead of comparing the product id "hardcoded" (as magic number)
	 */

	/* select the device input_ev
	 * TODO:
	 * sometimes even a device which has an productId == 0x02FD requires
	 * a xboxone_x-mapping, we don't know why at the moment - it even changes
	 * from time to time on the same device - why? furthermore you cannot simply say that
	 * 0x02FD is a xbox one s controller and
	 * 0x02E0 is a xbox one x controller
	 * maybe the length of the report-descriptor is a better indicator which mapping
	 * we should use?
	 */
	switch (hdev->product) {
	case 0x02E0: device_map = &xboxone_x_map; break;
	case 0x02FD: device_map = &xboxone_s_map; break;
	default:
		return RET_MAP_AUTO;
	}

	/* select the correct usage page input_ev withing the device input_ev */
	switch (upid) {
	case HID_UP_BUTTON:     mup = device_map->button;          break;
	case HID_UP_GENDESK:    mup = device_map->generic_desktop; break;
	case HID_UP_SIMULATION: mup = device_map->simulation;      break;
	case HID_UP_CONSUMER:   mup = device_map->consumer;        break;
	default:
		return RET_MAP_IGNORE;	
	}

	hid_dbg_lvl(DBG_LVL_FEW, hdev, "usage page: BUTTON, usage: 0x%02X\n", uid);

	/* If the usage is bigger than the biggest index */
	if (uid > mup.max){
		hid_dbg_lvl(DBG_LVL_FEW, hdev, "-> not mapped! uid out of range for this device (probably padding bits)\n");
		return RET_MAP_IGNORE;
	}

	/* it is safe here */
	m = mup.input_ev[uid];

	/* If we didn't declared a mapping, the value is '0'
		* that is because it is a static struct, which is automatically zeroed.
		*/
	if (m.map_type == MAP_IGNORE){
		hid_dbg_lvl(DBG_LVL_FEW, hdev, "-> not mapped! (forgotten or someone set it explicitely to MAP_IGNORE)\n");
		return RET_MAP_IGNORE;
	}

	/* If we marked the mapping entry explicitly as 'MAP_AUTO',
		* we want to let hid-core autodetect
		*/
	if (m.map_type == MAP_AUTO) {
		hid_dbg_lvl(DBG_LVL_FEW, hdev, "-> automatically mapped!\n");
		return RET_MAP_AUTO;
	}

	hid_map_usage_clear(hi, usage, bit, max, m.map_to_event_type, m.map_to_input_code);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "-> mapped to input-code 0x%03X (%i)\n", m.map_to_input_code, m.map_to_input_code);
	return RET_MAP_STATIC;
}


/*
 * HID RAW EVENT HOOK
 *
 * TODO:
 * maybe we have to parse the event for battery report and so on by hand
 * take a look at hid-sony.c
 */

int xpadneo_raw_event (struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	dbg_hex_dump_lvl(DBG_LVL_ALL, "xpadneo: raw_event: ", data, size);

	return 0;
}


/*
 * REPORT FIXUP HOOK
 * 
 * This is only used for development purposes
 * (printing out the whole report descriptor)
 */

static u8 *xpadneo_report_fixup (struct hid_device *hdev, u8 *rdesc,
	unsigned int *rsize)
{
	dbg_hex_dump_lvl(DBG_LVL_FEW, "xpadneo: report-descriptor: ", rdesc, *rsize);

	return rdesc;
}


/*
 * INPUT CONFIGURED HOOK
 *
 * We have to fix up the key-bitmap, because there is
 * no DPAD_UP, _RIGHT, _DOWN, _LEFT on the device by default
 *
 * TODO:
 * Furthermore we can set the idev value in xpadneo_data
 */

static int xpadneo_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct input_dev *input = hi->input;

	/*
	 * Add BTN_DPAD_* to the key-bitmap, since they where not originally
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
	if(dpad_as_buttons){
		__set_bit(BTN_DPAD_UP, input->keybit);
		__set_bit(BTN_DPAD_RIGHT, input->keybit);
		__set_bit(BTN_DPAD_DOWN, input->keybit);
		__set_bit(BTN_DPAD_LEFT, input->keybit);

		__clear_bit(ABS_HAT0X, input->absbit);
		__clear_bit(ABS_HAT0Y, input->absbit); /* TODO: necessary? */
	}

	/*
	 * In addition to adding new keys to the key-bitmap, we may also
	 * want to remove the old (original) axis from the absolues-bitmap.
	 * 
	 * TODO:
	 * Maybe we want both, our custom and the original mapping.
	 * If we decide so, remember that 0x39 is a hat switch on the official
	 * usage tables, but not at the input subsystem, so be sure to use the
	 * right constant!
	 * 
	 * Either let hid-core decide itself or input_ev it to ABS_HAT0X/Y by hand:
	 * #define ABS_HAT0X		0x10
	 * #define ABS_HAT0Y		0x11
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
 * EVENT HOOK
 *
 * This hook is called whenever an input event occurs that
 * is listed on xpadneo_driver.usage_table (which is NULL in our case,
 * therefore it is invoked on every input event).
 * 
 * We use this hook to attach some more events to our D-pad,
 * as the result our D-pad is reported to Input as both,
 * four buttons AND a hat-switch.
 * 
 * Before we can send additional input events, we have to enable
 * the corresponding keys in xpadneo_input_configured.
 */

int xpadneo_event (struct hid_device *hdev, struct hid_field *field,
	struct hid_usage *usage, __s32 value)
{
	/* return codes */
	#define EV_CONT_PROCESSING 0 /* let the hid-core autodetect the event */
	#define EV_STOP_PROCESSING 1 /* stop further processing */

	/* TODO: use hid_get_drvdata instead */
	struct hid_input *hidinput = list_entry(hdev->inputs.next, struct hid_input, list);
	struct input_dev *idev = hidinput->input;

	hid_dbg_lvl(DBG_LVL_SOME, hdev, "hid-upage: %02x, hid-usage: %02x, input-code: %02x, value: %02x\n", (usage->hid & HID_USAGE_PAGE), (usage->hid & HID_USAGE), usage->code, value );


	/* Yep, this is the D-pad event */
	if ((usage->hid & HID_USAGE) == 0x39) {

		/* NOTE:
		 * You can also press UP and RIGHT, RIGHT and DOWN, ... together!
		 * The value then is between:
		 * 
		 * value        U R D L
		 * --------------------
		 * 0000   0     0 0 0 0
		 * 0001   1     1 0 0 0
		 * 0010   2     1 1 0 0
		 * 0011   3     0 1 0 0
		 * 0100   4     0 1 1 0
		 * 0101   5     0 0 1 0
		 * 0110   6     0 0 1 1
		 * 0111   7     0 0 0 1
		 * 1000   8     1 0 0 1
		 * 
		 * U = ((value >= 1) && (value <= 2)) || (value == 8)
		 * R = (value >= 2) && (value <= 4)
		 * D = (value >= 4) && (value <= 6)
		 * L = (value >= 6) && (value <= 8)
		 */

		/* TODO:
		 * - XOR is most probably faster, use something like that
		 *   Macro: XOR_3 = (a^b^c) && !(a&&b&&c)
		 *   input_report_key(idev, BTN_DPAD_UP, XOR(value & 0b0111));
		 * - even faster: lookup table {{0,0,0,0}, {1,0,0,0}, ...}
		 * - do we really need to send FOUR events?
		 *   we can surely combine them into one! (if that is anyhow better)
		 */

		/* NOTE:
		 * It is perfectly fine to send those event even if dpad_as_buttons is false
		 * because the keymap decides if the event is really sent or not.
		 * It is also easier to send them anyway, because dpad_as_buttons may
		 * change also if the controller is connected.
		 * This way the behaviour does not change until the controller is reconnected
		 */
		input_report_key(idev, BTN_DPAD_UP, (((value >= 1) && (value <= 2)) || (value == 8)));
		input_report_key(idev, BTN_DPAD_RIGHT, ((value >= 2) && (value <= 4)));
		input_report_key(idev, BTN_DPAD_DOWN, ((value >= 4) && (value <= 6)));
		input_report_key(idev, BTN_DPAD_LEFT, ((value >= 6) && (value <= 8)));

		return EV_CONT_PROCESSING;
	}

	return EV_CONT_PROCESSING;
}


/*
 * DEVICE PROBE AND REMOVE HOOK
 */

static int xpadneo_probe_device (struct hid_device *hdev,
	const struct hid_device_id *id)
{
	int ret;
	struct xpadneo_devdata *xdata;

	hid_dbg_lvl(DBG_LVL_FEW, hdev, "hello %s\n", hdev->name);


	/* create a "nearly globally" accessible data structure
	 * (accessible through hid_get_drvdata)
	 * the structure is freed automatically as soon as hdev->dev is removed
	 */
	xdata = devm_kzalloc(&hdev->dev, sizeof(*xdata), GFP_KERNEL);
	if (xdata == NULL) {
		hid_err(hdev, "can't allocate xdata\n");
		return -ENOMEM;
	}
	xdata->hdev = hdev;
	hid_set_drvdata(hdev, xdata);

	/* parse the raw report (includes a call to report_fixup) */
	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed\n");
		goto err;
	}

	/* debugging */
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "hdev:\n");
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- dev_rdesc: (see above)\n"); // this is the original hid device descriptor
	// rdesc: this is the hid device descriptor after fixup!
	// debug report: sudo cat /sys/kernel/debug/hid/0005:045E:02FD.000B/rdesc
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- dev_rsize (original report size): %u\n", hdev->dev_rsize);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- bus: 0x%04X\n", hdev->bus);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- report group: %u\n", hdev->group);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- vendor: 0x%08X\n", hdev->vendor);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- version: 0x%08X\n", hdev->version);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- product: 0x%08X\n", hdev->product);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- country: %u\n", hdev->country);
	hid_dbg_lvl(DBG_LVL_FEW, hdev, "- driverdata: %lu\n", id->driver_data);

	/* we start our hardware without FF, we will add it afterwards by hand
	 * HID_CONNECT_DEFAULT
	 * = (HID_CONNECT_HIDINPUT|HID_CONNECT_HIDRAW|HID_CONNECT_HIDDEV|HID_CONNECT_FF)
	 */
	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT & ~HID_CONNECT_FF);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		goto err;
	}

	/* call the device initialization routine, including a "hello"-rumblepackage */
	xpadneo_initDevice(hdev);

	/* everything is fine */
	return 0;

err:
	return ret;
}


static void xpadneo_remove_device(struct hid_device *hdev)
{

	hid_dbg_lvl(DBG_LVL_SOME, hdev, "trying to stop underlying hid hw...\n");
	hid_hw_stop(hdev);
	/* TODO: do we need hid_hw_close too? */

	hid_dbg_lvl(DBG_LVL_FEW, hdev, "goodbye %s\n", hdev->name);
}


/*
 * Device ID Structure
 * 
 * Define all supported devices here
 */

static const struct hid_device_id xpadneo_devices[] = {
	/* TODO:
	 * looks like the ProductID is related to the Firmware Version, please verify!
	 * (it somehow changed from 0x02FD to 0x02E0 on one controller here)
	 * update: it changed back again - what the heck?!
	 * update2: 0x02fd seems to be the newer firmware, 0x02e0 is the old one
	 *          unfortunately you cannot tell from product id which mapping is used
	 *          since it looks like the newer firmware supports both mappings 
	 */

	/* XBOX ONE S */
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_MICROSOFT, 0x02FD) },
	/* XBOX ONE X (project scorpio) */
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_MICROSOFT, 0x02E0) },
	/* SENTINEL VALUE, indicates the end*/
	{ }
};

static struct hid_driver xpadneo_driver = {
	/* the name of the driver */
	.name = "xpadneo",

	/* which devices is this driver for */
	.id_table = xpadneo_devices,

	/* hooked as the input device is configured (and before it is registered)
	 * we need that because we do not configure the input-device ourself
	 * but leave it up to hid_hw_start()
	 */
	.input_configured = xpadneo_input_configured,

	/* invoked on input registering before mapping an usage */
	.input_mapping = xpadneo_mapping,

	/* if usage in usage_table, this hook is called
	 * we use this to fixup wrong events (like DPAD)
	 */
	.event = xpadneo_event,

	/* called before report descriptor parsing (NULL means nop) */
	.report_fixup = xpadneo_report_fixup,

	/* called when a new device is inserted */
	.probe = xpadneo_probe_device,

	/* called when a device is removed */
	.remove = xpadneo_remove_device,

	/* if report in report_table, this hook is called */
	.raw_event = xpadneo_raw_event
};

MODULE_DEVICE_TABLE(hid, xpadneo_devices);



/*
 * MODULE INIT AND EXIT
 * 
 * NOTE:
 * we may replace init and remove by module_hid_driver(xpadneo_driver)
 * in future versions, as long as there is nothing special in these two functions but
 * registering and unregistering the driver. up to now it is more useful for us to not "oversimplify"
 * the whole driver-registering thing
 * do _not_ use both! (module_hid_driver and hid_register/unregister_driver)
 */

static int __init xpadneo_initModule(void)
{
	printk("%s: hello there!\n", xpadneo_driver.name);
	return hid_register_driver(&xpadneo_driver);
}

static void __exit xpadneo_exitModule(void)
{
	hid_unregister_driver(&xpadneo_driver);
	printk("%s: goodbye!\n", xpadneo_driver.name);
}

/* telling the driver system which functions to call at initialization and removal of the module */
module_init(xpadneo_initModule);
module_exit(xpadneo_exitModule);




#if 0

//////////////////////
/// REWORK SECTION ///
//////////////////////

// we waste A LOT OF memory the way it is done at the moment just for speed reasons,
// but mapping is done only once at the beginning, therefore speed is not such a big deal.

struct input_ev {
	u8 map_to_event_type;	/* to which input event should the hid usage be mapped? (EV_KEY, EV_ABS, ...) */
	u16 map_to_input_code;	/* to which input code do you want to input_ev? (BTN_SOUTH, ABS_X, ...) */
};

u8 map_hid_to_input_0 (struct hid_usage *usage, struct input_ev *map_to) {

	unsigned int hid_usage = usage->hid & HID_USAGE;
	unsigned int hid_usage_page = usage->hid & HID_USAGE_PAGE;

	switch (hid_usage_page) {
	case HID_UP_BUTTON:
		switch (hid_usage) {
		case 0x01: *map_to = (struct input_ev){EV_KEY, BTN_SOUTH};   return MAP_STATIC;
		case 0x02: *map_to = (struct input_ev){EV_KEY, BTN_EAST};    return MAP_STATIC;
		case 0x03: *map_to = (struct input_ev){EV_KEY, BTN_WEST};    return MAP_STATIC;
		case 0x04: *map_to = (struct input_ev){EV_KEY, BTN_NORTH};   return MAP_STATIC;
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
	}

	return MAP_IGNORE;
}

u8 map_hid_to_input_rs335 (struct hid_usage *usage, struct input_ev *map_to) {

	// report-descriptor:
	// 05 01 09 05 a1 01 85 01 09 01 a1 00 09 30 09 31 15 00 27 ff ff 00 00 95 02 75 10 81 02 c0 09 01
	// a1 00 09 32 09 35 15 00 27 ff ff 00 00 95 02 75 10 81 02 c0 05 02 09 c5 15 00 26 ff 03 95 01 75
	// 0a 81 02 15 00 25 00 75 06 95 01 81 03 05 02 09 c4 15 00 26 ff 03 95 01 75 0a 81 02 15 00 25 00
	// 75 06 95 01 81 03 05 01 09 39 15 01 25 08 35 00 46 3b 01 66 14 00 75 04 95 01 81 42 75 04 95 01
	// 15 00 25 00 35 00 45 00 65 00 81 03 05 09 19 01 29 0f 15 00 25 01 75 01 95 0f 81 02 15 00 25 00
	// 75 01 95 01 81 03 05 0c 0a 24 02 15 00 25 01 95 01 75 01 81 02 15 00 25 00 75 07 95 01 81 03 05
	// 0c 09 01 85 02 a1 01 05 0c 0a 23 02 15 00 25 01 95 01 75 01 81 02 15 00 25 00 75 07 95 01 81 03
	// c0 05 0f 09 21 85 03 a1 02 09 97 15 00 25 01 75 04 95 01 91 02 15 00 25 00 75 04 95 01 91 03 09
	// 70 15 00 25 64 75 08 95 04 91 02 09 50 66 01 10 55 0e 15 00 26 ff 00 75 08 95 01 91 02 09 a7 15
	// 00 26 ff 00 75 08 95 01 91 02 65 00 55 00 09 7c 15 00 26 ff 00 75 08 95 01 91 02 c0 85 04 05 06
	// 09 20 15 00 26 ff 00 75 08 95 01 81 02 c0 00
	// hdev:
	// - dev_rdesc: (see above)
	// - dev_rsize (original report size): 335
	// - bus: 0x0005
	// - report group: 0
	// - vendor: 0x0000045E
	// - version: 0x00000903
	// - product: 0x000002FD
	// - country: 33
	// - driverdata: 0

	unsigned int hid_usage = usage->hid & HID_USAGE;
	unsigned int hid_usage_page = usage->hid & HID_USAGE_PAGE;

	switch(hid_usage_page) {
	case HID_UP_BUTTON:
		switch (hid_usage) {
		case 0x01: *map_to = (struct input_ev){EV_KEY, BTN_SOUTH};   return MAP_STATIC;
		case 0x02: *map_to = (struct input_ev){EV_KEY, BTN_EAST};    return MAP_STATIC;
		case 0x04: *map_to = (struct input_ev){EV_KEY, BTN_WEST};    return MAP_STATIC;
		case 0x05: *map_to = (struct input_ev){EV_KEY, BTN_NORTH};   return MAP_STATIC;
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
};

u8 map_hid_to_input_2 (struct hid_usage *usage, struct input_ev *map_to) {

	unsigned int hid_usage = usage->hid & HID_USAGE;
	unsigned int hid_usage_page = usage->hid & HID_USAGE_PAGE;

	switch (hid_usage_page) {
	case HID_UP_BUTTON:
		switch (hid_usage) {
		case 0x01: *map_to = (struct input_ev){EV_KEY, BTN_SOUTH};  return MAP_STATIC;
		case 0x02: *map_to = (struct input_ev){EV_KEY, BTN_EAST};   return MAP_STATIC;
		case 0x03: *map_to = (struct input_ev){EV_KEY, BTN_WEST};   return MAP_STATIC;
		case 0x04: *map_to = (struct input_ev){EV_KEY, BTN_NORTH};  return MAP_STATIC;
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
		case 0x32: *map_to = (struct input_ev){EV_ABS, ABS_RX};   return MAP_STATIC;
		case 0x35: *map_to = (struct input_ev){EV_ABS, ABS_RY};   return MAP_STATIC;
		case 0x39: *map_to = (struct input_ev){0, 0};             return MAP_AUTO;
		case 0x85: *map_to = (struct input_ev){EV_KEY, BTN_MODE}; return MAP_STATIC;
		}
	case HID_UP_SIMULATION:
		switch (hid_usage) {
		case 0xC4: *map_to = (struct input_ev){EV_ABS, ABS_RZ}; return MAP_STATIC;
		case 0xC5: *map_to = (struct input_ev){EV_ABS, ABS_Z};  return MAP_STATIC;
		}
	case HID_UP_CONSUMER:
		switch (hid_usage) {
		case 0x223: *map_to = (struct input_ev){EV_KEY, BTN_MODE};
		}
	}

	return MAP_IGNORE;
};

#endif
