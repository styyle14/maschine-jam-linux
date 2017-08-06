#include <linux/device.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/hid.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/rawmidi.h>

#include "hid-ids.h"

#define MASCHINE_JAM_NUMBER_KNOBS 2
#define MASCHINE_JAM_HID_REPORT_01_KNOB_BITS 4
#define MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES (MASCHINE_JAM_NUMBER_KNOBS * MASCHINE_JAM_HID_REPORT_01_KNOB_BITS) / 8 // 1
#define MASCHINE_JAM_NUMBER_BUTTONS 120
#define MASCHINE_JAM_HID_REPORT_01_BUTTON_BITS 1
#define MASCHINE_JAM_HID_REPORT_01_BUTTONS_BYTES (MASCHINE_JAM_NUMBER_BUTTONS * MASCHINE_JAM_HID_REPORT_01_BUTTON_BITS) / 8 // 15
#define MASCHINE_JAM_NUMBER_SMARTSTRIPS 16
#define MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_BITS 24
#define MASCHINE_JAM_HID_REPORT_02_SMARTSTRIPS_BYTES (MASCHINE_JAM_NUMBER_SMARTSTRIPS * MASCHINE_JAM_HID_REPORT_02_SMARTSTRIP_BITS) / 8 // 48

#define MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS VERIFY_OCTAL_PERMISSIONS(0664)

enum maschine_jam_midi_type{
	MJ_MIDI_TYPE_NOTE = 0x90,
	MJ_MIDI_TYPE_AFTERTOUCH = 0xA0,
	MJ_MIDI_TYPE_CONTROL_CHANGE = 0xB0
};
#define MJ_MIDI_TYPE_NOTE_STRING "note\n"
#define MJ_MIDI_TYPE_AFTERTOUCH_STRING "aftertouch\n"
#define MJ_MIDI_TYPE_CONTROL_CHANGE_STRING "control_change\n"

struct maschine_jam_midi_config {
	enum maschine_jam_midi_type type;
	uint8_t channel; // lower nibble 0-15
	uint8_t key; // 0-127
	uint8_t value_min; // 0
	uint8_t value_max; // 127
};

struct maschine_jam_driver_data {
	// Device Information
	struct hid_device 		*mj_hid_device;

	// Inputs
	struct maschine_jam_midi_config	midi_in_knob_configs[MASCHINE_JAM_NUMBER_KNOBS];
	uint8_t					hid_report01_knobs[MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES];
	struct maschine_jam_midi_config	midi_in_button_configs[MASCHINE_JAM_NUMBER_BUTTONS];
	uint8_t					hid_report01_buttons[MASCHINE_JAM_HID_REPORT_01_BUTTONS_BYTES];
	struct maschine_jam_midi_config	midi_in_smartstrip_configs[MASCHINE_JAM_NUMBER_SMARTSTRIPS];
	uint8_t					hid_report02_smartstrips[MASCHINE_JAM_HID_REPORT_02_SMARTSTRIPS_BYTES];

	// Outputs
	uint8_t					midi_out_led_buttons_mapping[120];
	uint8_t					hid_report_led_buttons_1[37];
	uint8_t					hid_report_led_buttons_2[16];
	spinlock_t				hid_report_led_buttons_lock;
	struct work_struct		hid_report_led_buttons_work;  
	uint8_t					midi_out_led_pads_mapping[120];
	uint8_t					hid_report_led_pads[80];
	spinlock_t				hid_report_led_pads_lock;
	struct work_struct		hid_report_led_pads_work;
	uint8_t					midi_out_led_strips_mapping[8];
	uint8_t					hid_report_led_strips[88];
	spinlock_t				hid_report_led_strips_lock;
	struct work_struct		hid_report_led_strips_work;
	struct work_struct		hid_report_write_report_work;
	
	// Sysfs Interface
	struct kobject *directory_inputs;
	struct kobject *directory_inputs_knobs;
	struct kobject *directory_inputs_buttons;
	struct kobject *directory_inputs_smartstrips;
	struct kobject *directory_outputs;
	struct kobject *directory_outputs_leds;
	struct kobject *directory_outputs_leds_buttons;
	struct kobject *directory_outputs_leds_smartstrips;

	// Sound/Midi Interface
	struct snd_card			*sound_card;
	int						sound_card_device_number;
	struct snd_rawmidi		*rawmidi_interface;
	struct snd_rawmidi_substream	*midi_in_substream;
	unsigned long			midi_in_up;
	spinlock_t				midi_in_lock;
	struct snd_rawmidi_substream	*midi_out_substream;
	unsigned long			midi_out_up;
	spinlock_t				midi_out_lock;
};

static void maschine_jam_hid_write_report(struct work_struct *);
static void maschine_jam_initialize_driver_data(struct maschine_jam_driver_data *mj_driver_data, struct hid_device *mj_hid_device){
	unsigned int i;
	
	// HID Device
	mj_driver_data->mj_hid_device = mj_hid_device;
	
	// Inputs
	for(i = 0; i < MASCHINE_JAM_NUMBER_KNOBS; i++){
		mj_driver_data->midi_in_knob_configs[i].type = MJ_MIDI_TYPE_NOTE;
		mj_driver_data->midi_in_knob_configs[i].channel = 0;
		mj_driver_data->midi_in_knob_configs[i].key = i;
		mj_driver_data->midi_in_knob_configs[i].value_min = 0;
		mj_driver_data->midi_in_knob_configs[i].value_max = 0x7F; // 127
	}
	memset(mj_driver_data->hid_report01_knobs, 0, sizeof(mj_driver_data->hid_report01_knobs));
	for(i = 0; i < MASCHINE_JAM_NUMBER_BUTTONS; i++){
		mj_driver_data->midi_in_button_configs[i].type = MJ_MIDI_TYPE_NOTE;
		mj_driver_data->midi_in_button_configs[i].channel = 0;
		mj_driver_data->midi_in_button_configs[i].key = i + MASCHINE_JAM_NUMBER_KNOBS;
		mj_driver_data->midi_in_button_configs[i].value_min = 0;
		mj_driver_data->midi_in_button_configs[i].value_max = 0x7F; // 127
	}
	memset(mj_driver_data->hid_report01_buttons, 0, sizeof(mj_driver_data->hid_report01_buttons));
	for(i = 0; i < MASCHINE_JAM_NUMBER_SMARTSTRIPS; i++){
		mj_driver_data->midi_in_smartstrip_configs[i].type = MJ_MIDI_TYPE_CONTROL_CHANGE;
		mj_driver_data->midi_in_smartstrip_configs[i].channel = 0;
		mj_driver_data->midi_in_smartstrip_configs[i].key = i;
		mj_driver_data->midi_in_smartstrip_configs[i].value_min = 0;
		mj_driver_data->midi_in_smartstrip_configs[i].value_max = 0x7F; // 127
	}
	memset(mj_driver_data->hid_report02_smartstrips, 0, sizeof(mj_driver_data->hid_report02_smartstrips));
	
	// Outputs
	memset(mj_driver_data->hid_report_led_buttons_1, 0, sizeof(mj_driver_data->hid_report_led_buttons_1));
	memset(mj_driver_data->hid_report_led_buttons_2, 0, sizeof(mj_driver_data->hid_report_led_buttons_2));
	spin_lock_init(&mj_driver_data->hid_report_led_buttons_lock);
	INIT_WORK(&mj_driver_data->hid_report_led_buttons_work, maschine_jam_hid_write_report);
	memset(mj_driver_data->hid_report_led_pads, 0, sizeof(mj_driver_data->hid_report_led_pads));
	spin_lock_init(&mj_driver_data->hid_report_led_pads_lock);
	INIT_WORK(&mj_driver_data->hid_report_led_pads_work, maschine_jam_hid_write_report);
	memset(mj_driver_data->hid_report_led_strips, 0, sizeof(mj_driver_data->hid_report_led_strips));
	spin_lock_init(&mj_driver_data->hid_report_led_strips_lock);
	INIT_WORK(&mj_driver_data->hid_report_led_strips_work, maschine_jam_hid_write_report);
	INIT_WORK(&mj_driver_data->hid_report_write_report_work, maschine_jam_hid_write_report);
	
	// Sysfs Interface
	mj_driver_data->directory_inputs = NULL;
	mj_driver_data->directory_inputs_knobs = NULL;
	mj_driver_data->directory_inputs_buttons = NULL;
	mj_driver_data->directory_inputs_smartstrips = NULL;
	mj_driver_data->directory_outputs = NULL;
	mj_driver_data->directory_outputs_leds = NULL;
	mj_driver_data->directory_outputs_leds_buttons = NULL;
	mj_driver_data->directory_outputs_leds_smartstrips = NULL;
	
	// Sound/Midi Interface
	mj_driver_data->sound_card = NULL;
	mj_driver_data->rawmidi_interface = NULL;
	mj_driver_data->midi_in_substream = NULL;
	mj_driver_data->midi_in_up = 0;
	spin_lock_init(&mj_driver_data->midi_in_lock);
	mj_driver_data->midi_out_substream = NULL;
	mj_driver_data->midi_out_up = 0;
	spin_lock_init(&mj_driver_data->midi_out_lock);
}

static void maschine_jam_hid_write_report(struct work_struct *work){
	int i;
	unsigned char buffer[54];
	struct maschine_jam_driver_data *mj_driver_data = container_of(work, struct maschine_jam_driver_data, hid_report_write_report_work);
	printk(KERN_ALERT "maschine_jam_hid_write_report() - 1");
	buffer[0] = 0x80;
	spin_lock(&mj_driver_data->hid_report_led_buttons_lock);
	for(i=0;i<sizeof(mj_driver_data->hid_report_led_buttons_1)/sizeof(mj_driver_data->hid_report_led_buttons_1[0]); i++){
		buffer[i+1] = mj_driver_data->hid_report_led_buttons_1[i];
	}
	for(i=0;i<sizeof(mj_driver_data->hid_report_led_buttons_2)/sizeof(mj_driver_data->hid_report_led_buttons_2[0]); i++){
		buffer[i+sizeof(mj_driver_data->hid_report_led_buttons_1)/sizeof(mj_driver_data->hid_report_led_buttons_1[0])] = mj_driver_data->hid_report_led_buttons_2[i];
	}
	spin_unlock(&mj_driver_data->hid_report_led_buttons_lock);
	hid_hw_output_report(mj_driver_data->mj_hid_device, (unsigned char*)&buffer, sizeof(buffer));
	printk(KERN_ALERT "maschine_jam_hid_write_report() - 2");
}

static int maschine_jam_write_midi_note(struct maschine_jam_driver_data *mj_driver_data,
	unsigned char status, unsigned char note, unsigned char velocity){
	int bytes_transmitted = 0;
	unsigned long flags;
	unsigned char buffer[3];

	buffer[0] = status;
	buffer[1] = note;
	buffer[2] = velocity;

	printk(KERN_ALERT "write_midi_note() - %02X %02X %02X", status, note, velocity);

	spin_lock_irqsave(&mj_driver_data->midi_in_lock, flags);
	if (mj_driver_data->midi_in_substream){
		bytes_transmitted = snd_rawmidi_receive(mj_driver_data->midi_in_substream, buffer, 3);
	}
	spin_unlock_irqrestore(&mj_driver_data->midi_in_lock, flags);
	printk(KERN_ALERT "snd_rawmidi_transmit() - %d", bytes_transmitted);

	return bytes_transmitted;
}

static inline uint8_t maschine_jam_get_knob_nibble(u8 *data, uint8_t offset){
	return (data[offset / 2] >> ((offset % 2) * 4)) & 0x0F;
}
// https://graphics.stanford.edu/~seander/bithacks.html#MaskedMerge
// a ^ ((a ^ b) & mask) = r = (a & ~mask) | (b & mask)
static inline void maschine_jam_set_knob_nibble(u8 *data, uint8_t offset, uint8_t value){
	data[offset / 2] = data[offset / 2] ^ ((data[offset / 2] ^ value) & (0x0F << ((offset % 2) * 4)));
}
static inline uint8_t maschine_jam_get_button_bit(u8 *data, uint8_t offset){
	return test_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static inline void maschine_jam_set_button_bit(u8 *data, uint8_t offset){
	set_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static inline void maschine_jam_clear_button_bit(u8 *data, uint8_t offset){
	clear_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static inline void maschine_jam_toggle_button_bit(u8 *data, uint8_t offset){
	change_bit(offset % 8, (long unsigned int*)&data[offset / 8]);
}
static int maschine_jam_parse_report01(struct maschine_jam_driver_data *mj_driver_data, struct hid_report *report, u8 *data, int size){
	int return_value = 0;
	unsigned int button_bit, knob_nibble, knobs_data_index, buttons_data_index;
	uint8_t old_knob_value, new_knob_value, old_button_value, new_button_value;
	struct maschine_jam_midi_config *knob_config;
	struct maschine_jam_midi_config *button_config;
	struct hid_field *knobs_hid_field = report->field[0];
	struct hid_field *buttons_hid_field = report->field[1];

	//printk(KERN_ALERT "report - %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X", data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);

	knobs_data_index = knobs_hid_field->report_offset / 8;
	for (knob_nibble = 0; knob_nibble < knobs_hid_field->report_count && knob_nibble < MASCHINE_JAM_NUMBER_KNOBS; knob_nibble++){
		old_knob_value = maschine_jam_get_knob_nibble(mj_driver_data->hid_report01_knobs, knob_nibble);
		new_knob_value = maschine_jam_get_knob_nibble(&data[knobs_data_index], knob_nibble);
		if (old_knob_value != new_knob_value){
			//printk(KERN_ALERT "knob_nibble: %d, old value: %d, new value: %d", knob_nibble, old_knob_value, new_knob_value);
			knob_config = &mj_driver_data->midi_in_knob_configs[knob_nibble];
			return_value = maschine_jam_write_midi_note(mj_driver_data, knob_config->type | knob_config->channel, knob_config->key, new_knob_value * 0x02);
			maschine_jam_set_knob_nibble(mj_driver_data->hid_report01_knobs, knob_nibble, new_knob_value);
		}
	}
	buttons_data_index = buttons_hid_field->report_offset / 8;
	for (button_bit = 0; button_bit < buttons_hid_field->report_count && button_bit < MASCHINE_JAM_NUMBER_BUTTONS; button_bit++){
		old_button_value = maschine_jam_get_button_bit(mj_driver_data->hid_report01_buttons, button_bit);
		new_button_value = maschine_jam_get_button_bit(&data[buttons_data_index], button_bit);
		if (old_button_value != new_button_value){
			//printk(KERN_ALERT "button_bit: %d, old value: %d, new value: %d", button_bit, old_button_value, new_button_value);
			button_config = &mj_driver_data->midi_in_button_configs[button_bit];
			maschine_jam_toggle_button_bit(mj_driver_data->hid_report01_buttons, button_bit);
			return_value = maschine_jam_write_midi_note(mj_driver_data, button_config->type | button_config->channel, button_config->key, button_config->value_max * new_button_value);
		}
	}
	return return_value;
}
struct smartstrip_triplet {
	uint16_t timestamp;
	uint8_t value;
};
static inline struct smartstrip_triplet maschine_jam_get_smartstrip_triplet(u8 *data, uint8_t index){
	u8 *smartstrip_data = &data[index * 3];
	return (struct smartstrip_triplet){.timestamp = smartstrip_data[0] + (smartstrip_data[1] << 8), .value = smartstrip_data[2]};
}
static inline void maschine_jam_set_smartstrip_triplet(u8 *data, uint8_t index, struct smartstrip_triplet triplet){
	u8 *smartstrip_data = &data[index * 3];
	smartstrip_data[0] = triplet.timestamp & 0xFF;
	smartstrip_data[1] = triplet.timestamp >> 8;
	smartstrip_data[2] = triplet.value;	
}
static int maschine_jam_parse_report02(struct maschine_jam_driver_data *mj_driver_data, struct hid_report *report, u8 *data, int size){
	int return_value = 0;
	unsigned int smartstrip_data_index, smartstrip_index;
	struct smartstrip_triplet old_triplet, new_triplet;
	struct maschine_jam_midi_config *smartstrip_config;
	struct hid_field *smartstrips_hid_field = report->field[0];

	//printk(KERN_ALERT "report - %02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
	//data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15],
	//data[16], data[17], data[18], data[19], data[20], data[21], data[22], data[23], data[24], data[25], data[26], data[27], data[28], data[29], data[30], data[31],
	//data[32], data[33], data[34], data[35], data[36], data[37], data[38], data[39], data[40], data[41], data[42], data[43], data[44], data[45], data[46], data[47]);
	
	smartstrip_data_index = smartstrips_hid_field->report_offset / 8;
	for (smartstrip_index = 0; smartstrip_index < smartstrips_hid_field->report_count && smartstrip_index < MASCHINE_JAM_NUMBER_SMARTSTRIPS; smartstrip_index++){
		old_triplet = maschine_jam_get_smartstrip_triplet(mj_driver_data->hid_report02_smartstrips, smartstrip_index);
		new_triplet = maschine_jam_get_smartstrip_triplet(&data[smartstrip_data_index], smartstrip_index);
		if (old_triplet.value != new_triplet.value){
			//printk(KERN_ALERT "old_triplet: smartstrip_index: %d, timestamp: %04X, value %02X", smartstrip_index, old_triplet.timestamp, old_triplet.value);
			//printk(KERN_ALERT "new_triplet: smartstrip_index: %d, timestamp: %04X, value %02X", smartstrip_index, new_triplet.timestamp, new_triplet.value);
			smartstrip_config = &mj_driver_data->midi_in_smartstrip_configs[smartstrip_index];
			maschine_jam_write_midi_note(mj_driver_data, smartstrip_config->type | smartstrip_config->channel, smartstrip_config->key, new_triplet.value / 2);
			maschine_jam_set_smartstrip_triplet(mj_driver_data->hid_report02_smartstrips, smartstrip_index, new_triplet);
		}
	}
	return return_value;
}

static int maschine_jam_raw_event(struct hid_device *mj_hid_device, struct hid_report *report, u8 *data, int size){
	int return_value = 0;
	struct maschine_jam_driver_data *mj_driver_data;

	if (mj_hid_device != NULL && report != NULL && data != NULL && report->id == data[0]){
		mj_driver_data = hid_get_drvdata(mj_hid_device);
		if (report->id == 0x01 && size == MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES + MASCHINE_JAM_HID_REPORT_01_BUTTONS_BYTES + 1){
			maschine_jam_parse_report01(mj_driver_data, report, &data[1], MASCHINE_JAM_HID_REPORT_01_KNOBS_BYTES + MASCHINE_JAM_HID_REPORT_01_BUTTONS_BYTES);
		} else if (report->id == 0x02 && size == MASCHINE_JAM_HID_REPORT_02_SMARTSTRIPS_BYTES + 1){
			maschine_jam_parse_report02(mj_driver_data, report, &data[1], MASCHINE_JAM_HID_REPORT_02_SMARTSTRIPS_BYTES);
		} else {
			printk(KERN_ALERT "maschine_jam_raw_event() - error - report id is unknown or bad data size");
		}
	} else {
		printk(KERN_ALERT "maschine_jam_raw_event() - error - bad parameters");
	}
	return return_value;
}

typedef enum maschine_jam_io_attribute_type_e {
	IO_ATTRIBUTE_KNOB,
	IO_ATTRIBUTE_BUTTON,
	IO_ATTRIBUTE_SMARTSTRIP,
} maschine_jam_io_attribute_type;
struct maschine_jam_io_attribute {
	struct kobj_attribute type_attribute;
	struct kobj_attribute channel_attribute;
	struct kobj_attribute key_attribute;
	struct kobj_attribute status_attribute;
	maschine_jam_io_attribute_type io_attribute_type;
	uint8_t io_index;
};
static ssize_t maschine_jam_inputs_type_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, type_attribute);
	enum maschine_jam_midi_type midi_type;

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		midi_type = mj_driver_data->midi_in_knob_configs[io_attribute->io_index].type;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		midi_type = mj_driver_data->midi_in_button_configs[io_attribute->io_index].type;
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
	switch (midi_type){
		case MJ_MIDI_TYPE_NOTE:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_NOTE_STRING);
		case MJ_MIDI_TYPE_AFTERTOUCH:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_AFTERTOUCH_STRING);
		case MJ_MIDI_TYPE_CONTROL_CHANGE:
			return scnprintf(buf, PAGE_SIZE, "%s", MJ_MIDI_TYPE_CONTROL_CHANGE_STRING);
		default:
			return scnprintf(buf, PAGE_SIZE, "unknown midi type\n");
	}
}
static ssize_t maschine_jam_inputs_type_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, type_attribute);
	enum maschine_jam_midi_type midi_type;

	if (strncmp(buf, MJ_MIDI_TYPE_NOTE_STRING, sizeof(MJ_MIDI_TYPE_NOTE_STRING)) == 0){
		midi_type = MJ_MIDI_TYPE_NOTE;
	} else if (strncmp(buf, MJ_MIDI_TYPE_AFTERTOUCH_STRING, sizeof(MJ_MIDI_TYPE_AFTERTOUCH_STRING)) == 0){
		midi_type = MJ_MIDI_TYPE_AFTERTOUCH;
	} else if (strncmp(buf, MJ_MIDI_TYPE_CONTROL_CHANGE_STRING, sizeof(MJ_MIDI_TYPE_CONTROL_CHANGE_STRING)) == 0){
		midi_type = MJ_MIDI_TYPE_CONTROL_CHANGE;
	} else {
		printk(KERN_ALERT "maschine_jam_inputs_type_store - invalid type");
		return count;
	}
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		mj_driver_data->midi_in_knob_configs[io_attribute->io_index].type = midi_type;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		mj_driver_data->midi_in_button_configs[io_attribute->io_index].type = midi_type;
	}
	return count;
}
static ssize_t maschine_jam_inputs_channel_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, channel_attribute);

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		return scnprintf(buf, PAGE_SIZE, "%d\n", mj_driver_data->midi_in_knob_configs[io_attribute->io_index].channel);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		return scnprintf(buf, PAGE_SIZE, "%d\n", mj_driver_data->midi_in_button_configs[io_attribute->io_index].channel);
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
}
static ssize_t maschine_jam_inputs_channel_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	unsigned int store_value;
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, channel_attribute);

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		mj_driver_data->midi_in_knob_configs[io_attribute->io_index].channel = store_value & 0xF;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		mj_driver_data->midi_in_button_configs[io_attribute->io_index].channel = store_value & 0xF;
	}
	return count;
}
static ssize_t maschine_jam_inputs_key_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, key_attribute);

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		return scnprintf(buf, PAGE_SIZE, "%d\n", mj_driver_data->midi_in_knob_configs[io_attribute->io_index].key);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		return scnprintf(buf, PAGE_SIZE, "%d\n", mj_driver_data->midi_in_button_configs[io_attribute->io_index].key);
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
}
static ssize_t maschine_jam_inputs_key_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	unsigned int store_value;
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, key_attribute);

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		mj_driver_data->midi_in_knob_configs[io_attribute->io_index].key = store_value & 0x7F;
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		mj_driver_data->midi_in_button_configs[io_attribute->io_index].key = store_value & 0x7F;
	}
	return count;
}
static ssize_t maschine_jam_inputs_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, status_attribute);

	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_knob_nibble(mj_driver_data->hid_report01_knobs, io_attribute->io_index));
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		return scnprintf(buf, PAGE_SIZE, "%d\n", maschine_jam_get_button_bit(mj_driver_data->hid_report01_buttons, io_attribute->io_index));
	} else {
		return scnprintf(buf, PAGE_SIZE, "unknown attribute type\n");
	}
}
static ssize_t maschine_jam_inputs_status_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count){
	unsigned int store_value;
	struct kobject *maschine_jam_inputs_button_dir = kobj;
	struct kobject *maschine_jam_inputs_dir = maschine_jam_inputs_button_dir->parent;
	struct kobject *maschine_jam_kobj = maschine_jam_inputs_dir->parent;
	struct device *dev = container_of(maschine_jam_kobj, struct device, kobj);
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct maschine_jam_driver_data *mj_driver_data = hid_get_drvdata(hdev);
	struct maschine_jam_io_attribute *io_attribute = container_of(attr, struct maschine_jam_io_attribute, status_attribute);

	sscanf(buf, "%u", &store_value);
	if (io_attribute->io_attribute_type == IO_ATTRIBUTE_KNOB){
		maschine_jam_set_knob_nibble(mj_driver_data->hid_report01_knobs, io_attribute->io_index, store_value & 0x0F);
	} else if (io_attribute->io_attribute_type == IO_ATTRIBUTE_BUTTON){
		if (store_value == 0){
			maschine_jam_clear_button_bit(mj_driver_data->hid_report01_buttons, io_attribute->io_index);
		} else {
			maschine_jam_set_button_bit(mj_driver_data->hid_report01_buttons, io_attribute->io_index);
		}
	}
	return count;
}
#define MJ_INPUTS_KNOB_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_inputs_knob_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = VERIFY_OCTAL_PERMISSIONS(MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS)}, \
			.show = maschine_jam_inputs_type_show, \
			.store = maschine_jam_inputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = VERIFY_OCTAL_PERMISSIONS(MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS)}, \
			.show = maschine_jam_inputs_channel_show, \
			.store = maschine_jam_inputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = VERIFY_OCTAL_PERMISSIONS(MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS)}, \
			.show = maschine_jam_inputs_key_show, \
			.store = maschine_jam_inputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = VERIFY_OCTAL_PERMISSIONS(MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS)}, \
			.show = maschine_jam_inputs_status_show, \
			.store = maschine_jam_inputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_KNOB, \
		.io_index = _index, \
	}; \
	static struct attribute *maschine_jam_inputs_knob_ ## _name ## _attributes[] = { \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_inputs_knob_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_inputs_knob_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_inputs_knob_ ## _name ## _attributes, \
	}
MJ_INPUTS_KNOB_ATTRIBUTE_GROUP(encoder, 0);
//MJ_INPUTS_KNOB_ATTRIBUTE_GROUP(unknown_knob, 1);
static const struct attribute_group *maschine_jam_inputs_knobs_groups[] = {
	&maschine_jam_inputs_knob_encoder_group,
	//&maschine_jam_inputs_knob_unknown_knob_group,
	NULL
};
#define MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(_name, _index) \
	struct maschine_jam_io_attribute maschine_jam_inputs_button_ ## _name ## _attribute = { \
		.type_attribute = { \
			.attr = {.name = "type", .mode = VERIFY_OCTAL_PERMISSIONS(MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS)}, \
			.show = maschine_jam_inputs_type_show, \
			.store = maschine_jam_inputs_type_store, \
		}, \
		.channel_attribute = { \
			.attr = {.name = "channel", .mode = VERIFY_OCTAL_PERMISSIONS(MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS)}, \
			.show = maschine_jam_inputs_channel_show, \
			.store = maschine_jam_inputs_channel_store, \
		}, \
		.key_attribute = { \
			.attr = {.name = "key", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_key_show, \
			.store = maschine_jam_inputs_key_store, \
		}, \
		.status_attribute = { \
			.attr = {.name = "status", .mode = MASCHINE_JAM_SYSFS_ATTRIBUTE_PERMISSIONS}, \
			.show = maschine_jam_inputs_status_show, \
			.store = maschine_jam_inputs_status_store, \
		}, \
		.io_attribute_type = IO_ATTRIBUTE_BUTTON, \
		.io_index = _index, \
	}; \
	static struct attribute *maschine_jam_inputs_button_ ## _name ## _attributes[] = { \
		&maschine_jam_inputs_button_ ## _name ## _attribute.type_attribute.attr, \
		&maschine_jam_inputs_button_ ## _name ## _attribute.channel_attribute.attr, \
		&maschine_jam_inputs_button_ ## _name ## _attribute.key_attribute.attr, \
		&maschine_jam_inputs_button_ ## _name ## _attribute.status_attribute.attr, \
		NULL \
	}; \
	static const struct attribute_group maschine_jam_inputs_button_ ## _name ## _group = { \
		.name = #_name, \
		.attrs = maschine_jam_inputs_button_ ## _name ## _attributes, \
	}
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(song, 0);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_1, 1);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_2, 2);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_3, 3);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_4, 4);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_5, 5);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_6, 6);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_7, 7);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(scene_8, 8);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(step, 9);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(pad_mode, 10);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(clear, 11);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(duplicate, 12);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_up, 13);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_left, 14);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_right, 15);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(navigate_down, 16);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(note_repeat, 17);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x1, 18);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x2, 19);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x3, 20);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x4, 21);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x5, 22);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x6, 23);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x7, 24);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_1x8, 25);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x1, 26);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x2, 27);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x3, 28);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x4, 29);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x5, 30);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x6, 31);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x7, 32);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_2x8, 33);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x1, 34);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x2, 35);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x3, 36);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x4, 37);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x5, 38);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x6, 39);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x7, 40);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_3x8, 41);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x1, 42);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x2, 43);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x3, 44);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x4, 45);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x5, 46);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x6, 47);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x7, 48);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_4x8, 49);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x1, 50);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x2, 51);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x3, 52);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x4, 53);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x5, 54);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x6, 55);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x7, 56);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_5x8, 57);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x1, 58);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x2, 59);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x3, 60);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x4, 61);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x5, 62);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x6, 63);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x7, 64);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_6x8, 65);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x1, 66);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x2, 67);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x3, 68);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x4, 69);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x5, 70);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x6, 71);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x7, 72);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_7x8, 73);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x1, 74);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x2, 75);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x3, 76);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x4, 77);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x5, 78);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x6, 79);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x7, 80);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(matrix_8x8, 81);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_a, 82);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_b, 83);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_c, 84);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_d, 85);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_e, 86);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_f, 87);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_g, 88);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(group_h, 89);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(mst, 90);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(grp, 91);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(in_1, 92);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(cue, 93);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(browse, 94);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(macro, 95);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(level, 96);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(aux, 97);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(control, 98);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(auto, 99);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(perform, 100);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(notes, 101);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(lock, 102);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(tune, 103);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(swing, 104);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(shift, 105);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(play, 106);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(rec, 107);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(page_left, 108);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(page_right, 109);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(tempo, 110);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(grid, 111);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(solo, 112);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(mute, 113);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(select, 114);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(encoder_touch, 115);
MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(encoder_push, 116);
//MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(unknown_117, 117); // footswitch/line-in?
//MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(unknown_118, 118);
//MJ_INPUTS_BUTTON_ATTRIBUTE_GROUP(unknown_119, 119);

static const struct attribute_group *maschine_jam_inputs_buttons_groups[] = {
	&maschine_jam_inputs_button_song_group,
	&maschine_jam_inputs_button_scene_1_group,
	&maschine_jam_inputs_button_scene_2_group,
	&maschine_jam_inputs_button_scene_3_group,
	&maschine_jam_inputs_button_scene_4_group,
	&maschine_jam_inputs_button_scene_5_group,
	&maschine_jam_inputs_button_scene_6_group,
	&maschine_jam_inputs_button_scene_7_group,
	&maschine_jam_inputs_button_scene_8_group,
	&maschine_jam_inputs_button_step_group,
	&maschine_jam_inputs_button_pad_mode_group,
	&maschine_jam_inputs_button_clear_group,
	&maschine_jam_inputs_button_duplicate_group,
	&maschine_jam_inputs_button_navigate_up_group,
	&maschine_jam_inputs_button_navigate_left_group,
	&maschine_jam_inputs_button_navigate_right_group,
	&maschine_jam_inputs_button_navigate_down_group,
	&maschine_jam_inputs_button_note_repeat_group,
	&maschine_jam_inputs_button_matrix_1x1_group,
	&maschine_jam_inputs_button_matrix_1x2_group,
	&maschine_jam_inputs_button_matrix_1x3_group,
	&maschine_jam_inputs_button_matrix_1x4_group,
	&maschine_jam_inputs_button_matrix_1x5_group,
	&maschine_jam_inputs_button_matrix_1x6_group,
	&maschine_jam_inputs_button_matrix_1x7_group,
	&maschine_jam_inputs_button_matrix_1x8_group,
	&maschine_jam_inputs_button_matrix_2x1_group,
	&maschine_jam_inputs_button_matrix_2x2_group,
	&maschine_jam_inputs_button_matrix_2x3_group,
	&maschine_jam_inputs_button_matrix_2x4_group,
	&maschine_jam_inputs_button_matrix_2x5_group,
	&maschine_jam_inputs_button_matrix_2x6_group,
	&maschine_jam_inputs_button_matrix_2x7_group,
	&maschine_jam_inputs_button_matrix_2x8_group,
	&maschine_jam_inputs_button_matrix_3x1_group,
	&maschine_jam_inputs_button_matrix_3x2_group,
	&maschine_jam_inputs_button_matrix_3x3_group,
	&maschine_jam_inputs_button_matrix_3x4_group,
	&maschine_jam_inputs_button_matrix_3x5_group,
	&maschine_jam_inputs_button_matrix_3x6_group,
	&maschine_jam_inputs_button_matrix_3x7_group,
	&maschine_jam_inputs_button_matrix_3x8_group,
	&maschine_jam_inputs_button_matrix_4x1_group,
	&maschine_jam_inputs_button_matrix_4x2_group,
	&maschine_jam_inputs_button_matrix_4x3_group,
	&maschine_jam_inputs_button_matrix_4x4_group,
	&maschine_jam_inputs_button_matrix_4x5_group,
	&maschine_jam_inputs_button_matrix_4x6_group,
	&maschine_jam_inputs_button_matrix_4x7_group,
	&maschine_jam_inputs_button_matrix_4x8_group,
	&maschine_jam_inputs_button_matrix_5x1_group,
	&maschine_jam_inputs_button_matrix_5x2_group,
	&maschine_jam_inputs_button_matrix_5x3_group,
	&maschine_jam_inputs_button_matrix_5x4_group,
	&maschine_jam_inputs_button_matrix_5x5_group,
	&maschine_jam_inputs_button_matrix_5x6_group,
	&maschine_jam_inputs_button_matrix_5x7_group,
	&maschine_jam_inputs_button_matrix_5x8_group,
	&maschine_jam_inputs_button_matrix_6x1_group,
	&maschine_jam_inputs_button_matrix_6x2_group,
	&maschine_jam_inputs_button_matrix_6x3_group,
	&maschine_jam_inputs_button_matrix_6x4_group,
	&maschine_jam_inputs_button_matrix_6x5_group,
	&maschine_jam_inputs_button_matrix_6x6_group,
	&maschine_jam_inputs_button_matrix_6x7_group,
	&maschine_jam_inputs_button_matrix_6x8_group,
	&maschine_jam_inputs_button_matrix_7x1_group,
	&maschine_jam_inputs_button_matrix_7x2_group,
	&maschine_jam_inputs_button_matrix_7x3_group,
	&maschine_jam_inputs_button_matrix_7x4_group,
	&maschine_jam_inputs_button_matrix_7x5_group,
	&maschine_jam_inputs_button_matrix_7x6_group,
	&maschine_jam_inputs_button_matrix_7x7_group,
	&maschine_jam_inputs_button_matrix_7x8_group,
	&maschine_jam_inputs_button_matrix_8x1_group,
	&maschine_jam_inputs_button_matrix_8x2_group,
	&maschine_jam_inputs_button_matrix_8x3_group,
	&maschine_jam_inputs_button_matrix_8x4_group,
	&maschine_jam_inputs_button_matrix_8x5_group,
	&maschine_jam_inputs_button_matrix_8x6_group,
	&maschine_jam_inputs_button_matrix_8x7_group,
	&maschine_jam_inputs_button_matrix_8x8_group,
	&maschine_jam_inputs_button_group_a_group,
	&maschine_jam_inputs_button_group_b_group,
	&maschine_jam_inputs_button_group_c_group,
	&maschine_jam_inputs_button_group_d_group,
	&maschine_jam_inputs_button_group_e_group,
	&maschine_jam_inputs_button_group_f_group,
	&maschine_jam_inputs_button_group_g_group,
	&maschine_jam_inputs_button_group_h_group,
	&maschine_jam_inputs_button_mst_group,
	&maschine_jam_inputs_button_grp_group,
	&maschine_jam_inputs_button_in_1_group,
	&maschine_jam_inputs_button_cue_group,
	&maschine_jam_inputs_button_browse_group,
	&maschine_jam_inputs_button_macro_group,
	&maschine_jam_inputs_button_level_group,
	&maschine_jam_inputs_button_aux_group,
	&maschine_jam_inputs_button_control_group,
	&maschine_jam_inputs_button_auto_group,
	&maschine_jam_inputs_button_perform_group,
	&maschine_jam_inputs_button_notes_group,
	&maschine_jam_inputs_button_lock_group,
	&maschine_jam_inputs_button_tune_group,
	&maschine_jam_inputs_button_swing_group,
	&maschine_jam_inputs_button_shift_group,
	&maschine_jam_inputs_button_play_group,
	&maschine_jam_inputs_button_rec_group,
	&maschine_jam_inputs_button_page_left_group,
	&maschine_jam_inputs_button_page_right_group,
	&maschine_jam_inputs_button_tempo_group,
	&maschine_jam_inputs_button_grid_group,
	&maschine_jam_inputs_button_solo_group,
	&maschine_jam_inputs_button_mute_group,
	&maschine_jam_inputs_button_select_group,
	&maschine_jam_inputs_button_encoder_touch_group,
	&maschine_jam_inputs_button_encoder_push_group,
	//&maschine_jam_inputs_button_unknown_117_group,
	//&maschine_jam_inputs_button_unknown_118_group,
	//&maschine_jam_inputs_button_unknown_119_group,
	NULL
};


static int maschine_jam_snd_dev_free(struct snd_device *dev){
	return 0;
}
static struct snd_device_ops maschine_jam_snd_device_ops = {
	.dev_free = maschine_jam_snd_dev_free,
};
static int maschine_jam_midi_in_open(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *mj_driver_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_open() - 1");
	spin_lock_irq(&mj_driver_data->midi_in_lock);
	mj_driver_data->midi_in_substream = substream;
	spin_unlock_irq(&mj_driver_data->midi_in_lock);
	printk(KERN_ALERT "in_open() - 2");
	return 0;
}

static int maschine_jam_midi_in_close(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *mj_driver_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_close() - 1");
	spin_lock_irq(&mj_driver_data->midi_in_lock);
	mj_driver_data->midi_in_substream = NULL;
	spin_unlock_irq(&mj_driver_data->midi_in_lock);
	printk(KERN_ALERT "in_close() - 2");
	return 0;
}

static void maschine_jam_midi_in_trigger(struct snd_rawmidi_substream *substream, int up){
	unsigned long flags;
	struct maschine_jam_driver_data *mj_driver_data = substream->rmidi->private_data;

	printk(KERN_ALERT "in_trigger() - 1");
	spin_lock_irqsave(&mj_driver_data->midi_in_lock, flags);
	mj_driver_data->midi_in_up = up;
	spin_unlock_irqrestore(&mj_driver_data->midi_in_lock, flags);
	printk(KERN_ALERT "in_trigger() - 2");
}

// MIDI_IN operations
static struct snd_rawmidi_ops maschine_jam_midi_in_ops = {
	.open = maschine_jam_midi_in_open,
	.close = maschine_jam_midi_in_close,
	.trigger = maschine_jam_midi_in_trigger
};

static int maschine_jam_midi_out_open(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *mj_driver_data = substream->rmidi->private_data;

	printk(KERN_ALERT "out_open() - 1");

	spin_lock_irq(&mj_driver_data->midi_out_lock);
	mj_driver_data->midi_out_substream = substream;
	mj_driver_data->midi_out_up = 0;
	spin_unlock_irq(&mj_driver_data->midi_out_lock);

	printk(KERN_ALERT "out_open() - 2");
	return 0;
}

static int maschine_jam_midi_out_close(struct snd_rawmidi_substream *substream){
	struct maschine_jam_driver_data *mj_driver_data = substream->rmidi->private_data;

	printk(KERN_ALERT "out_close() - 1");

	spin_lock_irq(&mj_driver_data->midi_out_lock);
	mj_driver_data->midi_out_substream = NULL;
	mj_driver_data->midi_out_up = 0;
	spin_unlock_irq(&mj_driver_data->midi_out_lock);

	printk(KERN_ALERT "out_close() - 2");   
	return 0;
}

// get virtual midi data and transmit to physical maschine jam, cannot block
static void maschine_jam_midi_out_trigger(struct snd_rawmidi_substream *substream, int up){
	unsigned long flags;
	unsigned char data;
	struct maschine_jam_driver_data *mj_driver_data = substream->rmidi->private_data;

	if (up != 0) {
		while (snd_rawmidi_transmit(substream, &data, 1) == 1) {
			printk(KERN_ALERT "out_trigger data - %d", data);
			spin_lock_irqsave(&mj_driver_data->hid_report_led_buttons_lock, flags);
			mj_driver_data->hid_report_led_buttons_1[0] = data;
			spin_unlock_irqrestore(&mj_driver_data->hid_report_led_buttons_lock, flags);
			schedule_work(&mj_driver_data->hid_report_write_report_work);
		}
	}else{
		printk(KERN_ALERT "out_trigger: up = 0");
	}
	spin_lock_irqsave(&mj_driver_data->midi_out_lock, flags);
	mj_driver_data->midi_out_up = up;
	spin_unlock_irqrestore(&mj_driver_data->midi_out_lock, flags);
}

// MIDI_OUT operations
static struct snd_rawmidi_ops maschine_jam_midi_out_ops = {
	.open = maschine_jam_midi_out_open,
	.close = maschine_jam_midi_out_close,
	.trigger = maschine_jam_midi_out_trigger
};
#define MASCHINE_JAM_SOUND_CARD_DEVICE_NUMBER 0
static int maschine_jam_create_sound_card(struct maschine_jam_driver_data *mj_driver_data){
	const char shortname[] = "MASCHINEJAM";
	const char longname[] = "Maschine Jam";
	int error_code;
	struct snd_card *sound_card;
	struct snd_rawmidi *rawmidi_interface;
	
	/* Setup sound card */
	error_code = snd_card_new(&mj_driver_data->mj_hid_device->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1, THIS_MODULE, 0, &sound_card);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to create Maschine Jam sound card.");
		goto return_error_code;
	}
	strncpy(sound_card->driver, shortname, sizeof(sound_card->driver));
	strncpy(sound_card->shortname, shortname, sizeof(sound_card->shortname));
	strncpy(sound_card->longname, longname, sizeof(sound_card->longname));
	
	/* Setup sound device */
	error_code = snd_device_new(sound_card, SNDRV_DEV_LOWLEVEL, mj_driver_data->mj_hid_device, &maschine_jam_snd_device_ops);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to create Maschine Jam sound device.");
		goto failure_snd_card_free;
	}

	/* Set up rawmidi */
	error_code = snd_rawmidi_new(sound_card, sound_card->shortname, 0, 1, 1, &rawmidi_interface);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to create Maschine Jam rawmidi device.");
		goto failure_snd_device_free;
	}
	strncpy(rawmidi_interface->name, sound_card->longname, sizeof(rawmidi_interface->name));
	rawmidi_interface->info_flags = SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	rawmidi_interface->private_data = mj_driver_data;
	snd_rawmidi_set_ops(rawmidi_interface, SNDRV_RAWMIDI_STREAM_INPUT, &maschine_jam_midi_in_ops);
	snd_rawmidi_set_ops(rawmidi_interface, SNDRV_RAWMIDI_STREAM_OUTPUT, &maschine_jam_midi_out_ops);

	/* Register sound card */
	error_code = snd_card_register(sound_card);
	if (error_code != 0) {
		printk(KERN_ALERT "Failed to register Maschine Jam sound card.");
		goto failure_snd_device_free;
	}
	
	mj_driver_data->sound_card = sound_card;
	mj_driver_data->rawmidi_interface = rawmidi_interface;
	goto return_error_code;

failure_snd_device_free:
	snd_device_free(sound_card, mj_driver_data->mj_hid_device);
failure_snd_card_free:
	snd_card_free(sound_card);
return_error_code:
	return error_code;
}
static int maschine_jam_delete_sound_card(struct maschine_jam_driver_data *mj_driver_data){
	int error_code = 0;
	struct snd_card *sound_card;

	if (mj_driver_data->sound_card != NULL) {
		sound_card = mj_driver_data->sound_card;
		mj_driver_data->sound_card = NULL;
		snd_card_disconnect(sound_card);
		snd_device_free(sound_card, mj_driver_data->mj_hid_device);
		snd_card_free_when_closed(sound_card);
	}
	
	return error_code;
}

static int maschine_jam_create_sysfs_inputs_interface(struct maschine_jam_driver_data *mj_driver_data){
	int error_code = 0;
	struct kobject* directory_inputs = NULL;
	struct kobject* directory_inputs_knobs = NULL;
	struct kobject* directory_inputs_buttons = NULL;
	struct kobject* directory_inputs_smartstrips = NULL;
	struct kobject *device_kobject = &mj_driver_data->mj_hid_device->dev.kobj;

	directory_inputs = kobject_create_and_add("inputs", device_kobject);
	if (directory_inputs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add inputs failed!");
		error_code = -1;
		goto return_error_code;
	}
	directory_inputs_knobs = kobject_create_and_add("knobs", directory_inputs);
	if (directory_inputs_knobs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add knobs failed!");
		error_code = -1;
		goto failure_delete_kobject_inputs;
	}
	error_code = sysfs_create_groups(directory_inputs_knobs, maschine_jam_inputs_knobs_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups knobs failed!");
		goto failure_delete_kobject_inputs_knobs;
	}
	directory_inputs_buttons = kobject_create_and_add("buttons", directory_inputs);
	if (directory_inputs_buttons == NULL) {
		printk(KERN_ALERT "kobject_create_and_add buttons failed!");
		error_code = -1;
		goto failure_remove_inputs_knobs_groups;
	}
	error_code = sysfs_create_groups(directory_inputs_buttons, maschine_jam_inputs_buttons_groups);
	if (error_code < 0) {
		printk(KERN_ALERT "sysfs_create_groups buttons failed!");
		goto failure_delete_kobject_inputs_buttons;
	}
	directory_inputs_smartstrips = kobject_create_and_add("smartstrips", directory_inputs);
	if (directory_inputs_smartstrips == NULL) {
		printk(KERN_ALERT "kobject_create_and_add smartstrips failed!");
		error_code = -1;
		goto failure_remove_inputs_buttons_groups;
	}
	//error_code = sysfs_create_groups(directory_inputs_buttons, maschine_jam_inputs_smartstrips_groups);
	//if (error_code < 0) {
		//printk(KERN_ALERT "sysfs_create_groups failed!");
		//goto failure_delete_kobject_inputs_smartstrips;
	//}
	mj_driver_data->directory_inputs = directory_inputs;
	mj_driver_data->directory_inputs_knobs = directory_inputs_knobs;
	mj_driver_data->directory_inputs_buttons = directory_inputs_buttons;
	mj_driver_data->directory_inputs_smartstrips = directory_inputs_smartstrips;
	goto return_error_code;

//failure_delete_kobject_inputs_smartstrips:
//	kobject_del(directory_inputs_smartstrips);
failure_remove_inputs_buttons_groups:
	sysfs_remove_groups(directory_inputs_buttons, maschine_jam_inputs_buttons_groups);
failure_delete_kobject_inputs_buttons:
	kobject_del(directory_inputs_buttons);
failure_remove_inputs_knobs_groups:
	sysfs_remove_groups(directory_inputs_knobs, maschine_jam_inputs_knobs_groups);
failure_delete_kobject_inputs_knobs:
	kobject_del(directory_inputs_knobs);
failure_delete_kobject_inputs:
	kobject_del(directory_inputs);
return_error_code:
	return error_code;
}
static void maschine_jam_delete_sysfs_inputs_interface(struct maschine_jam_driver_data *mj_driver_data){
	if (mj_driver_data->directory_inputs_smartstrips != NULL){
		//sysfs_remove_groups(mj_driver_data->directory_inputs_smartstrips, maschine_jam_inputs_smartstrips_groups);
		kobject_del(mj_driver_data->directory_inputs_smartstrips);
	}
	if (mj_driver_data->directory_inputs_buttons != NULL){
		sysfs_remove_groups(mj_driver_data->directory_inputs_buttons, maschine_jam_inputs_buttons_groups);
		kobject_del(mj_driver_data->directory_inputs_buttons);
	}
	if (mj_driver_data->directory_inputs_knobs != NULL){
		sysfs_remove_groups(mj_driver_data->directory_inputs_knobs, maschine_jam_inputs_knobs_groups);
		kobject_del(mj_driver_data->directory_inputs_knobs);
	}
	if (mj_driver_data->directory_inputs != NULL){
		kobject_del(mj_driver_data->directory_inputs);
	}
}
static int maschine_jam_create_sysfs_outputs_interface(struct maschine_jam_driver_data *mj_driver_data){
	int error_code = 0;
	struct kobject* directory_outputs = NULL;
	struct kobject* directory_outputs_leds = NULL;
	struct kobject* directory_outputs_leds_buttons = NULL;
	struct kobject* directory_outputs_leds_smartstrips = NULL;
	struct kobject *device_kobject = &mj_driver_data->mj_hid_device->dev.kobj;

	directory_outputs = kobject_create_and_add("outputs", device_kobject);
	if (directory_outputs == NULL) {
		printk(KERN_ALERT "kobject_create_and_add outputs failed!");
		error_code = -1;
		goto return_error_code;
	}
	directory_outputs_leds = kobject_create_and_add("leds", directory_outputs);
	if (directory_outputs_leds == NULL) {
		printk(KERN_ALERT "kobject_create_and_add leds failed!");
		error_code = -1;
		goto failure_delete_kobject_outputs;
	}
	directory_outputs_leds_buttons = kobject_create_and_add("buttons", directory_outputs_leds);
	if (directory_outputs_leds_buttons == NULL) {
		printk(KERN_ALERT "kobject_create_and_add buttons failed!");
		error_code = -1;
		goto failure_delete_kobject_outputs_leds;
	}
	directory_outputs_leds_smartstrips = kobject_create_and_add("smartstrips", directory_outputs_leds);
	if (directory_outputs_leds_smartstrips == NULL) {
		printk(KERN_ALERT "kobject_create_and_add smartstrips failed!");
		error_code = -1;
		goto failure_delete_kobject_outputs_leds_buttons;
	}
	mj_driver_data->directory_outputs = directory_outputs;
	mj_driver_data->directory_outputs_leds = directory_outputs_leds;
	mj_driver_data->directory_outputs_leds_buttons = directory_outputs_leds_buttons;
	mj_driver_data->directory_outputs_leds_smartstrips = directory_outputs_leds_smartstrips;
	goto return_error_code;
	
//failure_delete_kobject_outputs_leds_smartstrips:
	//kobject_del(directory_outputs_leds_smartstrips);
failure_delete_kobject_outputs_leds_buttons:
	kobject_del(directory_outputs_leds_buttons);
failure_delete_kobject_outputs_leds:
	kobject_del(directory_outputs_leds);
failure_delete_kobject_outputs:
	kobject_del(directory_outputs);
return_error_code:
	return error_code;
}
static void maschine_jam_delete_sysfs_outputs_interface(struct maschine_jam_driver_data *mj_driver_data){
	if (mj_driver_data->directory_outputs_leds_smartstrips != NULL){
		kobject_del(mj_driver_data->directory_outputs_leds_smartstrips);
	}
	if (mj_driver_data->directory_outputs_leds_buttons != NULL){
		kobject_del(mj_driver_data->directory_outputs_leds_buttons);
	}
	if (mj_driver_data->directory_outputs_leds != NULL){
		kobject_del(mj_driver_data->directory_outputs_leds);
	}
	if (mj_driver_data->directory_outputs != NULL){
		kobject_del(mj_driver_data->directory_outputs);
	}
}

static int maschine_jam_probe(struct hid_device *mj_hid_device, const struct hid_device_id *id){
	int error_code;
	struct usb_interface *intface = to_usb_interface(mj_hid_device->dev.parent);
	unsigned short interface_number = intface->cur_altsetting->desc.bInterfaceNumber;
	//unsigned long dd = id->driver_data;
	struct maschine_jam_driver_data *mj_driver_data = NULL;

	printk(KERN_ALERT "Found Maschine JAM!");
	if (interface_number != 0){
		printk(KERN_ALERT "Unexpected interface number %d was found. Unit may be in update mode.", interface_number);
		error_code = -EBADR; /* Interface 1 is the Device Firmware Upgrade Interface */
		goto return_error_code;
	}

	mj_driver_data = kzalloc(sizeof(struct maschine_jam_driver_data), GFP_KERNEL);
	if (mj_driver_data == NULL) {
		printk(KERN_ALERT "kzalloc(sizeof(struct maschine_jam_driver_data), GFP_KERNEL); FAILED");
		error_code = -ENOMEM;
		goto return_error_code;
	}
	maschine_jam_initialize_driver_data(mj_driver_data, mj_hid_device);
	error_code = maschine_jam_create_sound_card(mj_driver_data);
	if (error_code != 0){
		printk(KERN_ALERT "Failed to create sound card.");
		goto failure_free_driver_data;
	}
	error_code = maschine_jam_create_sysfs_inputs_interface(mj_driver_data);
	if (error_code != 0){
		printk(KERN_ALERT "Failed to create sysfs inputs interface attributes.");
		goto failure_delete_sound_card;
	}
	error_code = maschine_jam_create_sysfs_outputs_interface(mj_driver_data);
	if (error_code != 0){
		printk(KERN_ALERT "Failed to create sysfs outputs interface attributes.");
		goto failure_delete_sysfs_inputs_interface;
	}
	
	hid_set_drvdata(mj_hid_device, mj_driver_data);

	// TODO validate hid fields from some report
	error_code = hid_parse(mj_hid_device);
	if (error_code != 0) {
		printk(KERN_ALERT "hid parse failed");
		goto failure_delete_sysfs_outputs_interface;
	}
	error_code = hid_hw_start(mj_hid_device, HID_CONNECT_DEFAULT);
	if (error_code != 0) {
		printk(KERN_ALERT "hw start failed");
		goto failure_delete_sysfs_outputs_interface;
	}
	error_code = hid_hw_open(mj_hid_device);
	if (error_code != 0) {
		printk(KERN_ALERT "hw open failed");
		goto failure_hid_hw_stop;
	}

	printk(KERN_ALERT "Maschine JAM probe() finished - %d", error_code);
	goto return_error_code;

failure_hid_hw_stop:
	hid_hw_stop(mj_hid_device);
failure_delete_sysfs_outputs_interface:
	maschine_jam_delete_sysfs_outputs_interface(mj_driver_data);
failure_delete_sysfs_inputs_interface:
	maschine_jam_delete_sysfs_inputs_interface(mj_driver_data);
failure_delete_sound_card:
	maschine_jam_delete_sound_card(mj_driver_data);
failure_free_driver_data:
	kfree(mj_driver_data);
return_error_code:
	return error_code;
}

static void maschine_jam_remove(struct hid_device *mj_hid_device){
	struct maschine_jam_driver_data *mj_driver_data;

	if (mj_hid_device != NULL){
		mj_driver_data = hid_get_drvdata(mj_hid_device);

		hid_hw_stop(mj_hid_device);
		maschine_jam_delete_sound_card(mj_driver_data);
		maschine_jam_delete_sysfs_inputs_interface(mj_driver_data);
		maschine_jam_delete_sysfs_outputs_interface(mj_driver_data);
		kfree(mj_driver_data);
	}

	printk(KERN_ALERT "Maschine JAM removed!");
}

static const struct hid_device_id maschine_jam_devices[] = {
	{
		HID_USB_DEVICE(
			USB_VENDOR_ID_NATIVE_INSTRUMENTS,
			USB_DEVICE_ID_MASCHINE_JAM),
		.driver_data = 0
	},
	{ }
};
MODULE_DEVICE_TABLE(hid, maschine_jam_devices);

static struct hid_driver maschine_jam_driver = {
	.name = "maschine-jam",
	.id_table = maschine_jam_devices,
	.raw_event = maschine_jam_raw_event,
	.probe = maschine_jam_probe,
	.remove = maschine_jam_remove,
};
module_hid_driver(maschine_jam_driver);

MODULE_LICENSE("GPL");
